// =============================================================================
// a2l_bridge.hpp —— 桥接层门面（设计文档 §4.5，B-16/B-20）
//
// 这是主树唯一入口：Load/LoadAsync 管理 liba2l.dll 生命周期并发布不可变
// 快照；Database()/XcpInfo() 在快照发布后才可用（加载中 NotReady）。
// =============================================================================

#ifndef LIBXCP_A2L_A2L_BRIDGE_HPP_
#define LIBXCP_A2L_A2L_BRIDGE_HPP_

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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
    /// @brief IF_DATA XCP 来源 MODULE（B-17，批次10）
    /// @details 空=自动：恰一个 MODULE 含有效 IF_DATA 时取它；多个 MODULE
    /// 同时声明时报 AmbiguousName（require_if_data_xcp=true 时为加载错误，
    /// false 时降级为 info.ok=false）。非空=精确匹配 MODULE 名。
    std::string active_module;
    /// @brief 是否允许 /include 越出主 A2L 目录（B-18，批次10，默认禁止）
    /// @details 循环 include 与深度 >32 无论本开关一律拒绝（解析前预扫描）。
    bool allow_include_outside_root = false;
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
    // ---- 批次14（T14-11）：DAQ 侧比对项；nullopt = 未查询 → 跳过比对 ----
    /// @brief IDENTIFICATION_FIELD_TYPE 原码（GET_DAQ_PROCESSOR_INFO 的
    ///        DAQ_KEY_BYTE bit6-7；docs L1445 说它是**实际**编码的唯一来源）
    std::optional<std::uint8_t> daq_identification_field_type;
    /// @brief ADDRESS_EXTENSION 原码（DAQ_KEY_BYTE bit4-5：0=FREE/1=ODT/3=DAQ；
    ///        注意 3 不是枚举序号，与 A2L 侧同口径）
    std::optional<std::uint8_t> daq_address_extension_mode;
    /// @brief GRANULARITY_ODT_ENTRY_SIZE_DAQ（GET_DAQ_RESOLUTION_INFO，字节数）
    std::optional<std::uint8_t> daq_odt_entry_min_size_bytes;
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
     * @brief 加载期告警清单（批次15，F6/D4）
     * @return 只读引用；无告警时为空（快照未发布时也为空，不报错）
     * @details 告警 = "某个事实被放弃了，但不阻断加载"，当前两项：
     *          ① STRUCTURE/INSTANCE 与已有符号同名而被跳过的条目（B-13 保留
     *            测量/标定量，被丢弃的一方不能无声）；
     *          ② A2L 的 DAQ_LIST 未声明 FIRST_PID，解码只能按"列表号回退"
     *            （F1/D1：该回退未经实际取证，属弱权威，需让用户知情）。
     *          选择"新增只读接口"而不是把提示塞进 description：后者会污染
     *          展示文本并让既有断言被迫跟着改（B-19 的 message 只展示、不参与
     *          判定这一契约就破了）。不改 liba2l、不动 SDK ABI。
     */
    [[nodiscard]] const std::vector<LoadWarning>& ListLoadWarnings()
        const noexcept;

    /**
     * @brief IF_DATA XCP 汇总
     * @return 快照未发布时为 nullptr
     */
    [[nodiscard]] const IfDataXcpInfo* XcpInfo() const noexcept;

    /**
     * @brief 基于已冻结 STATIC DAQ 布局创建解码器
     * @return 无 IF_DATA XCP 或无预定义列表时返回结构化错误
     * @details 布局来源 = A2L `IF_DATA ... PREDEFINED`（快照
     *          `source=A2lPredefined`，按 EPK == 列表号路由）。
     */
    [[nodiscard]] Result<std::unique_ptr<IDaqLayout>> CreateDaqLayout() const;

    /**
     * @brief 用**本端 WRITE_DAQ 账本**冻结布局并创建解码器（批次14，B-6）
     * @param ledger 账本条目（由调用方从 `XcpMaster::DaqLedger()` 转换而来）
     * @param generation 配置代际（`XcpMaster::DaqConfigGeneration()`）
     * @details B-6 的权威分工在此落地：可配置 STATIC 以本端账本为准
     *          （A2L 的 `static_daq_lists` 只作候选/一致性约束），因此
     *          **账本与 A2L 顺序相反时按账本解码**；账本为空时得到的快照
     *          没有 PID 路由，`Decode` 一律 `InvalidLayout`，调用方保留
     *          raw DTO 原样（不猜符号与顺序）。
     * @return 成功返回解码器；快照未发布返回 `NotReady`；DYNAMIC 能力返回
     *         `UnsupportedOperation`（B-5 不变）
     */
    [[nodiscard]] Result<std::unique_ptr<IDaqLayout>> CreateDaqLayoutFromLedger(
        const std::vector<DaqLedgerEntryView>& ledger,
        std::uint32_t generation) const;

    /**
     * @brief 用 **ECU 回读**（READ_DAQ + 各列表 FIRST_PID）冻结布局并创建解码器
     *        （批次14，T14-10；B-6 里 PREDEFINED 列表的取证通路）
     * @param readback `READ_DAQ` 逐条回读的 Entry（AG
     * 换算与位偏归一由调用方完成）
     * @param list_pids 各 DAQ List 的 FIRST_PID（`GET_DAQ_LIST_INFO` 无此项，
     *        取自 `START_STOP_DAQ_LIST` 响应，docs L2222）
     * @param generation 配置代际
     * @details 与账本入口的分工：可配置列表以本端下发为权威，PREDEFINED 列表
     *          只能靠回读取证。两者都生成 PID 路由，解码一律按路由定位单个
     * ODT； 回读为空 → `Decode` 返回 `InvalidLayout`（不拿 A2L 顺序凑数）。
     */
    [[nodiscard]] Result<std::unique_ptr<IDaqLayout>>
    CreateDaqLayoutFromEcuReadback(
        const std::vector<DaqReadbackEntry>& readback,
        const std::vector<DaqListPid>& list_pids,
        std::uint32_t generation) const;

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
