#include <atomic>
#include <string>
#include <thread>

#include "gtest/gtest.h"
#include "tools/xcp_master/tui_model.hpp"

namespace {

using calmcar::xcp::tools::BoundedLineBuffer;
using calmcar::xcp::tools::CommandHistory;
using calmcar::xcp::tools::ConfirmationDecision;
using calmcar::xcp::tools::ConfirmationGate;
using calmcar::xcp::tools::LatestObservationModel;
using calmcar::xcp::tools::NextViewIndex;

/** @brief 验证面板缓冲在未满时保留插入顺序。 */
TEST(BoundedLineBufferTest, PreservesInsertionOrder) {
    BoundedLineBuffer buffer(3);
    buffer.Append("first");
    buffer.Append("second");

    ASSERT_EQ(buffer.Size(), 2U);
    EXPECT_EQ(buffer.Lines()[0], "first");
    EXPECT_EQ(buffer.Lines()[1], "second");
}

/** @brief 验证超过容量时淘汰最旧行并保留最新行。 */
TEST(BoundedLineBufferTest, EvictsOldestLineAtCapacity) {
    BoundedLineBuffer buffer(2);
    buffer.Append("old");
    buffer.Append("middle");
    buffer.Append("new");

    ASSERT_EQ(buffer.Size(), 2U);
    EXPECT_EQ(buffer.Lines()[0], "middle");
    EXPECT_EQ(buffer.Lines()[1], "new");
}

/** @brief 验证零容量缓冲不会保存任何行。 */
TEST(BoundedLineBufferTest, ZeroCapacityDiscardsLines) {
    BoundedLineBuffer buffer(0);
    buffer.Append("discarded");

    EXPECT_EQ(buffer.Size(), 0U);
    EXPECT_TRUE(buffer.Lines().empty());
}

/** @brief 验证同一变量的周期更新替换旧值且移除后消失。 */
TEST(LatestObservationModelTest, ReplacesLatestValueAndRemovesSymbol) {
    LatestObservationModel model(3);
    model.Update("ECU::Speed", "Speed = 10");
    model.Update("ECU::Temp", "Temp = 80");
    model.Update("ECU::Speed", "Speed = 12");

    ASSERT_EQ(model.Size(), 2U);
    EXPECT_EQ(model.Lines()[0], "Speed = 12");
    model.MarkAllStopped();
    EXPECT_EQ(model.Lines()[0], "Speed = 12 | 已停止");
    model.Update("ECU::Speed", "Speed = 13");
    EXPECT_EQ(model.Lines()[0], "Speed = 13");
    model.Remove("ECU::Speed");
    ASSERT_EQ(model.Size(), 1U);
    EXPECT_EQ(model.Lines()[0], "Temp = 80 | 已停止");
}

/** @brief 验证观测行上限淘汰最早加入的唯一变量。 */
TEST(LatestObservationModelTest, EvictsOldestSymbolAtCapacity) {
    LatestObservationModel model(2);
    model.Update("A", "A=1");
    model.Update("B", "B=1");
    model.Update("C", "C=1");

    ASSERT_EQ(model.Size(), 2U);
    EXPECT_EQ(model.Lines()[0], "B=1");
    EXPECT_EQ(model.Lines()[1], "C=1");
}

/** @brief 验证历史浏览后恢复进入历史前的输入草稿。 */
TEST(CommandHistoryTest, RestoresDraftAfterBrowsing) {
    CommandHistory history(3);
    history.Add("help");
    history.Add("list all");

    const auto previous = history.Previous("unfinished");
    ASSERT_TRUE(previous.has_value());
    EXPECT_EQ(*previous, "list all");

    const auto older = history.Previous(*previous);
    ASSERT_TRUE(older.has_value());
    EXPECT_EQ(*older, "help");

    const auto newer = history.Next();
    ASSERT_TRUE(newer.has_value());
    EXPECT_EQ(*newer, "list all");

    const auto restored = history.Next();
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(*restored, "unfinished");
    EXPECT_FALSE(history.Next().has_value());
}

/** @brief 验证历史上限淘汰最旧命令且忽略空命令。 */
TEST(CommandHistoryTest, EnforcesCapacityAndIgnoresEmptyCommands) {
    CommandHistory history(2);
    history.Add("first");
    history.Add("");
    history.Add("second");
    history.Add("third");

    const auto latest = history.Previous("");
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(*latest, "third");
    const auto retained = history.Previous(*latest);
    ASSERT_TRUE(retained.has_value());
    EXPECT_EQ(*retained, "second");
    const auto oldest_retained = history.Previous(*retained);
    ASSERT_TRUE(oldest_retained.has_value());
    EXPECT_EQ(*oldest_retained, "second");
}

/** @brief 验证零容量命令历史完全禁用。 */
TEST(CommandHistoryTest, ZeroCapacityDisablesHistory) {
    CommandHistory history(0);
    history.Add("help");

    EXPECT_FALSE(history.Previous("").has_value());
    EXPECT_FALSE(history.Next().has_value());
}

/** @brief 验证确认状态只接受明确选择，退出时未确认默认取消。 */
TEST(ConfirmationGateTest, AcceptsExplicitChoiceAndDefaultsToCancelOnExit) {
    std::atomic<bool> stopping{false};
    ConfirmationGate gate;
    gate.Begin();
    EXPECT_TRUE(gate.Pending());
    gate.Resolve(true);
    EXPECT_TRUE(gate.Pending());
    EXPECT_TRUE(gate.Wait(stopping));
    EXPECT_FALSE(gate.Pending());

    gate.Begin();
    stopping.store(true);
    EXPECT_FALSE(gate.Wait(stopping));
    EXPECT_FALSE(gate.Pending());
}

/** @brief 验证明确取消不会被解释为确认。 */
TEST(ConfirmationGateTest, ExplicitCancelReturnsFalse) {
    std::atomic<bool> stopping{false};
    ConfirmationGate gate;
    gate.Begin();
    gate.Cancel();

    EXPECT_FALSE(gate.Wait(stopping));
    EXPECT_FALSE(gate.Pending());
}

/** @brief 验证确认线程可从另一线程接收结果并被唤醒。 */
TEST(ConfirmationGateTest, WakesWaitingThreadWithResolvedChoice) {
    std::atomic<bool> stopping{false};
    ConfirmationGate gate;
    gate.Begin();
    bool accepted = false;
    std::thread waiter([&] { accepted = gate.Wait(stopping); });
    gate.Resolve(true);
    waiter.join();

    EXPECT_TRUE(accepted);
    EXPECT_FALSE(gate.Pending());
}

/** @brief 验证确认键区分接受、取消和默认忽略。 */
TEST(ConfirmationDecisionTest, AcceptsOnlyExplicitYesOrNoKeys) {
    ASSERT_TRUE(ConfirmationDecision("y").has_value());
    EXPECT_TRUE(*ConfirmationDecision("y"));
    ASSERT_TRUE(ConfirmationDecision("Y").has_value());
    EXPECT_TRUE(*ConfirmationDecision("Y"));

    ASSERT_TRUE(ConfirmationDecision("n").has_value());
    EXPECT_FALSE(*ConfirmationDecision("n"));
    ASSERT_TRUE(ConfirmationDecision("N").has_value());
    EXPECT_FALSE(*ConfirmationDecision("N"));

    EXPECT_FALSE(ConfirmationDecision("x").has_value());
    EXPECT_FALSE(ConfirmationDecision("yes").has_value());
    EXPECT_FALSE(ConfirmationDecision("").has_value());
}

/** @brief 验证 F2 视图索引循环切换及空菜单安全处理。 */
TEST(ViewSelectionTest, CyclesAndHandlesEmptyViewList) {
    EXPECT_EQ(NextViewIndex(0, 4), 1);
    EXPECT_EQ(NextViewIndex(2, 4), 3);
    EXPECT_EQ(NextViewIndex(3, 4), 0);
    EXPECT_EQ(NextViewIndex(-1, 4), 0);
    EXPECT_EQ(NextViewIndex(2, 0), 0);
}

}  // namespace
