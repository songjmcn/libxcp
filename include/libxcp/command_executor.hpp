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
                             IEventListener* event_listener = nullptr);

    /// @brief 析构；唤醒并终止内部等待
    ~CommandExecutor() override;

    // 禁止拷贝
    CommandExecutor(const CommandExecutor&) = delete;
    CommandExecutor& operator=(const CommandExecutor&) = delete;

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

    /// @brief 执行 SHORT_UPLOAD 命令（一次命令完成定址读取，不改动 MTA）
    /// @param number_of_elements 元素数
    /// @param extension 地址扩展（8 位）
    /// @param address 32 位地址
    /// @return 读取到的原始字节
    /// @throws XcpException 超时、协议错误、响应长度不符或恢复失败
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
     * @return 响应解析结果（Current Resource Protection Status）
     * @throws XcpException 超时、协议错误或恢复失败
     * @note Key 校验失败时 Slave 返回 ERR_ACCESS_LOCKED 并主动断开会话
     *       （规范 §7.5.1.9）；本方法捕获该错误并将 Session 标记为 Failed
     *       后原样上抛，调用方需重新 Connect() 才能继续。
     */
    [[nodiscard]] UnlockResponse ExecuteUnlock(std::uint8_t length_field,
                                               BytesView key_segment);

    /// @brief 发送 SYNCH 并等待 ERR_CMD_SYNCH 确认（超时恢复路径的第一步）
    /// @return true 表示 Slave 已用 ERR_CMD_SYNCH 确认；false 表示未获确认
    bool SendSynch();

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

private:
    /// @brief 单条命令的执行流程：状态检查 -> 发送 -> 等待 -> 错误分派 ->
    /// 恢复重试
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

    /// @brief 按 Session 字节序惰性创建/重建 Codec 与 Parser
    /// @param byte_order CONNECT 协商出的字节序
    void EnsureCodec(ByteOrder byte_order);

    /**
     * @brief 恢复成功后重建 WRITE_DAQ/READ_DAQ 依赖的隐含 DAQ 指针
     * @details docs L2783 明确要求「WRITE_DAQ 前重新 SET_DAQ_PTR」；指针不可
     *          查询（docs L2172），只能重放本层记录的最近一次成功 SET_DAQ_PTR。
     *          未记录（含被 CLEAR_DAQ_LIST 作废）时直接返回。
     */
    void RestoreDaqPtr();

    /// @brief 校验响应数据长度是否等于 elements * AG，不符判为畸形包
    /// @param cmd 命令码（用于错误定位）
    /// @param res 正响应
    /// @param elements 请求的元素数
    /// @throws XcpException(MalformedPacket) 长度与期望不符
    void CheckResLength(CommandCode cmd, const PositiveResponse& res,
                        ElementCount elements);

    // ---- 依赖 ----
    IXcpTransport& m_transport_;        ///< 传输层（非拥有）
    Session& m_session_;                ///< 会话（非拥有）
    CommandTimeouts m_timeouts_;        ///< 超时与重试配置
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
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_COMMAND_EXECUTOR_HPP_
