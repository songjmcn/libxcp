// =============================================================================
// a2l_bridge.hpp —— 桥接层门面（设计文档 §4.5，B-16/B-20）
//
// 这是主树唯一入口：Load/LoadAsync 管理 liba2l.dll 生命周期并发布不可变
// 快照；Database()/XcpInfo() 在快照发布后才可用（加载中 NotReady）。
// =============================================================================

#ifndef LIBXCP_A2L_A2L_BRIDGE_HPP_
#define LIBXCP_A2L_A2L_BRIDGE_HPP_

#include <functional>
#include <memory>
#include <string>

#include "libxcp/a2l/a2l_result.hpp"
#include "libxcp/a2l/daq_layout.hpp"
#include "libxcp/a2l/if_data_xcp.hpp"
#include "libxcp/a2l/ia2l_database.hpp"

namespace calmcar::xcp::a2l {

/**
 * @brief 加载选项
 */
struct LoadOptions {
    bool module_information_only =
        false;  ///< 快速模式：仅模块级信息（透传 SDK）
    bool require_if_data_xcp =
        true;  ///< 缺失 IF_DATA XCP 时是否判为错误（§6.1）
    int progress_notify_percent = 5;  ///< 进度回调的最小百分比间隔
};

/**
 * @brief A2L 声明值与 Slave 运行时值的单项差异（B-16）
 */
struct ParamDiscrepancy {
    std::string parameter_name;  ///< 参数名（如 MAX_CTO / BYTE_ORDER）
    std::string a2l_value;       ///< A2L 侧文本值（展示用）
    std::string runtime_value;   ///< 运行时查询文本值（真值来源）
    Severity severity = Severity::Warning;  ///< 分级：Error 只阻断受影响功能
    std::string
        affected_feature;  ///< 受影响功能（MemoryAccess / Daq / Transport 等）
};

/**
 * @brief Slave 运行时参数（CompareWithRuntime 的输入）
 * @details 字段来自现有 SessionParameters/GetCommModeInfo 等查询结果；
 *          t1..t7 不在此列——它们是 Master 超时配置，不作比对项（B-16）。
 */
struct RuntimeXcpParams {
    std::uint16_t protocol_version =
        0x0100;                 ///< CONNECT RES 的 TRANSFER_LAYER 版本
    std::uint8_t max_cto = 0;   ///< CONNECT RES MAX_CTO
    std::uint16_t max_dto = 0;  ///< CONNECT RES MAX_DTO
    ByteOrder byte_order = ByteOrder::MsbLast;  ///< COMM_MODE_BASIC bit0
    AddressGranularity address_granularity =
        AddressGranularity::Byte;  ///< bit1-2
    bool has_daq = false;          ///< 资源位含 DAQ
};

/**
 * @class A2lBridge
 * @brief A2L 数据库门面：加载、查询入口与一致性比对。
 *
 * 线程契约（B-20/A-11）：
 *   - Load/LoadAsync 完成回调之前禁止调用 Database()/XcpInfo()；
 *   - 快照发布后 Database()/XcpInfo() 可并发只读；
 *   - 析构会等待异步解析结束（SDK 契约：回调返回前不得销毁对象）。
 */
class A2lBridge {
public:
    /**
     * @brief 同步加载 A2L 文件
     * @param file_path 本地路径（首里程碑只支持本地文件，§6.3）
     * @param options 加载选项
     * @return 成功时返回就绪的桥接对象；失败时 Error.phase == Phase::Load
     */
    [[nodiscard]] static Result<std::unique_ptr<A2lBridge>> Load(
        const std::string& file_path, const LoadOptions& options = {});

    /**
     * @brief 异步加载：解析在 liba2l.dll 内部线程进行
     * @param file_path 本地路径
     * @param options 加载选项
     * @param ready 完成回调（可能运行在 SDK 线程；参数为加载结果）
     * @return 受理成功/失败；受理失败时不会调用 ready
     * @note 回调返回之前不得销毁本对象（A-11）。
     */
    [[nodiscard]] static Result<std::unique_ptr<A2lBridge>> LoadAsync(
        const std::string& file_path, const LoadOptions& options,
        std::function<void(Result<void>)> ready);

    ~A2lBridge();

    A2lBridge(const A2lBridge&) = delete;
    A2lBridge& operator=(const A2lBridge&) = delete;

    /// @brief 加载进度 0~100（同步 Load 成功后恒 100）
    [[nodiscard]] int Progress() const noexcept;

    /**
     * @brief 符号数据库视图
     * @return 加载中或加载失败时为 nullptr（业务代码须配合 Count()==NotReady
     * 判定）
     */
    [[nodiscard]] const IA2lDatabase* Database() const noexcept;

    /**
     * @brief IF_DATA XCP 汇总
     * @return 快照未发布时为 nullptr
     */
    [[nodiscard]] const IfDataXcpInfo* XcpInfo() const noexcept;

    /**
     * @brief 基于已冻结 STATIC DAQ 布局创建解码器
     * @return 无 IF_DATA XCP 或无预定义列表时返回结构化错误
     */
    [[nodiscard]] Result<std::unique_ptr<IDaqLayout>> CreateDaqLayout() const;

    /**
     * @brief A2L 声明 vs Slave 运行时一致性比对（B-16，规范 §8.4 要求）
     * @param runtime 运行时查询结果（真值来源）
     * @return 差异列表（稳定排序；Error 项只阻断受影响功能，不整体失败）
     */
    [[nodiscard]] Result<std::vector<ParamDiscrepancy>> CompareWithRuntime(
        const RuntimeXcpParams& runtime) const;

    /**
     * @brief 最近一次错误的展示文本（仅 UI 格式化视图，B-19）
     * @details 业务逻辑禁止匹配此字符串，应使用 Result 的 ErrorCode。
     */
    [[nodiscard]] std::string LastError() const;

private:
    A2lBridge();

    struct Impl;                    ///< 前置声明：内部持有 IDoc 与领域快照
    std::unique_ptr<Impl> m_impl_;  ///< Pimpl（隔离 SDK 头文件，R1）
};

}  // namespace calmcar::xcp::a2l

#endif  // LIBXCP_A2L_A2L_BRIDGE_HPP_
