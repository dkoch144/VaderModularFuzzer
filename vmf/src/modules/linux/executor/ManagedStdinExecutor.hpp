// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "AFLCoverageUtil.hpp"
#include "ExecutorModule.hpp"
#include <array>
#include <chrono>
#include <vector>

namespace vmf
{
class ManagedStdinExecutor : public ExecutorModule
{
public:
    explicit ManagedStdinExecutor(std::string name);
    static Module* build(std::string name);

    void init(ConfigInterface& config) override;
    void registerStorageNeeds(StorageRegistry& registry) override;
    void runTestCase(StorageModule& storage, StorageEntry* entry) override;

private:
    static constexpr std::size_t coverageMapSize = 65536;
    std::vector<std::string> arguments;
    std::chrono::milliseconds timeout{};
    AFLCoverageUtil coverage;
    alignas(uint64_t) std::array<uint8_t, coverageMapSize> cumulative{};

    int testcaseKey{};
    int timingKey{};
    int traceKey{};
    int coverageCountKey{};
    int novelTag{};
    int successTag{};
    int incompleteTag{};
};
}
