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
    explicit CommandCodec(ByteOrder byte_order) noexcept;

    /// @brief 当前使用的字节序
    [[nodiscard]] ByteOrder GetByteOrder() const noexcept;

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

    /// @brief 编码 SHORT_UPLOAD 命令（一次命令完成定址读取，不改动 MTA）
    /// @param number_of_elements 要读取的元素数（以 AG 为单位），1..0xFF
    /// @param extension 地址扩展（8 位）
    /// @param address 32 位地址，按 Session 字节序编码
    /// @return CTO:
    /// [0xF4][number_of_elements][reserved][extension][addr_b0..b3]
    /// @throws XcpException(InvalidArgument) 元素数为 0 或超过单字段 0xFF 上限
    [[nodiscard]] Bytes EncodeShortUpload(ElementCount number_of_elements,
                                          AddressExtension extension,
                                          Address address) const;

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

    /**
     * @brief 编码 DOWNLOAD 命令（docs L1979-1987；Mandatory，CAL/PAG 可用时）
     * @param number_of_elements 本帧写入的元素数（以 AG 为单位，1..0xFF）
     * @param data 本帧数据字节
     * @return CTO: [0xF0][number_of_elements][data...]
     * @throws XcpException(InvalidArgument) 元素数为 0/超 0xFF，或数据段为空
     * @note 「数据字节数 == 元素数 × AG」与「整帧 ≤ MAX_CTO」不在此校验——
     *       Codec 不持有 Session 参数（既有设计），这两条由 MemoryAccess
     *       的分块编排负责（与 UPLOAD 侧同一范式，禁止两处口径不一致）。
     * @note MTA 由前置 SET_MTA 决定，DOWNLOAD 完成后 Slave 自动前移 MTA
     *       （docs L1983）。
     */
    [[nodiscard]] Bytes EncodeDownload(ElementCount number_of_elements,
                                       BytesView data) const;

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
     */
    [[nodiscard]] Bytes EncodeShortDownload(ElementCount number_of_elements,
                                            AddressExtension extension,
                                            Address address,
                                            BytesView data) const;

private:
    /// @brief 构造时确定的 Session 字节序
    ByteOrder m_byte_order_;

    /// @brief 按 Session 字节序向缓冲区写入 16 位值
    void WriteU16(Bytes& buf, std::uint16_t val) const;
    /// @brief 按 Session 字节序向缓冲区写入 32 位值
    void WriteU32(Bytes& buf, std::uint32_t val) const;
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_COMMAND_CODEC_HPP_