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
#include <string>
#include <string_view>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include "libxcp/a2l/a2l_bridge.hpp"

namespace {

/** @brief 与现场 A2L（87 MB / 246 783 行）一致的符号规模，作为渲染基准的输入。 */
constexpr std::size_t kFieldCandidateCount = 198556U;

/** @brief 单帧渲染参考值（毫秒）：本机实测 198 556 行时约 170 ms。 */
constexpr double kFrameRenderReferenceMs = 170.0;

/** @brief 单帧渲染绝对上限（毫秒）：超过即视为量级劣化。 */
constexpr double kFrameRenderHardLimitMs = 1500.0;

/** @brief 单次全库检索参考值（毫秒）：本机实测 198 556 个符号约 90 ms。 */
constexpr double kSearchReferenceMs = 90.0;

/** @brief 单次全库检索绝对上限（毫秒）。 */
constexpr double kSearchHardLimitMs = 1500.0;

/** @brief 候选过滤（拷贝 SymbolInfo）参考值（毫秒）：本机实测约 60 ms。 */
constexpr double kFilterReferenceMs = 60.0;

/** @brief 候选过滤绝对上限（毫秒）。 */
constexpr double kFilterHardLimitMs = 1200.0;

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

/** @brief 单帧渲染在 198 556 个候选下不得突破量级预算。 */
TEST(XcpMasterRenderBenchTest, RendersFrameWithFieldSizeCandidateList) {
    const double frame_ms = MeasureFrameMs(kFieldCandidateCount);
    const double reference_limit = kFrameRenderReferenceMs * 3.0;
    std::printf("[PERF] 渲染 198556 行单帧 = %.1f ms（预算 参考=%.0f ms 硬上限=%.0f ms）\n",
                frame_ms, reference_limit, kFrameRenderHardLimitMs);
    EXPECT_LT(frame_ms, reference_limit)
        << "单帧渲染耗时较参考值劣化超过 3 倍：候选 " << kFieldCandidateCount
        << " 行";
    EXPECT_LT(frame_ms, kFrameRenderHardLimitMs) << "单帧渲染耗时突破绝对上限";
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