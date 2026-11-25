// SPDX-License-Identifier: GPL-2.0-only
#include "ExecutorModule.hpp"
#include "AFLFeedback.hpp"
#include "ModuleFactory.hpp"
#include "ModuleTestHelper.hpp"
#include <gtest/gtest.h>
#include <cstring>

using namespace vmf;

TEST(ManagedStdinExecutorTest, PublishesRepeatableCoverageForBinaryStdin)
{
    ModuleTestHelper helper;
    auto& factory = ModuleFactory::getInstance();
    auto* executor = ExecutorModule::castTo(factory.buildModule("ManagedStdinExecutor", "ManagedStdinExecutor"));
    auto* feedback = FeedbackModule::castTo(factory.buildModule("AFLFeedback", "AFLFeedback"));
    helper.addModule(executor);
    helper.addModule(feedback);
    helper.getConfig()->setStringVectorParam("ManagedStdinExecutor", "sutArgv",
                                            {VMF_TEST_DOTNET, VMF_TEST_MANAGED_HARNESS});
    auto* registry = helper.getRegistry();
    const int testcase = registry->registerKey("TEST_CASE", StorageRegistry::BUFFER, StorageRegistry::WRITE_ONLY);
    const int trace = registry->registerKey("AFL_TRACE_BITS", StorageRegistry::BUFFER_TEMP, StorageRegistry::READ_ONLY);
    const int novel = registry->registerTag("HAS_NEW_COVERAGE", StorageRegistry::READ_ONLY);
    const int success = registry->registerTag("RAN_SUCCESSFULLY", StorageRegistry::READ_ONLY);
    helper.initializeModulesAndStorage();
    auto& storage = *helper.getStorage();
    char a[] = {0, static_cast<char>(255)};
    char b[] = {0, static_cast<char>(254)};
    auto* first = storage.createNewEntry();
    auto* second = storage.createNewEntry();
    auto* repeated = storage.createNewEntry();
    first->allocateAndCopyBuffer(testcase, sizeof(a), a);
    second->allocateAndCopyBuffer(testcase, sizeof(b), b);
    repeated->allocateAndCopyBuffer(testcase, sizeof(a), a);
    auto entries = storage.getNewEntries();
    executor->runTestCases(storage, entries);
    for (auto* entry : {first, second, repeated})
    {
        ASSERT_TRUE(entry->hasTag(success));
        ASSERT_EQ(entry->getBufferSize(trace), 65536);
    }
    EXPECT_NE(std::memcmp(first->getBufferPointer(trace), second->getBufferPointer(trace), 65536), 0);
    EXPECT_EQ(std::memcmp(first->getBufferPointer(trace), repeated->getBufferPointer(trace), 65536), 0);
    EXPECT_TRUE(first->hasTag(novel));
    EXPECT_TRUE(second->hasTag(novel));
    EXPECT_FALSE(repeated->hasTag(novel));

    entries->resetIndex();
    feedback->evaluateTestCaseResults(storage, entries);
    auto saved = storage.getSavedEntries();
    std::size_t savedCount = 0;
    while (saved->hasNext())
    {
        auto* entry = saved->getNext();
        EXPECT_TRUE(entry == first || entry == second);
        ++savedCount;
    }
    EXPECT_EQ(savedCount, 2u);
}
