/**
 * @file memory_access.hpp
 * @brief 高层内存读取：SHORT_UPLOAD 优先，必要时降级 SET_MTA+UPLOAD 分块。
 *
 * 依据 code-plan/XCP_1.3.0_详细设计_类接口与头文件.md 第 13 节实现。
 */

#ifndef CALMCAR_XCP_MEMORY_ACCESS_HPP_
#define CALMCAR_XCP_MEMORY_ACCESS_HPP_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "libxcp/command_executor.hpp"
#include "libxcp/session.hpp"

namespace calmcar::xcp {

/**
 * @brief 标定数据写后校验的结论
 */
enum class CalibrationVerificationStatus : std::uint8_t {
    Match,          ///< 已确认写入内容与期望一致
    Mismatch,       ///< 已确认写入内容与期望**不**一致
    Indeterminate   ///< 无法取得可信证据（既不能确认也不能否认）
};

/**
 * @brief 校验所用的手段
 */
enum class CalibrationVerificationMethod : std::uint8_t {
    Readback,       ///< SET_MTA + UPLOAD 读回后与期望字节逐字节比较
    SlaveChecksum   ///< BUILD_CHECKSUM 由 Slave 计算，本地复算后比较
};

/**
 * @brief VerifyBytes 的结果
 * @details `Indeterminate` 不是失败也不是成功，而是“没有证据”：调用方不得
 *          把它当成 Match 继续，也不应该当成 Mismatch 触发重写（重写可能
 *          覆盖并发修改）。
 */
struct CalibrationVerificationResult {
    /// @brief 结论
    CalibrationVerificationStatus status{CalibrationVerificationStatus::Indeterminate};
    /// @brief 实际采用的手段
    CalibrationVerificationMethod method{CalibrationVerificationMethod::Readback};
    /// @brief SlaveChecksum 路径下的算法类型（Readback 路径为空）
    std::optional<CalibrationChecksumType> checksum_type;
    /// @brief Slave 回报的校验和（仅 SlaveChecksum 路径有效）
    std::uint32_t slave_checksum{0};
    /// @brief 本地复算的校验和（仅 SlaveChecksum 且本地可复算时有效）
    std::uint32_t local_checksum{0};
};

/**
 * @brief 请求把标定数据保存到非易失存储的结果
 * @details 依据规范（原始 PDF §7.5.2.13 SET_REQUEST）：正响应没有参数表，
 *          只表示 Slave 已**接受**请求；真正完成由 GET_STATUS 中
 *          STORE_CAL_REQ 位被清除表达。因此本结构区分"已接受未完成"与
 *          "已确认完成"。
 */
enum class CalibrationStoreStatus : std::uint8_t {
    Completed,  ///< GET_STATUS 中 STORE_CAL_REQ 已清除：Slave 声明本次保存完成
    Pending,    ///< 请求已被接受，但观察期内请求位仍置位（可能仍在写入）
    Unknown     ///< 无法观察（通信失败/应答丢失），保存是否发生不可知
};

/**
 * @brief RequestCalibrationStore 的结果
 */
struct CalibrationStoreResult {
    /// @brief 结论
    CalibrationStoreStatus status{CalibrationStoreStatus::Pending};
    /// @brief SET_REQUEST 是否已被 Slave 接受（Unknown 时可能为 true）
    bool request_accepted{false};
    /// @brief 实际执行的 GET_STATUS 轮询次数
    std::size_t polls{0};
};

/**
 * @brief 内存访问接口
 *
 * 封装轮询读取逻辑：优先 SHORT_UPLOAD，必要时降级到 SET_MTA+UPLOAD；
 * 自动处理 AG 换算、分块和地址溢出检查。
 *
 * @par 长度上限（原始 PDF §7.5.1.12 UPLOAD / §7.5.1.13 SHORT_UPLOAD）
 *   UPLOAD:       1 <= NumberOfElements <= MAX_CTO / AG - 1
 *   SHORT_UPLOAD: 1 <= NumberOfElements <= MAX_CTO / AG - 1
 * @note 两条上限数值相同但依据不同（各自的 CTO 语义独立），实现中分别独立
 *       计算，不互相派生；共同点是响应结构 [PID][n×AG] 必须为 PID 留出
 *       1 字节，故上限都是 MAX_CTO/AG - 1。
 */
class MemoryAccess {
public:
    /**
     * @brief 构造
     * @param executor 命令执行器（非拥有）
     * @param session Session（非拥有，用于获取 AG/MAX_CTO/降级标记）
     */
    MemoryAccess(CommandExecutor& executor, Session& session);

    /**
     * @brief 读取内存（以元素为单位）
     * @param address 32 位地址
     * @param extension 地址扩展
     * @param element_count 元素数（以 AG 为单位）
     * @return 读取到的原始字节
     * @throws XcpException(InvalidArgument) 参数非法或地址溢出
     * @throws XcpException 协议错误或超时
     */
    [[nodiscard]] Bytes ReadElements(Address address,
                                     AddressExtension extension,
                                     ElementCount element_count);

    /// @brief 读取内存（以字节为单位，内部换算为元素数后走 ReadElements）
    /// @param address 32 位地址
    /// @param extension 地址扩展
    /// @param byte_count 字节数，必须是 AG 的整数倍
    /// @return 读取到的原始字节
    /// @throws XcpException(InvalidArgument) 字节数为 0 或不能被 AG 整除
    /// @throws XcpException 协议错误或超时
    [[nodiscard]] Bytes ReadBytes(Address address, AddressExtension extension,
                                  ByteCount byte_count);

    /// @brief 用 SHORT_UPLOAD 一次性读取（不改动 Slave 隐含 MTA）
    /// @param address 32 位起始地址
    /// @param extension 地址扩展
    /// @param element_count 元素数（以 AG 为单位）
    /// @return 读取到的原始字节
    /// @throws XcpException(InvalidArgument) 参数非法或超过单包上限 MAX_CTO/AG
    [[nodiscard]] Bytes ShortUpload(Address address, AddressExtension extension,
                                    ElementCount element_count);

    /// @brief 用 SET_MTA + UPLOAD 分块读取任意长度
    /// @details 每块元素数取 MaxUploadElements()；会推进 Slave 的隐含 MTA，
    ///          属共享状态，因此按单 Outstanding Command 模型串行使用。
    /// @param address 32 位起始地址
    /// @param extension 地址扩展
    /// @param element_count 总元素数（以 AG 为单位）
    /// @return 各块拼接后的完整字节
    /// @throws XcpException(InvalidArgument) 参数非法，或 MAX_CTO/AG
    /// 过小无法分块
    [[nodiscard]] Bytes UploadChunked(Address address,
                                      AddressExtension extension,
                                      ElementCount element_count);

    // ---- 批次14：写回（T14-06，R9）----

    /**
     * @brief 写入内存（以字节为单位，内部按 AG 派生元素数后编排 DOWNLOAD）
     * @param address 32 位起始地址
     * @param extension 地址扩展
     * @param data 待写入字节；长度必须是 AG 的整数倍
     * @details 路径与读取侧严格对称：能一帧装下且 Slave 支持时走
     *          SHORT_DOWNLOAD；Slave 回 ERR_CMD_UNKNOWN 或数据装不下一帧时，
     *          回落 SET_MTA + DOWNLOAD 分块。**写回不做静默截断**：任一块
     *          失败即抛出带"已完成 x/y 元素"上下文的异常，调用方必须把
     *          整个写操作视为未完成（docs L1985 的原子性只对单块成立）。
     * @throws XcpException(InvalidArgument) 数据为空、长度不能被 AG 整除、
     *         地址溢出，或 MAX_CTO 过小无法分块
     * @throws XcpException 协议错误（含 ERR_WRITE_PROTECTED）/超时/恢复失败
     * @note 与读取侧不同，这里**不**提供"按元素数"的第二重载：写入的元素数
     *       由数据长度唯一决定（bytes = elements × AG），再加一个同名不同参
     *       的入口只会制造歧义（登记见 code-plan 批次14 记录）。
     */
    void WriteBytes(Address address, AddressExtension extension,
                    BytesView data);

    /**
     * @brief 用 SET_MTA + DOWNLOAD 分块写入任意长度
     * @details 每块元素数取 MaxDownloadElements()；每块前都显式 SET_MTA，
     *          因此不依赖 Slave 的 MTA 自增语义（超时恢复后重放也更安全）。
     * @param address 32 位起始地址
     * @param extension 地址扩展
     * @param data 待写字节（长度必须是 AG 整数倍，由 WriteBytes 前置校验）
     * @throws XcpException 同 WriteBytes
     */
    void DownloadChunked(Address address, AddressExtension extension,
                         BytesView data);

    // ---- 变量标定批次：MODIFY_BITS（docs §7.5.2.5）----

    /**
     * @brief 对指定地址的 32-bit 位置执行原子 AND/XOR 位修改
     * @param address 32 位起始地址（须 AG 对齐；MODIFY_BITS 操作对象为
     *        MTA 指向的 32-bit 位置，故先 SET_MTA 再 MODIFY_BITS）
     * @param extension 地址扩展
     * @param shift Shift Value S（0..16，Codec 层预检）
     * @param and_mask AND Mask（16-bit，零扩展后左移 S 清位）
     * @param xor_mask XOR Mask（同上，置位/翻转）
     * @details Slave 端计算 `Result = (Value & (AND<<S)) ^ (XOR<<S)`；
     *          执行后 MTA 不变。SET_MTA 与 MODIFY_BITS 之间有隐含状态依赖，
     *          超时恢复由 CommandExecutor 重放 m_last_mta_（见其 ModifyBits
     *          分支）。
     * @throws XcpException(InvalidArgument) 未连接、地址溢出或 Shift 越界
     * @throws XcpException 协议错误/超时/恢复失败
     */
    void ModifyBits(Address address, AddressExtension extension,
                    std::uint8_t shift, std::uint16_t and_mask,
                    std::uint16_t xor_mask);

    // ---- B1：标定闭环（写后校验 + 持久化请求）----

    /**
     * @brief 校验指定地址已写入的内容是否与期望字节一致
     * @param address 32 位起始地址
     * @param extension 地址扩展
     * @param expected 期望的原始字节；**不得为空**，且长度必须是 AG 的整数倍
     * @return 校验结论；`status == Indeterminate` 表示没有取得证据
     * @details 优先用 BUILD_CHECKSUM（Slave 计算、本地复算后比较，避免把整段
     *          标定数据搬回主机）；当 Slave 不支持该命令、或回报的算法本地
     *          无法复现（`ComputeXcpChecksum` 返回空）时，退化为 SET_MTA +
     *          UPLOAD 读回逐字节比较。读回过程中发生通信异常时返回
     *          `Indeterminate` 而不是抛出——校验的语义就是"能否确认"，
     *          确认不了本身就是结论，调用方据此决定是否重试。
     * @throws XcpException(InvalidArgument) 参数非法（空、非 AG 整数倍、
     *         地址溢出）或未连接
     * @note 本方法**只**验证主机发起的这次写入在 Slave 内存中的可见结果，
     *       不构成"已保存到非易失存储"的证明；持久化要用
     *       `RequestCalibrationStore`。
     */
    [[nodiscard]] CalibrationVerificationResult VerifyBytes(
        Address address, AddressExtension extension, BytesView expected);

    /**
     * @brief 请求 Slave 把标定数据保存到非易失存储，并观察其完成
     * @param completion_timeout 等待完成的观察上限，必须大于 0
     * @return 保存请求的观察结论
     * @details 发送 SET_REQUEST（Mode 位 `STORE_CAL_REQ`）后轮询 GET_STATUS，
     *          直到对应位被清除（`Completed`）或观察超时（`Pending`）。
     *          `EV_STORE_CAL` 事件只作为"请核实"的提示，不作为完成证据——
     *          事件可能丢失，且收到事件不等于数据已经落盘。
     * @throws XcpException(InvalidArgument) `completion_timeout <= 0` 或未连接
     * @throws XcpException(UnsupportedFeature) Slave 回 ERR_CMD_UNKNOWN：
     *         规范没有可等效的自动持久化后备，必须明确报告
     * @throws XcpException 其他协议错误（如不支持该 mode 时
     *         ERR_OUT_OF_RANGE）或发送失败
     * @note 观察期内通信失败时返回 `Unknown`（不是抛出）：请求可能已经生效，
     *       调用方重发可能造成重复的持久化动作。
     */
    [[nodiscard]] CalibrationStoreResult RequestCalibrationStore(
        std::chrono::milliseconds completion_timeout);

    /**
     * @brief 显式启用/停用块传输写入
     * @param enabled true 表示允许 WriteBytes 在能力齐备时编排 DOWNLOAD /
     *        DOWNLOAD_NEXT 块序列
     * @details **默认关闭**。块序列在 Slice 侧依赖 Slave 逐帧自增 MTA，且
     *          中间帧没有应答：一旦丢帧，后续数据会静默落到错误地址，主站
     *          无法从协议层察觉。逐块 SET_MTA 虽然多一次往返，但每块都重新
     *          落地址，超时恢复后可安全重试。只有在确认对端 Slave 的块实现
     *          可信、且写入量足够大（往返开销成为瓶颈）时才应开启。
     * @note 仅在启用后仍会检查 Slave 协商结果（SLAVE_BLOCK_MODE 与 MAX_BS）；
     *       协商不满足时仍然退回逐块通路，不会因开启而强行走块序列。
     */
    void SetBlockModeEnabled(bool enabled);

private:
    /// @brief 命令执行器（非拥有）
    CommandExecutor& m_executor_;
    /// @brief 会话（非拥有），提供 MAX_CTO / AG / SHORT_UPLOAD 可用性
    Session& m_session_;
    /// @brief 是否允许块传输写入（默认关闭，见 SetBlockModeEnabled）
    bool m_block_mode_enabled_{false};

    /// @brief 是否可走 SHORT_UPLOAD：能力可用且元素数不超过单包上限
    [[nodiscard]] bool CanUseShortUpload(ElementCount element_count) const;
    /// @brief UPLOAD 单块最大元素数（= MAX_CTO/AG - 1，为 PID 留出 1 字节）
    [[nodiscard]] ElementCount MaxUploadElements() const;
    /// @brief SHORT_UPLOAD 单包最大元素数（= MAX_CTO/AG - 1，原始 PDF §7.5.1.13）
    /// @note 与 MaxUploadElements 数值相同但各自独立计算，不得互相派生
    [[nodiscard]] ElementCount MaxShortUploadElements() const;

    /**
     * @brief DOWNLOAD 单块最大元素数（= (MAX_CTO - 2) / AG）
     * @details CTO 头占 [CMD][SIZE] 两字节（xcp.h:578-581）。docs L2010-2012
     *          的 "MAX_CTO/AG - 1" 说的是**无长度字段**的 DOWNLOAD_MAX，本库
     *          不使用该 Optional 命令，因此按 DOWNLOAD 自身布局取整。
     */
    [[nodiscard]] ElementCount MaxDownloadElements() const;

    /**
     * @brief 是否可走块传输（DOWNLOAD / DOWNLOAD_NEXT 序列）
     * @details 需要同时满足：调用方已显式启用块传输、已取得
     *          GET_COMM_MODE_INFO 且其 `COMM_MODE_OPTIONAL` 置位
     *          MASTER_BLOCK_MODE(bit0)、MAX_BS >= 1。任一条件缺失即视为
     *          "不能确认可块传"，退回逐块 SET_MTA + DOWNLOAD。
     *
     *          **不使用 CONNECT 的 SLAVE_BLOCK_MODE(bit6)**：该位描述
     *          Slave→Master 上传块，与 Master→Slave 下载块不是同一能力，
     *          用它会同时造成"支持下载块却被降级"与"仅支持上传却下发续帧"。
     *
     *          **默认不启用**的理由：块序列依赖 Slave 逐帧自增 MTA，且中间
     *          帧没有应答——丢一帧就会静默把后续数据写错位置，而逐块
     *          SET_MTA 每块都重新落地址，代价是多一次往返但可恢复。因此
     *          块传输只在调用方明确承担该风险时开启，不作为默认路径。
     * @return true 表示可编排块序列
     */
    [[nodiscard]] bool CanUseBlockMode() const;

    /**
     * @brief 用块传输序列写入（DOWNLOAD 首帧 + DOWNLOAD_NEXT 续帧/末帧）
     * @param address 32 位起始地址
     * @param extension 地址扩展
     * @param data 待写字节（长度必须是 AG 整数倍）
     * @param element_count 由 data 派生的元素数
     * @details 规范（原始 PDF §7.5.2.9 / §7.5.2.10）：
     *          - 首帧 DOWNLOAD 的 size 字段是**本块总元素数**，携带首帧数据；
     *          - 续帧 DOWNLOAD_NEXT 的 size 字段是**剩余元素数**（含本帧）；
     *          - 块内只有**末帧**被肯定应答，中间帧无 RESPONSE；
     *          - 块内 MTA 由 Slave 逐帧自增，不得重放 SET_MTA（会回到块首）。
     *          块长度上限 = `min(255, MAX_BS × 单帧元素数)`。
     * @throws XcpException 中间帧发送失败时立即中断：已发送的部分必须视为
     *         "已写入但未经确认"，**不得**改用逐帧 DOWNLOAD 重发同一段数据
     */
    void DownloadBlock(Address address, AddressExtension extension,
                       BytesView data, ElementCount element_count);

    /**
     * @brief SHORT_DOWNLOAD 单包最大元素数（= (MAX_CTO - 8) / AG，docs L2024）
     * @details MAX_CTO <= 8 时返回 0——报文头已占满一帧，无处放数据
     *          （docs L2026），必须走 SET_MTA + DOWNLOAD。
     */
    [[nodiscard]] ElementCount MaxShortDownloadElements() const;

    /// @brief 校验读取参数（已连接、元素数非 0、地址不溢出）
    /// @throws XcpException(InvalidArgument)
    void ValidateRead(Address address, ElementCount element_count) const;

    /**
     * @brief 校验写入参数（已连接、数据非空、长度可被 AG 整除、地址不溢出）
     * @param address 起始地址
     * @param data 待写字节
     * @return 由数据长度派生的元素数（bytes / AG）
     * @throws XcpException(InvalidArgument) 任一条件不满足
     */
    [[nodiscard]] ElementCount ValidateWrite(Address address,
                                             BytesView data) const;
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_MEMORY_ACCESS_HPP_
