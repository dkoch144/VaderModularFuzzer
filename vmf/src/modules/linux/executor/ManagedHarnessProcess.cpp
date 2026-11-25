// SPDX-License-Identifier: GPL-2.0-only
#include "ManagedHarnessProcess.hpp"
#include "RuntimeException.hpp"

#include <cerrno>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

using namespace vmf;

namespace
{
using Clock = std::chrono::steady_clock;

int spawnHarness(char* const* argv, int stdinFd, pid_t& child, bool useProtocol = false)
{
    posix_spawn_file_actions_t actions;
    int result = posix_spawn_file_actions_init(&actions);
    if (result != 0)
        return result;

    result = posix_spawn_file_actions_adddup2(&actions, stdinFd, STDIN_FILENO);
    if (result == 0 && useProtocol)
        result = posix_spawn_file_actions_adddup2(&actions, stdinFd, STDOUT_FILENO);
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

ManagedHarnessResult waitForHarness(pid_t child, Clock::time_point deadline)
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
            if (reaped != child)
                throw RuntimeException("Cannot reap managed harness", RuntimeException::UNEXPECTED_ERROR);
            if (!exited)
                return ManagedHarnessResult::TimedOut;
            return WIFEXITED(status) && WEXITSTATUS(status) == 0 ?
                ManagedHarnessResult::Success : ManagedHarnessResult::Failed;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

bool hasExited(pid_t child)
{
    siginfo_t info{};
    int result;
    do
        result = waitid(P_PID, child, &info, WEXITED | WNOHANG | WNOWAIT);
    while (result < 0 && errno == EINTR);
    if (result < 0)
        throw RuntimeException("Cannot check managed harness status", RuntimeException::UNEXPECTED_ERROR);
    return info.si_pid == child;
}

ManagedHarnessResult transfer(int channel, pid_t child, const char* outgoing, char* incoming,
                              std::size_t remaining, Clock::time_point deadline)
{
    while (remaining != 0)
    {
        if (outgoing && hasExited(child))
            return ManagedHarnessResult::Failed;
        const auto timeLeft = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
        if (timeLeft.count() <= 0)
            return ManagedHarnessResult::TimedOut;

        pollfd descriptor{channel, static_cast<short>(outgoing ? POLLOUT : POLLIN), 0};
        // Check child exit even when a descendant has inherited the protocol descriptor.
        const int ready = poll(&descriptor, 1, std::min<int64_t>(timeLeft.count(), 25));
        if (ready == 0 && hasExited(child))
            return ManagedHarnessResult::Failed;
        if (ready == 0 || (ready < 0 && errno == EINTR))
            continue;
        if (ready < 0)
            throw RuntimeException("Cannot poll managed harness channel", RuntimeException::UNEXPECTED_ERROR);

        const auto count = outgoing
            ? send(channel, outgoing, remaining, MSG_DONTWAIT | MSG_NOSIGNAL)
            : recv(channel, incoming, remaining, MSG_DONTWAIT);
        if (count < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (count <= 0)
            return ManagedHarnessResult::Failed;
        remaining -= count;
        if (outgoing)
            outgoing += count;
        else
            incoming += count;
    }
    return ManagedHarnessResult::Success;
}
}

ManagedHarnessResult vmf::runManagedHarness(const std::vector<std::string>& arguments, const std::filesystem::path& stdinPath,
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

ManagedHarnessSession::ManagedHarnessSession(const std::vector<std::string>& arguments,
                                             std::chrono::milliseconds timeout)
{
    std::vector<char*> argv;
    for (const auto& argument : arguments)
        argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);

    int endpoints[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, endpoints) < 0)
        throw RuntimeException("Cannot open managed harness channel", RuntimeException::UNEXPECTED_ERROR);
    const int childChannel = fcntl(endpoints[1], F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    close(endpoints[1]);
    channel = endpoints[0];
    if (childChannel < 0)
    {
        stop();
        throw RuntimeException("Cannot duplicate managed harness channel", RuntimeException::UNEXPECTED_ERROR);
    }
    const auto deadline = Clock::now() + timeout;
    const int error = spawnHarness(argv.data(), childChannel, child, true);
    close(childChannel);
    if (error != 0)
    {
        child = -1;
        stop();
        throw RuntimeException(("Cannot start managed harness: " + std::string(std::strerror(error))).c_str(),
                               RuntimeException::CONFIGURATION_ERROR);
    }

    char response{};
    try
    {
        if (transfer(channel, child, nullptr, &response, 1, deadline) == ManagedHarnessResult::Success &&
            response == 'R')
            return;
    }
    catch (...)
    {
        stop();
        throw;
    }
    stop();
    throw RuntimeException("Managed harness startup failed or timed out before signaling readiness",
                           RuntimeException::CONFIGURATION_ERROR);
}

ManagedHarnessSession::~ManagedHarnessSession()
{
    stop();
}

bool ManagedHarnessSession::isRunning() const
{
    return child > 0 && !hasExited(child);
}

void ManagedHarnessSession::stop() noexcept
{
    if (channel >= 0)
    {
        close(channel);
        channel = -1;
    }
    if (child > 0)
    {
        kill(-child, SIGKILL);
        while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
        child = -1;
    }
}

ManagedHarnessResult ManagedHarnessSession::run(const char* input, std::uint32_t size,
                                                std::chrono::milliseconds timeout)
{
    const auto deadline = Clock::now() + timeout;
    std::array<char, 4> length{};
    for (std::size_t index = 0; index < length.size(); ++index)
        length[index] = static_cast<char>(size >> (8 * index));

    char response{};
    auto result = transfer(channel, child, length.data(), nullptr, length.size(), deadline);
    if (result == ManagedHarnessResult::Success)
        result = transfer(channel, child, input, nullptr, size, deadline);
    if (result == ManagedHarnessResult::Success)
        result = transfer(channel, child, nullptr, &response, 1, deadline);
    if (result == ManagedHarnessResult::Success && response == 'C')
        return result;

    stop();
    if (result == ManagedHarnessResult::Success && response == 'X')
        return ManagedHarnessResult::ResetFailed;
    if (result == ManagedHarnessResult::Success)
        throw RuntimeException("Managed harness sent an invalid completion response",
                               RuntimeException::UNEXPECTED_ERROR);
    return result;
}
