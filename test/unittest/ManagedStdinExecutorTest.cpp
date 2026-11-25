// SPDX-License-Identifier: GPL-2.0-only
#include "ExecutorModule.hpp"
#include "AFLFeedback.hpp"
#include "ModuleFactory.hpp"
#include "ModuleTestHelper.hpp"
#include <gtest/gtest.h>
#include <cstring>

using namespace vmf;

class ManagedStdinExecutorTest : public testing::Test
{
protected:
    void configure(const std::string& assembly = VMF_TEST_MANAGED_HARNESS, bool reuseProcess = false)
    {
        auto& factory = ModuleFactory::getInstance();
        executor = ExecutorModule::castTo(factory.buildModule("ManagedStdinExecutor", "ManagedStdinExecutor"));
        feedback = FeedbackModule::castTo(factory.buildModule("AFLFeedback", "AFLFeedback"));
        helper.addModule(executor);
        helper.addModule(feedback);
        helper.getConfig()->setStringVectorParam("ManagedStdinExecutor", "sutArgv", {VMF_TEST_DOTNET, assembly});
        helper.getConfig()->setIntParam("ManagedStdinExecutor", "timeoutInMs", 3000);
        if (reuseProcess)
            helper.getConfig()->setBoolParam("ManagedStdinExecutor", "reuseProcess", true);
        auto* registry = helper.getRegistry();
        testcase = registry->registerKey("TEST_CASE", StorageRegistry::BUFFER, StorageRegistry::WRITE_ONLY);
        trace = registry->registerKey("AFL_TRACE_BITS", StorageRegistry::BUFFER_TEMP, StorageRegistry::READ_ONLY);
        novel = registry->registerTag("HAS_NEW_COVERAGE", StorageRegistry::READ_ONLY);
        success = registry->registerTag("RAN_SUCCESSFULLY", StorageRegistry::READ_ONLY);
        crashed = registry->registerTag("CRASHED", StorageRegistry::READ_ONLY);
        hung = registry->registerTag("HUNG", StorageRegistry::READ_ONLY);
        helper.initializeModulesAndStorage();
    }

    StorageEntry* addTestcase(char first, char second)
    {
        char bytes[] = {first, second};
        auto* entry = helper.getStorage()->createNewEntry();
        entry->allocateAndCopyBuffer(testcase, sizeof(bytes), bytes);
        return entry;
    }

    void expectReplacementStartupFailure(char input)
    {
        configure(VMF_TEST_MANAGED_HARNESS, true);
        auto& storage = *helper.getStorage();
        auto* stopped = addTestcase(input, 0);
        auto* next = addTestcase('F', 0);

        executor->runTestCase(storage, stopped);
        EXPECT_TRUE(stopped->hasTag(crashed));
        try
        {
            executor->runTestCase(storage, next);
            FAIL() << "Expected replacement harness startup to fail";
        }
        catch (RuntimeException& error)
        {
            EXPECT_EQ(error.getErrorCode(), RuntimeException::CONFIGURATION_ERROR);
            EXPECT_NE(std::string(error.getReason()).find("startup"), std::string::npos);
        }
        EXPECT_FALSE(next->hasTag(crashed));
        EXPECT_FALSE(next->hasTag(hung));
    }

    ModuleTestHelper helper;
    ExecutorModule* executor{};
    FeedbackModule* feedback{};
    int testcase{};
    int trace{};
    int novel{};
    int success{};
    int crashed{};
    int hung{};
};

#ifdef __linux__
TEST_F(ManagedStdinExecutorTest, ReusedHarnessDeliversEmptyStdinForAnUnallocatedTestcase)
{
    configure(VMF_TEST_MANAGED_HARNESS, true);
    auto& storage = *helper.getStorage();
    auto* entry = storage.createNewEntry();

    executor->runTestCase(storage, entry);

    EXPECT_TRUE(entry->hasTag(success));
}

TEST_F(ManagedStdinExecutorTest, ReusesHarnessAndPublishesRepeatableCoverage)
{
    configure(VMF_TEST_MANAGED_HARNESS, true);
    auto& storage = *helper.getStorage();
    auto* first = addTestcase(0, static_cast<char>(255));
    auto* requiresPriorTestcase = addTestcase('P', 0);
    auto* repeated = addTestcase(0, static_cast<char>(255));

    auto entries = storage.getNewEntries();
    executor->runTestCases(storage, entries);

    ASSERT_TRUE(first->hasTag(success));
    EXPECT_TRUE(requiresPriorTestcase->hasTag(success));
    ASSERT_TRUE(repeated->hasTag(success));
    ASSERT_EQ(first->getBufferSize(trace), 65536);
    ASSERT_EQ(requiresPriorTestcase->getBufferSize(trace), 65536);
    ASSERT_EQ(repeated->getBufferSize(trace), 65536);
    EXPECT_NE(std::memcmp(first->getBufferPointer(trace), requiresPriorTestcase->getBufferPointer(trace), 65536), 0);
    EXPECT_EQ(std::memcmp(first->getBufferPointer(trace), repeated->getBufferPointer(trace), 65536), 0);
}

TEST_F(ManagedStdinExecutorTest, ReplacesReusedHarnessAfterTestcaseFailures)
{
    configure(VMF_TEST_MANAGED_HARNESS, true);
    auto& storage = *helper.getStorage();
    auto* rejected = addTestcase('R', 0);
    auto* unexpected = addTestcase('E', 0);
    auto* afterException = addTestcase('F', 0);
    auto* timedOut = addTestcase('T', 0);
    auto* afterTimeout = addTestcase('F', 0);
    auto* exited = addTestcase('D', 0);
    auto* afterExit = addTestcase('F', 0);
    auto entries = storage.getNewEntries();

    executor->runTestCases(storage, entries);

    for (auto* entry : {rejected, afterException, afterTimeout, afterExit})
        EXPECT_TRUE(entry->hasTag(success));
    EXPECT_TRUE(unexpected->hasTag(crashed));
    EXPECT_TRUE(exited->hasTag(crashed));
    EXPECT_TRUE(timedOut->hasTag(hung));
    for (auto* entry : {unexpected, timedOut, exited})
        EXPECT_FALSE(entry->hasTag(success));
}

TEST_F(ManagedStdinExecutorTest, ReportsResetFailureAndReplacesHarness)
{
    configure(VMF_TEST_MANAGED_HARNESS, true);
    auto& storage = *helper.getStorage();
    auto* resetFailure = addTestcase('S', 0);
    auto* afterReset = addTestcase('F', 0);
    auto entries = storage.getNewEntries();
    executor->runTestCases(storage, entries);

    EXPECT_FALSE(resetFailure->hasTag(success));
    EXPECT_FALSE(resetFailure->hasTag(crashed));
    EXPECT_FALSE(resetFailure->hasTag(hung));
    EXPECT_FALSE(resetFailure->hasTag(novel));
    EXPECT_TRUE(afterReset->hasTag(success));
}

TEST_F(ManagedStdinExecutorTest, ReportsReplacementStartupFailureAsCampaignError)
{
    expectReplacementStartupFailure('N');
}

TEST_F(ManagedStdinExecutorTest, ReportsReplacementReadinessTimeoutAsCampaignError)
{
    expectReplacementStartupFailure('W');
}

TEST_F(ManagedStdinExecutorTest, StartsFreshProcessesByDefault)
{
    configure();
    auto& storage = *helper.getStorage();
    auto* first = addTestcase('F', 0);
    auto* second = addTestcase('F', 0);
    auto entries = storage.getNewEntries();

    executor->runTestCases(storage, entries);

    EXPECT_TRUE(first->hasTag(success));
    EXPECT_TRUE(second->hasTag(success));
}
#endif

TEST_F(ManagedStdinExecutorTest, PublishesRepeatableCoverageForBinaryStdin)
{
    configure();
    auto& storage = *helper.getStorage();
    auto* first = addTestcase(0, static_cast<char>(255));
    auto* second = addTestcase(0, static_cast<char>(254));
    auto* repeated = addTestcase(0, static_cast<char>(255));
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

TEST_F(ManagedStdinExecutorTest, ClassifiesTestcaseOutcomesAndContinues)
{
    configure();
    auto& storage = *helper.getStorage();
    auto* rejected = addTestcase('R', 0);
    auto* unexpected = addTestcase('E', 0);
    auto* timedOut = addTestcase('T', 0);
    auto* valid = addTestcase(0, static_cast<char>(255));
    auto entries = storage.getNewEntries();

    executor->runTestCases(storage, entries);

    EXPECT_TRUE(rejected->hasTag(success));
    EXPECT_TRUE(unexpected->hasTag(crashed));
    EXPECT_TRUE(timedOut->hasTag(hung));
    EXPECT_TRUE(valid->hasTag(success));
    for (auto* entry : {rejected, valid})
    {
        EXPECT_FALSE(entry->hasTag(crashed));
        EXPECT_FALSE(entry->hasTag(hung));
    }
    EXPECT_FALSE(unexpected->hasTag(hung));
    EXPECT_FALSE(timedOut->hasTag(crashed));
    EXPECT_FALSE(unexpected->hasTag(success));
    EXPECT_FALSE(timedOut->hasTag(success));
    EXPECT_FALSE(unexpected->hasTag(novel));
    EXPECT_FALSE(timedOut->hasTag(novel));
}

TEST_F(ManagedStdinExecutorTest, ReportsMissingHarnessAsSetupError)
{
    configure(std::string(VMF_TEST_MANAGED_HARNESS) + ".missing.dll");
    auto* entry = addTestcase(0, static_cast<char>(255));
    try
    {
        executor->runTestCase(*helper.getStorage(), entry);
        FAIL() << "Expected harness setup to fail";
    }
    catch (RuntimeException& error)
    {
        EXPECT_EQ(error.getErrorCode(), RuntimeException::CONFIGURATION_ERROR);
        EXPECT_NE(std::string(error.getReason()).find("startup"), std::string::npos);
    }
    EXPECT_FALSE(entry->hasTag(crashed));
    EXPECT_FALSE(entry->hasTag(hung));
}
