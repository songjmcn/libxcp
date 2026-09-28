// =============================================================================
// a2l_result.hpp —— C++20 自有 Result<T>/Error（B-19，设计文档 §4.2 附注）
//
// 纪律：
//   1. 异常不跨 liba2l.dll 边界（SDK 侧已捕获并转整型码），桥接层内部同样
//      一律用 Result 表达失败，禁止用异常做控制流。
//   2. ErrorCode / Severity / Phase 为枚举，业务逻辑只匹配枚举，
//      禁止匹配 message 字符串（LastError() 仅作 UI 展示视图）。
//   3. Error 携带定位信息（path/line/column/module/symbol），全部可空；
//      多错误场景由上层收集后按 (Phase, Code, Path) 稳定排序展示。
// =============================================================================

#ifndef LIBXCP_A2L_A2L_RESULT_HPP_
#define LIBXCP_A2L_A2L_RESULT_HPP_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace calmcar::xcp::a2l {

/**
 * @brief 桥接层统一错误码（B-19）
 *
 * 与 liba2l::ErrorCode 语义对齐但独立定义（R1：SDK 枚举不直接充当领域类型），
 * 映射关系见 a2l_database_impl.cpp 的 MapSdkError()。
 */
enum class ErrorCode : std::uint16_t {
    Ok = 0,                     ///< 成功（仅占位，Error 中不会出现）
    NotReady,                   ///< 快照尚未发布（加载中，B-20）
    NotFound,                   ///< 符号不存在（B-13）
    AmbiguousName,              ///< 裸名命中多个符号（B-13/B-17）
    UnsupportedAddressingMode,  ///< INDIRECT/SUB_ADDRESS/ECU_CALIBRATION_OFFSET（B-1）
    AddressOverflow,            ///< 元素地址溢出或未按 AG 对齐（B-1）
    UnsupportedDataType,      ///< 未知/不支持的 ASAM 类型，禁止猜宽度（B-3）
    UnsupportedConversion,    ///< FORM 公式等不可执行换算（B-8）
    ConversionNotInvertible,  ///< 逆换算不唯一或不可数值求逆（B-8/B-15）
    ConversionOutOfRange,     ///< 逆换算结果越界 / 正算输入在表外（B-15）
    RawSizeMismatch,          ///< raw 字节长度与符号宽度不符（§6.1）
    InvalidLayout,         ///< 不规则数组/记录布局，拒绝扁平化猜测（B-2/B-8）
    UnsupportedOperation,  ///< 首里程碑明确拒绝的操作（Curve/Map
                           ///< 写入、结构体读写等）
    BadArgument,           ///< 参数非法（空名、越界计数等）
    ParseFailed,           ///< A2L 解析失败（来自 SDK）
    IoError,               ///< 文件读写错误（来自 SDK）
    AbiMismatch,           ///< liba2l.dll ABI 版本不匹配（R5）
    Internal,              ///< 未分类内部错误
};

/// @brief 错误严重级（B-16/B-19）
enum class Severity : std::uint8_t {
    Info,     ///< 仅提示
    Warning,  ///< 可继续，采用保守值
    Error,    ///< 阻断受影响功能
};

/**
 * @brief 错误发生的阶段（B-19）
 *
 * 用于把同一错误码区分到调用链的不同环节，便于 UI 归因。
 */
enum class Phase : std::uint8_t {
    Load,        ///< 文件加载/解析
    Query,       ///< 数据库查询（Find/Search/Count/ByteSizeOf）
    Address,     ///< 地址计算（B-1）
    Conversion,  ///< 物理值换算（ToPhysical/FromPhysical）
    Layout,      ///< DAQ/记录布局冻结与解码
    Compare,     ///< A2L 与运行时一致性比对（B-16）
    Runtime,     ///< 生命周期/线程契约违规（如加载中查询）
};

/// @brief ErrorCode → 可读名称（日志/断言用，不参与业务判断）
[[nodiscard]] const char* ToString(ErrorCode code) noexcept;

/// @brief Severity → 可读名称
[[nodiscard]] const char* ToString(Severity severity) noexcept;

/// @brief Phase → 可读名称
[[nodiscard]] const char* ToString(Phase phase) noexcept;

/**
 * @brief 加载期告警（批次15，F6/D4）
 *
 * @details 与 `Error` 的区别：告警**不阻断加载**，但代表"某个事实被放弃了"，
 *          必须让用户知情而不是静默消失。当前两个来源：
 *          ① STRUCTURE/INSTANCE 与已有符号**同名冲突**时被跳过的结构体
 *            （B-13 保留 MEASUREMENT/CHARACTERISTIC，但被丢弃的一方不能无声）；
 *          ② A2L 的 DAQ_LIST **未声明 FIRST_PID**，解码只能按"列表号回退"
 *            （批次15 F1/D1：该回退未经实际取证，属弱权威）。
 *          消费方只关心成败时可完全忽略本列表；`message` 仅展示用（B-19），
 *          业务分流只看 `code`/`phase`。
 */
struct LoadWarning {
    ErrorCode code = ErrorCode::Ok;  ///< 归并后的错误码（与 Error 同一口径）
    Phase phase = Phase::Load;       ///< "被放弃的那一步"所属阶段
    std::string subject;             ///< 受影响的规范键 / 对象名
    std::string message;             ///< 人读说明（不参与判定）
};

/**
 * @brief 结构化错误（B-19）
 *
 * 所有定位字段均可空：SDK 只给行号时列留空，无模块上下文时 module 留空。
 */
struct Error {
    ErrorCode code = ErrorCode::Internal;  ///< 错误码
    Severity severity = Severity::Error;   ///< 严重级
    Phase phase = Phase::Query;            ///< 发生阶段
    std::string path;                      ///< 关联文件路径（可空）
    std::optional<std::uint32_t> line;     ///< A2L 行号（LineNo()，可空）
    std::optional<std::uint32_t> column;   ///< A2L 列号（可空）
    std::string module;                    ///< 所属 MODULE 名（可空）
    std::string symbol;                    ///< 关联符号名（可空）
    std::string message;                   ///< 人类可读描述（UTF-8，仅展示）
    std::string cause;  ///< 底层原因文本（如 SDK LastError，可空）
    /**
     * @brief include 链（B-18 结构化载体，批次12）
     *
     * 仅 `/include` 预扫描类错误（循环、深度超限、越根）非空；其余错误恒空。
     * 元素为 **canonical 全路径（UTF-8）**，顺序 = 主文件 → 触发点，
     * 首元素恒为主文件；循环错误时链尾为回指节点（故链内必有重复项）。
     * 与 `message` 中的箭头链文本（仅文件名，B-19 显示契约）互不替代：
     * 业务逻辑仍只判 `code`/`phase`，本字段供 UI 与诊断定位用。
     */
    std::vector<std::string> include_chain;
};

namespace detail {

/** @brief Result 的空占位类型（T=void 时的成功载荷）。 */
struct Unit {};

}  // namespace detail

/**
 * @class Result
 * @brief `T` 或 `Error` 的二选一返回值（B-19）
 *
 * @tparam T 成功载荷类型；允许 void（等价于 Result<(), Error>）。
 *
 * 约定：
 *   - HasValue() 为 true 时 Value() 安全；否则 ErrorInfo() 有效。
 *   - 不做隐式 bool 转换以外的魔法；请显式使用 HasValue()。
 */
template <typename T>
class [[nodiscard]] Result {
public:
    /// @brief 从成功值构造
    Result(T value)
        : m_storage_(std::move(value)) {
    }  // NOLINT(google-explicit-constructor)

    /// @brief 从错误构造
    Result(Error error)
        : m_storage_(std::move(error)) {
    }  // NOLINT(google-explicit-constructor)

    /// @brief 是否成功
    [[nodiscard]] bool HasValue() const noexcept {
        return std::holds_alternative<T>(m_storage_);
    }

    /// @brief 兼容 std::expected 风格的显式布尔
    explicit operator bool() const noexcept { return HasValue(); }

    /// @brief 成功载荷引用（未检查；调用方须先判 HasValue()）
    [[nodiscard]] T& Value() & noexcept { return std::get<T>(m_storage_); }
    /// @copydoc Value()
    [[nodiscard]] const T& Value() const& noexcept {
        return std::get<T>(m_storage_);
    }
    /// @copydoc Value()
    [[nodiscard]] T&& Value() && noexcept {
        return std::get<T>(std::move(m_storage_));
    }

    /// @brief 错误信息（未检查；调用方须先判 !HasValue()）
    [[nodiscard]] const Error& ErrorInfo() const& noexcept {
        return std::get<Error>(m_storage_);
    }
    /// @brief 错误信息（从左值取副本；移动语义在 Error 较小时非必需）
    [[nodiscard]] Error TakeError() const& noexcept {
        return std::get<Error>(m_storage_);
    }
    /// @brief 错误信息（右值取出）
    [[nodiscard]] Error TakeError() && noexcept {
        return std::get<Error>(std::move(m_storage_));
    }

    /// @brief 成功取值，失败时回落默认值
    [[nodiscard]] T ValueOr(T fallback) const& {
        return HasValue() ? Value() : std::move(fallback);
    }

private:
    std::variant<T, Error> m_storage_;
};

/** @brief void 特化：只表达成功/失败。 */
template <>
class [[nodiscard]] Result<void> {
public:
    /// @brief 成功
    Result() : m_storage_(detail::Unit{}) {}
    /// @brief 失败
    Result(Error error)
        : m_storage_(std::move(error)) {
    }  // NOLINT(google-explicit-constructor)

    /// @brief 是否成功
    [[nodiscard]] bool HasValue() const noexcept {
        return std::holds_alternative<detail::Unit>(m_storage_);
    }

    /// @brief 兼容显式布尔
    explicit operator bool() const noexcept { return HasValue(); }

    /// @brief 错误信息（未检查）
    [[nodiscard]] const Error& ErrorInfo() const& noexcept {
        return std::get<Error>(m_storage_);
    }
    /// @brief 错误信息（左值取副本）
    [[nodiscard]] Error TakeError() const& noexcept {
        return std::get<Error>(m_storage_);
    }
    /// @brief 错误信息（右值取出）
    [[nodiscard]] Error TakeError() && noexcept {
        return std::get<Error>(std::move(m_storage_));
    }

private:
    std::variant<detail::Unit, Error> m_storage_;
};

namespace detail {

/** @brief 按 Error 构造 Result 的公共辅助（避免各实现处重复样板）。 */
inline Error MakeError(ErrorCode code, Phase phase, std::string message,
                       Severity severity = Severity::Error) {
    Error e;
    e.code = code;
    e.phase = phase;
    e.severity = severity;
    e.message = std::move(message);
    return e;
}

}  // namespace detail

}  // namespace calmcar::xcp::a2l

#endif  // LIBXCP_A2L_A2L_RESULT_HPP_
