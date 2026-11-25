// SPDX-License-Identifier: GPL-2.0-only
#include "ManagedStdinExecutor.hpp"
#include "ManagedHarnessProcess.hpp"
#include "Logging.hpp"
#include "ModuleFactory.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <random>
#include <utility>

using namespace vmf;
REGISTER_MODULE(ManagedStdinExecutor);

namespace
{
using Clock = std::chrono::steady_clock;

class ScratchDirectory
{
public:
    ScratchDirectory()
    {
        const auto root = std::filesystem::temp_directory_path();
#ifdef _WIN32
        std::random_device random;
        for (int attempt = 0; attempt < 16; ++attempt)
        {
            auto candidate = root / ("vmf-managed-" + std::to_string(random()) + "-" + std::to_string(random()));
            if (std::filesystem::create_directory(candidate))
            {
                directory = std::move(candidate);
                return;
            }
        }
#else
        auto pattern = (root / "vmf-managed-XXXXXX").string();
        if (mkdtemp(pattern.data()))
        {
            directory = std::move(pattern);
            return;
        }
#endif
        throw RuntimeException("Cannot create managed harness scratch directory", RuntimeException::UNEXPECTED_ERROR);
    }

    ~ScratchDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }

    std::filesystem::path path(const char* name) const { return directory / name; }

private:
    std::filesystem::path directory;
};
}

struct ManagedStdinExecutor::ReusedHarness
{
    ScratchDirectory scratch;
    std::unique_ptr<ManagedHarnessSession> session;
};

ManagedStdinExecutor::ManagedStdinExecutor(std::string name) : ExecutorModule(std::move(name))
{
    cumulative.fill(255);
}

ManagedStdinExecutor::~ManagedStdinExecutor() = default;

Module* ManagedStdinExecutor::build(std::string name)
{
    return new ManagedStdinExecutor(std::move(name));
}

void ManagedStdinExecutor::init(ConfigInterface& config)
{
    arguments = config.getStringVectorParam(getModuleName(), "sutArgv");
    timeout = std::chrono::milliseconds(config.getIntParam(getModuleName(), "timeoutInMs", 5000));
    if (arguments.empty() || arguments.front().empty() || timeout.count() <= 0)
        throw RuntimeException("ManagedStdinExecutor requires sutArgv and a positive timeoutInMs",
                               RuntimeException::CONFIGURATION_ERROR);
    reusedHarness.reset();
    reuseProcess = config.getBoolParam(getModuleName(), "reuseProcess", false);
}

void ManagedStdinExecutor::registerStorageNeeds(StorageRegistry& registry)
{
    testcaseKey = registry.registerKey("TEST_CASE", StorageRegistry::BUFFER, StorageRegistry::READ_ONLY);
    timingKey = registry.registerKey("EXEC_TIME_US", StorageRegistry::UINT, StorageRegistry::WRITE_ONLY);
    traceKey = registry.registerKey("AFL_TRACE_BITS", StorageRegistry::BUFFER_TEMP, StorageRegistry::WRITE_ONLY);
    coverageCountKey = registry.registerKey("COVERAGE_COUNT", StorageRegistry::UINT, StorageRegistry::WRITE_ONLY);
    novelTag = registry.registerTag("HAS_NEW_COVERAGE", StorageRegistry::WRITE_ONLY);
    successTag = registry.registerTag("RAN_SUCCESSFULLY", StorageRegistry::WRITE_ONLY);
    crashedTag = registry.registerTag("CRASHED", StorageRegistry::WRITE_ONLY);
    hungTag = registry.registerTag("HUNG", StorageRegistry::WRITE_ONLY);
}

void ManagedStdinExecutor::runTestCase(StorageModule&, StorageEntry* entry)
{
    std::optional<ScratchDirectory> scratch;
    std::filesystem::path outputPath;
    auto start = Clock::now();
    ManagedHarnessResult result;
    const auto size = entry->getBufferSize(testcaseKey);
    if (reuseProcess)
    {
        if (!reusedHarness)
            reusedHarness = std::make_unique<ReusedHarness>();
        auto& harness = *reusedHarness;
        outputPath = harness.scratch.path("coverage");
        if (!harness.session || !harness.session->isRunning())
        {
            auto command = arguments;
            command.push_back(outputPath.u8string());
            command.push_back(harness.scratch.path("ready").u8string());
            command.push_back("--vmf-reuse-stdin");
            harness.session.reset();
            harness.session = std::make_unique<ManagedHarnessSession>(command, timeout);
        }
        std::filesystem::remove(outputPath);
        try
        {
            const char* input = size > 0 ? entry->getBufferPointer(testcaseKey) : nullptr;
            result = harness.session->run(input, static_cast<std::uint32_t>(std::max(size, 0)), timeout);
        }
        catch (...)
        {
            // A partial request cannot be retried on the same stream.
            harness.session.reset();
            throw;
        }
    }
    else
    {
        scratch.emplace();
        const auto inputPath = scratch->path("stdin");
        outputPath = scratch->path("coverage");
        const auto readyPath = scratch->path("ready");
        auto command = arguments;
        command.push_back(outputPath.u8string());
        command.push_back(readyPath.u8string());

        std::ofstream input(inputPath, std::ios::binary);
        if (size > 0)
            input.write(entry->getBufferPointer(testcaseKey), size);
        input.close();
        if (!input)
            throw RuntimeException("Cannot stage managed harness stdin", RuntimeException::UNEXPECTED_ERROR);

        start = Clock::now();
        result = runManagedHarness(command, inputPath, timeout);
        if (!std::filesystem::is_regular_file(readyPath))
        {
            throw RuntimeException(
                result == ManagedHarnessResult::TimedOut ?
                    "Managed harness startup timed out before signaling readiness" :
                    "Managed harness startup failed before signaling readiness; check sutArgv and the harness arguments",
                RuntimeException::CONFIGURATION_ERROR);
        }
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start);
    entry->setValue(timingKey, static_cast<unsigned int>(std::clamp<decltype(elapsed)::rep>(
        elapsed.count(), 1, std::numeric_limits<unsigned int>::max())));
    entry->setValue(coverageCountKey, 0u);
    if (result == ManagedHarnessResult::ResetFailed)
    {
        LOG_ERROR << "Managed harness failed to reset testcase state. VMF will start a replacement harness.";
        return;
    }
    if (result != ManagedHarnessResult::Success)
    {
        entry->addTag(result == ManagedHarnessResult::TimedOut ? hungTag : crashedTag);
        return;
    }

    alignas(uint64_t) std::array<uint8_t, coverageMapSize> trace{};
    std::ifstream map(outputPath, std::ios::binary);
    if (!map.read(reinterpret_cast<char*>(trace.data()), trace.size()) || map.peek() != EOF)
        throw RuntimeException("Managed harness must write a 65536-byte coverage map",
                               RuntimeException::CONFIGURATION_ERROR);

    coverage.classifyCounts(trace.data(), coverageMapSize);
    entry->allocateAndCopyBuffer(traceKey, coverageMapSize, reinterpret_cast<char*>(trace.data()));
    entry->setValue(coverageCountKey, coverage.countBytes(trace.data(), coverageMapSize));
    if (coverage.hasNewBits(trace.data(), cumulative.data(), coverageMapSize))
        entry->addTag(novelTag);
    entry->addTag(successTag);
}
