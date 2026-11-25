// SPDX-License-Identifier: GPL-2.0-only
#include "ManagedHarnessProcess.hpp"
#include "RuntimeException.hpp"

#include <windows.h>
#include <array>
#include <memory>
#include <type_traits>

using namespace vmf;

namespace
{
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
}

ManagedHarnessResult vmf::runManagedHarness(const std::vector<std::string>& arguments, const std::filesystem::path& stdinPath,
                           std::chrono::milliseconds timeout)
{
    std::wstring command;
    for (const auto& argument : arguments)
    {
        if (!command.empty())
            command += L' ';
        command += quoteArgument(std::filesystem::u8path(argument).wstring());
    }

    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle input(CreateFileW(stdinPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, &inheritable,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    Handle output(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (input.get() == INVALID_HANDLE_VALUE || output.get() == INVALID_HANDLE_VALUE)
        processError("Cannot open managed harness standard handles", RuntimeException::UNEXPECTED_ERROR);

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
    std::array<HANDLE, 2> handles{input.get(), output.get()};
    std::unique_ptr<std::remove_pointer_t<LPPROC_THREAD_ATTRIBUTE_LIST>, decltype(deleteAttributes)>
        attributes(attributeList, deleteAttributes);

    if (!UpdateProcThreadAttribute(attributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   handles.data(), sizeof(handles), nullptr, nullptr))
        processError("Cannot restrict managed harness handle inheritance", RuntimeException::UNEXPECTED_ERROR);

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.get();
    startup.StartupInfo.hStdOutput = output.get();
    startup.StartupInfo.hStdError = output.get();
    startup.lpAttributeList = attributeList;
    PROCESS_INFORMATION child{};
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup.StartupInfo, &child))
        processError("Cannot start managed harness", RuntimeException::CONFIGURATION_ERROR);

    input.reset();
    output.reset();
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

    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    const auto result = WaitForSingleObject(process.get(),
        remaining.count() > 0 ? static_cast<DWORD>(remaining.count()) : 0);
    if (result == WAIT_TIMEOUT)
    {
        if (!TerminateJobObject(job.get(), 1))
            processError("Cannot stop managed harness job", RuntimeException::UNEXPECTED_ERROR);
        WaitForSingleObject(process.get(), INFINITE);
        return ManagedHarnessResult::TimedOut;
    }
    DWORD exitCode = 0;
    if (result != WAIT_OBJECT_0 || !GetExitCodeProcess(process.get(), &exitCode))
        processError("Cannot read managed harness exit status", RuntimeException::UNEXPECTED_ERROR);
    return exitCode == 0 ? ManagedHarnessResult::Success : ManagedHarnessResult::Failed;
}
