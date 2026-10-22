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
#include <string>
#include <vector>

#include "libxcp/command_executor.hpp"
#include "libxcp/ixcp_transport.hpp"
#include "libxcp/memory_access.hpp"
#include "libxcp/protocol_version.hpp"
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
 * @brief XCP Master 构造配置聚合（批次 B1 引入）
 *
 * @details 批次 B1 之前，`XcpMaster` 的构造参数是逐个位置参数
 *          （`CommandTimeouts`、`IEventListener*`、字节序、AG……）。随着版本
 *          策略与后续批次能力的加入，位置参数会持续膨胀且难以安全演进。
 *          本结构把它们聚合为**单一配置对象**，新增字段不再改变构造函数签名。
 *
 * @par 破坏性变更说明（版本 2.0.0）
 *      批次 B1 之前构造函数是**逐个位置参数**
 *      `XcpMaster(transport, timeouts, event_listener)`。本次改为单一
 *      `XcpMasterOptions`，后续批次新增能力（版本策略、警告回调等）不再反复
 *      改动签名。**旧的三参调用形式不再隐式可用**，须显式构造配置对象。
 *
 * @note 字节序与地址粒度**不来自本结构**：它们由对端 CONNECT 响应决定
 *       （`Session::GetByteOrder()` 取自 COMM_MODE_BASIC bit0，
 *       `Session::GetAddressGranularity()` 取自 CONNECT 的 AG 字段）。
 *       本结构中的同名字段仅在调用方需要**显式覆盖**探测结果时使用，
 *       默认值表示"遵循对端上报"。
 */
struct XcpMasterOptions {
    /// @brief 命令超时配置
    CommandTimeouts timeouts{};

    /// @brief 事件监听器（可选，**非拥有**；生命周期须长于 XcpMaster）
    IEventListener* event_listener{nullptr};

    /// @brief 线路字节序**覆盖值**（`std::nullopt` 表示遵循对端 CONNECT 上报）
    /// @details 正常场景保持 `nullopt`：规范中字节序是**对端宣告的属性**
    ///          （CONNECT 的 COMM_MODE_BASIC bit0），Master 不应自作主张。
    ///          仅在对端宣告有误、需强制纠正时使用。
    std::optional<ByteOrder> byte_order;

    /// @brief 地址粒度**覆盖值**（`std::nullopt` 表示遵循对端 CONNECT 上报）
    /// @details 规范原文（原始 PDF 第 112 页 Table 42）："The address
    ///          granularity indicates the size of an element contained at a
    ///          single address." 它同样是 CONNECT 协商结果，正常不覆盖。
    std::optional<AddressGranularity> address_granularity;

    /// @brief 版本策略（PL/TL 先验、是否自动探测 GET_VERSION）
    /// @details 默认全部未配置：库**不得自动猜测版本**（方案 §4.2）。
    VersionPolicy version_policy{};

    /// @brief 是否在 CONNECT 后自动调用 GET_VERSION 拉取精确版本
    /// @details 默认 true；探测失败（对端未实现/超时）**不构成连接失败**，
    ///          只让版本进入 `ConnectMajor` + `MajorOnly` 保守状态。
    bool autodetect_version{true};

    /// @brief 是否要求 GET_VERSION 必须成功，否则连接失败
    /// @details 默认 **false**。置 true 只应在"确知对端支持 V1.4+ 且必须
    ///          精确版本"的场景使用；更严的 `Require` 语义由后续批次引入。
    bool require_get_version{false};
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
     * @param options 构造配置聚合（超时、监听器、字节序、AG、版本策略）
     */
    explicit XcpMaster(std::unique_ptr<IXcpTransport> transport,
                       XcpMasterOptions options = {});

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

    /**
     * @brief 取消当前挂起的操作（F-11）
     * @details 线程安全，可从其他线程调用。唤醒正在等待响应的调用，使其以
     *          `ErrorCategory::Cancelled` 返回，而**不是** `Timeout` 或
     *          `ProtocolError`。
     *
     * @par 何时需要
     *      规范恢复矩阵为 `ERR_CMD_BUSY` 规定 `wait t7 → repeat ∞ times`。
     *      协议语义上这意味着持续等待资源，但库实现不得形成不可中断的同步
     *      调用（方案 §4.6 第 6 条），故提供本入口供调用方中断等待。
     *
     * @note 取消在等待期间生效；已完成的命令不受影响。`Disconnect`、
     *       `Transport` 关闭与析构同样会触发取消语义。下一次顶层命令开始前
     *       取消标志自动清除。
     */
    void CancelPendingOperation();

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
     * @brief 动态 DAQ 整表编排（批次20，T20-01b）：FREE_DAQ → ALLOC_DAQ →
     *        ALLOC_ODT → ALLOC_ODT_ENTRY → SET_DAQ_PTR/WRITE_DAQ →
     *        SET_DAQ_LIST_MODE
     * @param specs 整表快照：每个 DaqListSpec 一个 List；XCPlite 由 ALLOC_DAQ
     *        按分配序自增编号，daq_list 必须严格等于下标 0..N-1（docs
     *        §7.5.4.7：n=2 产生 List 0 与 1）
     * @details Slave 侧时序硬门（xcplite.c:1148/1176）：ALLOC_DAQ 一次性给出
     *          List 总数；全表 ODT 分配完才开始分 Entry；ALLOC 出的 Entry 槽
     *          size=0，必须再 WRITE_DAQ 填。FREE_DAQ 作用域是整张表：旧账本
     *          快照后全表重建，未出现在 specs 里的 List 一并删除、代际递增；
     *          失败则账本回滚到调用前快照（不留半份账，B-6）。
     * @note  只适用于声明 DAQ_PROPERTY_CONFIG_TYPE=DYNAMIC 的 Slave；SET_DAQ_
     *          LIST_MODE 位与 prescaler 按 spec 透传，不替 Slave 强制
     *          TIMESTAMP（XCPlite 缺 TIMESTAMP 位回 CRC_CMD_SYNTAX，错误如实
     *          上浮，B-16）。pid 不回填：本路径识别字段非 Absolute，动态启动
     *          请走 StartDaqSync()（D12：XCPlite 拒绝单列表 START(mode=1)）。
     * @throws XcpException(InvalidArgument) specs 空 / daq_list 非下标序 /
     *         spec 字段非法（同 ConfigureDaqList 口径）/ Slave 不支持
     *         动态分配 / 单 ODT 超 MAX_DTO（含时间戳信封预检，G3 修正）
     * @throws XcpException 协议错误、超时或恢复失败
     */
    void ConfigureDaqListsDynamic(const std::vector<DaqListSpec>& specs);
    void ConfigureDaqListsDynamic(const std::vector<DaqListSpec>& specs,
                                  std::size_t dto_identification_bytes);

    /**
     * @brief 动态列表的同步启动：逐 List Select → START_STOP_SYNCH(Start
     *        Selected)（批次20，T20-01b）
     * @details 绕开拒绝单列表 START(mode=1) 的 Slave（D12：xcplite.c:2668-
     *          2671 TEST_CHECKS 下回 CRC_MODE_NOT_VALID）。Select 响应的
     *          FIRST_PID 不回填账本（本路径识别字段非 Absolute，docs
     *          L2228）。运行态由 Session 登记（Disconnect 自动收尾）。
     * @throws XcpException(InvalidState) 账本中无任何 List
     * @throws XcpException 协议错误、超时或恢复失败
     */
    void StartDaqSync();

    /**
     * @brief 经 GET_ID(IDT_ASAM_UPLOAD=4) + 连续 UPLOAD 拉取对端 A2L 文件
     *        （批次21 21-2）
     * @details XCPlite 实然线格式：GET_ID CRO 只有 2 字节（byte1 直接是
     *          IDT，无规范 §7.5.1.6 的 MODE/reserved 字节，xcp.h:519-520）；
     *          响应 LENGTH 是 DWORD@b4..7。对端打开 run 目录里的
     *          <a2l名>.a2l 进入 FILE MTA 后是**纯顺序读**（xcpappl.c:
     *          546-596，无 fseek），必须按 LENGTH 严格连续分块 UPLOAD
     *          （单块 ≤ MAX_CTO-1 且 ≤ 255）；中途失败对端 closeFile，
     *          需重发 GET_ID 重开文件再整读。
     * @return A2L 文件的完整字节流（与盘上文件逐字节一致）
     * @throws XcpException(UnsupportedFeature) 对端上报长度 0（无上传
     *         能力/.a2l 缺失）或 MODE≠0（非 UPLOAD 通路）
     * @throws XcpException 协议错误、超时或恢复失败
     */
    [[nodiscard]] Bytes FetchA2lViaUpload();

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
     * @brief 查询单个事件通道的运行时信息（GET_DAQ_EVENT_INFO，v0.3，Optional）
     * @param event_channel 事件通道号（uint16_t）
     * @return 解析结果；Slave 回 ERR_CMD_UNKNOWN 时为 std::nullopt
     * @details 事件通道号**不在响应中回显**（调用方持有入参）。v0.4
     *          MeasurementPlanner 据此做 DAQ List 分配与 time_cycle/time_unit/
     *          priority 取证。事件名主体经 UPLOAD 通路按 MTA 读取，本方法只拿
     *          name_length 等六字段（见 GetDaqEventInfoResponse）。
     */
    [[nodiscard]] std::optional<GetDaqEventInfoResponse> QueryDaqEventInfo(
        std::uint16_t event_channel);

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

    // ---- 变量标定批次：Calibration / Page Switching（docs §7.5.2.5 / §7.5.3）----

    /**
     * @brief CONNECT 后 Slave 是否声明 CAL/PAG 资源（Resource bit0）
     * @return false 时以下全部标定 API 均本地拒绝（UnsupportedFeature），
     *         不发送命令
     */
    [[nodiscard]] bool HasCalPagResource() const;

    /**
     * @brief 本会话 GET_PAG_PROCESSOR_INFO 的一次性取证缓存
     * @details nullopt = Slave 不支持该 Optional 命令（ERR_CMD_UNKNOWN）或
     *          未声明 CAL/PAG 资源；区分"尚未查询"由内部布尔记录。
     *          MAX_SEGMENT/PAG_PROPERTIES 是 Page/Segment 预校验的唯一
     *          运行时真值，**不猜**（B-3 口径）。
     */
    [[nodiscard]] std::optional<GetPagProcessorInfoResponse>
    QueryPagProcessorInfo();

    /**
     * @brief 切换标定页（SET_CAL_PAGE）
     * @param mode ECU/XCP/ALL 位组合（须含 ECU|XCP 之一）
     * @param segment Segment Number（mode 含 ALL 时被 Slave 忽略）
     * @param page Page Number
     * @throws XcpException(UnsupportedFeature) Slave 未声明 CAL/PAG 资源
     * @throws XcpException(InvalidArgument) Mode 组合非法
     * @throws XcpException 协议错误（ERR_MODE_NOT_VALID/ERR_PAGE_NOT_VALID/
     *         ERR_SEGMENT_NOT_VALID/ERR_ACCESS_LOCKED）、超时或恢复失败
     */
    void SetCalPage(CalPageModeBit mode, std::uint8_t segment,
                    std::uint8_t page);

    /**
     * @brief 查询当前激活标定页（GET_CAL_PAGE）
     * @param access_mode Ecu(0x01)=Application 侧 / Xcp(0x02)=Master 侧
     * @param segment Segment Number
     * @return 逻辑 Page Number；Slave 回 ERR_CMD_UNKNOWN 时 nullopt
     * @throws XcpException(UnsupportedFeature) 未声明 CAL/PAG 资源
     * @throws XcpException 协议错误、超时或恢复失败
     */
    [[nodiscard]] std::optional<GetCalPageResponse> GetCalPage(
        CalPageAccessMode access_mode, std::uint8_t segment);

    /**
     * @brief 查询 Segment 信息（GET_SEGMENT_INFO，变长响应按 Mode 分支）
     * @param mode BasicInfo（地址/长度）/StandardProperties/MappingInfo
     * @param segment Segment Number
     * @param info Mode 0: Address/Length；Mode 2: SrcAddr/DstAddr/Length
     * @param mapping_index 仅 Mode 2 使用（0..MAX_MAPPING-1）
     * @return variant 响应；Slave 回 ERR_CMD_UNKNOWN 时 nullopt
     * @throws XcpException(UnsupportedFeature) 未声明 CAL/PAG 资源
     * @throws XcpException 协议错误、超时或恢复失败
     */
    [[nodiscard]] std::optional<GetSegmentInfoResponse> GetSegmentInfo(
        SegmentInfoMode mode, std::uint8_t segment, SegmentInfoSelector info,
        std::uint8_t mapping_index = 0U);

    /**
     * @brief 查询 Page 访问属性与初始化来源（GET_PAGE_INFO）
     * @param segment Segment Number
     * @param page Page Number
     * @return PAGE_PROPERTIES（三组访问者类型已拆解）+ INIT_SEGMENT；
     *         Slave 回 ERR_CMD_UNKNOWN 时 nullopt
     * @throws XcpException(UnsupportedFeature) 未声明 CAL/PAG 资源
     * @throws XcpException 协议错误、超时或恢复失败
     */
    [[nodiscard]] std::optional<GetPageInfoResponse> GetPageInfo(
        std::uint8_t segment, std::uint8_t page);

    /**
     * @brief 设置 Segment FREEZE 模式（SET_SEGMENT_MODE）
     * @param freeze true=冻结该 Segment（进入随后 STORE_CAL_REQ 的冻结
     *               处理范围）；false=取消冻结
     * @param segment Segment Number
     * @throws XcpException(UnsupportedFeature) 未声明 CAL/PAG 资源
     * @throws XcpException 协议错误、超时或恢复失败
     */
    void SetSegmentFreeze(bool freeze, std::uint8_t segment);

    /**
     * @brief 读取 Segment 当前 Mode（GET_SEGMENT_MODE，主要查 FREEZE）
     * @param segment Segment Number
     * @return Mode 位域；HasSegmentMode(*r, kFreeze) 判断冻结态；
     *         Slave 回 ERR_CMD_UNKNOWN 时 nullopt
     * @throws XcpException(UnsupportedFeature) 未声明 CAL/PAG 资源
     * @throws XcpException 协议错误、超时或恢复失败
     */
    [[nodiscard]] std::optional<GetSegmentModeResponse> GetSegmentMode(
        std::uint8_t segment);

    /**
     * @brief 复制标定页（COPY_CAL_PAGE）
     * @param request 源/目标 Segment+Page 四元组
     * @details 目标区域写保护（Flash）时 Slave 回 ERR_WRITE_PROTECTED，
     *          应改走 Flash Programming 流程（本计划外）。
     * @throws XcpException(UnsupportedFeature) 未声明 CAL/PAG 资源
     * @throws XcpException 协议错误、超时或恢复失败
     */
    void CopyCalPage(const CopyCalPageRequest& request);

    /**
     * @brief 对指定地址的 32-bit 位置执行原子 AND/XOR 位修改（MODIFY_BITS）
     * @param address 32 位地址（操作对象为 MTA 指向的 DWORD）
     * @param extension 地址扩展
     * @param shift Shift Value S（0..16）
     * @param and_mask AND Mask（16-bit，零扩展后左移 S 清位）
     * @param xor_mask XOR Mask（同上，翻转置位）
     * @details 内部编排 SET_MTA + MODIFY_BITS（docs §7.5.2.5：命令本身
     *          不带地址）；超时恢复自动重放 MTA。
     * @throws XcpException(UnsupportedFeature) 未声明 CAL/PAG 资源
     * @throws XcpException(InvalidArgument) Shift 越界/地址溢出
     * @throws XcpException 协议错误（含 ERR_ACCESS_LOCKED——需先 Unlock
     *         Resource::CalPag）、超时或恢复失败
     */
    void ModifyBits(Address address, AddressExtension extension,
                    std::uint8_t shift, std::uint16_t and_mask,
                    std::uint16_t xor_mask);

    /// @brief 测量会话（v0.5）需要读取时间戳宽度取证缓存用于规划与解码；
    ///        该口径不外扩为公共 API，故以 friend 授权（不改变其余可见性）。
    friend class MeasurementSession;

    // ---- 版本证据（批次 B1）----

    /**
     * @brief 协议层版本快照（含来源、置信度、生效值与冲突标志）
     * @return 不可变快照；未连接或无证据时 `evidence.source == Unknown`
     * @details 这是调用方判断"对端到底是什么协议版本"的**唯一合法入口**。
     *          `effective` 是能力裁决依据，`evidence.reported` 是对端原话；
     *          二者在封顶（`capped`）或保守（`conservative`）时会不同。
     * @note 本方法为只读查询，不触发任何命令交互。
     */
    [[nodiscard]] ProtocolVersionSnapshot GetProtocolVersionSnapshot() const;

    /**
     * @brief 传输层版本快照（含来源、置信度、生效值与冲突标志）
     * @return 不可变快照；未连接或无证据时 `evidence.source == Unknown`
     * @details 传输层与协议层是**独立维度**：传输层决定线路布局
     *          （Tail/Alignment/Packet 边界），不得由协议层版本推导。
     */
    [[nodiscard]] TransportVersionSnapshot GetTransportVersionSnapshot() const;

    /**
     * @brief 主动探测对端版本（GET_VERSION）
     * @return 探测结果分类与响应（对端未实现该 optional 命令属正常结果）
     * @throws XcpException 仅抛真正的传输故障或恢复失败，**不抛"命令不可用"**
     * @details 用于构造时关闭了自动探测、之后又想补测的场景。探测成功后
     *          会重新裁决双快照；失败则保留原证据不变（**不得降级为假定版本**）。
     */
    CommandExecutor::GetVersionProbe ProbeVersion();

    /**
     * @brief 版本裁决产生的冲突描述列表
     * @return 冲突文本；空表示无冲突
     * @details 用户仲裁（m03674）：**不提供静默模式**，冲突必须始终可被
     *          调用方观察到。典型冲突：GET_VERSION 与调用方配置不一致、
     *          GET_VERSION 与 A2L 声明不一致、对端版本高于支持上限。
     */
    [[nodiscard]] std::vector<std::string> VersionConflicts() const;

    // ---- 能力边界（批次 G：`1.x-conservative` 能力基线的对外表达）----

    /// @brief 本库在当前会话状态下明确"不支持"的能力项
    /// @details 对应方案 §4.2.1 冻结的 `Xcp13CoreBaseline` 能力表中
    ///          判为 **Block** 的分组。这些能力**不是"尚未查询"**，而是本
    ///          实现**确定不提供**：调用方据此避免发送注定失败的请求。
    /// @note 本枚举是"能力集"的一部分：Minor 未确认（`conservative`）时，
    ///       高版本特性一律归入 `Block`，**不得**依据对端返回的错误码反推
    ///       版本（方案 §4.3：禁止内容探测式尝试）。
    enum class UnsupportedCapability : std::uint8_t {
        /// SET_DAQ_PACKED_MODE（0xC0,0x01）/ GET_DAQ_PACKED_MODE（0xC0,0x02）
        /// —— 原始 PDF 确认为 V1.5 新特性（§4.1.10、Table 172/155 的
        /// DAQ_LIST_PROPERTIES/DAQ_EVENT_PROPERTIES Bit4 PACKED），
        /// 本实现未提供编码路径。
        DaqPackedMode = 0,

        /// 0xC0 族（Level-1 双字节命令码）中除 GET_VERSION（0xC0,0x00）外的
        /// 其它子命令 —— 本实现只登记并实现了 GET_VERSION。
        Level1OtherSubcommand = 1,

        /// IPv6 传输 —— 本版本基础 Profile 只承诺 **IPv4 UDP 单播**。
        Ipv6Transport = 2,

        /// 四个 Optional TL Subcommand —— 本版本基础 Profile 不实现。
        OptionalTlSubcommand = 3,
    };

    /// @brief 查询某项能力在本会话状态下是否**明确不支持**
    /// @param capability 待查询的能力项
    /// @return true 表示确定不支持（调用方应改走替代路径或放弃该功能）；
    ///         false 表示本项**不在此黑名单内**，但**不代表已实现或已验证**
    /// @details 返回 false 只说明"该能力未被本基线判为 Block"，调用方仍须
    ///          依赖对端能力查询结果与既有 API 的运行时校验。
    /// @note 与 `HasCalPagResource()` 的区别：后者反映**对端资源位**，
    ///       本方法反映**本库能力边界**，二者互不替代。
    [[nodiscard]] bool IsCapabilityUnsupported(
        UnsupportedCapability capability) const;

    /// @brief 若该能力明确不支持则抛 `UnsupportedFeature`，否则返回
    /// @param capability 待断言的能力项
    /// @throws XcpException(UnsupportedFeature) 当
    ///         `IsCapabilityUnsupported(capability)` 为真
    /// @details 供调用方在**发起注定失败的操作前**做前置断路，把"能力缺失"
    ///          与"对端拒绝/超时"区分开。异常消息包含能力名与替代路径提示，
    ///          便于日志定位。
    /// @note 这是**本地判定**，不发送任何报文。
    void ThrowIfUnsupported(UnsupportedCapability capability) const;

private:
    void ConfigureDaqListsDynamicImpl(const std::vector<DaqListSpec>& specs,
                                      std::size_t dto_identification_bytes,
                                      bool legacy_timestamp);

    /**
     * @brief CONNECT 之后的版本裁决编排（批次 B1）
     * @param connect 已完成的 CONNECT 响应（提供 PL/TL 主版本兜底证据）
     *
     * @details 流程：按 `m_options_` 决定是否探测 GET_VERSION → 收集
     *          CONNECT 主版本、探测结果、调用方配置、A2L 声明 → 交给
     *          `VersionResolver::Resolve` → 落盘双快照与冲突列表。
     *
     * @note **探测失败不抛异常**（除非 `require_get_version` 为真）：规范
     *       明文把"未实现 optional 命令"作为 GET_VERSION 的正常返回
     *       （原始 PDF 第 106 页 §7.4）。方案 B1 特别要求"不得以 timeout
     *       冒充老版本"——失败只导致 `MajorOnly` 保守状态。
     */
    void ResolveVersionAfterConnect(const ConnectResponse& connect);

    /**
     * @brief 用一次已完成的探测结果重新裁决版本快照（不发起任何通信）
     * @param probe 调用方已取得或已从缓存补取的 GET_VERSION 探测结果
     * @throws XcpException `require_get_version` 为真但探测不可用时抛
     *         `InvalidState`（语义与连接期裁决一致）
     *
     * @details 从 `ResolveVersionAfterConnect` 中抽出，使「连接期自动探测」
     *          与「连接后主动补测」共用同一套裁决与冲突记录逻辑，避免两处
     *          实现各写一遍而漂移。本方法只做本地裁决——探测动作由调用方
     *          负责发起（见 `ProbeVersion()`）。
     * @note 调用前必须处于已连接状态，否则 CONNECT 主版本兜底证据不可用。
     */
    void ApplyVersionProbe(const CommandExecutor::GetVersionProbe& probe);

    std::unique_ptr<IXcpTransport> m_transport_;  ///< 拥有的 Transport
    Session m_session_;                           ///< 会话状态与协商参数

    /// @brief 构造配置（批次 B1）：超时、监听器、字节序/AG 覆盖、版本策略
    /// @details 保存整个配置对象而非拆散字段，便于后续批次新增能力时
    ///          不改动成员布局；生命周期与本对象一致。
    XcpMasterOptions m_options_;

    /// @brief 协议层版本快照（批次 B1），由 VersionResolver 裁决后写入
    /// @details 断连时清空为默认值：版本证据是**会话态**，不得跨会话残留
    ///          （否则重连到不同版本对端会沿用旧裁决）。
    ProtocolVersionSnapshot m_protocol_version_;

    /// @brief 传输层版本快照（批次 B1），与协议层独立裁决
    TransportVersionSnapshot m_transport_version_;

    /// @brief 版本裁决产生的冲突描述（批次 B1）
    /// @details 只记录不阻断；调用方通过 `VersionConflicts()` 观察。
    std::vector<std::string> m_version_conflicts_;
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
    /**
     * @brief 本会话 GET_DAQ_PROCESSOR_INFO 的完整取证快照（批次 E2，F-05/O-2）
     * @details 批次 E2 起除 DAQ_PROPERTIES 外还需消费三项：
     *          - `min_daq`：F-05 要求可配置 List 编号区间为
     *            `[MIN_DAQ, MAX_DAQ-1]`，实现不得再硬性要求从 0 连续编号；
     *          - `key_byte`：O-2 要求拦下 `Address_Extension` 的规范禁止组合
     *            （bit5-4 == `10`，原始 PDF 第 182 页 Table 142 "Not allowed"）；
     *          - `properties` 的 bit7-6：O-2 要求 `OVERLOAD_MSB` 启用
     *            （`01`）时限制 ODT 编号严格小于 `0x7C`（原始 PDF 第 180 页）。
     *          `nullopt` 与 `m_daq_processor_properties_` 同义（未支持/未取证）。
     */
    std::optional<GetDaqProcessorInfoResponse> m_daq_processor_info_;

    /**
     * @brief 本会话 GET_DAQ_RESOLUTION_INFO 的完整取证快照（批次 E2，F-12）
     * @details 批次 E2 起除时间戳宽度外还需消费粒度与上限：
     *          `granularity_daq`、`max_odt_entry_size_daq` 用于 C1～C5 校验
     *          （原始 PDF 第 14-15 页 §4.1.2）；`timestamp_mode` 的
     *          `size_code`/`unit_code` 用于 Table 147/148 的合法性判定。
     *          nullopt = Slave 不支持该 Optional 命令，**不校验也不猜**（B-3）。
     */
    std::optional<GetDaqResolutionInfoResponse> m_daq_resolution_info_;

    /**
     * @brief 本会话 GET_DAQ_RESOLUTION_INFO 的一次性取证缓存（批次20，G3）
     * @details 仅供信封预检：时间戳宽度计入 header（XCPlite 每事件首 ODT
     *          带 4B 时间戳，此前按纯 PID 口径预检会放行超限配置）。
     *          nullopt = Slave 不支持该 Optional 命令或未声明宽度，按 0 计——
     *          **不猜**（B-3/B-16）。
     */
    bool m_daq_resolution_info_queried_{false};
    std::optional<std::uint8_t> m_daq_timestamp_bytes_;

    /**
     * @brief 本会话 GET_PAG_PROCESSOR_INFO 的一次性取证缓存（变量标定批次）
     * @details nullopt = Slave 不支持该 Optional 命令或未声明 CAL/PAG 资源；
     *          布尔值区分"尚未查询"和"已知不支持"（仿 DAQ 处理器信息缓存）。
     */
    bool m_pag_processor_info_queried_{false};
    std::optional<GetPagProcessorInfoResponse> m_pag_processor_info_;

    /**
     * @brief 懒查 GET_DAQ_RESOLUTION_INFO 并缓存 Slave 声明的时间戳字节宽
     *        （批次20，G3）。未声明/Optional 不支持 → 0——宽度**不猜**（B-3）。
     * @note 调用时机由信封预检决定：仅当 spec.timestamp 开启才查询，不给
     *       既有静态路径加命令；FIXED 时间戳 Slave 与 spec.timestamp=false
     *       的组合本就与 Slave 的 MODE 校验冲突，错误由 Slave 侧如实上浮。
     */
    [[nodiscard]] std::size_t DaqTimestampBytesCached();

    /**
     * @brief Slave 是否启用了 OVERLOAD_MSB 过载指示（批次 E2，O-2）
     * @details 判据为 GET_DAQ_PROCESSOR_INFO 响应的 `DAQ_PROPERTIES`
     *          bit7-6 == `01`（原始 PDF 第 179 页 Table 139）。
     *          启用后规范要求 ODT 编号 `0x00 <= N < 0x7C`
     *          （原始 PDF 第 180 页）。
     * @return true = 启用；未取证或未支持时返回 false——**不猜**（B-3）
     */
    [[nodiscard]] bool IsOverloadMsbEnabled() const;

    /// @brief 动态编排中途失败后的清理：整表 FREE_DAQ 尽力恢复到干净空表，
    ///        二次失败只尽力不掩盖原异常；账本/解码代际同步作废（批次20）
    void ClearDynamicTableBestEffort();

};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_XCP_MASTER_HPP_
