// SPDX-License-Identifier: GPL-2.0-only
#include "ManagedHarnessProcess.hpp"
#include "RuntimeException.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

using namespace vmf;

namespace
{
using Clock = std::chrono::steady_clock;

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

bool vmf::runManagedHarness(const std::vector<std::string>& arguments, const std::filesystem::path& stdinPath,
                           std::chrono::milliseconds timeout)
{
    std::vector<char*> argv;
    for (const auto& argument : arguments)
        argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);

    const auto closeFile = [](FILE* file) { fclose(file); };
    std::unique_ptr<FILE, decltype(closeFile)> input(fopen(stdinPath.c_str(), "rb"), closeFile);
    if (!input)
        throw RuntimeException("Cannot open managed harness stdin", RuntimeException::UNEXPECTED_ERROR);
    pid_t child = 0;
    const auto deadline = Clock::now() + timeout;
    const int error = spawnHarness(argv.data(), fileno(input.get()), child);
    if (error != 0)
        throw RuntimeException(("Cannot start managed harness: " + std::string(std::strerror(error))).c_str(),
                               RuntimeException::CONFIGURATION_ERROR);
    return waitForHarness(child, deadline);
}
