// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace vmf
{
enum class ManagedHarnessResult
{
    Success,
    Failed,
    TimedOut,
    ResetFailed
};

ManagedHarnessResult runManagedHarness(const std::vector<std::string>& arguments, const std::filesystem::path& stdinPath,
                       std::chrono::milliseconds timeout);

class ManagedHarnessSession
{
public:
    ManagedHarnessSession(const std::vector<std::string>& arguments, std::chrono::milliseconds timeout);
    ~ManagedHarnessSession();
    ManagedHarnessSession(const ManagedHarnessSession&) = delete;
    ManagedHarnessSession& operator=(const ManagedHarnessSession&) = delete;

    bool isRunning() const;
    ManagedHarnessResult run(const char* input, std::uint32_t size, std::chrono::milliseconds timeout);

private:
    void stop() noexcept;
#ifdef _WIN32
    struct Process;
    std::unique_ptr<Process> process;
#else
    int channel = -1;
    int child = -1;
#endif
};
}
