/**
 * @file command_executor.hpp
 * @brief 命令执行器：编码 -> 发送 -> 等待响应 -> 解析 -> 超时恢复。
 *
 * 依据 code-plan/XCP_1.3.0_详细设计_类接口与头文件.md 第 12 节实现。
 * 实现 IPacketListener，回调在 Transport 工作线程执行。
 */

#ifndef CALMCAR_XCP_COMMAND_EXECUTOR_HPP_
#define CALMCAR_XCP_COMMAND_EXECUTOR_HPP_

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "libxcp/command_codec.hpp"
#include "libxcp/ixcp_transport.hpp"
#include "libxcp/response_parser.hpp"
#include "libxcp/session.hpp"
#include "libxcp/xcp_error.hpp"

namespace calmcar::xcp {

/**
 * @brief 命令超时配置
 *
 * 数值由调用方提供；XCP 的 t1..t7 通常来自 A2L，本阶段不宣称协议固定值
 * （计划文档 §11.8）。默认值为项目保守取值。
 */
struct CommandTimeouts {
    /// @brief 普通命令超时（毫秒）
    std::chrono::milliseconds command_timeout{1000};

    /// @brief SYNCH 恢复超时（毫秒）
    std::chrono::milliseconds synch_timeout{1000};

    /// @brief 最大恢复重试次数（不含首次尝试，设计决策 D2）
    int max_retries{2};
};

/**
 * @brief 错误恢复策略参数（F-11，方案 §4.6 的冻结默认值）
 *
 * @details 恢复策略不继续扩展现有 `CommandTimeouts`，而是由本结构集中表达，
 *          以区分「单命令超时/重试」与「BUSY 等待/总体 deadline」两类语义。
 *
 * @par 冻结默认值（方案 §4.6）
 * @verbatim
 *          timeout_retry_limit                = 2
 *          busy_retry_delay                   = 100 ms
 *          busy_total_timeout                 = 10 s
 *          seed_key_transaction_restart_limit = 1
 * @endverbatim
 *
 * @warning 上述数值是**项目工程默认值**，不得表述为 XCP V1.5 的固定常量
 *          （方案 §4.6 第 8 条）。调用方显式值或来自可信 A2L 的 t7 必须
 *          保留来源并参与诊断。
 */
struct RecoveryPolicy {
    /// @brief `ERR_CMD_BUSY` 的默认等待间隔 t7（方案 §4.6 第 3 条）
    /// @details 有可信 A2L/调用方 t7 时覆盖本默认值。
    std::chrono::milliseconds busy_retry_delay{100};

    /// @brief 顶层操作的总体 deadline（方案 §4.6 第 3 条）
    /// @details 自顶层调用**首次发送前**开始计时，覆盖全部等待、重发与响应
    ///          时间；**重试后不得重新计时**（与 `EV_CMD_PENDING` 重启单轮
    ///          响应计时不同）。
    std::chrono::milliseconds busy_total_timeout{10000};

    /// @brief `ERR_CMD_BUSY` 是否为可恢复错误而重试
    /// @details 置 false 可关闭 BUSY 重试（退回旧行为），供调用方在已知对端
    ///          不实现 t7 等待语义时使用。默认为 true。
    bool busy_retry_enabled{true};

    /// @brief 校验参数合法性
    /// @return 合法返回空字符串；否则返回中文原因
    /// @details 方案 §4.6 第 7 条：负数、零值、溢出及 `retry_limit < 0` 须在
    ///          构造或 Connect 前失败。本方法不做抛出，便于调用方汇总诊断。
    [[nodiscard]] std::string Validate() const;
};

/**
 * @brief 事件观察者接口（可选，用于上层接收异步 EV/SERV/DTO）
 *
 * 回调在 Transport 工作线程执行；实现者不得在其中阻塞等待命令响应，
 * 否则会与单 Outstanding Command 模型自锁。
 */
class IEventListener {
public:
    virtual ~IEventListener() = default;

    /// @brief 收到异步 Event
    virtual void OnEvent(const EventPacket& event) = 0;

    /// @brief 收到异步 Service Request
    virtual void OnService(const ServicePacket& service) = 0;

    /**
     * @brief 收到 DTO
     * @details 批次14（R12 定稿）：`dto.data` 是**完整 DTO 帧**（`data[0]`
     *          即 PID），可直接喂给 `IDaqLayout::Decode(frame_layout, data)`；
     *          本层仍不解析内容（envelope 与净荷的解释属桥接层 DAQ 解码）。
     */
    virtual void OnDto(const DtoPacket& dto) = 0;
};

/**
 * @brief 命令执行器
 *
 * 负责命令的发送-等待-解析-恢复。Standard Communication Model：同一时刻最多
 * 一个 Pending Command。Codec/Parser 在 CONNECT 协商出字节序后按该字节序创建。
 *
 * @par 死锁规避（设计 §16.3）
 *   OnPacketReceived() 在工作线程执行，先在锁内取出响应/事件，释放锁后再回调
 *   IEventListener；不在持锁状态下回调外部监听器。
 */
class CommandExecutor : public IPacketListener {
public:
    /**
     * @brief 构造执行器
     * @param transport Transport 实例（非拥有）
     * @param session Session 实例（非拥有）
     * @param timeouts 超时配置
     * @param event_listener 事件监听器（可选，可为 nullptr）
     */
    explicit CommandExecutor(IXcpTransport& transport, Session& session,
                             CommandTimeouts timeouts = {},
                             IEventListener* event_listener = nullptr,
                             RecoveryPolicy recovery_policy = {});

    /// @brief 析构；唤醒并终止内部等待
    ~CommandExecutor() override;

    // 禁止拷贝
    CommandExecutor(const CommandExecutor&) = delete;
    CommandExecutor& operator=(const CommandExecutor&) = delete;

    /**
     * @brief 取消当前挂起的操作（F-11）
     * @details 线程安全。唤醒所有等待者（响应等待、SYNCH 确认、BUSY 等待），
     *          清理 pending 状态，并让被取消的调用返回
     *          `ErrorCategory::Cancelled`。重复调用幂等；无挂起操作时为空操作。
     *
     * @par 与 `repeat ∞ times` 的关系
     *      规范恢复矩阵中 `ERR_CMD_BUSY → wait t7 → repeat ∞ times` 表示协议
     *      语义上持续等待资源，但库实现**必须**允许调用方取消，不能形成不可
     *      中断的同步调用（方案 §4.6 第 6 条）。
     */
    void CancelPendingOperation();

    /// @brief 当前恢复策略（供诊断与测试）
    [[nodiscard]] RecoveryPolicy GetRecoveryPolicy() const;

    /// @brief Transport 收到一个完整 XCP Packet 时的回调
    /// @details 在 Transport 工作线程执行；按单 Outstanding Command 模型把响应
    ///          放入槽位并唤醒等待者。EV/SERV/DTO 走事件分派，不当作命令响应。
    void OnPacketReceived(BytesView packet) override;

    /// @brief Transport 关闭时的回调：置关闭标记、记录原因并唤醒全部等待者
    void OnTransportClosed(std::string_view reason) override;

    /// @brief Transport 诊断告警（缺包/重复/乱序等）的接收点，不改变命令结果
    void OnTransportWarning(std::string_view message) override;

    /// @brief 以 IPacketListener 接口引用暴露自身
    /// @details 供 XcpMaster 在打开 Transport
    /// 时登记监听器，避免调用点自行转型。
    /// @return 指向自身 IPacketListener 子对象的引用
    [[nodiscard]] IPacketListener& AsListener() noexcept;

    /**
     * @brief 通知执行器底层 Transport 已重新打开，复位通道级关闭状态
     *
     * @details 修复「一次 Disconnect 后再也无法 Connect」的既有缺陷：
     *          `Disconnect()` 会关闭 Transport，从而由 `OnTransportClosed()`
     *          把 `m_transport_closed_` 置位；而 `XcpMaster::Connect()` 重新
     *          `Open()` 时没有任何地方把它清回 false。后果是重连后第一条
     *          命令的 `WaitForResponse` 立刻看到"通道已关闭"而提前返回，
     *          CONNECT 随之超时，恢复又因会话处于 `Connecting` 而抛
     *          `InvalidState`——即重连恒定失败。
     *
     *          本方法只复位**通道级**状态（关闭标志、原因文本、迟到响应槽位、
     *          同步/恢复标志），不触碰会话状态——会话由 `XcpMaster` 与
     *          `Session` 各自管理，执行器不得越权修改。
     *
     * @note 必须在 `Transport::Open()` 之后立即调用；由 `XcpMaster::Connect()`
     *       负责，不依赖调用方自觉。
     * @warning 不得在仍有命令在途时调用：复位会丢弃迟到响应，属新会话语义。
     */
    void OnTransportReopened() noexcept;

    /**
     * @brief 查询底层 Transport 是否已被观察到关闭
     * @return true 表示自上次成功打开以来 Transport 已关闭
     * @details 供上层诊断"命令为何立即失败"，也便于测试直接断言复位效果。
     */
    [[nodiscard]] bool IsTransportClosed() const noexcept;

    // ---- 命令执行 ----

    /**
     * @brief 执行 CONNECT 命令并按响应字节序建立 Codec/Parser
     * @param mode 0x00=普通, 0x01=用户自定义
     * @throws XcpException 超时、协议错误、参数非法或恢复失败
     */
    [[nodiscard]] ConnectResponse ExecuteConnect(std::uint8_t mode = 0x00);

    /// @brief 执行 DISCONNECT 命令，并把 Session 状态推进到已断开
    /// @throws XcpException 超时、协议错误或恢复失败
    void ExecuteDisconnect();

    /// @brief 执行 GET_STATUS 命令
    /// @return GET_STATUS 响应解析结果
    /// @throws XcpException 超时、协议错误或恢复失败
    [[nodiscard]] GetStatusResponse ExecuteGetStatus();

    /// @brief 执行 GET_COMM_MODE_INFO 命令（仅在 CONNECT
    /// 表明可选信息可用时调用）
    /// @return 解析结果；Slave 返回 ERR_CMD_UNKNOWN 时按替代路径降级并返回
    /// std::nullopt
    /// @throws XcpException 超时、其他协议错误或恢复失败
    [[nodiscard]] std::optional<GetCommModeInfoResponse>
    ExecuteGetCommModeInfo();

    /// @brief 执行 SET_MTA 命令，设置 Slave 的隐含内存地址
    /// @param extension 地址扩展（8 位）
    /// @param address 32 位地址
    /// @throws XcpException 超时、协议错误或恢复失败
    void ExecuteSetMta(AddressExtension extension, Address address);

    /// @brief 执行 UPLOAD 命令，从当前隐含 MTA 读取指定元素数
    /// @param number_of_elements 元素数（按 AG 换算字节数）
    /// @return 读取到的原始字节
    /// @throws XcpException 超时、协议错误、响应长度不符或恢复失败
    [[nodiscard]] Bytes ExecuteUpload(ElementCount number_of_elements);

    /// @brief 执行 GET_ID 命令（批次 21 21-2；XCPlite 实然方言：byte1=IDT，
    ///        无规范 §7.5.1.6 的 MODE/reserved 字节）
    /// @param identification_type IDT 编号（4=ASAM_UPLOAD：对端 A2L 文件）
    /// @return 解析结果（MODE / LENGTH / 响应内 DATA）
    /// @throws XcpException 超时、协议错误或恢复失败
    [[nodiscard]] GetIdResponse ExecuteGetId(std::uint8_t identification_type);

    /// @brief GET_VERSION 探测结果分类（批次 B1）
    ///
    /// @details 命名的用意是把「命令成功但版本不可用」与「命令本身失败」严格
    ///          分开，使版本裁决器不必解析错误文本。
    enum class GetVersionOutcome {
        /// @brief 成功：响应携带完整 PL/TL Major+Minor
        Available,

        /// @brief 对端未实现该 optional 命令（`ERR_CMD_UNKNOWN`）
        /// @details 原始 PDF 第 106 页 §7.4："An attempt to execute a not
        ///          implemented optional command will return ERR_CMD_UNKNOWN
        ///          and does not have any effect."
        UnimplementedCommand,

        /// @brief 对端认识 `0xC0` 族但不认识 `0x00` 子命令（`ERR_SUBCMD_UNKNOWN`）
        /// @details 与上者同为「不可用」。注意：本条与上条的**区分机制属推论**
        ///          （PDF 无逐字对应句），实施上把两者同等对待是保守且安全的
        ///          做法（方案 §1.4 证据强度标注）。
        UnimplementedSubcommand,

        /// @brief 对端不响应（超时）
        /// @details **不得**据此推断对端为老版本（方案 §5 批次 B1：
        ///          「不得以 timeout 冒充老版本」）。仅表示版本未确认。
        TimedOut,
    };

    /// @brief GET_VERSION 探测返回（结果分类 + 可选数据）
    struct GetVersionProbe {
        /// @brief 结果分类
        GetVersionOutcome outcome{GetVersionOutcome::TimedOut};

        /// @brief 仅在 `outcome == Available` 时有值
        std::optional<GetVersionResponse> response;

        /// @brief 该结果是否表示"命令成功且版本可用"
        [[nodiscard]] bool IsAvailable() const noexcept {
            return outcome == GetVersionOutcome::Available &&
                   response.has_value();
        }
    };

    /**
     * @brief 执行 GET_VERSION 命令（批次 B1，Level-1 双字节命令码）
     * @return 探测结果（**不抛异常表示命令不可用**，只抛真正的传输/恢复故障）
     *
     * @details 三种「不可用」情形（未实现命令、未实现子命令、超时）都以
     *          `GetVersionProbe` 的正常返回值表达，**不作为异常上抛**。这是本
     *          方法与其余 `Execute*` 的刻意差异：GET_VERSION 是 optional 命令
     *          且其"不可用"是**预期内的正常结果**（原始 PDF 第 106 页：规范
     *          设计该机制正是 "This lets the master device detect not
     *          implemented optional commands easily."），把它做成异常会让
     *          调用方用 catch 表达正常控制流。
     *
     * @throws XcpException 真正的传输故障、恢复失败或畸形响应（非上述三类）
     */
    [[nodiscard]] GetVersionProbe ExecuteGetVersion();

    /// @brief 执行 SHORT_UPLOAD 命令（一次命令完成定址读取，**会**改动 MTA）
    /// @param number_of_elements 元素数
    /// @param extension 地址扩展（8 位）
    /// @param address 32 位地址
    /// @return 读取到的原始字节
    /// @throws XcpException 超时、协议错误、响应长度不符或恢复失败
    /// @note F-09：规范要求成功后 MTA 指向「上传数据块之后的第一个数据字节」
    ///       （原始 PDF §7.5.1.8），本方法会同步本端隐含 MTA。
    [[nodiscard]] Bytes ExecuteShortUpload(ElementCount number_of_elements,
                                           AddressExtension extension,
                                           Address address);

    /**
     * @brief 执行 GET_SEED 命令（读取解锁 Seed 的指定分段，批次 7）
     * @param resource 要解锁的资源（协议要求恰为单个资源位）
     * @param mode First=首段（length 为 Seed 总长）；Remainder=续取后续分段
     * @return 响应解析结果（Length 字段 + 本帧 Seed 分段）
     * @throws XcpException 超时、协议错误（Slave 侧 ERR_OUT_OF_RANGE /
     *         ERR_SEQUENCE）或恢复失败
     */
    [[nodiscard]] GetSeedResponse ExecuteGetSeed(Resource resource,
                                                 SeedMode mode);

    /**
     * @brief 执行 UNLOCK 命令（发送 Key 的一个分段，批次 7）
     * @param length_field Length 字段：首帧填 Key 总长度，后续帧填剩余长度
     * @param key_segment 本帧携带的 Key 字节
     * @param is_final_segment 本帧是否为序列末段（F-10：决定超时恢复策略，
     *        末段超时属结果未知，禁止自动重放）
     * @return 响应解析结果（Current Resource Protection Status）
     * @throws XcpException 超时、协议错误或恢复失败
     * @note Key 校验失败时 Slave 返回 ERR_ACCESS_LOCKED 并主动断开会话
     *       （规范 §7.5.1.9）；本方法捕获该错误并将 Session 标记为 Failed
     *       后原样上抛，调用方需重新 Connect() 才能继续。
     */
    [[nodiscard]] UnlockResponse ExecuteUnlock(std::uint8_t length_field,
                                               BytesView key_segment,
                                               bool is_final_segment);

    /// @brief 发送 SYNCH 并等待 ERR_CMD_SYNCH 确认（超时恢复路径的第一步）
    /// @return true 表示 Slave 已用 ERR_CMD_SYNCH 确认；false 表示未获确认
    bool SendSynch();

    /**
     * @brief 把分段 Seed&Key 事务整体重置（F-10）
     * @details 清除段位置与重启计数。由事务编排层（XcpMaster::Unlock）
     *          在事务完成或放弃时调用，避免分段状态泄漏到后续调用。
     *          注意：恢复路径内部只清段位置、**保留**重启计数（见
     *          ClearSeedKeySegment），否则重启预算永远无法累积。
     */
    void ResetSeedKeyTransaction();

    /**
     * @brief 只清除分段 Seed&Key 的段位置，保留重启计数（F-10）
     * @details 供恢复路径内部使用：段位置必须立刻失效（防止误重放），
     *          但重启预算要在整个事务期间累积，故不可一并清零。
     */
    void ClearSeedKeySegment();

    /**
     * @brief 隐含 DAQ 指针的本端记录（批次14，T14-05）
     * @details XCP 的 DAQ 指针**不可查询**（docs L2172「写过最后一个 Entry
     *          后 Pointer 值未定义」），所以本端只能记住最近一次成功
     *          SET_DAQ_PTR 的三元组，用于 SYNCH 恢复后重放。
     * @note WRITE_DAQ/READ_DAQ 会让 Slave 侧指针自增，而本端记录**不自增**：
     *       编排层（XcpMaster 的 DAQ 配置）对每个 Entry 都显式发一次
     *       SET_DAQ_PTR，从而彻底避开"指针越过 ODT 末位后未定义"的陷阱
     *       （docs L2172 要求跨 ODT/DAQ List 必须重新 SET_DAQ_PTR）。
     */
    struct DaqPointer {
        std::uint16_t daq_list{0};   ///< DAQ List 号（EPK）
        std::uint8_t odt_number{0};  ///< ODT 号（0 基）
        std::uint8_t odt_entry{0};   ///< ODT Entry 号（0 基）
    };

    // ---- 批次14：DAQ 命令组（T14-05）----
    //
    // 与既有命令完全同一条执行链（RunCommand：单 Outstanding → 超时 →
    // SYNCH 恢复 → 重试），不新增事务循环；差异只在于**隐含 DAQ 指针**
    // 也需要在恢复后重建（docs L2783「WRITE_DAQ 前重新 SET_DAQ_PTR」）。

    /**
     * @brief 执行 SET_DAQ_PTR（docs L2141-2153；Mandatory）
     * @param daq_list DAQ List 号（EPK）
     * @param odt_number ODT 号（0 基）
     * @param odt_entry_number ODT Entry 号（0 基）
     * @details 成功后记录为当前隐含指针，供 WRITE_DAQ/READ_DAQ 超时恢复时
     *          重放（docs L2172 指针不可查询、L2783 恢复要求）。
     * @throws XcpException 超时、协议错误（指定对象不存在 → ERR_OUT_OF_RANGE）
     *         或恢复失败
     */
    void ExecuteSetDaqPtr(std::uint16_t daq_list, std::uint8_t odt_number,
                          std::uint8_t odt_entry_number);

    /**
     * @brief 执行 WRITE_DAQ（docs L2155-2172；Mandatory）
     * @param bit_offset 位偏；无位偏填 kDaqBitOffsetNone(0xFF)
     * @param size 本 Entry 元素数（以 AG 为单位）
     * @param extension 地址扩展
     * @param address 32 位地址
     * @details 成功后 Slave 的隐含指针在同 ODT 内自动前移一位；本层同步推进
     *          记录值，**超过 0xFF 或写过 ODT 末位后指针不可知**（docs
     * L2172）， 因此记录被清空，调用方必须重新 SET_DAQ_PTR 才能继续跨 ODT
     * 配置。
     * @throws XcpException 超时、协议错误（PREDEFINED 列表 →
     * ERR_WRITE_PROTECTED） 或恢复失败
     */
    void ExecuteWriteDaq(std::uint8_t bit_offset, std::uint8_t size,
                         AddressExtension extension, Address address);

    /**
     * @brief 执行 CLEAR_DAQ_LIST（docs L2431-2444；Static Mandatory）
     * @param daq_list DAQ List 号（EPK）
     * @details 无论 PREDEFINED 还是 configurable 列表，正在运行的数据传输都会
     *          停止（docs L2444）；因此本命令**清空隐含指针记录**，后续
     *          WRITE_DAQ 必须显式 SET_DAQ_PTR。
     * @throws XcpException 超时、协议错误或恢复失败
     */
    void ExecuteClearDaqList(std::uint16_t daq_list);

    // ---- 批次20：动态 DAQ 分配命令组（FREE/ALLOC 三件套；XCPlite 实然
    //      见 xcplite.c:2562-2591，响应均为纯 RES）----

    /**
     * @brief 执行 FREE_DAQ（释放 Slave 侧全部 DAQ 资源）
     * @details 动态配置的第一步：一次性作废 LIST/ODT/ENTRY 全部对象；
     *          隐含指针记录随全表失效一并清空（口径与 ExecuteClearDaqList
     *          一致，只是作用域是全表）。运行中调用 → CRC_DAQ_ACTIVE
     *          （xcplite.c:2567），编排层必须先停。
     * @throws XcpException 超时、协议错误或恢复失败
     */
    void ExecuteFreeDaq();

    /**
     * @brief 执行 ALLOC_DAQ（动态分配 count 个 DAQ List）
     * @param count 本次分配的 List 数（>=1；XCPlite 侧 0 → CRC_OUT_OF_RANGE）
     * @details XcpAllocDaq 要求既有 odt/entry 计数为零（xcplite.c:1148），
     *          故一次编排必须**一次给足总数**，不得在 ODT 分配后追加。
     * @throws XcpException 超时、协议错误（CRC_SEQUENCE/CRC_DAQ_ACTIVE）
     *         或恢复失败
     */
    void ExecuteAllocDaq(std::uint16_t count);

    /**
     * @brief 执行 ALLOC_ODT（为指定 List 追加 count 个 ODT）
     * @details 顺序硬门：任何 ALLOC_ODT_ENTRY 之前必须完成**全表所有 List**
     *          的 ODT 分配（XcpAllocOdt 要求 odt_entry_count==0，
     *          xcplite.c:1176）——由编排层保证，本方法不越权替 Slave 校验。
     * @throws XcpException 超时、协议错误或恢复失败
     */
    void ExecuteAllocOdt(std::uint16_t daq_list, std::uint8_t count);

    /**
     * @brief 执行 ALLOC_ODT_ENTRY（为指定 ODT 分配 count 个 Entry 槽）
     * @details 分配出的槽位 size=0、地址为空，仍需 WRITE_DAQ 填充后方可用。
     * @throws XcpException 超时、协议错误或恢复失败
     */
    void ExecuteAllocOdtEntry(std::uint16_t daq_list, std::uint8_t odt_number,
                              std::uint8_t count);

    /**
     * @brief 执行 SET_DAQ_LIST_MODE（docs L2174-2204；Mandatory）
     * @param mode MODE 位组合
     * @param daq_list DAQ List 号（EPK）
     * @param event_channel 事件通道号（0 = 不由该通道触发）
     * @param prescaler 降频因子（1 = 不降频）
     * @param priority 优先级（0xFF 最高）
     * @throws XcpException 超时、协议错误（如 ALTERNATING+TIMESTAMP 非法组合
     *         → ERR_CMD_SYNTAX）或恢复失败
     */
    void ExecuteSetDaqListMode(DaqListModeBit mode, std::uint16_t daq_list,
                               std::uint16_t event_channel,
                               std::uint8_t prescaler, std::uint8_t priority);

    /**
     * @brief 执行 START_STOP_DAQ_LIST（docs L2206-2228；Mandatory）
     * @param action Stop/Start/Select
     * @param daq_list DAQ List 号（EPK）
     * @return 响应解析结果（含 FIRST_PID；Stop 时 Slave 也回该字段，语义按
     *         docs L2222 仅在 Absolute ODT Number 模式下有用）
     * @throws XcpException 超时、协议错误或恢复失败
     */
    [[nodiscard]] StartStopDaqListResponse ExecuteStartStopDaqList(
        DaqListAction action, std::uint16_t daq_list);

    /**
     * @brief 执行 START_STOP_SYNCH（docs L2230-2242；Mandatory）
     * @param action StopAll/StartSelected/StopSelected
     * @throws XcpException 超时、协议错误或恢复失败
     */
    void ExecuteStartStopSynch(DaqSynchAction action);

    /**
     * @brief 执行 GET_DAQ_LIST_INFO（docs L2446-2465；**Optional**）
     * @param daq_list DAQ List 号（EPK）
     * @return 解析结果；Slave 回 ERR_CMD_UNKNOWN 时返回 std::nullopt
     *         （docs L1631：未实现的可选命令必回 ERR_CMD_UNKNOWN 且无副作用）
     * @throws XcpException 超时、其他协议错误或恢复失败
     */
    [[nodiscard]] std::optional<GetDaqListInfoResponse> ExecuteGetDaqListInfo(
        std::uint16_t daq_list);

    /**
     * @brief 执行 GET_DAQ_EVENT_INFO（v0.3；**Optional**，XCPlite 实然
     *        xcp.h:816-825：CRO 4 字节 = [D7][事件通道 WORD]，RES 六字段）
     * @param event_channel 事件通道号（uint16_t）
     * @return 解析结果；Slave 回 ERR_CMD_UNKNOWN 时返回 std::nullopt
     *         （docs L1631：未实现的可选命令必回 ERR_CMD_UNKNOWN 且无副作用）
     * @details 事件信息用于 v0.4 MeasurementPlanner 的 DAQ List 分配与
     *          time_cycle/time_unit/priority 取证；事件通道号**不在响应中
     *          回显**（调用方持有入参）。
     * @throws XcpException 超时、其他协议错误或恢复失败
     */
    [[nodiscard]] std::optional<GetDaqEventInfoResponse> ExecuteGetDaqEventInfo(
        std::uint16_t event_channel);

    /**
     * @brief 执行 GET_DAQ_PROCESSOR_INFO（docs L2285-2311；**Optional**）
     * @return 解析结果；Slave 回 ERR_CMD_UNKNOWN 时返回 std::nullopt
     * @details B-16 的 identification_field_type / address_extension_mode
     *          两类比对只能以本响应的 DAQ_KEY_BYTE 为运行时真值（docs L1445），
     *          因此桥接层 `RuntimeXcpParams` 的 DAQ 字段来自这里。
     * @throws XcpException 超时、其他协议错误或恢复失败
     */
    [[nodiscard]] std::optional<GetDaqProcessorInfoResponse>
    ExecuteGetDaqProcessorInfo();

    /**
     * @brief 执行 GET_DAQ_RESOLUTION_INFO（docs L2340-2364；**Optional**）
     * @return 解析结果；Slave 回 ERR_CMD_UNKNOWN 时返回 std::nullopt
     * @throws XcpException 超时、其他协议错误或恢复失败
     */
    [[nodiscard]] std::optional<GetDaqResolutionInfoResponse>
    ExecuteGetDaqResolutionInfo();

    /**
     * @brief 执行 READ_DAQ（docs L2257-2261；**Optional**）
     * @details 读当前隐含 DAQ 指针处的 Entry 并自动前移；PREDEFINED 与
     *          configurable 列表都可读（docs L2261）——这是 B-6 实际账本在
     *          PREDEFINED 侧唯一可取证的通路。
     * @return 解析结果；Slave 回 ERR_CMD_UNKNOWN 时返回 std::nullopt
     * @throws XcpException 超时、其他协议错误或恢复失败
     */
    [[nodiscard]] std::optional<ReadDaqResponse> ExecuteReadDaq();

    // ---- 批次14：写回（T14-06 的底层通道）----

    /**
     * @brief 执行 DOWNLOAD（docs L1979-1987；Mandatory，CAL/PAG 可用时）
     * @param number_of_elements 本帧元素数（以 AG 为单位）
     * @param data 本帧数据（字节数必须 = 元素数 × AG，由 MemoryAccess 保证）
     * @details 从当前隐含 MTA 起写入，完成后 Slave 自动前移 MTA；本层同步
     *          推进 MTA 记录，使分块写回在超时恢复后仍落在正确地址。
     * @throws XcpException 超时、协议错误（写保护 → ERR_WRITE_PROTECTED）
     *         或恢复失败
     */
    void ExecuteDownload(ElementCount number_of_elements, BytesView data);

    /**
     * @brief 执行 SHORT_DOWNLOAD（docs L2018-2026；**Optional**）
     * @param number_of_elements 元素数（以 AG 为单位）
     * @param extension 地址扩展
     * @param address 32 位地址
     * @param data 数据字节
     * @throws XcpException 超时、其他协议错误或恢复失败
     * @note Slave 回 ERR_CMD_UNKNOWN 不降级为"部分成功"——写回必须有确认，
     *       故本方法把 ERR_CMD_UNKNOWN 原样上抛，由 MemoryAccess 回落
     *       SET_MTA+DOWNLOAD 通路（与 SHORT_UPLOAD 的读降级不同）。
     */
    void ExecuteShortDownload(ElementCount number_of_elements,
                              AddressExtension extension, Address address,
                              BytesView data);

    // ---- 变量标定批次：MODIFY_BITS + Page Switching（docs §7.5.2.5/§7.5.3）----

    /**
     * @brief 执行 MODIFY_BITS（docs §7.5.2.5；**Optional**，需 CAL/PAG 权限）
     * @param shift Shift Value S（0..16，Codec 层预检）
     * @param and_mask AND Mask（16-bit，零扩展后左移 S 作用于 32-bit MTA 位置）
     * @param xor_mask XOR Mask（同上）
     * @details 命令本身不设地址——操作对象是当前隐含 MTA，故调用方
     *          （MemoryAccess::ModifyBits）必须先 SET_MTA；超时恢复时本层
     *          与 UPLOAD/DOWNLOAD 一样重放 m_last_mta_。
     * @throws XcpException 超时、协议错误（Shift 越界 Slave 回 ERR_OUT_OF_RANGE）
     */
    void ExecuteModifyBits(std::uint8_t shift, std::uint16_t and_mask,
                           std::uint16_t xor_mask);

    /**
     * @brief 执行 SET_CAL_PAGE（docs §7.5.3.1；**Optional**）
     * @param mode ECU/XCP/ALL 位组合（Codec 预检须含 ECU|XCP 之一）
     * @param segment Segment Number（mode 含 ALL 时被 Slave 忽略）
     * @param page Page Number
     * @throws XcpException 超时、ERR_MODE_NOT_VALID / ERR_PAGE_NOT_VALID /
     *         ERR_SEGMENT_NOT_VALID 等协议错误
     */
    void ExecuteSetCalPage(CalPageModeBit mode, std::uint8_t segment,
                           std::uint8_t page);

    /**
     * @brief 执行 GET_CAL_PAGE（docs §7.5.3.2；**Optional**）
     * @param access_mode 仅允许 Ecu(0x01)/Xcp(0x02)
     * @param segment Segment Number
     * @return 当前激活的逻辑 Page Number；Slave 回 ERR_CMD_UNKNOWN 时 nullopt
     * @throws XcpException 超时或其他协议错误
     */
    [[nodiscard]] std::optional<GetCalPageResponse> ExecuteGetCalPage(
        CalPageAccessMode access_mode, std::uint8_t segment);

    /**
     * @brief 执行 GET_PAG_PROCESSOR_INFO（docs §7.5.3.3；**Optional**）
     * @return MAX_SEGMENT/PAG_PROPERTIES；Slave 不支持 Paging 时 nullopt
     * @throws XcpException 超时或其他协议错误
     */
    [[nodiscard]] std::optional<GetPagProcessorInfoResponse>
    ExecuteGetPagProcessorInfo();

    /**
     * @brief 执行 GET_SEGMENT_INFO（docs §7.5.3.4；**Optional**，变长响应）
     * @param mode 决定响应布局（BasicInfo/StandardProperties/MappingInfo）
     * @param segment Segment Number
     * @param info Mode 0: Address/Length；Mode 2: SrcAddr/DstAddr/Length
     * @param mapping_index 仅 Mode 2 使用
     * @return 按 Mode 填充的 variant 响应；ERR_CMD_UNKNOWN 时 nullopt
     * @throws XcpException 超时或其他协议错误
     */
    [[nodiscard]] std::optional<GetSegmentInfoResponse> ExecuteGetSegmentInfo(
        SegmentInfoMode mode, std::uint8_t segment, SegmentInfoSelector info,
        std::uint8_t mapping_index = 0U);

    /**
     * @brief 执行 GET_PAGE_INFO（docs §7.5.3.5；**Optional**）
     * @param segment Segment Number
     * @param page Page Number
     * @return PAGE_PROPERTIES（已拆解三组访问者类型）+ INIT_SEGMENT；
     *         ERR_CMD_UNKNOWN 时 nullopt
     * @throws XcpException 超时或其他协议错误
     */
    [[nodiscard]] std::optional<GetPageInfoResponse> ExecuteGetPageInfo(
        std::uint8_t segment, std::uint8_t page);

    /**
     * @brief 执行 SET_SEGMENT_MODE（docs §7.5.3.6；**Optional**）
     * @param mode bit0 FREEZE（置 1 后该 Segment 进入随后 STORE_CAL_REQ
     *             的冻结处理范围）
     * @param segment Segment Number
     * @throws XcpException 超时或协议错误
     */
    void ExecuteSetSegmentMode(SegmentModeBit mode, std::uint8_t segment);

    /**
     * @brief 执行 GET_SEGMENT_MODE（docs §7.5.3.7；**Optional**）
     * @param segment Segment Number
     * @return 当前 Segment Mode（主要查 FREEZE）；ERR_CMD_UNKNOWN 时 nullopt
     * @throws XcpException 超时或其他协议错误
     */
    [[nodiscard]] std::optional<GetSegmentModeResponse> ExecuteGetSegmentMode(
        std::uint8_t segment);

    /**
     * @brief 执行 COPY_CAL_PAGE（docs §7.5.3.8；**Optional**）
     * @param request 源/目标 Segment+Page 四元组
     * @details 目标区域写保护（Flash）时 Slave 回 ERR_WRITE_PROTECTED，
     *          应改走 Flash Programming 流程（本计划外）。
     * @throws XcpException 超时或协议错误
     */
    void ExecuteCopyCalPage(const CopyCalPageRequest& request);
/// @brief 分段 Seed&Key 事务的段位置（F-10，隐含状态）
    /// @details 规范对「多段序列中途超时」未给出专门处理，而通用恢复路径
    ///          （SYNCH + 重放当前段）在该空白下会失步：某段超时后 Slave 侧
    ///          段偏移可能已推进或复位，重放本端认为的当前段会导致错位；
    ///          加之 Key 在序列完成后才校验（原始 PDF 第 130 页），错位
    ///          **不会立即报错**，属静默失步。
    ///          本枚举让 RunCommand 识别段位置，从而拒绝盲重放并按事务
    ///          语义分派恢复策略（见 CommandExecutor::RunSegmentedRecovery）。
    enum class SeedKeySegment {
        NotSegmented,  ///< 非分段 Seed&Key 命令（GET_SEED Mode=0 属此列）
        NonFinal,      ///< 多段序列的中间段：重放不安全，须完整重启事务
        Final          ///< 末段：结果未知，禁止自动重放
    };

private:
    /// @brief 单条命令的执行流程：状态检查 -> 发送 -> 等待 -> 错误分派 ->
    /// 恢复重试；MODIFY_BITS 超时不重放并报告 OperationOutcomeUnknown
    [[nodiscard]] ParsedPacket RunCommand(CommandCode cmd,
                                          const Bytes& encoded_packet);
    /// @brief 一次发送-等待尝试：占用 Outstanding Command 槽位后发送并等待
    /// @param cmd 命令码
    /// @param encoded_packet 已编码的完整 CTO Packet
    /// @return 收到的响应；nullopt 表示超时或 Transport 关闭
    /// @throws 发送阶段的 XcpException（抛出前会先释放命令槽位）
    [[nodiscard]] std::optional<ParsedPacket> PerformAttempt(
        CommandCode cmd, BytesView encoded_packet);

    /// @brief 等待响应槽位被填充
    /// @details 收到 EV_CMD_PENDING 时重启本轮超时计时，但不重发原命令（计划
    /// §6.3）；
    ///          Transport 关闭时立即返回。
    /// @param timeout 单次等待上限
    /// @return 最终响应；nullopt 表示超时或 Transport 已关闭
    [[nodiscard]] std::optional<ParsedPacket> WaitForResponse(
        std::chrono::milliseconds timeout);

    /// @brief 分派已收到的响应：负响应转成结构化异常，含降级与重试判定
    /// @details ERR_CMD_UNKNOWN 对 GET_COMM_MODE_INFO / SHORT_UPLOAD
    /// 走替代路径降级；
    ///          未知错误码保留原值上报 ProtocolError，不用 Generic 顶替。
    /// @param cmd 本次命令码
    /// @param response 已解析的响应
    /// @return 正响应原样返回
    /// @throws XcpException 负响应或需要终止的错误
    [[nodiscard]] ParsedPacket DispatchResponse(CommandCode cmd,
                                                const ParsedPacket& response);

    /// @brief 超时后执行 SYNCH 恢复；Transport 已关闭则直接判定恢复失败
    /// @param cmd 触发恢复的命令码
    /// @throws XcpException(RecoveryFailed) SYNCH 未获确认，或重试次数用尽
    void PerformRecovery(CommandCode cmd);

    /// @brief 恢复成功后重建 UPLOAD / DOWNLOAD 所依赖的隐含 MTA
    /// @details 与 RestoreDaqPtr 同属"隐含状态重放"，见上面 RunCommand 注释。
    void RestoreUploadMta();

    /**
     * @brief 分段 Seed&Key 事务的超时恢复（F-10）
     * @param cmd 触发恢复的命令码（GetSeed 或 Unlock）
     * @param segment 出事段的位置（NonFinal / Final）
     * @throws XcpException(OperationOutcomeUnknown) 末段超时（结果未知，不重放）
     * @throws XcpException(RecoveryFailed) 重启上限用尽或 SYNCH 失败
     * @details 规范对「多段序列中途超时」未给出专门处理，通用路径的
     *          「SYNCH + 重放当前段」会失步（方案 §2C.2 工程策略）：
     *          - GET_SEED 任一段：清本地分段状态 + SYNCH，成功后由事务层
     *            从 GET_SEED(Mode=0) 完整重启，最多 1 次；
     *          - UNLOCK 非末段：Slave 可能已缓存部分 Key，单独 SYNCH 不足以
     *            证明分段状态已复位，必须先断开重连再完整重启，最多 1 次；
     *          - UNLOCK 末段：资源可能已解锁，属结果未知，禁止自动重放，
     *            立即返回 OperationOutcomeUnknown 交由调用方决定。
     */
    void RunSegmentedRecovery(CommandCode cmd, SeedKeySegment segment);

    /**
     * @brief 设置当前 Seed&Key 命令的段位置（F-10）
     * @param segment 段位置；命令返回后须复位为 NotSegmented
     */
    void SetSeedKeySegment(SeedKeySegment segment);

    /// @brief 按 Session 字节序惰性创建/重建 Codec 与 Parser
    /// @param byte_order CONNECT 协商出的字节序
    /// @param ag Session 地址粒度。默认取 Session 当前值；只有 DOWNLOAD
    /// 家族（EncodeDownload 需按 AG 插对齐字节）关心它，但 Codec 是共享
    /// 实例，故一律按 AG 参与重建判定，避免同一 Codec 在不同 AG 间串用。
    void EnsureCodec(ByteOrder byte_order,
                     AddressGranularity ag = AddressGranularity::Byte);

    /**
     * @brief 恢复成功后重建 WRITE_DAQ/READ_DAQ 依赖的隐含 DAQ 指针
     * @details docs L2783 明确要求「WRITE_DAQ 前重新 SET_DAQ_PTR」；指针不可
     *          查询（docs L2172），只能重放本层记录的最近一次成功 SET_DAQ_PTR。
     *          未记录（含被 CLEAR_DAQ_LIST 作废）时直接返回。
     */
    void RestoreDaqPtr();

    /// @brief 查询取消标志（F-11）
    /// @return 已被调用方取消返回 true
    [[nodiscard]] bool IsCancelled() const;

    /// @brief 判断解析结果是否为 `ERR_CMD_BUSY` 负响应（F-11）
    /// @param response 已解析的 Packet
    /// @details 仅识别 `ERR_CMD_BUSY`。其他负响应（含未知错误码）不在此处理，
    ///          交由 `DispatchResponse` 维持既有结构化报错语义。
    [[nodiscard]] static bool IsBusyResponse(const ParsedPacket& response);

    /// @brief 在 `ERR_CMD_BUSY` 后等待 t7（F-11）
    /// @param delay 本次等待时长
    /// @return `true` 等待期满；`false` 期间被取消或 Transport 关闭
    /// @details 使用条件变量而非 `sleep`，使取消、断连与析构能立即唤醒。
    bool WaitBusyDelay(std::chrono::milliseconds delay);

    /// @brief 校验上传响应长度，并裁剪精确匹配的 XCPlite 对齐填充
    /// @param cmd 命令码（用于错误定位）
    /// @param res 正响应；仅当长度精确匹配已知对齐公式时裁剪尾部填充
    /// @param elements 请求的元素数
    /// @throws XcpException(MalformedPacket) 长度既非精确长度也非已知填充长度
    void CheckResLength(CommandCode cmd, PositiveResponse& res,
                        ElementCount elements);

    // ---- 依赖 ----
    IXcpTransport& m_transport_;        ///< 传输层（非拥有）
    Session& m_session_;                ///< 会话（非拥有）
    CommandTimeouts m_timeouts_;        ///< 超时与重试配置
    RecoveryPolicy m_recovery_policy_;  ///< 错误恢复策略（F-11）
    IEventListener* m_event_listener_;  ///< 事件监听器（非拥有，可空）

    std::unique_ptr<CommandCodec>
        m_codec_;  ///< 命令编码器（按 Session 字节序创建）
    std::unique_ptr<ResponseParser>
        m_parser_;  ///< 响应解析器（按 Session 字节序创建）

    // ---- 同步状态 ----
    mutable std::mutex m_mutex_;                      ///< 保护以下字段
    std::condition_variable m_response_cv_;           ///< 最终响应到达通知
    std::condition_variable m_synch_cv_;              ///< SYNCH 确认到达通知
    std::optional<ParsedPacket> m_pending_response_;  ///< 待取走的最终响应
    /// @brief 取消标志（F-11）
    /// @details 由 `CancelPendingOperation()`、Transport 关闭、Disconnect 与
    ///          析构置位；等待循环据此提前返回并抛出 `Cancelled`。下一次
    ///          顶层命令开始前由 `RunCommand` 清除。
    bool m_cancelled_{false};
    bool m_response_ready_{false};                    ///< 响应槽位已被填充
    bool m_synch_confirmed_{false};                   ///< 已收到 ERR_CMD_SYNCH
    bool m_in_recovery_{false};             ///< 当前正在等待 SYNCH 确认
    bool m_transport_closed_{false};        ///< Transport 已关闭
    std::string m_transport_close_reason_;  ///< 关闭原因
    bool m_cmd_pending_received_{
        false};  ///< 本轮收到 EV_CMD_PENDING，需重启计时
    std::optional<XcpAddress40>
        m_last_mta_;  ///< 最近一次成功 SET_MTA 的地址（隐含状态）
    /// @brief 最近一次成功 SET_DAQ_PTR 的指针三元组（批次14，隐含状态）
    /// @details WRITE_DAQ/READ_DAQ 超时恢复时重放本值；CLEAR_DAQ_LIST 与
    ///          CONNECT/DISCONNECT 会将其作废（置 nullopt）。
    std::optional<DaqPointer> m_last_daq_ptr_;

    /// @brief 当前 Seed&Key 分段事务的段位置（F-10）
    /// @details 由 ExecuteGetSeed / ExecuteUnlock 在执行前设置，命令返回后
    ///          复位。仅在分段事务进行中为非 NotSegmented。
    SeedKeySegment m_seed_key_segment_{SeedKeySegment::NotSegmented};

    /// @brief 分段 Seed&Key 事务的重启次数（F-10）
    /// @details 事务级重启上限，与单命令 `repeat 2 times` 语义不同，
    ///          不得混为一谈（方案 §2C.2）。
    int m_seed_key_restarts_{0};
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_COMMAND_EXECUTOR_HPP_
