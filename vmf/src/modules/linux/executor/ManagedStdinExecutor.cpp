// SPDX-License-Identifier: GPL-2.0-only
#include "ManagedStdinExecutor.hpp"
#include "ModuleFactory.hpp"
#include <algorithm>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <utility>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

using namespace vmf;
REGISTER_MODULE(ManagedStdinExecutor);

namespace
{
using Clock = std::chrono::steady_clock;

class CoverageFile
{
public:
    CoverageFile()
    {
        std::string pattern = (std::filesystem::temp_directory_path() / "vmf-managed-XXXXXX").string();
        if (!mkdtemp(pattern.data()))
            throw RuntimeException("Cannot create managed harness scratch directory", RuntimeException::UNEXPECTED_ERROR);
        directory = std::move(pattern);
    }

    ~CoverageFile()
    {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }

    std::string path() const { return directory + "/coverage"; }

private:
    std::string directory;
};

int spawnHarness(char* const* argv, int stdinFd, pid_t& child)
{
    posix_spawn_file_actions_t actions;
    int result = posix_spawn_file_actions_init(&actions);
    if (result != 0)
        return result;

    result = posix_spawn_file_actions_adddup2(&actions, stdinFd, STDIN_FILENO);
    if (result == 0 && stdinFd != STDIN_FILENO)
        result = posix_spawn_file_actions_addclose(&actions, stdinFd);
    posix_spawnattr_t attributes;
    if (result == 0)
    {
        result = posix_spawnattr_init(&attributes);
        if (result == 0)
        {
            result = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
            if (result == 0)
                result = posix_spawnp(&child, argv[0], &actions, &attributes, argv, environ);
            posix_spawnattr_destroy(&attributes);
        }
    }
    posix_spawn_file_actions_destroy(&actions);
    return result;
}

bool waitForHarness(pid_t child, Clock::time_point deadline)
{
    int status = 0;
    for (;;)
    {
        siginfo_t info{};
        const auto result = waitid(P_PID, child, &info, WEXITED | WNOHANG | WNOWAIT);
        if (result < 0 && errno != EINTR)
            throw RuntimeException("Cannot wait for managed harness", RuntimeException::UNEXPECTED_ERROR);
        const bool exited = info.si_pid == child;
        if (exited || Clock::now() >= deadline)
        {
            // Keep the child unreaped until its process group has been cleaned up.
            if (kill(-child, SIGKILL) < 0 && errno != ESRCH && !exited)
                throw RuntimeException("Cannot stop managed harness", RuntimeException::UNEXPECTED_ERROR);
            pid_t reaped;
            do
                reaped = waitpid(child, &status, 0);
            while (reaped < 0 && errno == EINTR);
            return exited && reaped == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
}

ManagedStdinExecutor::ManagedStdinExecutor(std::string name) : ExecutorModule(std::move(name))
{
    cumulative.fill(255);
}

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
}

void ManagedStdinExecutor::registerStorageNeeds(StorageRegistry& registry)
{
    testcaseKey = registry.registerKey("TEST_CASE", StorageRegistry::BUFFER, StorageRegistry::READ_ONLY);
    timingKey = registry.registerKey("EXEC_TIME_US", StorageRegistry::UINT, StorageRegistry::WRITE_ONLY);
    traceKey = registry.registerKey("AFL_TRACE_BITS", StorageRegistry::BUFFER_TEMP, StorageRegistry::WRITE_ONLY);
    coverageCountKey = registry.registerKey("COVERAGE_COUNT", StorageRegistry::UINT, StorageRegistry::WRITE_ONLY);
    novelTag = registry.registerTag("HAS_NEW_COVERAGE", StorageRegistry::WRITE_ONLY);
    successTag = registry.registerTag("RAN_SUCCESSFULLY", StorageRegistry::WRITE_ONLY);
    incompleteTag = registry.registerTag("INCOMPLETE", StorageRegistry::WRITE_ONLY);
}

void ManagedStdinExecutor::runTestCase(StorageModule&, StorageEntry* entry)
{
    CoverageFile output;
    auto outputPath = output.path();
    std::vector<char*> argv;
    for (auto& argument : arguments)
        argv.push_back(argument.data());
    argv.push_back(outputPath.data());
    argv.push_back(nullptr);

    const auto closeFile = [](FILE* file) { fclose(file); };
    std::unique_ptr<FILE, decltype(closeFile)> input(tmpfile(), closeFile);
    if (!input)
        throw RuntimeException("Cannot open managed harness stdin file", RuntimeException::UNEXPECTED_ERROR);
    const auto size = static_cast<std::size_t>(entry->getBufferSize(testcaseKey));
    if ((size != 0 && fwrite(entry->getBufferPointer(testcaseKey), 1, size, input.get()) != size) ||
        fseek(input.get(), 0, SEEK_SET) != 0)
        throw RuntimeException("Cannot stage managed harness stdin", RuntimeException::UNEXPECTED_ERROR);

    pid_t child = 0;
    const auto start = Clock::now();
    const int error = spawnHarness(argv.data(), fileno(input.get()), child);
    if (error != 0)
        throw RuntimeException(("Cannot start managed harness: " + std::string(std::strerror(error))).c_str(),
                               RuntimeException::CONFIGURATION_ERROR);

    const bool finished = waitForHarness(child, start + timeout);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start);
    entry->setValue(timingKey, static_cast<unsigned int>(std::clamp<decltype(elapsed)::rep>(
        elapsed.count(), 1, std::numeric_limits<unsigned int>::max())));
    entry->setValue(coverageCountKey, 0u);
    if (!finished)
    {
        entry->addTag(incompleteTag);
        return;
    }

    alignas(uint64_t) std::array<uint8_t, coverageMapSize> trace{};
    std::ifstream map(outputPath, std::ios::binary);
    if (!map.read(reinterpret_cast<char*>(trace.data()), trace.size()) || map.peek() != EOF)
        throw RuntimeException("Managed harness must write a 65536-byte coverage map",
                               RuntimeException::CONFIGURATION_ERROR);

    coverage.classifyCounts(trace.data(), trace.size());
    entry->allocateAndCopyBuffer(traceKey, trace.size(), reinterpret_cast<char*>(trace.data()));
    entry->setValue(coverageCountKey, coverage.countBytes(trace.data(), trace.size()));
    if (coverage.hasNewBits(trace.data(), cumulative.data(), trace.size()))
        entry->addTag(novelTag);
    entry->addTag(successTag);
}
