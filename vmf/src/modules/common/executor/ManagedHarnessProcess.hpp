// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace vmf
{
enum class ManagedHarnessResult
{
    Success,
    Failed,
    TimedOut
};

ManagedHarnessResult runManagedHarness(const std::vector<std::string>& arguments, const std::filesystem::path& stdinPath,
                       std::chrono::milliseconds timeout);
}
