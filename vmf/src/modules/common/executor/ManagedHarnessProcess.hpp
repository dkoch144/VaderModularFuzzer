// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace vmf
{
bool runManagedHarness(const std::vector<std::string>& arguments, const std::filesystem::path& stdinPath,
                       std::chrono::milliseconds timeout);
}
