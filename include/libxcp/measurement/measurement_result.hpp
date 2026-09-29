/**
 * @file measurement_result.hpp
 * @brief 测量子系统的结果封装类型（v0.4，A2L-free）。
 *
 * 依 AGENTS.md I8 与架构文档 §18：测量层**自持**一套与 A2L `a2l_result.hpp`
 * 风格一致的 `MeasurementResult<T>`，但核心不依赖 A2L 类型、不 `#include` 任何
 * `libxcp/a2l/*`。对应测量子系统代码增长计划 §4.4 的测量错误分类。
 */

#ifndef CALMCAR_XCP_MEASUREMENT_MEASUREMENT_RESULT_HPP_
#define CALMCAR_XCP_MEASUREMENT_MEASUREMENT_RESULT_HPP_

#include <optional>
#include <string>
#include <utility>

namespace calmcar::xcp {

/**
 * @brief 测量层错误码（核心自持枚举）
 * @details 与架构文档 §18 的错误分类对应。这些是测量路径上的**可预期、可恢复**
 *          业务错误，与传输层 `XcpException`（fatal，需 Stop）分开：
 *             - 单帧 DTO 解码失败 / 单符号换算失败 → drop 该帧/样本 + 统计；
 *             - EOF 配置失败 / 代际失配 / 断开 → 走 `XcpException` 上抛并 Stop。
 */
enum class MeasurementErrorCode {
    NotFound,             ///< 未找到名为 name 的测量
    AmbiguousName,        ///< name 有歧义（库内同名多个）
    RawSizeMismatch,      ///< raw 字节长度与元素宽度不符（B-1/B-3）
    UnsupportedDataType,  ///< 元素类型不支持换算（无固定宽度，ElementSizeOf==0）
    UnsupportedConversion,  ///< 换算规则不支持（如不可逆/未知公式）
    InvalidLayout,        ///< 布局/装箱非法（跨 ODT、单变量超 MAX_DTO 等）
    DaqConfigurationError,  ///< DAQ 配置类错误（容量不足、识别字段不支持等）
    DtoDecodeError,       ///< DTO 解码错误
    Busy,                 ///< 会话忙（如 DAQ 已运行再配置）
    NotRunning,           ///< 会话未在运行
    Fatal,                ///< 致命错误（应在调用方上抛 XcpException 并 Stop）
};

/**
 * @brief 测量路径上的单个错误
 * @details 携带错误码、可选的测量名与人类可读说明；同 `a2l_result.hpp::Error`
 *          的最小核心子集（不含 A2L 的 phase/path/line/column 等桥接细节 —— 需要
 *          时由适配器补在 message 里）。
 */
struct MeasurementError {
    MeasurementErrorCode code{MeasurementErrorCode::Fatal};
    std::string symbol;      ///< 相关测量名（空表示不针对单个符号）
    std::string message;     ///< 中文人类可读说明

    MeasurementError() = default;
    MeasurementError(MeasurementErrorCode code, std::string message,
                     std::string symbol = {})
        : code(code),
          symbol(std::move(symbol)),
          message(std::move(message)) {}
};

/**
 * @brief 测量层结果封装（A2L-free）
 * @tparam T 成功时的值类型；可为 `void`。
 * @details 语义对齐 `a2l_result.hpp::Result<T>`：显式 `operator bool`、
 *          `HasValue()/Value()/ErrorInfo()`。核心只此一种结果类型，供
 *          `IMeasurementDatabase`/`MeasurementPlanner`/后续 `MeasurementSession`
 *          返回可恢复错误。
 */
template <class T>
class MeasurementResult {
public:
    MeasurementResult(T value) : m_value_(std::move(value)) {}
    MeasurementResult(MeasurementError error)
        : m_error_(std::move(error)) {}

    /// @brief 是否承载成功值
    [[nodiscard]] bool HasValue() const noexcept { return m_value_.has_value(); }
    /// @brief 显式布尔：成功为 true
    explicit operator bool() const noexcept { return m_value_.has_value(); }

    /// @brief 成功值（无值时行为未定义；先判 HasValue）
    [[nodiscard]] T& Value() { return *m_value_; }
    /// @brief 成功值 const 版
    [[nodiscard]] const T& Value() const { return *m_value_; }

    /// @brief 错误信息（成功时无意义）
    [[nodiscard]] const MeasurementError& ErrorInfo() const { return m_error_; }

    /// @brief 取出错误（适合 move 语义处）
    [[nodiscard]] MeasurementError TakeError() { return m_error_; }

private:
    std::optional<T> m_value_;
    MeasurementError m_error_;
};

/**
 * @brief `void` 特化：只携带"成功/失败"，无值体。
 */
template <>
class MeasurementResult<void> {
public:
    MeasurementResult() = default;  // 成功
    MeasurementResult(MeasurementError error) : m_error_(std::move(error)) {}

    [[nodiscard]] bool HasValue() const noexcept { return !m_error_.has_value(); }
    explicit operator bool() const noexcept { return HasValue(); }
    [[nodiscard]] const MeasurementError& ErrorInfo() const { return *m_error_; }

private:
    std::optional<MeasurementError> m_error_;
};

namespace detail {

/// @brief 便捷工厂：成功值
template <class T>
[[nodiscard]] MeasurementResult<T> MakeMeasurementOk(T value) {
    return MeasurementResult<T>(std::move(value));
}

/// @brief 便捷工厂：错误
template <class T>
[[nodiscard]] MeasurementResult<T> MakeMeasurementError(
    MeasurementErrorCode code, std::string message, std::string symbol = {}) {
    return MeasurementResult<T>(
        MeasurementError(code, std::move(message), std::move(symbol)));
}

}  // namespace detail

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_MEASUREMENT_MEASUREMENT_RESULT_HPP_