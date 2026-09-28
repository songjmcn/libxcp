/**
 * @file xcp_master.hpp
 * @brief XCP Master 顶层门面：用户唯一入口。
 *
 * 依据 code-plan/XCP_1.3.0_详细设计_类接口与头文件.md 第 14 节实现。
 */

#ifndef CALMCAR_XCP_XCP_MASTER_HPP_
#define CALMCAR_XCP_XCP_MASTER_HPP_

#include <functional>
#include <memory>
#include <optional>

#include "libxcp/command_executor.hpp"
#include "libxcp/ixcp_transport.hpp"
#include "libxcp/memory_access.hpp"
#include "libxcp/session.hpp"
#include "libxcp/xcp_error.hpp"

namespace calmcar::xcp {

/**
 * @brief Seed→Key 算法回调（Seed&Key，批次 7）
 *
 * XCP 不规定算法，A2L 的 SEED_AND_KEY_EXTERNAL_FUNCTION 仅给出供应商函数名；
 * 本库以回调注入方式由调用方提供算法（可为函数、lambda 或捕获上下文的可调用
 * 对象）。参数顺序与规范 §9.2 的 XCP_ComputeKeyFromSeed 一致：先特权资源、
 * 后 Seed。
 * @note seed 按 XCP Packet 实际传输顺序原样传入，返回的 Key 字节同样按传输
 *       顺序、禁止按本机字节序重排（docs/XCP_1.3.0_document.md §9.2）。
 */
using SeedKeyCalculator =
    std::function<Bytes(Resource resource, BytesView seed)>;

/**
 * @brief XcpMaster::Unlock 的执行结果（Seed&Key，批次 7）
 */
struct UnlockResult {
    /// @brief true = GET_SEED 返回 Length 0，资源本就未保护（未发送 UNLOCK）
    bool was_already_unlocked{false};
    /// @brief UNLOCK 末帧响应的 Current Resource Protection Status
    /// @details 资源本就未保护时为 std::nullopt——GET_SEED 响应协议上不含该
    ///          字段，不伪造值；需要权威保护掩码请调用 QueryStatus()。
    std::optional<ResourceMask> resource_protection;
};

/**
 * @brief 一个 ODT Entry 的写入配置（批次14，T14-08）
 * @details 字段与 WRITE_DAQ 一一对应（docs L2161-2166）：地址、地址扩展、
 *          SIZE（**以 AG 为单位的元素数**）、BIT_OFFSET。
 *          BIT_OFFSET 只在 0 或 kDaqBitOffsetNone(0xFF) 时被本库编排使用；
 *          位元素 ODT 属后续里程碑（A2L 侧同样只做元数据，B-9）。
 */
struct DaqEntrySpec {
    Address address{0};                          ///< 32 位地址
    AddressExtension extension{0};               ///< 8 位地址扩展
    std::uint8_t size{0};                        ///< 元素数（以 AG 为单位）
    std::uint8_t bit_offset{kDaqBitOffsetNone};  ///< 位偏；0xFF=无位偏
};

/// @brief 一个 ODT 的 Entry 序列（写入顺序即协议顺序；账本按此顺序编号）
struct DaqOdtSpec {
    std::vector<DaqEntrySpec> entries;  ///< ODT 内 Entry 列表（0 基索引）
};

/**
 * @brief 一个可配置 STATIC DAQ List 的配置入参（批次14，T14-08）
 * @details 只覆盖本里程碑需要的字段；ALTERNATING / DYNAMIC / PREDEFINED
 *          改写均不在范围内（B-5 维持拒绝）。
 * @note `pid_off = true` 会被 A2L 侧解码器显式拒绝（B-7：本库没有
 *       Transport 层 DAQ List 关联通道），因此这里即便配置了也无法解码。
 */
struct DaqListSpec {
    std::uint16_t daq_list{0};       ///< DAQ List 号（EPK）
    std::uint16_t event_channel{0};  ///< 事件通道号（0=不由通道触发）
    std::uint8_t prescaler{1};       ///< 降频因子（1=不降频）
    std::uint8_t priority{0};        ///< 优先级（0xFF 最高，0=允许缓冲）
    bool stim_direction{false};      ///< true=STIM，false=DAQ（docs L2192）
    bool dto_counter{false};         ///< DTO 携带计数器
    bool timestamp{false};           ///< DTO 携带时间戳
    bool pid_off{false};             ///< DTO 不带识别字段（解码侧拒绝）
    std::vector<DaqOdtSpec> odts;    ///< ODT 序列（写入顺序 = 配置顺序）
};

/**
 * @brief 本端 WRITE_DAQ 账本条目（批次14，T14-08；B-6 的解码权威）
 * @details B-6：DTO 解码必须按**实际下发顺序**，不得用 A2L 的
 *          PREDEFINED 顺序猜测。本结构就是"我到底写了什么、写在第几个 PID"
 *          的记账，`pid` 在 START 拿到 FIRST_PID 后回填。
 */
struct DaqLedgerEntry {
    std::uint16_t daq_list{0};      ///< DAQ List 号（EPK）
    std::uint8_t odt_number{0};     ///< ODT 号（0 基，SET_DAQ_PTR 用值）
    std::uint8_t odt_entry{0};      ///< Entry 号（0 基）
    Address address{0};             ///< 写入的 32 位地址
    AddressExtension extension{0};  ///< 写入的地址扩展
    std::uint8_t size{0};           ///< 元素数（以 AG 为单位）
    std::uint8_t bit_offset{kDaqBitOffsetNone};  ///< 位偏（0xFF=无）
    std::optional<std::uint8_t> pid;  ///< Absolute ODT Number 下的 PID
                                      ///< （= FIRST_PID + 相对 ODT 号）；
                                      ///< 未 START 前为 nullopt（不猜）
};

/**
 * @brief XCP Master 顶层门面
 *
 * 内部组合 Session、CommandExecutor、MemoryAccess，并持有 Transport 所有权。
 * 典型调用序列见设计文档附录 B.1。
 */
class XcpMaster {
public:
    /**
     * @brief 构造
     * @param transport Transport 实例（XcpMaster 接管所有权）
     * @param timeouts 命令超时配置
     * @param event_listener 事件监听器（可选，非拥有；生命周期须长于本对象）
     */
    explicit XcpMaster(std::unique_ptr<IXcpTransport> transport,
                       CommandTimeouts timeouts = {},
                       IEventListener* event_listener = nullptr);

    /// @brief 析构：尽力断开逻辑会话并关闭 Transport
    ~XcpMaster();

    // 禁止拷贝
    XcpMaster(const XcpMaster&) = delete;
    XcpMaster& operator=(const XcpMaster&) = delete;

    // ---- 连接管理 ----

    /**
     * @brief 建立 XCP 连接
     * @details Transport.Open -> CONNECT -> [GET_COMM_MODE_INFO] -> GET_STATUS
     * @throws XcpException 连接失败（失败时已关闭通道并清理本地状态）
     */
    void Connect();

    /**
     * @brief 断开 XCP 连接
     * @details DISCONNECT -> Transport.Close。即使 DISCONNECT
     * 失败也释放本地资源， 并向调用方抛出原始错误；重复断开在本地幂等。
     */
    void Disconnect();

    /// @brief 是否已连接
    [[nodiscard]] bool IsConnected() const;

    // ---- 内存读取 ----

    /**
     * @brief 读取内存（以字节为单位）
     * @param address 32 位地址
     * @param extension 地址扩展
     * @param byte_count 字节数（必须可被 AG 整除）
     */
    [[nodiscard]] Bytes ReadMemoryBytes(Address address,
                                        AddressExtension extension,
                                        ByteCount byte_count);

    /**
     * @brief 读取内存（以元素为单位，按 AG 计数）
     * @details 与字节版本分开命名：ByteCount 与 ElementCount 同为 uint32_t
     * 别名， 若共用 ReadMemory 名称会导致字面量调用产生重载歧义。
     */
    [[nodiscard]] Bytes ReadMemory(Address address, AddressExtension extension,
                                   ElementCount element_count);

    // ---- 内存写入（批次14，T14-06 / R9）----

    /**
     * @brief 写入内存（以字节为单位；SHORT_DOWNLOAD 优先，必要时分块 DOWNLOAD）
     * @param address 32 位起始地址
     * @param extension 地址扩展
     * @param data 待写入字节（长度必须可被 AG 整除）
     * @details 与读取侧对称的编排：一帧装得下走 SHORT_DOWNLOAD，Slave 不支持
     *          （ERR_CMD_UNKNOWN）或装不下则回落 SET_MTA + DOWNLOAD 分块。
     * @throws XcpException(InvalidArgument) 数据为空、不可被 AG 整除、
     *         地址溢出或 MAX_CTO 过小
     * @throws XcpException(ProtocolError) Slave 拒绝写入
     *         （如 ERR_WRITE_PROTECTED 未开 CAL/PAG 权限）
     * @throws XcpException 超时或恢复失败
     * @note 写回**没有跨块原子性**：中途失败时 Slave 内存处于"前半已写、
     *       后半未写"状态，异常消息带"已完成 x/y 元素"；调用方必须重试整个
     *       写操作而不是忽略（docs L1985）。
     * @note 读取侧有 ReadMemoryBytes / ReadMemory 两个入口是因为"数量"的
     *       单位可以是字节或元素；写入的数量由数据长度唯一决定，故这里
     *       只有一个入口，不再造一个同名不同参的冗余 API。
     */
    void WriteMemoryBytes(Address address, AddressExtension extension,
                          BytesView data);

    // ---- Seed&Key 解锁（批次 7）----

    /**
     * @brief 解锁受 Seed&Key 保护的单个资源
     * @param resource 要解锁的资源（必须恰为 CAL/PAG、DAQ、STIM、PGM 之一）
     * @param calculator Seed→Key 算法回调（不可为空）
     * @return 解锁结果（本就未解锁短路 / UNLOCK 末帧保护掩码）
     * @throws XcpException(InvalidArgument) resource 非单资源位、回调为空、
     *         回调返回的 Key 为空或超过 255 字节（Length 字段上限）
     * @throws XcpException(ProtocolError) 协议错误；Key 校验失败时 Session
     *         转入 Failed（Slave 已主动断开，再次 Connect() 自动 Reset 重建）
     * @throws XcpException 超时或恢复失败
     * @details 编排流程：GET_SEED(First) -> [GET_SEED(Remainder)*] ->
     *          calculator(seed) -> [UNLOCK 分段]*，Seed/Key 按 MAX_CTO-2
     *          分段。计划 §6.4：ERR_ACCESS_LOCKED 只报告、不自动触发解锁，
     *          本方法必须由调用方显式调用。
     */
    [[nodiscard]] UnlockResult Unlock(Resource resource,
                                      const SeedKeyCalculator& calculator);

    // ---- 状态查询 ----

    /// @brief 获取 Session 参数快照
    [[nodiscard]] SessionParameters GetSessionParameters() const;

    /// @brief 获取当前 Session 状态
    [[nodiscard]] SessionState GetSessionState() const;

    /// @brief 手动查询 GET_STATUS 并更新 Session
    [[nodiscard]] GetStatusResponse QueryStatus();

    // ---- DAQ 编排（批次14，T14-08：最小"可配置 STATIC"通路）----

    /**
     * @brief 配置一个可配置 STATIC DAQ List（CLEAR + 逐 Entry
     * SET_DAQ_PTR/WRITE_DAQ
     *        + SET_DAQ_LIST_MODE），并登记本端 WRITE_DAQ 账本
     * @param spec 列表配置（ODT/Entry 顺序即下发顺序）
     * @details 严格约束（B-5/B-6/B-7）：
     *          - 只支持**可配置**列表：PREDEFINED 列表由 Slave 拒写
     *            （WRITE_DAQ → ERR_WRITE_PROTECTED，docs L2168），本方法不
     *            做任何"当成可配置来写"的尝试；
     *          - 每个 Entry 前都显式 SET_DAQ_PTR（不依赖 Slave 指针自增，
     *            避开 docs L2172「写过末位后指针未定义」）；
     *          - 任一步失败即上抛，账本回滚到本次调用前的状态——
     *            **不留半份账**，否则解码会按错位布局解释 DTO。
     * @throws XcpException(InvalidArgument) spec 非法（无 ODT/无 Entry/
     *         Entry size 为 0/超过单字节字段上限）
     * @throws XcpException 协议错误、超时或恢复失败
     */
    void ConfigureDaqList(const DaqListSpec& spec);

    /**
     * @brief 启动一个已配置的 DAQ List（START_STOP_DAQ_LIST(Start)）
     * @param daq_list DAQ List 号（EPK）
     * @return Slave 响应的 FIRST_PID（Absolute ODT Number 模式，docs L2222）
     * @details 拿到 FIRST_PID 后回填账本里该列表的 `pid` 字段
     *          （`pid = FIRST_PID + 相对 ODT 号`，docs L2225），并把列表登记进
     *          Session 的 DAQ 运行态，供 Disconnect/析构补发 STOP。
     * @throws XcpException(InvalidState) 该列表尚未配置
     * @throws XcpException 协议错误、超时或恢复失败
     */
    [[nodiscard]] std::uint8_t StartDaqList(std::uint16_t daq_list);

    /**
     * @brief 停止指定 DAQ List（START_STOP_DAQ_LIST(Stop)）
     * @param daq_list DAQ List 号（EPK）
     * @throws XcpException 协议错误、超时或恢复失败
     */
    void StopDaqList(std::uint16_t daq_list);

    /**
     * @brief 停止全部 DAQ List（START_STOP_SYNCH(stop all)）
     * @details 一次性收尾入口；成功后清空 Session 的 DAQ 运行态（账本保留，
     *          因为配置本身仍在 Slave 里，重新 Start 不需要重写）。
     * @throws XcpException 协议错误、超时或恢复失败
     */
    void StopDaq();

    /// @brief 释放并清空指定 DAQ List 的配置（CLEAR_DAQ_LIST；账本同步删除）
    /// @throws XcpException 协议错误、超时或恢复失败
    void ClearDaqList(std::uint16_t daq_list);

    /// @brief 本端 WRITE_DAQ 账本快照（B-6：DTO 解码的唯一权威布局来源）
    [[nodiscard]] const std::vector<DaqLedgerEntry>& DaqLedger() const noexcept;

    /// @brief 当前 DAQ 配置代际（与 Session 一致；解码侧用于识别陈旧账本）
    [[nodiscard]] std::uint32_t DaqConfigGeneration() const;

    // ---- DAQ 运行时取证（批次14；T14-10 的 PREDEFINED 通路与 T14-11 的
    //      B-16 比对都必须有运行时数据源，故在此薄转发 CommandExecutor 的
    //      Execute*。四个方法零策略、零新逻辑，仅解决"Executor 是私有成员、
    //      调用方拿不到取证入口"这一点；登记见批次14 记录 §0.2 偏差 5）----

    /**
     * @brief 查询 DAQ 处理器能力（GET_DAQ_PROCESSOR_INFO，Optional）
     * @return 解析结果；Slave 回 ERR_CMD_UNKNOWN 时为 std::nullopt
     * @details DAQ_KEY_BYTE 是 identification_field_type /
     *          address_extension_mode 的**唯一**运行时真值（docs L1445），
     *          桥接层 `RuntimeXcpParams` 的对应字段应由这里填充。
     */
    [[nodiscard]] std::optional<GetDaqProcessorInfoResponse>
    QueryDaqProcessorInfo();

    /**
     * @brief 查询 ODT Entry
     * 粒度与时间戳能力（GET_DAQ_RESOLUTION_INFO，Optional）
     * @return 解析结果；Slave 回 ERR_CMD_UNKNOWN 时为 std::nullopt
     */
    [[nodiscard]] std::optional<GetDaqResolutionInfoResponse>
    QueryDaqResolutionInfo();

    /**
     * @brief 查询单个 DAQ List 的容量与固定事件（GET_DAQ_LIST_INFO，Optional）
     * @param daq_list DAQ List 号（EPK）
     * @return 解析结果；Slave 回 ERR_CMD_UNKNOWN 时为 std::nullopt
     * @note 本响应**不含 FIRST_PID**（docs L2452-2457）；FIRST_PID 只能由
     *       `StartDaqList()` 的 START_STOP_DAQ_LIST 响应取得（docs L2222）。
     */
    [[nodiscard]] std::optional<GetDaqListInfoResponse> QueryDaqListInfo(
        std::uint16_t daq_list);

    /**
     * @brief 回读指定位置的 ODT Entry（SET_DAQ_PTR + READ_DAQ）
     * @param daq_list DAQ List 号（EPK）
     * @param odt_number ODT 号（0 基）
     * @param odt_entry ODT Entry 号（0 基）
     * @return 回读结果；Slave 回 ERR_CMD_UNKNOWN 时为 std::nullopt
     * @details B-6：PREDEFINED 列表的布局只能这样取证（A2L 顺序只作候选）。
     *          每次调用都显式 SET_DAQ_PTR，不依赖隐含指针的自增状态。
     */
    [[nodiscard]] std::optional<ReadDaqResponse> ReadDaqEntryAt(
        std::uint16_t daq_list, std::uint8_t odt_number,
        std::uint8_t odt_entry);

private:
    std::unique_ptr<IXcpTransport> m_transport_;  ///< 拥有的 Transport
    Session m_session_;                           ///< 会话状态与协商参数
    std::unique_ptr<CommandExecutor>
        m_executor_;  ///< 命令执行器（同时是包监听器）
    std::unique_ptr<MemoryAccess> m_memory_access_;  ///< 内存访问
    /**
     * @brief 本端 WRITE_DAQ 账本（批次14，T14-08）
     * @details 按 (daq_list, odt, entry) 下发顺序追加；ConfigureDaqList 失败
     *          时回滚，ClearDaqList/重连时删除对应项。B-6 要求解码只用本账本，
     *          不用 A2L 的 PREDEFINED 顺序。
     */
    std::vector<DaqLedgerEntry> m_daq_ledger_;
    /**
     * @brief 本会话 GET_DAQ_PROCESSOR_INFO 的一次性取证缓存
     * @details `std::nullopt` 表示 Slave 对 Optional 命令返回 CMD_UNKNOWN，
     *          不能据此猜测静态/动态能力；布尔值区分“尚未查询”和“已知不支持”。
     */
    bool m_daq_processor_info_queried_{false};
    std::optional<std::uint8_t> m_daq_processor_properties_;
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_XCP_MASTER_HPP_
