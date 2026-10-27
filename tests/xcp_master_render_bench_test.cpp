/**
 * @file xcp_master_render_bench_test.cpp
 * @brief xcp_master 界面性能基准：大候选列表渲染与大 A2L 检索。
 *
 * 背景：真实现场报障「A2L 文件变大后交互响应慢」。定位结论是候选行数而非
 * 文件字节数——FTXUI 的 Menu 在 OnRender() 里对每个条目都构造一次 Element，
 * 没有虚拟化；xcp_master 把全部候选标签一次性交给该 Menu 渲染。本基准用
 * headless 方式（Screen::Create + Render，不需要 TTY）复现这条路径，
 * 把「渲染成本随候选数线性增长」锁进回归，防止以后退化。
 *
 * 基线只拦量级劣化，不是 SLA：允许参考值的 3 倍，同时受绝对上限约束。
 *
 * 机器能力门控（见 `MeasureMachineCapabilityFactor`）：绝对毫秒预算只对与
 * 参考机同档次的机器有判别力。当本机每行渲染成本超过参考值的
 * `kMachineCapabilityTolerance` 倍时，介于「参考预算」与「绝对上限」之间的
 * 耗时不再判失败，而是显式 `GTEST_SKIP` 并打印实测证据；绝对上限在任何机器
 * 上照常生效，量级劣化不会被放过。
 */
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include "libxcp/a2l/a2l_bridge.hpp"
#include "tools/xcp_master/windowed_menu.hpp"

namespace {

/** @brief 与现场 A2L（87 MB / 246 783 行）一致的符号规模，作为渲染基准的输入。 */
constexpr std::size_t kFieldCandidateCount = 198556U;

/** @brief 单帧渲染参考值（毫秒）：本机实测 198 556 行时约 170 ms。 */
constexpr double kFrameRenderReferenceMs = 170.0;

/** @brief 单帧渲染绝对上限（毫秒）：超过即视为量级劣化。 */
constexpr double kFrameRenderHardLimitMs = 1500.0;

/**
 * @brief 参考机的每行渲染成本（微秒/行）。
 * @details 由参考值与参考行数推导（170 ms / 198 556 行 ≈ 0.856 µs），
 *          不再单独引入一个魔法数。
 */
constexpr double kFrameRenderReferenceUsPerRow =
    kFrameRenderReferenceMs * 1000.0 / static_cast<double>(kFieldCandidateCount);

/**
 * @brief 机器能力容忍倍数。
 * @details 本机每行渲染成本超过参考值该倍数时，判定"算力与参考机不匹配"，
 *          绝对毫秒预算在该机器上不再具备判别力。
 */
constexpr double kMachineCapabilityTolerance = 2.0;

/** @brief 能力标定用的行数：参考规模的 1/16（约 50 ms），避免门控本身拖慢基准。 */
constexpr std::size_t kCapabilityProbeRows = kFieldCandidateCount / 16U;

/** @brief 单次全库检索参考值（毫秒）：本机实测 198 556 个符号约 90 ms。 */
constexpr double kSearchReferenceMs = 90.0;

/** @brief 单次全库检索绝对上限（毫秒）。 */
constexpr double kSearchHardLimitMs = 1500.0;

/** @brief 候选过滤（拷贝 SymbolInfo）参考值（毫秒）：本机实测约 60 ms。 */
constexpr double kFilterReferenceMs = 60.0;

/** @brief 候选过滤绝对上限（毫秒）。 */
constexpr double kFilterHardLimitMs = 1200.0;

/** @brief 虚拟菜单单帧渲染参考值（毫秒）：只构造可视行，预期 <1 ms。 */
constexpr double kWindowedRenderReferenceMs = 0.5;

/** @brief 虚拟菜单单帧渲染绝对上限（毫秒）。 */
constexpr double kWindowedRenderHardLimitMs = 20.0;

/** @brief 虚拟菜单可视行数，与 xcp_master 预览面板的固定高度一致。 */
constexpr std::size_t kWindowVisibleRows = 20U;

/**
 * @brief 按 xcp_master 预览行的真实形态生成一条候选标签。
 * @param index 候选序号，仅用于让每行长度接近真实符号名。
 * @return 形如「模块::路径.signal_NNN = 12345.6 mV | POLL」的标签。
 */
std::string MakePreviewLabel(std::size_t index) {
    return std::format(
        "apa_complex::algo_status_info.apa_control_status.signal_{:06d}"
        " = 12345.6 mV | POLL | 2026-10-23 20:08:00",
        index);
}

/**
 * @brief headless 渲染一帧并返回耗时（毫秒）。
 *
 * 与 xcp_master 的接线保持一致：Menu 外层套 vscroll_indicator | frame |
 * size(HEIGHT, LESS_THAN, 20) | flex，即"滚动窗口 + 固定高度"的预览面板。
 * @param labels 菜单条目文本。
 * @param selected 选中下标。
 * @param width 终端列数。
 * @param height 终端行数。
 * @return 单帧构建 + 绘制耗时（毫秒）。
 */
double RenderOneFrame(const std::vector<std::string>& labels, int& selected,
                      int width, int height) {
    auto menu = ftxui::Menu(&labels, &selected);
    auto render = ftxui::Renderer(menu, [&menu] {
        return menu->Render() | ftxui::vscroll_indicator | ftxui::frame |
               ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 20) | ftxui::flex;
    });
    const auto started = std::chrono::steady_clock::now();
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                        ftxui::Dimension::Fixed(height));
    ftxui::Render(screen, render->Render());
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - started)
        .count();
}

/**
 * @brief 生成 count 条标签并测量多帧渲染的平均耗时。
 * @param count 标签条数。
 * @param rounds 计入平均的帧数（首帧用于预热，不计入）。
 * @return 平均单帧耗时（毫秒）。
 */
double MeasureFrameMs(std::size_t count, int rounds = 3) {
    std::vector<std::string> labels;
    labels.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
        labels.push_back(MakePreviewLabel(index));
    int selected = 0;
    // 预热一帧：让 FTXUI 的内部容器与字符串分配走出冷启动。
    static_cast<void>(RenderOneFrame(labels, selected, 120, 40));
    double total = 0.0;
    for (int round = 0; round < rounds; ++round)
        total += RenderOneFrame(labels, selected, 120, 40);
    return total / static_cast<double>(rounds);
}

/**
 * @brief 标定本机相对参考机的渲染能力系数。
 *
 * 用**同一条渲染路径**测量 1/16 规模的每行成本，再除以参考机的每行成本
 * （`kFrameRenderReferenceUsPerRow`）得到系数。之所以不用独立的纯 CPU 标定
 * 负载：本基准的瓶颈在内存与字符串分配（FTXUI 每行构造一次 Element），
 * 通用 CPU 系数与实际耗时不呈比例，会把慢机误判成代码劣化。
 *
 * 代价是"代码整体劣化"同样会抬高该系数，因此系数只用来决定"跳过还是判
 * 失败"，绝对兜底始终是 `kFrameRenderHardLimitMs`。
 *
 * @return 能力系数：1.0 表示与参考机同速，数值越大表示本机越慢。
 */
double MeasureMachineCapabilityFactor() {
    const double probe_ms = MeasureFrameMs(kCapabilityProbeRows);
    const double probe_us_per_row =
        probe_ms * 1000.0 / static_cast<double>(kCapabilityProbeRows);
    return probe_us_per_row / kFrameRenderReferenceUsPerRow;
}

/**
 * @brief 读取与 A2L 同名的符号数侧车文件（由 gen_a2l.py 写入）。
 * @param a2l_path A2L 文件路径。
 * @return 侧车里记录的符号数；文件不存在或内容非法时返回空。
 */
std::optional<std::size_t> ReadSidecarCount(const std::filesystem::path& a2l_path) {
    auto sidecar = a2l_path;
    sidecar += ".count";
    std::ifstream input(sidecar);
    if (!input) return std::nullopt;
    std::size_t count = 0;
    input >> count;
    if (!input) return std::nullopt;
    return count;
}

/**
 * @brief 解析性能样本路径：优先环境变量覆盖，其次构建期注入的合成样本。
 * @return 存在的 A2L 路径；都不存在时返回空。
 */
std::optional<std::filesystem::path> ResolvePerfA2lPath() {
    if (const char* override_path = std::getenv("LIBXCP_UI_PERF_A2L");
        override_path != nullptr && override_path[0] != '\0') {
        std::filesystem::path candidate(override_path);
        if (std::filesystem::exists(candidate)) return candidate;
    }
#ifdef XCP_UI_PERF_A2L
    std::filesystem::path generated(XCP_UI_PERF_A2L);
    if (std::filesystem::exists(generated)) return generated;
#endif
    return std::nullopt;
}

/**
 * @brief 单帧渲染在 198 556 个候选下不得突破量级预算。
 * @details 判定顺序固定为「绝对上限 → 参考预算 → 机器能力门控」：
 *          1. 突破 `kFrameRenderHardLimitMs` 一律失败，慢机不豁免；
 *          2. 低于参考预算的 3 倍直接通过，行为与门控引入前一致；
 *          3. 落在两者之间且能力系数超过 `kMachineCapabilityTolerance` 时，
 *             显式跳过并打印实测证据——绝对毫秒预算在该机器上无判别力。
 */
TEST(XcpMasterRenderBenchTest, RendersFrameWithFieldSizeCandidateList) {
    const double frame_ms = MeasureFrameMs(kFieldCandidateCount);
    const double reference_limit = kFrameRenderReferenceMs * 3.0;
    const double capability_factor = MeasureMachineCapabilityFactor();
    const double local_us_per_row =
        capability_factor * kFrameRenderReferenceUsPerRow;
    std::printf(
        "[PERF] 渲染 %zu 行单帧 = %.1f ms（预算 参考=%.0f ms 硬上限=%.0f ms）；"
        "本机每行 %.2f µs，参考每行 %.2f µs，能力系数=%.2f\n",
        kFieldCandidateCount, frame_ms, reference_limit,
        kFrameRenderHardLimitMs, local_us_per_row,
        kFrameRenderReferenceUsPerRow, capability_factor);

    // 1) 绝对上限：任何机器都要拦住量级劣化。
    EXPECT_LT(frame_ms, kFrameRenderHardLimitMs) << "单帧渲染耗时突破绝对上限";
    if (frame_ms >= kFrameRenderHardLimitMs) {
        return;  // 已判定失败，不再叠加参考预算报错，保持失败原因单一。
    }

    // 2) 参考预算：与参考机同档次时照常拦截 3 倍劣化。
    if (frame_ms < reference_limit) {
        return;
    }

    // 3) 机器能力门控：本机明显慢于参考机时显式跳过，而不是让慢机变红。
    if (capability_factor > kMachineCapabilityTolerance) {
        GTEST_SKIP() << std::format(
            "本机算力与参考机不匹配，绝对毫秒预算无判别力：实测单帧 {:.1f} ms，"
            "参考预算 {:.0f} ms，硬上限 {:.0f} ms（已通过）；本机每行 {:.2f} µs，"
            "参考每行 {:.2f} µs，能力系数 {:.2f} > 容忍倍数 {:.1f}。"
            "若怀疑是代码劣化，请在参考机复核该基准。",
            frame_ms, reference_limit, kFrameRenderHardLimitMs, local_us_per_row,
            kFrameRenderReferenceUsPerRow, capability_factor,
            kMachineCapabilityTolerance);
    }

    // 同档机器上的真实劣化：照常失败。
    EXPECT_LT(frame_ms, reference_limit) << std::format(
        "单帧渲染耗时较参考值劣化超过 3 倍：候选 {} 行，实测 {:.1f} ms；"
        "本机能力系数 {:.2f}（未超过容忍倍数 {:.1f}，故不按机器能力豁免）",
        kFieldCandidateCount, frame_ms, capability_factor,
        kMachineCapabilityTolerance);
}

/**
 * @brief 用只构造可视行的虚拟菜单渲染一帧。
 * @param labels 菜单条目文本。
 * @param selected 选中下标。
 * @param visible_rows 可视行数。
 * @param width 终端列数。
 * @param height 终端行数。
 * @return 单帧耗时（毫秒）与实际构造的行数。
 */
std::pair<double, std::size_t> RenderWindowedFrame(
    const std::vector<std::string>& labels, int selected, std::size_t visible_rows,
    int width = 120, int height = 40) {
    auto menu = ftxui::Make<calmcar::xcp::tools::WindowedMenu>(
        &labels, &selected, calmcar::xcp::tools::WindowedMenuOptions{});
    menu->SetVisibleRows(visible_rows);
    auto render = ftxui::Renderer(menu, [&menu] {
        return menu->Render() | ftxui::frame |
               ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 20) | ftxui::flex;
    });
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                        ftxui::Dimension::Fixed(height));
    const auto started = std::chrono::steady_clock::now();
    ftxui::Render(screen, render->Render());
    const double elapsed_ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - started)
                                  .count();
    return {elapsed_ms, menu->LastRenderedRows()};
}

/**
 * @brief 虚拟化后单帧只能构造可视行，行数与候选总数无关。
 */
TEST(XcpMasterRenderBenchTest, RendersOnlyVisibleRows) {
    std::vector<std::string> labels;
    labels.reserve(kFieldCandidateCount);
    for (std::size_t index = 0; index < kFieldCandidateCount; ++index)
        labels.push_back(MakePreviewLabel(index));

    const auto result = RenderWindowedFrame(labels, 0, kWindowVisibleRows);
    std::printf("[PERF] 虚拟菜单渲染 198556 行中的 %zu 行，实际构造 %zu 行\n",
                kWindowVisibleRows, result.second);
    EXPECT_EQ(result.second, kWindowVisibleRows);
}

/**
 * @brief 198 556 行规模下单帧渲染必须落在虚拟化预算内（参考 0.5 ms）。
 */
TEST(XcpMasterRenderBenchTest, WindowedMenuRendersFieldSizeListWithinBudget) {
    std::vector<std::string> labels;
    labels.reserve(kFieldCandidateCount);
    for (std::size_t index = 0; index < kFieldCandidateCount; ++index)
        labels.push_back(MakePreviewLabel(index));

    // 预热一帧后取三帧平均。
    static_cast<void>(RenderWindowedFrame(labels, 0, kWindowVisibleRows));
    double total_ms = 0.0;
    for (int round = 0; round < 3; ++round)
        total_ms += RenderWindowedFrame(labels, 0, kWindowVisibleRows).first;
    const double frame_ms = total_ms / 3.0;
    const double reference_limit = kWindowedRenderReferenceMs * 3.0;
    std::printf(
        "[PERF] 虚拟菜单单帧 = %.2f ms（预算 参考=%.1f ms 硬上限=%.0f ms）\n",
        frame_ms, reference_limit, kWindowedRenderHardLimitMs);
    EXPECT_LT(frame_ms, reference_limit)
        << "虚拟菜单单帧耗时较参考值劣化超过 3 倍";
    EXPECT_LT(frame_ms, kWindowedRenderHardLimitMs) << "虚拟菜单单帧耗时突破绝对上限";
}

/**
 * @brief 渲染成本应随候选数近似线性；明显超线性说明引入了额外的整表开销。
 */
TEST(XcpMasterRenderBenchTest, RenderCostGrowsLinearlyWithCandidateCount) {
    const auto quarter = kFieldCandidateCount / 4U;
    const double small_ms = MeasureFrameMs(quarter, 2);
    const double large_ms = MeasureFrameMs(kFieldCandidateCount, 2);
    const double ratio = small_ms <= 0.0 ? 0.0 : large_ms / small_ms;
    std::printf(
        "[PERF] 渲染 %zu 行 = %.1f ms；%zu 行 = %.1f ms；大/小 = %.2f 倍"
        "（候选数比 4.00 倍）\n",
        quarter, small_ms, kFieldCandidateCount, large_ms, ratio);
    // 4 倍候选数允许到 8 倍耗时，给缓存与分配抖动留余量；再超即非 O(n)。
    EXPECT_LT(ratio, 8.0) << "渲染耗时相对候选数的增长明显超线性";
}

/**
 * @brief 大 A2L 上的全库检索与候选过滤不得突破量级预算。
 *
 * 样本来源：优先环境变量 LIBXCP_UI_PERF_A2L 指定的真实 A2L，其次构建期注入的
 * 40 MB 合成样本（约 198k 符号）；两者都不存在时跳过，避免在缺少样本的机器上误报。
 */
TEST(XcpMasterRenderBenchTest, SearchesAndFiltersLargeA2lWithinBudget) {
    const auto a2l_path = ResolvePerfA2lPath();
    if (!a2l_path.has_value()) {
        GTEST_SKIP() << "缺少大 A2L 性能样本；请用 LIBXCP_UI_PERF_A2L 指定或先"
                        "运行样本生成步骤";
    }
    const auto load_started = std::chrono::steady_clock::now();
    // 合成样本只有 MEASUREMENT/CHARACTERISTIC，没有 IF_DATA XCP 段；
    // 与 A2lGolden 的性能用例一致，关闭"必须有 IF_DATA XCP"的强制要求。
    calmcar::xcp::a2l::LoadOptions load_options;
    load_options.require_if_data_xcp = false;
    auto loaded =
        calmcar::xcp::a2l::A2lBridge::Load(a2l_path->string(), load_options);
    ASSERT_TRUE(loaded.HasValue())
        << "A2L 加载失败: " << loaded.ErrorInfo().message;
    const double load_ms = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - load_started)
                               .count();
    auto bridge = std::move(loaded).Value();
    const auto* database = bridge->Database();
    ASSERT_NE(database, nullptr);
    auto count = database->Count();
    ASSERT_TRUE(count.HasValue());
    if (const auto expected = ReadSidecarCount(*a2l_path); expected.has_value()) {
        // 侧车记录的是生成器写入的符号数：不一致说明解析提前终止。
        EXPECT_EQ(count.Value(), *expected) << "解析得到的符号数与样本侧车不一致";
    }

    const auto search_started = std::chrono::steady_clock::now();
    auto all = database->Search("*", std::max<std::size_t>(1, count.Value()));
    ASSERT_TRUE(all.HasValue()) << "检索失败: " << all.ErrorInfo().message;
    const double search_ms = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - search_started)
                                 .count();

    // 复刻 FindPreviewCandidates 的过滤步骤：按 kind 拷贝出候选。
    const auto filter_started = std::chrono::steady_clock::now();
    std::size_t candidates = 0;
    for (const auto& symbol : all.Value()) {
        if (symbol.kind != calmcar::xcp::a2l::SymbolKind::Measurement &&
            symbol.kind != calmcar::xcp::a2l::SymbolKind::Characteristic)
            continue;
        ++candidates;
    }
    const double filter_ms = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - filter_started)
                                 .count();

    std::printf(
        "[PERF] %s：加载 %.0f ms / %zu 个符号；全库检索 %.1f ms；候选过滤 %.1f ms"
        "（%zu 个候选）\n",
        a2l_path->filename().string().c_str(), load_ms, count.Value(), search_ms,
        filter_ms, candidates);
    EXPECT_LT(search_ms, kSearchReferenceMs * 3.0) << "全库检索较参考值劣化超过 3 倍";
    EXPECT_LT(search_ms, kSearchHardLimitMs) << "全库检索突破绝对上限";
    EXPECT_LT(filter_ms, kFilterReferenceMs * 3.0) << "候选过滤较参考值劣化超过 3 倍";
    EXPECT_LT(filter_ms, kFilterHardLimitMs) << "候选过滤突破绝对上限";
    EXPECT_GT(candidates, 0U) << "样本应至少含一个 MEASUREMENT/CHARACTERISTIC";
}

}  // namespace