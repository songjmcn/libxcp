#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "libxcp/a2l/a2l_types.hpp"
#include "symbol_display.hpp"
#include "tui_model.hpp"

namespace calmcar::xcp::tools {

/**
 * @brief 标定视图中的一行：一个已登记的标定对象及其最新样本。
 * @details 视图行与 A2L 符号分离，这样"符号已被 A2L 淘汰"时仍能显示名字
 *          并给出明确提示，而不是让整行消失。
 */
struct CalibrationViewRow {
    std::string name;             ///< 符号规范名（module::name）。
    bool symbol_valid{false};     ///< 该符号当前仍能在 A2L 数据库中解析。
    std::string kind_name;        ///< 显示用类型名，如 CHARACTERISTIC/VALUE。
    std::string base_type;        ///< A2L 基础数据类型名。
    std::string data_shape;       ///< 数据形态描述（标量/数组维度）。
    std::string unit;             ///< 物理单位；无单位时为空。
    ObservationSnapshot sample;   ///< 最新观测样本；从未读取时为默认值。
};

/**
 * @brief 标定视图行的文本格式化策略。
 * @details 抽成抽象接口是为了让 `CliApp` 只依赖"如何显示一行"，
 *          单测可以直接派生一个记录型实现来验证 `CliApp` 侧的行构造逻辑，
 *          而不必启动 FTXUI 界面或真实 XCP 连接。
 */
class CalibrationViewFormatter {
public:
    virtual ~CalibrationViewFormatter() = default;

    /**
     * @brief 把一行标定视图数据格式化为界面文本。
     * @param row 标定视图行数据。
     * @param compact 终端是否处于窄屏紧凑模式。
     * @return 用于 FTXUI 菜单的整行文本。
     */
    [[nodiscard]] virtual std::string Format(const CalibrationViewRow& row,
                                             bool compact) const = 0;
};

/**
 * @brief 默认标定视图格式化器：`名称 | 类型 | 当前值 单位 | 状态`。
 */
class DefaultCalibrationViewFormatter final : public CalibrationViewFormatter {
public:
    /**
     * @brief 格式化一行标定视图。
     * @param row 标定视图行数据。
     * @param compact 窄屏模式下只保留名称、类型和值。
     * @return 以 ` | ` 分隔的整行文本。
     */
    [[nodiscard]] std::string Format(const CalibrationViewRow& row,
                                     bool compact) const override {
        // A2L 候选已失效：名字仍要显示，否则用户无从知道该删除哪一项。
        if (!row.symbol_valid) {
            return compact ? row.name + " | A2L候选已失效"
                           : row.name + " | A2L候选已失效（可用 Del 移出名单）";
        }
        const std::string type_label =
            FormatSymbolTypeLabelOf(row.kind_name, row.base_type, row.data_shape,
                                    compact);
        std::string label;
        if (compact) {
            // 窄屏优先保证名称与值可见，类型压缩到最短。
            label = row.name + " | " + type_label + " | ";
        } else {
            label = row.name + " | " + type_label + " | ";
        }
        const ObservationSnapshot& sample = row.sample;
        if (sample.has_value) {
            if (sample.is_array) {
                label += "[" + std::to_string(sample.array_values.size()) +
                         " elements]";
            } else {
                label += sample.value;
            }
            if (!row.unit.empty()) label += " " + row.unit;
            if (sample.stale) label += "（旧值）";
            if (sample.stopped) label += "（已停止）";
        } else if (!sample.status.empty()) {
            label += sample.status;
        } else {
            label += "—";
        }
        if (!compact && !sample.timestamp.empty())
            label += " | " + sample.timestamp;
        return label;
    }

private:
    /**
     * @brief 在没有完整 SymbolInfo 时按已提取字段拼出类型标签。
     * @param kind_name 显示用类型名。
     * @param base_type A2L 基础数据类型名。
     * @param data_shape 数据形态描述。
     * @param compact 是否窄屏紧凑模式。
     * @return 与 `FormatSymbolTypeLabel()` 同风格的标签。
     */
    [[nodiscard]] static std::string FormatSymbolTypeLabelOf(
        const std::string& kind_name, const std::string& base_type,
        const std::string& data_shape, bool compact) {
        std::string shape = data_shape;
        if (compact) {
            const std::size_t array_label = shape.find("数组[");
            if (array_label != std::string::npos)
                shape = shape.substr(array_label);
        }
        return kind_name + " · " + base_type + " · " + shape;
    }
};

/**
 * @brief 标定名单为空时显示的唯一提示行。
 * @return 引导用户登记的固定文案。
 */
[[nodiscard]] inline std::string EmptyCalibrationViewHint() {
    return "（标定名单为空；在变量预览中按 F4，或输入 calmark <name> 登记）";
}

/**
 * @brief 把一组行数据格式化成菜单文本，并保证列表至少有一行可聚焦。
 * @param rows 标定视图行数据。
 * @param formatter 行格式化策略。
 * @param compact 是否窄屏紧凑模式。
 * @return 与 `rows` 等长，或在 `rows` 为空时只含空名单提示。
 */
[[nodiscard]] inline std::vector<std::string> BuildCalibrationViewLabels(
    const std::vector<CalibrationViewRow>& rows,
    const CalibrationViewFormatter& formatter, bool compact) {
    std::vector<std::string> labels;
    labels.reserve(rows.size() + 1U);
    for (const auto& row : rows) labels.push_back(formatter.Format(row, compact));
    if (labels.empty()) labels.push_back(EmptyCalibrationViewHint());
    return labels;
}

/**
 * @brief 把选中行夹紧到合法范围，并在列表为空时返回零。
 * @param selected 期望的选中行索引。
 * @param row_count 当前可显示的行数。
 * @return 位于 `[0, max(0, row_count - 1)]` 的索引。
 */
[[nodiscard]] inline int ClampCalibrationSelection(int selected,
                                                   std::size_t row_count) {
    if (row_count == 0U) return 0;
    if (selected < 0) return 0;
    const auto last = static_cast<int>(row_count - 1U);
    return selected > last ? last : selected;
}

/**
 * @brief 把 A2L 符号字段填入标定视图行（不涉及观测样本）。
 * @param symbol 已解析的 A2L 符号。
 * @param kind_name 显示用类型名。
 * @param row 待填充的行数据。
 */
inline void FillCalibrationViewRowFromSymbol(
    const calmcar::xcp::a2l::SymbolInfo& symbol, std::string_view kind_name,
    CalibrationViewRow& row) {
    const SymbolTypeDescription description = DescribeSymbolType(symbol);
    // 必须与 CliApp::QualifiedName() 的拼法保持一致，否则标定名单里会
    // 出现 "DemoM::" 与非 "DemoM::" 两种写法而无法互相查找。
    row.name = symbol.module_name + "::" + symbol.name;
    row.symbol_valid = true;
    row.kind_name = std::string(kind_name);
    row.base_type = description.base_type;
    row.data_shape = description.data_shape;
    row.unit = symbol.conversion.unit;
}

}  // namespace calmcar::xcp::tools
