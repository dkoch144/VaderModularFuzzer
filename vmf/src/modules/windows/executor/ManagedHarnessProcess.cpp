// SPDX-License-Identifier: GPL-2.0-only
#include "ManagedHarnessProcess.hpp"
#include "RuntimeException.hpp"

#include <windows.h>
#include <array>
#include <memory>
#include <random>
#include <type_traits>

using namespace vmf;

namespace
{
using Clock = std::chrono::steady_clock;

struct CloseHandleDeleter
{
    void operator()(void* handle) const
    {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
};
using Handle = std::unique_ptr<void, CloseHandleDeleter>;

std::wstring quoteArgument(const std::wstring& argument)
{
    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (wchar_t ch : argument)
    {
        if (ch == L'\\')
        {
            ++backslashes;
            continue;
        }
        quoted.append(ch == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        quoted += ch;
        backslashes = 0;
    }
    quoted.append(backslashes * 2, L'\\');
    quoted += L'"';
    return quoted;
}

[[noreturn]] void processError(const char* operation, RuntimeException::ErrorCodeEnum code)
{
    const auto error = GetLastError();
    const auto message = std::string(operation) + " (Windows error " + std::to_string(error) + ")";
    throw RuntimeException(message.c_str(), code);
}
struct Child
{
    Handle process;
    Handle job;
};

Child spawnHarness(const std::vector<std::string>& arguments, HANDLE input, HANDLE output, HANDLE errors)
{
    std::wstring command;
    for (const auto& argument : arguments)
    {
        if (!command.empty())
            command += L' ';
        command += quoteArgument(std::filesystem::u8path(argument).wstring());
    }

    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job)
        processError("Cannot create managed harness job", RuntimeException::UNEXPECTED_ERROR);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        processError("Cannot configure managed harness job", RuntimeException::UNEXPECTED_ERROR);

    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    auto attributeBuffer = std::make_unique<unsigned char[]>(attributeBytes);
    auto* attributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBuffer.get());
    if (!InitializeProcThreadAttributeList(attributeList, 1, 0, &attributeBytes))
        processError("Cannot prepare managed harness handle list", RuntimeException::UNEXPECTED_ERROR);
    const auto deleteAttributes = [](LPPROC_THREAD_ATTRIBUTE_LIST list) { DeleteProcThreadAttributeList(list); };
    std::array<HANDLE, 3> handles{input, output, errors};
    const auto handleCount = output == errors ? 2u : 3u;
    std::unique_ptr<std::remove_pointer_t<LPPROC_THREAD_ATTRIBUTE_LIST>, decltype(deleteAttributes)>
        attributes(attributeList, deleteAttributes);

    if (!UpdateProcThreadAttribute(attributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   handles.data(), handleCount * sizeof(HANDLE), nullptr, nullptr))
        processError("Cannot restrict managed harness handle inheritance", RuntimeException::UNEXPECTED_ERROR);

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input;
    startup.StartupInfo.hStdOutput = output;
    startup.StartupInfo.hStdError = errors;
    startup.lpAttributeList = attributeList;
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup.StartupInfo, &child))
        processError("Cannot start managed harness", RuntimeException::CONFIGURATION_ERROR);

    Handle process(child.hProcess);
    Handle thread(child.hThread);
    if (!AssignProcessToJobObject(job.get(), process.get()))
    {
        const DWORD error = GetLastError();
        TerminateProcess(process.get(), 1);
        SetLastError(error);
        processError("Cannot assign managed harness to job", RuntimeException::UNEXPECTED_ERROR);
    }
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1))
        processError("Cannot resume managed harness", RuntimeException::UNEXPECTED_ERROR);
    return {std::move(process), std::move(job)};
}

DWORD remainingTime(Clock::time_point deadline)
{
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return remaining > 0 ? static_cast<DWORD>(remaining) : 0;
}

Handle openNull(SECURITY_ATTRIBUTES& inheritable)
{
    Handle output(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (output.get() == INVALID_HANDLE_VALUE)
        processError("Cannot open managed harness output", RuntimeException::UNEXPECTED_ERROR);
    return output;
}

struct Pipe
{
    Handle parent;
    Handle child;
};

Pipe openPipe(bool parentWrites, SECURITY_ATTRIBUTES& inheritable, HANDLE event)
{
    std::random_device random;
    const auto name = L"\\\\.\\pipe\\vmf-managed-" + std::to_wstring(GetCurrentProcessId()) +
                      L"-" + std::to_wstring(random()) + L"-" + std::to_wstring(random()) +
                      L"-" + std::to_wstring(random()) + L"-" + std::to_wstring(random());
    const DWORD access = parentWrites ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND;
    Handle parent(CreateNamedPipeW(name.c_str(), access | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                   1, 4096, 4096, 0, nullptr));
    if (parent.get() == INVALID_HANDLE_VALUE)
        processError("Cannot create managed harness pipe", RuntimeException::UNEXPECTED_ERROR);

    Handle child(CreateFileW(name.c_str(), parentWrites ? GENERIC_READ : GENERIC_WRITE, 0, &inheritable,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (child.get() == INVALID_HANDLE_VALUE)
        processError("Cannot open managed harness pipe", RuntimeException::UNEXPECTED_ERROR);

    // CreateFile has already connected the only client of this pipe instance.
    OVERLAPPED connect{};
    connect.hEvent = event;
    if (!ConnectNamedPipe(parent.get(), &connect))
    {
        const DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING)
        {
            CancelIoEx(parent.get(), &connect);
            DWORD count;
            GetOverlappedResult(parent.get(), &connect, &count, TRUE);
        }
        if (error != ERROR_PIPE_CONNECTED)
        {
            SetLastError(error);
            processError("Cannot connect managed harness pipe", RuntimeException::UNEXPECTED_ERROR);
        }
    }
    return {std::move(parent), std::move(child)};
}

bool pipeClosed(DWORD error)
{
    return error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA || error == ERROR_PIPE_NOT_CONNECTED;
}

template<typename StartIo>
ManagedHarnessResult transfer(HANDLE pipe, HANDLE child, HANDLE event, DWORD size,
                              Clock::time_point deadline, StartIo startIo)
{
    DWORD offset = 0;
    while (offset < size)
    {
        const DWORD remaining = remainingTime(deadline);
        if (!remaining)
            return ManagedHarnessResult::TimedOut;

        OVERLAPPED io{};
        io.hEvent = event;
        if (!ResetEvent(event))
            processError("Cannot reset managed harness I/O event", RuntimeException::UNEXPECTED_ERROR);
        DWORD count = 0;
        if (!startIo(offset, size - offset, &io))
        {
            const DWORD error = GetLastError();
            if (pipeClosed(error))
                return ManagedHarnessResult::Failed;
            if (error != ERROR_IO_PENDING)
                processError("Cannot start managed harness I/O", RuntimeException::UNEXPECTED_ERROR);

            // Prefer completed I/O over child exit so a terminal response can be read.
            const std::array<HANDLE, 2> handles{event, child};
            const DWORD status = WaitForMultipleObjects(2, handles.data(), FALSE, remainingTime(deadline));
            if (status != WAIT_OBJECT_0)
            {
                const DWORD waitError = GetLastError();
                CancelIoEx(pipe, &io);
                const bool completed = GetOverlappedResult(pipe, &io, &count, TRUE) != FALSE;
                if (status == WAIT_TIMEOUT)
                    return ManagedHarnessResult::TimedOut;
                if (status == WAIT_OBJECT_0 + 1)
                {
                    if (!completed || !count)
                        return ManagedHarnessResult::Failed;
                    offset += count;
                    continue;
                }
                SetLastError(waitError);
                processError("Cannot wait for managed harness I/O", RuntimeException::UNEXPECTED_ERROR);
            }
        }
        if (!GetOverlappedResult(pipe, &io, &count, FALSE))
        {
            if (pipeClosed(GetLastError()))
                return ManagedHarnessResult::Failed;
            processError("Cannot finish managed harness I/O", RuntimeException::UNEXPECTED_ERROR);
        }
        if (!count)
            return ManagedHarnessResult::Failed;
        offset += count;
    }
    return ManagedHarnessResult::Success;
}
}

ManagedHarnessResult vmf::runManagedHarness(const std::vector<std::string>& arguments,
                                            const std::filesystem::path& stdinPath,
                                            std::chrono::milliseconds timeout)
{
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle input(CreateFileW(stdinPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, &inheritable,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (input.get() == INVALID_HANDLE_VALUE)
        processError("Cannot open managed harness stdin", RuntimeException::UNEXPECTED_ERROR);
    auto output = openNull(inheritable);
    const auto deadline = Clock::now() + timeout;
    auto child = spawnHarness(arguments, input.get(), output.get(), output.get());
    input.reset();
    output.reset();

    const auto result = WaitForSingleObject(child.process.get(), remainingTime(deadline));
    if (result == WAIT_TIMEOUT)
    {
        if (!TerminateJobObject(child.job.get(), 1))
            processError("Cannot stop managed harness job", RuntimeException::UNEXPECTED_ERROR);
        WaitForSingleObject(child.process.get(), INFINITE);
        return ManagedHarnessResult::TimedOut;
    }
    DWORD exitCode = 0;
    if (result != WAIT_OBJECT_0 || !GetExitCodeProcess(child.process.get(), &exitCode))
        processError("Cannot read managed harness exit status", RuntimeException::UNEXPECTED_ERROR);
    return exitCode == 0 ? ManagedHarnessResult::Success : ManagedHarnessResult::Failed;
}

struct ManagedHarnessSession::Process
{
    Handle input;
    Handle output;
    Handle event;
    Child child;

    ManagedHarnessResult send(const char* data, DWORD size, Clock::time_point deadline)
    {
        return transfer(input.get(), child.process.get(), event.get(), size, deadline,
                        [&](DWORD offset, DWORD length, OVERLAPPED* io) {
            return WriteFile(input.get(), data + offset, length, nullptr, io);
        });
    }

    ManagedHarnessResult receive(char& response, Clock::time_point deadline)
    {
        return transfer(output.get(), child.process.get(), event.get(), 1, deadline,
                        [&](DWORD, DWORD length, OVERLAPPED* io) {
            return ReadFile(output.get(), &response, length, nullptr, io);
        });
    }
};

ManagedHarnessSession::ManagedHarnessSession(const std::vector<std::string>& arguments,
                                             std::chrono::milliseconds timeout)
{
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event)
        processError("Cannot create managed harness I/O event", RuntimeException::UNEXPECTED_ERROR);
    auto input = openPipe(true, inheritable, event.get());
    auto output = openPipe(false, inheritable, event.get());
    auto errors = openNull(inheritable);

    const auto deadline = Clock::now() + timeout;
    process = std::make_unique<Process>();
    process->child = spawnHarness(arguments, input.child.get(), output.child.get(), errors.get());
    input.child.reset();
    output.child.reset();
    errors.reset();
    process->input = std::move(input.parent);
    process->output = std::move(output.parent);
    process->event = std::move(event);

    char response{};
    if (process->receive(response, deadline) != ManagedHarnessResult::Success || response != 'R')
    {
        stop();
        throw RuntimeException("Managed harness startup failed or timed out before signaling readiness",
                               RuntimeException::CONFIGURATION_ERROR);
    }
}

ManagedHarnessSession::~ManagedHarnessSession()
{
    stop();
}

void ManagedHarnessSession::stop() noexcept
{
    if (!process)
        return;
    if (TerminateJobObject(process->child.job.get(), 1))
        WaitForSingleObject(process->child.process.get(), INFINITE);
    process.reset();
}

bool ManagedHarnessSession::isRunning() const
{
    if (!process)
        return false;
    const auto status = WaitForSingleObject(process->child.process.get(), 0);
    if (status == WAIT_FAILED)
        processError("Cannot check managed harness status", RuntimeException::UNEXPECTED_ERROR);
    return status == WAIT_TIMEOUT;
}

ManagedHarnessResult ManagedHarnessSession::run(const char* input, std::uint32_t size,
                                                std::chrono::milliseconds timeout)
{
    const auto deadline = Clock::now() + timeout;
    std::array<char, 4> length{};
    for (std::size_t index = 0; index < length.size(); ++index)
        length[index] = static_cast<char>(size >> (8 * index));
    auto result = process->send(length.data(), static_cast<DWORD>(length.size()), deadline);
    if (result == ManagedHarnessResult::Success)
        result = process->send(input, size, deadline);
    char response{};
    if (result == ManagedHarnessResult::Success)
        result = process->receive(response, deadline);
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
