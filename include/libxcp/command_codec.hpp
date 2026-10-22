/**
 * @file command_codec.hpp
 * @brief XCP 命令编码器：高层参数 -> CTO Byte Sequence。
 *
 * 依据 code-plan/XCP_1.3.0_详细设计_类接口与头文件.md 第 9 节实现。
 * 无状态，纯函数风格；所有 reserved 字节填 0；多字节字段按 Session Byte Order
 * 编码。
 */

#ifndef CALMCAR_XCP_COMMAND_CODEC_HPP_
#define CALMCAR_XCP_COMMAND_CODEC_HPP_

#include <cstdint>

#include "libxcp/protocol_types.hpp"
#include "libxcp/xcp_error.hpp"

namespace calmcar::xcp {

// ---------------------------------------------------------------------------
// 批次14：DAQ 命令组与写回命令的字节布局权威依据
//
//   字段语义 / mandatory 标注：docs/XCP_1.3.0_document.md
//     命令码表 L1662-1686；SET_DAQ_PTR L2141-2153；WRITE_DAQ L2155-2172；
//     SET_DAQ_LIST_MODE L2174-2204；START_STOP_DAQ_LIST L2206-2228；
//     START_STOP_SYNCH L2230-2242；READ_DAQ L2257-2261；CLEAR_DAQ_LIST
//     L2431-2444；GET_DAQ_LIST_INFO L2446-2465；GET_DAQ_RESOLUTION_INFO
//     L2340-2364；DOWNLOAD 组 L1979-2026。
//   字节偏移：上述文档**没有 Position 字节表**（已实测），故偏移一律交叉
//     参考只读的 thirdparty/XCPlite/src/xcp.h（Vector 实现，XCP 1.4；
//     DAQ 组与 DOWNLOAD 组布局自 1.0 起稳定）：
//       CLEAR_DAQ_LIST   LEN 4   DAQ_LIST=WORD@2..3            xcp.h:684-686
//       SET_DAQ_PTR      LEN 6   reserved@1 DAQ=WORD@2..3
//                              ODT@4 ENTRY@5                    xcp.h:689-693
//       WRITE_DAQ        LEN 8   BITOFFSET@1 SIZE@2 EXT@3
//                              ADDR=DWORD@4..7                  xcp.h:696-701
//       SET_DAQ_LIST_MODE LEN 8  MODE@1 DAQ=WORD@2..3
//                              EVENT=WORD@4..5 PRESCALER@6
//                              PRIORITY@7                       xcp.h:713-719
//       START_STOP_DAQ_LIST LEN 4 MODE@1 DAQ=WORD@2..3          xcp.h:731-735
//       START_STOP_SYNCH   LEN 2  MODE@1                        xcp.h:738-740
//       READ_DAQ           LEN 1  无参（用隐含 DAQ 指针）
//                              RES LEN 8: BITOFFSET@1 SIZE@2
//                                   EXT@3 ADDR=DWORD@4..7       xcp.h:781-786
//       GET_DAQ_LIST_INFO  REQ LEN 4  DAQ=WORD@2..3
//                              RES LEN 6: PROPERTIES@1 MAX_ODT@2
//                                   MAX_ODT_ENTRY@3 FIXED_EVENT=WORD@4..5
//                                                               xcp.h:808-814
//       GET_DAQ_RESOLUTION_INFO RES LEN 8: GRANULARITY_DAQ@1
//                              MAX_SIZE_DAQ@2 GRANULARITY_STIM@3
//                              MAX_SIZE_STIM@4 TIMESTAMP_MODE@5
//                              TIMESTAMP_TICKS=WORD@6..7        xcp.h:799-805
//       DOWNLOAD           LEN 2+n  SIZE@1 DATA 起 @2           xcp.h:578-582
//                          **ASAM V1.5.0 Table 82：AG=DWORD 时须在
//                          [F0][SIZE] 后插 2 字节对齐，数据起 @4；
//                          XCPlite 无此概念（xcp.h:581 数据固定 @2），
//                          因其 AG 恒为 BYTE，AG>2 分支不触发，两者不冲突**
//       SHORT_DOWNLOAD     LEN 8+n  SIZE@1 EXT@3 ADDR=DWORD@4..7
//                              DATA 起 @8                       xcp.h:597-602
//     只可参考其 DAQ/DOWNLOAD 组：XCPlite 的 SET_MTA 是 XCP 1.4 的 8 字节
//     变体（xcp.h:549-551），与本库既有 7 字节实现（command_codec.cpp
//     EncodeSetMta + 测试 GoldenLengthsMatchMaxCtoEightLayout）不同，
//     **不得照抄**；SHORT_UPLOAD 家族同理，以本库既有布局为准。
// ---------------------------------------------------------------------------

/// @brief XCP 命令编码器
///
/// 将命令参数编码为 CTO Byte Sequence。编码使用当前 Session 的 Byte Order
/// （CONNECT 协商后确定）。所有 reserved 字节填 0。
class CommandCodec {
public:
    /// @brief 构造编码器
    /// @param byte_order Session 字节序（CONNECT
    /// 后确定），决定多字节字段编码方向
    /// @param ag 当前 Session 的地址粒度（AG）。DOWNLOAD 在 AG=DWORD 时
    /// 必须在数据段前插入 2 字节对齐（见 EncodeDownload 的 @note），
    /// 故编码器需持有该参数。
    explicit CommandCodec(ByteOrder byte_order,
                          AddressGranularity ag = AddressGranularity::Byte) noexcept;

    /// @brief 当前使用的字节序
    [[nodiscard]] ByteOrder GetByteOrder() const noexcept;

    /// @brief 当前使用的地址粒度
    [[nodiscard]] AddressGranularity GetAddressGranularity() const noexcept;

    // ---- 命令编码 ----

    /// @brief 编码 CONNECT 命令
    /// @param mode 0x00=普通, 0x01=用户自定义
    /// @return CTO: [0xFF][mode]
    [[nodiscard]] Bytes EncodeConnect(std::uint8_t mode = 0x00) const;

    /// @brief 编码 DISCONNECT 命令
    /// @return CTO: [0xFE][0x00]
    [[nodiscard]] Bytes EncodeDisconnect() const;

    /// @brief 编码 GET_STATUS 命令
    /// @return CTO: [0xFD][0x00]
    [[nodiscard]] Bytes EncodeGetStatus() const;

    /// @brief 编码 SYNCH 命令（超时恢复的第一步，Slave 以 ERR_CMD_SYNCH 确认）
    /// @return CTO: [0xFC][0x00]
    [[nodiscard]] Bytes EncodeSynch() const;

    /// @brief 编码 GET_COMM_MODE_INFO 命令
    /// @return CTO: [0xFB][0x00]
    [[nodiscard]] Bytes EncodeGetCommModeInfo() const;

    /**
     * @brief 编码 GET_VERSION 命令（Level-1 命令族的 GET_VERSION 子命令）
     * @return CTO: `[0xC0][0x00]`
     *
     * @par 规范依据（原始 PDF 第 114 页 Table 44「GET_VERSION command
     *      structure」，逐字）
     * @verbatim
     *      Position Type Description
     *      0        BYTE Command Code = 0xC0
     *      1        BYTE Level 1 Command Code = 0x00
     * @endverbatim
     *      共 **2 字节**，无保留字节、**无长度字段**。类别
     *      `Category Standard, optional`。
     *
     * @par Level-1 命令族（原始 PDF 第 106 页 §7.4 逐字）
     *      原文："Commands are defined as "Level 0" commands and "Level 1"
     *      commands; the first defined by a one byte command code
     *      (0xC1..0xFF), the latter defined by a **two byte command code
     *      starting with 0xC0**."
     *      故 GET_VERSION 是**双字节命令码**，第二个字节是**子命令码**，
     *      **不是参数或长度**——这一点是探测逻辑与错误码判定的基础。
     *
     * @par 为什么需要它
     *      CONNECT 响应**只返回协议层主版本字节**（Table 38，原始 PDF 第 109
     *      页；§3.1.1 原文 "The slave only returns the most significant byte"），
     *      因此**仅凭 CONNECT 无法获得 Minor**，也无法区分 V1.3/V1.4/V1.5。
     *      GET_VERSION 是该维度上唯一的运行时来源。
     *
     * @note 本命令为 optional：对端未实现时返回 `ERR_CMD_UNKNOWN(0x20)`；
     *       认识 `0xC0` 族但不认识 `0x00` 子命令时返回
     *       `ERR_SUBCMD_UNKNOWN(0x34)`。调用方必须把两者都视为"不可用"，
     *       **不得**据此推定任何具体 Minor。
     */
    [[nodiscard]] Bytes EncodeGetVersion() const;

    /// @brief 编码 SET_MTA 命令（设置 Slave 隐含内存地址）
    /// @param extension 地址扩展（8 位）
    /// @param address 32 位地址，按 Session 字节序编码
    /// @return CTO: [0xF6][reserved][extension][addr_b0..b3]
    [[nodiscard]] Bytes EncodeSetMta(AddressExtension extension,
                                     Address address) const;

    /// @brief 编码 UPLOAD 命令（从当前隐含 MTA 读取）
    /// @param number_of_elements 要读取的元素数（以 AG 为单位），1..0xFF
    /// @return CTO: [0xF5][number_of_elements]
    /// @throws XcpException(InvalidArgument) 元素数为 0 或超过单字段 0xFF 上限
    [[nodiscard]] Bytes EncodeUpload(ElementCount number_of_elements) const;

    /// @brief 编码 SHORT_UPLOAD 命令（一次命令完成定址读取）
    /// @param number_of_elements 要读取的元素数（以 AG 为单位），1..0xFF
    /// @param extension 地址扩展（8 位）
    /// @param address 32 位地址，按 Session 字节序编码
    /// @return CTO:
    /// @note SHORT_UPLOAD **会**改变 Slave 的 MTA：规范规定成功后 MTA 指向
    ///       「上传数据块之后的第一个数据字节」（原始 PDF §7.5.1.8）。
    ///       本函数只负责编码；同步本端隐含 MTA 由
    ///       CommandExecutor::ExecuteShortUpload 完成。
    /// [0xF4][number_of_elements][reserved][extension][addr_b0..b3]
    /// @throws XcpException(InvalidArgument) 元素数为 0 或超过单字段 0xFF 上限
    [[nodiscard]] Bytes EncodeShortUpload(ElementCount number_of_elements,
                                          AddressExtension extension,
                                          Address address) const;

    /// @brief 编码 GET_ID 命令（批次 21 21-2；XCPlite 实然线格式）
    /// @param identification_type IDT 编号（0=ASCII、1=ASAM_NAME、2=PATH、
    ///        3=URL、4=ASAM_UPLOAD 文件上传、5=EPK；xcp.h:248-258）
    /// @return CTO: [0xFA][identification_type]
    /// @note 实然优先：XCPlite 在 byte1 直接读 IDT（CRO_GET_ID_TYPE=
    ///       CRO_BYTE(1)，xcp.h:519-520），无规范 §7.5.1.6 的 MODE/reserved
    ///       布局；对接规范严格实现的对端时需另开通路（本批不做）。
    [[nodiscard]] Bytes EncodeGetId(std::uint8_t identification_type) const;

    /// @brief 编码 GET_SEED 命令（读取解锁 Seed 的指定分段）
    /// @param resource 要解锁的资源（协议要求恰为单个资源位）
    /// @param mode First=首段（响应含 Seed 总长度）；Remainder=续取后续分段
    /// @return CTO: [0xF8][mode][resource]
    /// @note 报文为 Mode 在前、Resource 在后；全部单字节字段，
    ///       与 Session Byte Order 无关。
    [[nodiscard]] Bytes EncodeGetSeed(Resource resource, SeedMode mode) const;

    /// @brief 编码 UNLOCK 命令（发送 Key 的一个分段）
    /// @param length_field Length 字段：首帧填 Key 总长度，后续帧填剩余长度
    /// @param key_segment 本帧携带的 Key 字节（分段上限 MAX_CTO-2
    /// 由编排层保证）
    /// @return CTO: [0xF7][length][key...]
    /// @throws XcpException(InvalidArgument) length_field 小于本帧字节数
    [[nodiscard]] Bytes EncodeUnlock(std::uint8_t length_field,
                                     BytesView key_segment) const;

    // ---- 批次14：DAQ 命令组与写回（字节偏移交叉参考只读
    //      thirdparty/XCPlite/src/xcp.h:684-814；字段语义取
    //      docs/XCP_1.3.0_document.md §7.5.4 各节）----

    /**
     * @brief 编码 CLEAR_DAQ_LIST 命令（docs L2431-2444）
     * @param daq_list DAQ List 号（EPK，16 位）
     * @return CTO: [0xE3][reserved][daq_lo][daq_hi]
     */
    [[nodiscard]] Bytes EncodeClearDaqList(std::uint16_t daq_list) const;

    /**
     * @brief 编码 SET_DAQ_PTR 命令（docs L2141-2153；Mandatory）
     * @param daq_list DAQ List 号（EPK，16 位）
     * @param odt_number ODT 号（0 基）
     * @param odt_entry_number ODT Entry 号（0 基）
     * @return CTO: [0xE2][reserved][daq_lo][daq_hi][odt][entry]
     * @note 该命令设定**隐含 DAQ 指针**；WRITE_DAQ 会自动前移指针，
     *       指针状态不可查询（docs L2172），故超时恢复必须重发本命令
     *       （docs L2783），见 CommandExecutor::ExecuteWriteDaq。
     */
    [[nodiscard]] Bytes EncodeSetDaqPtr(std::uint16_t daq_list,
                                        std::uint8_t odt_number,
                                        std::uint8_t odt_entry_number) const;

    /**
     * @brief 编码 WRITE_DAQ 命令（docs L2155-2172；Mandatory）
     * @param bit_offset 位偏；无位偏必须填 kDaqBitOffsetNone(0xFF)
     * @param size 本 Entry 的元素数（以 AG 为单位，1..0xFF）
     * @param extension 地址扩展（8 位）
     * @param address 32 位地址
     * @return CTO: [0xE1][bit_offset][size][extension][addr_b0..b3]
     * @throws XcpException(InvalidArgument) size 为 0 或超过 0xFF
     */
    [[nodiscard]] Bytes EncodeWriteDaq(std::uint8_t bit_offset,
                                       std::uint8_t size,
                                       AddressExtension extension,
                                       Address address) const;

    /**
     * @brief 编码 READ_DAQ 命令（docs L2257-2261；Optional）
     * @return CTO: [0xDB]
     * @note 无参数，读当前隐含 DAQ 指针处的 Entry，并自动前移指针。
     */
    [[nodiscard]] Bytes EncodeReadDaq() const;

    /**
     * @brief 编码 SET_DAQ_LIST_MODE 命令（docs L2174-2204；Mandatory）
     * @param mode MODE 位（DaqListModeBit 组合）
     * @param daq_list DAQ List 号（EPK）
     * @param event_channel 事件通道号（0 = 不由该通道触发）
     * @param prescaler 降频因子（1 = 不降频；仅 DAQ 方向有意义）
     * @param priority 优先级（0xFF 最高，0 = 允许 Slave 缓冲）
     * @return CTO: [0xE0][mode][daq_lo][daq_hi][event_lo][event_hi][prescaler]
     *         [priority]
     */
    [[nodiscard]] Bytes EncodeSetDaqListMode(DaqListModeBit mode,
                                             std::uint16_t daq_list,
                                             std::uint16_t event_channel,
                                             std::uint8_t prescaler,
                                             std::uint8_t priority) const;

    /**
     * @brief 编码 START_STOP_DAQ_LIST 命令（docs L2206-2228；Mandatory）
     * @param action Stop/Start/Select
     * @param daq_list DAQ List 号（EPK）
     * @return CTO: [0xDE][mode][daq_lo][daq_hi]
     */
    [[nodiscard]] Bytes EncodeStartStopDaqList(DaqListAction action,
                                               std::uint16_t daq_list) const;

    /**
     * @brief 编码 START_STOP_SYNCH 命令（docs L2230-2242；Mandatory）
     * @param action StopAll/StartSelected/StopSelected
     * @return CTO: [0xDD][mode]
     */
    [[nodiscard]] Bytes EncodeStartStopSynch(DaqSynchAction action) const;

    /**
     * @brief 编码 GET_DAQ_LIST_INFO 命令（docs L2446-2465；Optional）
     * @param daq_list DAQ List 号（EPK）
     * @return CTO: [0xD8][reserved][daq_lo][daq_hi]
     */
    [[nodiscard]] Bytes EncodeGetDaqListInfo(std::uint16_t daq_list) const;

    /**
     * @brief 编码 GET_DAQ_EVENT_INFO 命令（v0.3；XCPlite 实然 xcp.h:817-818：
     *        CRO_GET_DAQ_EVENT_INFO_LEN=4，索引 CRO_WORD(1)：事件通道号 WORD，
     *        **无** reserved 字节，与 GET_DAQ_LIST_INFO 的 [reserved] 不同）
     * @param event_channel 事件通道号（uint16_t）
     * @return CTO: [0xD7][event_lo][event_hi]（3+… 实际 4 字节含 PID 头部逻辑）
     * @details 响应六字节数据见 ResponseParser::ParseGetDaqEventInfoRes；
     *          事件通道号**不在响应中回显**。
     */
    [[nodiscard]] Bytes EncodeGetDaqEventInfo(std::uint16_t event_channel) const;

    /**
     * @brief 编码 GET_DAQ_PROCESSOR_INFO 命令（docs L2285-2311；Optional）
     * @return CTO: [0xDA]（1 字节无参；thirdparty/XCPlite/src/xcp.h:789）
     * @details DAQ 能力查询的首要命令：DAQ_PROPERTIES / MAX_DAQ /
     *          MAX_EVENT_CHANNEL / MIN_DAQ / DAQ_KEY_BYTE（后两者给出
     *          address_extension 与 identification_field 的**实际**编码，
     *          B-16 比对以此为运行时真值）。
     */
    [[nodiscard]] Bytes EncodeGetDaqProcessorInfo() const;

    /**
     * @brief 编码 GET_DAQ_RESOLUTION_INFO 命令（docs L2340-2364；Optional）
     * @return CTO: [0xD9]（1 字节，无参；thirdparty/XCPlite/src/xcp.h:798）
     */
    [[nodiscard]] Bytes EncodeGetDaqResolutionInfo() const;

    // ---- 批次20：动态 DAQ 分配命令组（docs §7.5.4.6-§7.5.4.9；
    //      CRO 布局对照 thirdparty/XCPlite/src/xcp.h:831-851；
    //      四条命令响应均为纯 RES，CRM_*_LEN=1）----

    /**
     * @brief 编码 FREE_DAQ 命令
     * @return CTO: [0xD6]（1 字节无参；xcp.h:832 CRO_FREE_DAQ_LEN=1）
     * @details 释放 Slave 侧全部 DAQ 资源（List/ODT/Entry），用于动态配置的
     *          第一步。XCPlite：DAQ 运行中调用 → CRC_DAQ_ACTIVE
     *          （xcplite.c:2567），调用方必须先停。
     */
    [[nodiscard]] Bytes EncodeFreeDaq() const;

    /**
     * @brief 编码 ALLOC_DAQ 命令
     * @param count 要分配的 DAQ List 数（>=1；0 → Slave 回 CRC_OUT_OF_RANGE）
     * @return CTO: [0xD5][reserved][count(WORD)]（4 字节；xcp.h:836-838）
     * @details XCPlite 的 XcpAllocDaq 要求 odt/entry 计数为零（否则
     *          CRC_SEQUENCE，xcplite.c:1148），因此**一次调用分配本次编排的
     *          全部 List 数**，不得分批追加。
     */
    [[nodiscard]] Bytes EncodeAllocDaq(std::uint16_t count) const;

    /**
     * @brief 编码 ALLOC_ODT 命令
     * @param daq_list 目标 DAQ List 号
     * @param count 为该 List 追加的 ODT 数（1..0xFB）
     * @return CTO: [0xD4][reserved][daq(WORD)][count]（5 字节；xcp.h:841-844）
     * @details 任何 ALLOC_ODT_ENTRY 之前必须完成全部 List 的 ODT 分配
     *          （XcpAllocOdt 要求 odt_entry_count==0，xcplite.c:1176）。
     */
    [[nodiscard]] Bytes EncodeAllocOdt(std::uint16_t daq_list,
                                       std::uint8_t count) const;

    /**
     * @brief 编码 ALLOC_ODT_ENTRY 命令
     * @param daq_list 目标 DAQ List 号
     * @param odt_number 该 List 内的相对 ODT 号（0 基）
     * @param count 为该 ODT 分配的 Entry 槽数（>=1）
     * @return CTO: [0xD3][reserved][daq(WORD)][odt][count]
     *         （6 字节；xcp.h:847-851）
     * @details 分配出的 Entry 槽 size=0，仍需 WRITE_DAQ 填充地址后才可用。
     */
    [[nodiscard]] Bytes EncodeAllocOdtEntry(std::uint16_t daq_list,
                                            std::uint8_t odt_number,
                                            std::uint8_t count) const;

    /**
     * @brief 编码 DOWNLOAD 命令（ASAM XCP V1.5.0 原始 PDF 第 142 页 Table 82；
     *        Mandatory，CAL/PAG 可用时）
     * @param number_of_elements 本帧写入的元素数（以 AG 为单位，1..0xFF）
     * @param data 本帧数据字节
     * @return CTO: [0xF0][number_of_elements][pad...][data...]
     * @throws XcpException(InvalidArgument) 元素数为 0/超 0xFF，或数据段为空
     * @note 对齐规则（原始 PDF 第 142 页 Table 82 原文）：对齐字节
     *       ".. BYTEs Used for alignment, **only if AG >2**"；
     *       首元素位置：`AG = 1: 2`、`AG > 1: AG`；且原文
     *       "If AG = DWORD, 2 alignment bytes must be used"。
     *       即**仅 AG=DWORD** 时在数据前插入 2 字节对齐，AG=BYTE/WORD 不插。
     *       （注意与 UPLOAD 响应不同：Table 71 第 134 页为 "only if AG > 1"，
     *       WORD 也要对齐——两者不对称，不得互相套用。）
     * @note 本函数**只负责对齐字节**；「数据字节数 == 元素数 × AG」与
     *       「整帧 ≤ MAX_CTO」仍由 MemoryAccess 的分块编排负责，且其容量口径
     *       必须按对齐字节数扣减（见 MemoryAccess::MaxDownloadAlignmentBytes）。
     * @note MTA 由前置 SET_MTA 决定，DOWNLOAD 完成后 Slave 自动前移 MTA
     *       （原始 PDF 第 142 页："The MTA will be post-incremented by the
     *       number of data elements"）。
     */
    [[nodiscard]] Bytes EncodeDownload(ElementCount number_of_elements,
                                       BytesView data) const;

    /**
     * @brief DOWNLOAD 在数据段前需要插入的对齐字节数
     * @return AG=DWord 时返回 2，其余返回 0
     * @note 依据原始 PDF 第 142 页 Table 82："Used for alignment, only if
     *       AG >2"；"If AG = DWORD, 2 alignment bytes must be used"。
     *       供 MemoryAccess 计算本帧可用元素数时扣减。
     */
    [[nodiscard]] std::size_t DownloadAlignmentBytes() const noexcept;

    /**
     * @brief 编码 SHORT_DOWNLOAD 命令（docs L2018-2026；Optional）
     * @param number_of_elements 元素数（以 AG 为单位）
     * @param extension 地址扩展（8 位）
     * @param address 32 位地址
     * @param data 数据字节
     * @return CTO:
     *         [0xED][number_of_elements][reserved][extension][addr_b0..b3][data...]
     * @throws XcpException(InvalidArgument) 元素数为 0/超 0xFF，或数据段为空
     * @note 固定头 8 字节：MAX_CTO=8 时无处放数据（docs L2026），编排层必须
     *       在此情形下回落 SET_MTA+DOWNLOAD；AG/MAX_CTO 校验同 EncodeDownload。
     * @note SHORT_DOWNLOAD 的数据段前**不插对齐字节**：其 CTO 头固定 8 字节，
     *       数据自然位于 8 字节边界（8 mod 4 = 0），已满足原始 PDF 第 142 页
     *       Table 82 的对齐要求，故 AG 不参与本函数编码。
     */
    [[nodiscard]] Bytes EncodeShortDownload(ElementCount number_of_elements,
                                            AddressExtension extension,
                                            Address address,
                                            BytesView data) const;

    // ---- 变量标定批次：MODIFY_BITS + Page Switching 命令组（docs §7.5.2.5 /
    //      §7.5.3；CRO 布局对照 thirdparty/XCPlite/src/xcp.h:604-660）----

    /**
     * @brief 编码 MODIFY_BITS 命令（docs §7.5.2.5；Optional，需 CAL/PAG）
     * @param shift Shift Value S（有效范围 0..16）
     * @param and_mask AND Mask（16bit，零扩展后左移 S 与目标相与）
     * @param xor_mask XOR Mask（16bit，零扩展后左移 S 与目标相异或）
     * @return CTO: [0xEC][shift][and_mask(WORD)][xor_mask(WORD)]（6 字节）
     * @throws XcpException(InvalidArgument) shift > 16
     * @note 操作对象为当前 MTA 指向的 32-bit 位置；MTA 由前置 SET_MTA 决定，
     *       执行后 MTA 不变。AND/XOR Mask 字段按 Session Byte Order 编码。
     */
    [[nodiscard]] Bytes EncodeModifyBits(std::uint8_t shift,
                                         std::uint16_t and_mask,
                                         std::uint16_t xor_mask) const;

    /**
     * @brief 编码 SET_CAL_PAGE 命令（docs §7.5.3.1；Optional，需 CAL/PAG）
     * @param mode Mode 位组合（ECU 0x01 / XCP 0x02 / ALL 0x80，可叠加）
     * @param segment Segment Number（mode 含 ALL 时被 Slave 忽略，仍原样发送）
     * @param page Page Number
     * @return CTO: [0xEB][mode][segment][page]（4 字节）
     * @throws XcpException(InvalidArgument) mode 不含任何 ECU/XCP 位
     */
    [[nodiscard]] Bytes EncodeSetCalPage(CalPageModeBit mode,
                                         std::uint8_t segment,
                                         std::uint8_t page) const;

    /**
     * @brief 编码 GET_CAL_PAGE 命令（docs §7.5.3.2；Optional，需 CAL/PAG）
     * @param access_mode Access Mode：仅 0x01(ECU)/0x02(XCP)
     * @param segment Segment Number
     * @return CTO: [0xEA][access_mode][segment]（3 字节）
     * @throws XcpException(InvalidArgument) access_mode 非 ECU/XCP
     */
    [[nodiscard]] Bytes EncodeGetCalPage(CalPageAccessMode access_mode,
                                         std::uint8_t segment) const;

    /**
     * @brief 编码 GET_PAG_PROCESSOR_INFO 命令（docs §7.5.3.3；Optional）
     * @return CTO: [0xE9]（1 字节无参；xcp.h CRO_GET_PAG_PROCESSOR_INFO_LEN=1）
     */
    [[nodiscard]] Bytes EncodeGetPagProcessorInfo() const;

    /**
     * @brief 编码 GET_SEGMENT_INFO 命令（docs §7.5.3.4；Optional）
     * @param mode 信息类别（0=基本信息/1=标准属性/2=Address Mapping）
     * @param segment Segment Number
     * @param info SEGMENT_INFO 选择子（Mode 0: 0=地址/1=长度；Mode 2:
     *             0=源地址/1=目的地址/2=长度；Mode 1 不使用，发 0）
     * @param mapping_index Mapping Index（仅 Mode 2 使用）
     * @return CTO: [0xE8][mode][segment][info][mapping_index]（5 字节）
     * @throws XcpException(InvalidArgument) mode > 2 或 info > 2
     */
    [[nodiscard]] Bytes EncodeGetSegmentInfo(SegmentInfoMode mode,
                                             std::uint8_t segment,
                                             SegmentInfoSelector info,
                                             std::uint8_t mapping_index) const;

    /**
     * @brief 编码 GET_PAGE_INFO 命令（docs §7.5.3.5；Optional）
     * @param segment Segment Number
     * @param page Page Number
     * @return CTO: [0xE7][reserved][segment][page]（4 字节）
     */
    [[nodiscard]] Bytes EncodeGetPageInfo(std::uint8_t segment,
                                          std::uint8_t page) const;

    /**
     * @brief 编码 SET_SEGMENT_MODE 命令（docs §7.5.3.6；Optional）
     * @param mode Mode 位（FREEZE 0x01；不置位即取消冻结）
     * @param segment Segment Number
     * @return CTO: [0xE6][mode][segment]（3 字节）
     */
    [[nodiscard]] Bytes EncodeSetSegmentMode(SegmentModeBit mode,
                                             std::uint8_t segment) const;

    /**
     * @brief 编码 GET_SEGMENT_MODE 命令（docs §7.5.3.7；Optional）
     * @param segment Segment Number
     * @return CTO: [0xE5][reserved][segment]（3 字节）
     */
    [[nodiscard]] Bytes EncodeGetSegmentMode(std::uint8_t segment) const;

    /**
     * @brief 编码 COPY_CAL_PAGE 命令（docs §7.5.3.8；Optional）
     * @param request 源/目标 Segment 与 Page
     * @return CTO: [0xE4][src_segment][src_page][dst_segment][dst_page]
     *         （5 字节）
     */
    [[nodiscard]] Bytes EncodeCopyCalPage(const CopyCalPageRequest& request) const;

private:
    /// @brief 构造时确定的 Session 字节序
    ByteOrder m_byte_order_;

    /// @brief 构造时确定的 Session 地址粒度（用于 DOWNLOAD 对齐）
    AddressGranularity m_ag_;

    /// @brief 按 Session 字节序向缓冲区写入 16 位值
    void WriteU16(Bytes& buf, std::uint16_t val) const;
    /// @brief 按 Session 字节序向缓冲区写入 32 位值
    void WriteU32(Bytes& buf, std::uint32_t val) const;
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_COMMAND_CODEC_HPP_