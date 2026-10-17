#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>

#include "gtest/gtest.h"
#include "tools/xcp_master/tui_model.hpp"

namespace {

using calmcar::xcp::tools::BoundedLineBuffer;
using calmcar::xcp::tools::CalculateTuiLayout;
using calmcar::xcp::tools::CalibrationFormModel;
using calmcar::xcp::tools::CalibrationSelection;
using calmcar::xcp::tools::CommandHistory;
using calmcar::xcp::tools::ConfirmationDecision;
using calmcar::xcp::tools::ConfirmationGate;
using calmcar::xcp::tools::LatestObservationModel;
using calmcar::xcp::tools::NextViewIndex;
using calmcar::xcp::tools::PanelViewport;
using calmcar::xcp::tools::SearchInputRevision;
using calmcar::xcp::tools::TuiLayoutMetrics;
using calmcar::xcp::tools::VariableModalPage;
using calmcar::xcp::tools::VariableModalState;

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
    EXPECT_EQ(model.Lines()[0], "ECU::Speed = Speed = 12 | 已停止");
    model.Update("ECU::Speed", "Speed = 13");
    EXPECT_EQ(model.Lines()[0], "Speed = 13");
    model.Remove("ECU::Speed");
    ASSERT_EQ(model.Size(), 1U);
    EXPECT_EQ(model.Lines()[0], "ECU::Temp = Temp = 80 | 已停止");
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

/** @brief 队列拒绝命令时保留草稿，接受后才记入历史并清空。 */
TEST(CommandHistoryTest, PreservesDraftUntilCommandIsAccepted) {
    CommandHistory history(3);
    std::string draft = "write ECU::gain 1.25";

    EXPECT_FALSE(history.Commit(draft, false));
    EXPECT_EQ(draft, "write ECU::gain 1.25");
    EXPECT_FALSE(history.Previous("").has_value());

    EXPECT_TRUE(history.Commit(draft, true));
    EXPECT_TRUE(draft.empty());
    const auto committed = history.Previous("");
    ASSERT_TRUE(committed.has_value());
    EXPECT_EQ(*committed, "write ECU::gain 1.25");
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
    const auto first = gate.Begin();
    EXPECT_TRUE(gate.Pending());
    gate.Resolve(first, true);
    EXPECT_TRUE(gate.Pending());
    EXPECT_TRUE(gate.Wait(first, stopping));
    EXPECT_TRUE(gate.Authorized(first, stopping));
    EXPECT_FALSE(gate.Pending());

    const auto second = gate.Begin();
    stopping.store(true);
    EXPECT_FALSE(gate.Wait(second, stopping));
    EXPECT_FALSE(gate.Pending());
}

/** @brief 验证明确取消不会被解释为确认。 */
TEST(ConfirmationGateTest, ExplicitCancelReturnsFalse) {
    std::atomic<bool> stopping{false};
    ConfirmationGate gate;
    const auto ticket = gate.Begin();
    gate.Cancel();

    EXPECT_FALSE(gate.Wait(ticket, stopping));
    EXPECT_FALSE(gate.Pending());
}

/** @brief 验证确认线程可从另一线程接收结果并被唤醒。 */
TEST(ConfirmationGateTest, WakesWaitingThreadWithResolvedChoice) {
    std::atomic<bool> stopping{false};
    ConfirmationGate gate;
    const auto ticket = gate.Begin();
    bool accepted = false;
    std::thread waiter([&] { accepted = gate.Wait(ticket, stopping); });
    gate.Resolve(ticket, true);
    waiter.join();

    EXPECT_TRUE(accepted);
    EXPECT_FALSE(gate.Pending());
}

/** @brief 替换周期必须唤醒旧 ticket 等待者，即使新周期未回答。 */
TEST(ConfirmationGateTest, SupersedingTicketWakesOldWaiter) {
    std::atomic<bool> stopping{false};
    ConfirmationGate gate;
    const auto old = gate.Begin();
    std::promise<void> entered;
    std::promise<bool> completed;
    auto started = entered.get_future();
    auto result = completed.get_future();
    std::thread waiter([&] {
        entered.set_value();
        completed.set_value(gate.Wait(old, stopping));
    });
    started.wait();
    const auto next = gate.Begin();
    const auto status = result.wait_for(std::chrono::milliseconds(250));
    if (status != std::future_status::ready) gate.Cancel();
    waiter.join();
    EXPECT_EQ(status, std::future_status::ready);
    EXPECT_FALSE(result.get());
    EXPECT_NE(old, next);
}

/** @brief Dispatch先消费许可后，取消快速返回且不撤回在途发送。 */
TEST(ConfirmationGateTest, DispatchClaimsBeforeSendWithoutHoldingGateLock) {
    std::atomic<bool> stopping{false};
    ConfirmationGate gate;
    const auto ticket = gate.Begin();
    gate.Resolve(ticket, true);
    ASSERT_TRUE(gate.Wait(ticket, stopping));
    std::promise<void> sending;
    std::promise<void> finish_send;
    auto started = sending.get_future();
    auto release = finish_send.get_future().share();
    std::atomic<int> writes{0};
    std::thread writer([&] {
        static_cast<void>(SendIfConfirmed(gate, ticket, stopping, [&] {
            sending.set_value();
            release.wait();
            ++writes;
        }));
    });
    started.wait();
    std::promise<void> stop_started;
    std::promise<void> cancelled;
    std::atomic<bool> cancellation_won{true};
    auto stop_ready = stop_started.get_future();
    auto cancellation = cancelled.get_future();
    std::thread stopper([&] {
        stopping.store(true);
        stop_started.set_value();
        cancellation_won.store(gate.CancelBeforeDispatch());
        cancelled.set_value();
    });
    stop_ready.wait();
    EXPECT_EQ(cancellation.wait_for(std::chrono::milliseconds(250)),
              std::future_status::ready);
    EXPECT_FALSE(cancellation_won.load());
    finish_send.set_value();
    writer.join();
    stopper.join();
    EXPECT_EQ(writes.load(), 1);  // Dispatch已消费许可，stop不撤回在途请求。
    EXPECT_FALSE(SendIfConfirmed(gate, ticket, stopping, [&] { ++writes; }));
    EXPECT_EQ(writes.load(), 1);
}

/** @brief 已收到 Y 但 stop 抢先到达时，等待结果与下发代理均默认取消。 */
TEST(ConfirmationGateTest, StopBeforeWaitOverridesYes) {
    std::atomic<bool> stopping{false};
    ConfirmationGate gate;
    const auto ticket = gate.Begin();
    gate.Resolve(ticket, true);
    stopping.store(true);
    EXPECT_FALSE(gate.Wait(ticket, stopping));
    int write_calls = 0;
    EXPECT_FALSE(
        SendIfConfirmed(gate, ticket, stopping, [&] { ++write_calls; }));
    EXPECT_EQ(write_calls, 0);
}

/** @brief 确认通过后收到 stop/quit，写命令及 FREE_DAQ 均不得下发。 */
TEST(ConfirmationGateTest, StopAfterYesRevokesBothDangerousEffects) {
    std::atomic<bool> stopping{false};
    ConfirmationGate gate;
    int writes = 0;
    int free_daq = 0;
    const auto write = gate.Begin();
    gate.Resolve(write, true);
    EXPECT_TRUE(gate.Wait(write, stopping));
    gate.Cancel();  // stop/quit 在命令实际发送之前撤销许可。
    EXPECT_FALSE(SendIfConfirmed(gate, write, stopping, [&] { ++writes; }));

    const auto daq = gate.Begin();
    gate.Resolve(daq, true);
    EXPECT_TRUE(gate.Wait(daq, stopping));
    stopping.store(true);
    EXPECT_FALSE(SendIfConfirmed(gate, daq, stopping, [&] { ++free_daq; }));
    EXPECT_EQ(writes, 0);
    EXPECT_EQ(free_daq, 0);
}

/** @brief Wait收到Y后、Dispatch前的Esc/N仍可撤销操作。 */
TEST(ConfirmationGateTest, ResolveCancelAfterAcceptanceRevokesBeforeDispatch) {
    std::atomic<bool> stopping{false};
    ConfirmationGate gate;
    const auto ticket = gate.Begin();
    gate.Resolve(ticket, true);
    EXPECT_TRUE(gate.Wait(ticket, stopping));

    gate.Resolve(ticket, false);
    gate.Resolve(ticket, true);
    int send_calls = 0;
    EXPECT_FALSE(gate.Dispatch(ticket, stopping, [&] { ++send_calls; }));

    const auto resized = gate.Begin();
    gate.Resolve(resized, true);
    EXPECT_TRUE(gate.Wait(resized, stopping));
    EXPECT_TRUE(gate.CancelBeforeDispatch());
    EXPECT_FALSE(gate.Dispatch(resized, stopping, [&] { ++send_calls; }));
    EXPECT_EQ(send_calls, 0);
}

/** @brief 旧确认、取消后的 Y 和未回答场景均不得触发危险效果。 */
TEST(ConfirmationGateTest, RejectsStaleDuplicateAndCancelledTickets) {
    std::atomic<bool> stopping{false};
    ConfirmationGate gate;
    int writes = 0;
    int free_daq = 0;
    const auto stale = gate.Begin();
    const auto current = gate.Begin();
    gate.Resolve(stale, true);
    EXPECT_FALSE(gate.Authorized(stale, stopping));
    gate.Resolve(current, false);
    gate.Resolve(current, true);
    EXPECT_FALSE(gate.Wait(current, stopping));
    EXPECT_FALSE(SendIfConfirmed(gate, current, stopping, [&] { ++writes; }));

    const auto refused = gate.Begin();
    gate.Cancel();
    gate.Resolve(refused, true);
    EXPECT_FALSE(gate.Wait(refused, stopping));
    EXPECT_FALSE(SendIfConfirmed(gate, refused, stopping, [&] { ++free_daq; }));
    EXPECT_EQ(writes, 0);
    EXPECT_EQ(free_daq, 0);
}

/** @brief 只有本周期明确通过时写入代理和 DAQ 配置代理才会执行。 */
TEST(ConfirmationGateTest, OnlyAcceptedTicketDispatches) {
    std::atomic<bool> stopping{false};
    ConfirmationGate gate;
    int writes = 0;
    int free_daq = 0;
    const auto ticket = gate.Begin();
    EXPECT_FALSE(SendIfConfirmed(gate, ticket, stopping, [&] { ++writes; }));
    gate.Resolve(ticket, true);
    EXPECT_TRUE(gate.Wait(ticket, stopping));
    EXPECT_TRUE(SendIfConfirmed(gate, ticket, stopping, [&] { ++writes; }));
    EXPECT_FALSE(SendIfConfirmed(gate, ticket, stopping, [&] { ++writes; }));
    EXPECT_EQ(writes, 1);
    EXPECT_EQ(free_daq, 0);
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

/** @brief 完整目录 1000 行必须逐页可达，而诊断行仍维持 512 上限。 */
TEST(PanelViewportTest, FullListingRemainsBrowsablePastBoundedLogLimit) {
    PanelViewport viewport(3);
    BoundedLineBuffer diagnostics(512);
    std::vector<std::string> listing;
    for (int i = 0; i < 1000; ++i) {
        listing.push_back("item-" + std::to_string(i));
        diagnostics.Append(listing.back());
    }
    ASSERT_EQ(listing.size(), 1000U);
    EXPECT_EQ(diagnostics.Size(), 512U);
    viewport.ResetToTop(1);
    std::vector<bool> seen(listing.size(), false);
    for (;;) {
        const auto start = viewport.Start(1, listing.size());
        for (auto i = start;
             i < std::min(listing.size(), start + viewport.PageSize()); ++i)
            seen[i] = true;
        viewport.PageDown(1, listing.size());
        if (viewport.Start(1, listing.size()) == start) break;
    }
    EXPECT_TRUE(std::all_of(seen.begin(), seen.end(),
                            [](bool present) { return present; }));
    EXPECT_EQ(viewport.Start(1, listing.size()), 982U);
    viewport.PageUp(1, listing.size());
    EXPECT_EQ(viewport.Start(1, listing.size()), 964U);
    EXPECT_EQ(viewport.Start(2, 1000), 982U);  // 其他视图仍跟随尾部。
}

/** @brief 终端缩放改变页高时，末尾跟随和历史窗口仍保持一致。 */
TEST(PanelViewportTest, UpdatesPageSizeAfterTerminalResize) {
    PanelViewport viewport(3, 18);
    EXPECT_EQ(viewport.Start(1, 1000), 982U);
    viewport.SetPageSize(30);
    EXPECT_EQ(viewport.PageSize(), 30U);
    EXPECT_EQ(viewport.Start(1, 1000), 970U);
    viewport.ResetToTop(1);
    viewport.PageDown(1, 1000);
    EXPECT_EQ(viewport.Start(1, 1000), 30U);
    viewport.SetPageSize(13);
    EXPECT_EQ(viewport.Start(1, 1000), 13U);
}

/** @brief 浏览历史时新增行不抢焦点，重置与缩短边界均安全。 */
TEST(PanelViewportTest, PreservesHistoryAndClampsShortenedViews) {
    PanelViewport viewport(3);
    EXPECT_EQ(viewport.Start(1, 0), 0U);
    viewport.PageUp(1, 60);
    EXPECT_EQ(viewport.Start(1, 60), 24U);
    EXPECT_EQ(viewport.Start(1, 61), 24U);
    viewport.EvictFront(1);
    EXPECT_EQ(viewport.Start(1, 60), 23U);
    viewport.PageDown(1, 60);
    EXPECT_EQ(viewport.Start(1, 60), 41U);
    viewport.PageDown(1, 60);
    EXPECT_EQ(viewport.Start(1, 60), 42U);
    EXPECT_EQ(viewport.Start(1, 80), 62U);  // 到底后继续跟随。
    viewport.ResetToTop(1);
    EXPECT_EQ(viewport.Start(1, 80), 0U);
    EXPECT_EQ(viewport.Start(1, 3), 0U);
    viewport.FollowBottom(1);
    EXPECT_EQ(viewport.Start(1, 80), 62U);
}

/** @brief 标定选择变化隔离旧读取/写入结果，A→B→A 也不得复用旧快照。 */
TEST(CalibrationSelectionTest, IsolatesDraftAndAsyncResultEpoch) {
    CalibrationSelection selection;
    EXPECT_TRUE(selection.Select("module::A"));
    const auto old_a = selection.Current();
    EXPECT_FALSE(selection.Select("module::A"));
    EXPECT_TRUE(selection.Matches(old_a));
    EXPECT_TRUE(selection.Select("module::B"));
    const auto b = selection.Current();
    EXPECT_FALSE(selection.Matches(old_a));
    EXPECT_TRUE(selection.Matches(b));
    EXPECT_TRUE(selection.Select("module::A"));
    EXPECT_FALSE(selection.Matches(old_a));
    EXPECT_TRUE(selection.Select({}));
    EXPECT_FALSE(selection.Matches(selection.Current()));
}

/** @brief 切换与重新搜索清除旧草稿和当前值，旧异步回读不可回填。 */
TEST(CalibrationFormModelTest, ClearsDraftAndIgnoresStaleReadback) {
    CalibrationFormModel form;
    EXPECT_TRUE(form.Select("module::A"));
    form.target = "12.5";
    const auto old = form.Snapshot();
    EXPECT_TRUE(form.Apply(old, "读取成功", "12.0"));
    EXPECT_TRUE(form.Select("module::B"));
    EXPECT_TRUE(form.target.empty());
    EXPECT_EQ(form.current, "尚未读取");
    EXPECT_EQ(form.status, "请选择变量并读取当前值");
    EXPECT_FALSE(form.Apply(old, "旧 A 回读", "旧 A 值"));
    EXPECT_EQ(form.current, "尚未读取");
    form.target = "13";
    EXPECT_FALSE(form.Select("module::B"));
    EXPECT_EQ(form.target, "13");
    EXPECT_TRUE(form.Select("module::B", true));
    EXPECT_TRUE(form.target.empty());
    EXPECT_FALSE(form.Apply(old, "旧状态"));
    EXPECT_TRUE(form.Select({}));
    EXPECT_EQ(form.status, "没有匹配项");
}

/** @brief 旧异步搜索完成后不得覆盖已编辑的新查询草稿。 */
TEST(SearchInputRevisionTest, RejectsResultAfterSearchInputWasEdited) {
    SearchInputRevision revision;
    const auto submitted = revision.Capture();
    EXPECT_TRUE(revision.Matches(submitted));

    revision.MarkEdited();
    EXPECT_FALSE(revision.Matches(submitted));
    const auto latest = revision.Capture();
    EXPECT_TRUE(revision.Matches(latest));
}

/** @brief 验证 F2 在合并后的三个视图间循环。 */
TEST(ViewSelectionTest, CyclesAndHandlesEmptyViewList) {
    EXPECT_EQ(NextViewIndex(0, 3), 1);
    EXPECT_EQ(NextViewIndex(1, 3), 2);
    EXPECT_EQ(NextViewIndex(2, 3), 0);
    EXPECT_EQ(NextViewIndex(-1, 3), 0);
    EXPECT_EQ(NextViewIndex(2, 0), 0);
}

/** @brief 验证三视图面板页高随终端尺寸变化且窄屏约束有效。 */
TEST(TuiLayoutTest, ComputesResponsiveRowsAndModalBounds) {
    const auto wide = CalculateTuiLayout(120, 40);
    EXPECT_FALSE(wide.too_small);
    EXPECT_FALSE(wide.compact);
    EXPECT_EQ(wide.panel_rows, 30U);
    EXPECT_EQ(wide.preview_rows, 29U);
    EXPECT_EQ(wide.modal_width, 88);
    EXPECT_EQ(wide.modal_height, 20);

    const auto minimum = CalculateTuiLayout(80, 24);
    EXPECT_FALSE(minimum.too_small);
    EXPECT_TRUE(minimum.compact);
    EXPECT_EQ(minimum.panel_rows, 14U);
    EXPECT_EQ(minimum.preview_rows, 13U);
    EXPECT_EQ(minimum.modal_width, 72);
    EXPECT_EQ(minimum.modal_height, 18);
}

/** @brief 过小终端仍得到正尺寸，但禁用编辑和危险确认。 */
TEST(TuiLayoutTest, RejectsInteractiveOperationsBelowMinimumSize) {
    for (const auto [width, height] :
         {std::pair{79, 24}, std::pair{80, 23}, std::pair{-1, -1}}) {
        const auto layout = CalculateTuiLayout(width, height);
        EXPECT_TRUE(layout.too_small);
        EXPECT_GE(layout.panel_rows, 1U);
        EXPECT_GE(layout.preview_rows, 1U);
        EXPECT_GE(layout.modal_width, 1);
        EXPECT_GE(layout.modal_height, 1);
    }
}

/** @brief Enter 打开详情弹窗，Esc/确认取消返回同一详情并保留表单变量。 */
TEST(VariableModalStateTest, OpensDetailsAndReturnsAfterConfirmation) {
    const auto layout = CalculateTuiLayout(100, 30);
    VariableModalState modal;
    ASSERT_TRUE(modal.Open("ECU::gain", layout));
    EXPECT_TRUE(modal.IsOpen());
    EXPECT_TRUE(modal.BlocksBackgroundListUpdates());
    EXPECT_EQ(modal.Page(), VariableModalPage::Details);
    EXPECT_EQ(modal.SelectedName(), "ECU::gain");
    EXPECT_FALSE(modal.BeginConfirmation(false, layout));
    ASSERT_TRUE(modal.BeginConfirmation(true, layout));
    EXPECT_EQ(modal.Page(), VariableModalPage::Confirmation);
    modal.CancelConfirmation();
    EXPECT_EQ(modal.Page(), VariableModalPage::Details);
    ASSERT_TRUE(modal.BeginConfirmation(true, layout));
    modal.CompleteConfirmation();
    EXPECT_EQ(modal.Page(), VariableModalPage::Details);
    EXPECT_EQ(modal.SelectedName(), "ECU::gain");
    modal.Close();
    EXPECT_FALSE(modal.IsOpen());
    EXPECT_FALSE(modal.BlocksBackgroundListUpdates());
    EXPECT_TRUE(modal.SelectedName().empty());
}

/** @brief 低于最低终端尺寸时不能打开详情或危险确认。 */
TEST(VariableModalStateTest, RejectsDetailsAndConfirmationOnSmallTerminal) {
    const auto normal_layout = CalculateTuiLayout(100, 30);
    const auto small_layout = CalculateTuiLayout(79, 24);
    VariableModalState modal;
    EXPECT_FALSE(modal.Open("ECU::gain", small_layout));
    EXPECT_FALSE(modal.IsOpen());
    EXPECT_FALSE(modal.BlocksBackgroundListUpdates());
    EXPECT_FALSE(modal.OpenCommandConfirmation(small_layout));
    ASSERT_TRUE(modal.Open("ECU::gain", normal_layout));
    EXPECT_TRUE(modal.BlocksBackgroundListUpdates());
    EXPECT_FALSE(modal.BeginConfirmation(true, small_layout));
    EXPECT_EQ(modal.Page(), VariableModalPage::Details);
}

/** @brief 通用命令确认也仅在合适尺寸下打开，并可独立关闭。 */
TEST(VariableModalStateTest, OpensCommandConfirmationWithoutVariableDetails) {
    VariableModalState modal;
    const auto layout = CalculateTuiLayout(100, 30);
    ASSERT_TRUE(modal.OpenCommandConfirmation(layout));
    EXPECT_TRUE(modal.IsOpen());
    EXPECT_TRUE(modal.BlocksBackgroundListUpdates());
    EXPECT_EQ(modal.Page(), VariableModalPage::Confirmation);
    EXPECT_TRUE(modal.SelectedName().empty());
    modal.Close();
    EXPECT_FALSE(modal.IsOpen());
    EXPECT_FALSE(modal.BlocksBackgroundListUpdates());
}

/** @brief 读取失败保留最后值并显式标记为旧值。 */
TEST(LatestObservationModelTest, FailureKeepsPreviousValueAsStale) {
    LatestObservationModel model(2);
    model.UpdateValue("ECU::gain", "1.25 V", "手动读取", "12:00:00");
    model.MarkFailure("ECU::gain", "读取超时", "12:00:01");

    const auto sample = model.Find("ECU::gain");
    ASSERT_TRUE(sample.has_value());
    EXPECT_EQ(sample->value, "1.25 V");
    EXPECT_EQ(sample->status, "读取超时");
    EXPECT_TRUE(sample->stale);
    EXPECT_TRUE(sample->has_value);
    EXPECT_NE(model.Lines()[0].find("1.25 V"), std::string::npos);
    EXPECT_NE(model.Lines()[0].find("旧值"), std::string::npos);
}

}  // namespace
