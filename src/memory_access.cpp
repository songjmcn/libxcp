/**
 * @file memory_access.cpp
 * @brief MemoryAccess 实现：AG 换算、分块、降级与溢出检查。
 */

#include "libxcp/memory_access.hpp"
#include "libxcp/checksum.hpp"
#include "libxcp/log.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <utility>

namespace calmcar::xcp {

MemoryAccess::MemoryAccess(CommandExecutor& executor, Session& session)
    : m_executor_(executor), m_session_(session) {}

ElementCount MemoryAccess::MaxShortUploadElements() const {
    const auto max_cto = static_cast<ElementCount>(m_session_.MaxCto());
    const auto ag = static_cast<ElementCount>(
        AgToBytes(m_session_.GetAddressGranularity()));
    if (ag == 0U) {
        return 0U;
    }
    return max_cto / ag;
}

ElementCount MemoryAccess::MaxUploadElements() const {
    const auto limit = MaxShortUploadElements();
    return limit == 0U ? 0U : limit - 1U;
}

void MemoryAccess::ValidateRead(Address address,
                                ElementCount element_count) const {
    if (!m_session_.IsConnected()) {
        LIBXCP_LOG_ERROR("Memory read rejected: XCP session is not connected");
        throw detail::MakeInvalidState("读取内存前必须先建立 XCP 连接");
    }
    if (element_count == 0U) {
        LIBXCP_LOG_ERROR("Memory read rejected: element_count is zero");
        throw detail::MakeInvalidArgument("读取元素数必须大于 0");
    }
    const auto ag = m_session_.GetAddressGranularity();
    const auto byte_count =
        static_cast<std::uint64_t>(element_count) * AgToBytes(ag);
    const auto end_exclusive = static_cast<std::uint64_t>(address) + byte_count;
    if (end_exclusive > 0x100000000ULL) {
        LIBXCP_LOG_ERROR(
            "Memory read rejected: 32-bit address overflow address={} "
            "elements={} ag_bytes={}",
            address, element_count, AgToBytes(ag));
        throw detail::MakeInvalidArgument(
            "读取范围导致 32 位地址溢出: address=0x" + std::to_string(address) +
            ", elements=" + std::to_string(element_count) +
            ", AG=" + std::to_string(AgToBytes(ag)));
    }
}

bool MemoryAccess::CanUseShortUpload(ElementCount element_count) const {
    if (!m_session_.Parameters().short_upload_available) {
        return false;
    }
    return element_count <= MaxShortUploadElements();
}

Bytes MemoryAccess::ShortUpload(Address address, AddressExtension extension,
                                ElementCount element_count) {
    ValidateRead(address, element_count);
    if (element_count > MaxShortUploadElements()) {
        LIBXCP_LOG_ERROR("SHORT_UPLOAD rejected: element_count={} maximum={}",
                         element_count, MaxShortUploadElements());
        throw detail::MakeInvalidArgument(
            "SHORT_UPLOAD 元素数 " + std::to_string(element_count) +
            " 超过单包上限 MAX_CTO/AG = " +
            std::to_string(MaxShortUploadElements()));
    }
    return m_executor_.ExecuteShortUpload(element_count, extension, address);
}

Bytes MemoryAccess::UploadChunked(Address address, AddressExtension extension,
                                  ElementCount element_count) {
    ValidateRead(address, element_count);
    const auto chunk_limit = MaxUploadElements();
    if (chunk_limit == 0U) {
        LIBXCP_LOG_ERROR("UPLOAD rejected: MAX_CTO/AG leaves no chunk capacity");
        throw detail::MakeInvalidArgument(
            "当前 Session 的 MAX_CTO/AG 过小，无法使用 UPLOAD 分块读取");
    }

    const auto ag = m_session_.GetAddressGranularity();
    Bytes result;
    result.reserve(static_cast<std::size_t>(element_count) * AgToBytes(ag));

    XcpAddress40 cursor{address, extension};
    ElementCount remaining = element_count;
    ElementCount completed = 0;

    while (remaining > 0U) {
        const auto chunk = (remaining < chunk_limit) ? remaining : chunk_limit;

        try {
            m_executor_.ExecuteSetMta(cursor.extension, cursor.address);
            const Bytes part = m_executor_.ExecuteUpload(chunk);
            result.insert(result.end(), part.begin(), part.end());
        } catch (const XcpException& e) {
            throw XcpException(
                e.Category(),
                "UPLOAD 分块读取失败：已完成 " + std::to_string(completed) +
                    "/" + std::to_string(element_count) +
                    " 个元素（起始地址 0x" + std::to_string(address) + "）。" +
                    std::string(e.what()),
                e.GetCommandCode(), e.GetErrorCode(), e.RetryCount(),
                std::string(e.TransportError()));
        }

        remaining -= chunk;
        completed += chunk;
        if (remaining == 0U) {
            break;
        }
        const auto next = cursor.Advance(chunk, ag);
        if (!next) {
            LIBXCP_LOG_ERROR("UPLOAD rejected: chunk address advance overflow");
            throw detail::MakeInvalidArgument(
                "UPLOAD 分块推进时 32 位地址溢出");
        }
        cursor = *next;
    }
    return result;
}

Bytes MemoryAccess::ReadElements(Address address, AddressExtension extension,
                                 ElementCount element_count) {
    ValidateRead(address, element_count);

    if (CanUseShortUpload(element_count)) {
        try {
            return m_executor_.ExecuteShortUpload(element_count, extension,
                                                  address);
        } catch (const XcpException& e) {
            const bool fallback =
                e.GetErrorCode() &&
                *e.GetErrorCode() == ErrorCode::CmdUnknown &&
                e.GetCommandCode() &&
                *e.GetCommandCode() == CommandCode::ShortUpload;
            if (!fallback) {
                throw;
            }
        }
    }
    return UploadChunked(address, extension, element_count);
}

Bytes MemoryAccess::ReadBytes(Address address, AddressExtension extension,
                              ByteCount byte_count) {
    const auto ag_bytes = AgToBytes(m_session_.GetAddressGranularity());
    if (byte_count == 0U) {
        LIBXCP_LOG_ERROR("Memory read rejected: byte_count is zero");
        throw detail::MakeInvalidArgument("读取字节数必须大于 0");
    }
    if ((byte_count % ag_bytes) != 0U) {
        LIBXCP_LOG_ERROR(
            "Memory read rejected: byte_count={} is not aligned to ag_bytes={}",
            byte_count, ag_bytes);
        throw detail::MakeInvalidArgument(
            "读取字节数 " + std::to_string(byte_count) +
            " 不能被 Address Granularity (" + std::to_string(ag_bytes) +
            " Byte/Address) 整除");
    }
    return ReadElements(address, extension, byte_count / ag_bytes);
}

// ---------------------------------------------------------------------------
// 批次14：写回（T14-06，R9）——与读取侧严格对称的上限与编排
// ---------------------------------------------------------------------------

ElementCount MemoryAccess::MaxDownloadElements() const {
    const auto max_cto = static_cast<ElementCount>(m_session_.MaxCto());
    const auto ag = static_cast<ElementCount>(
        AgToBytes(m_session_.GetAddressGranularity()));
    // CTO 布局：[CMD][SIZE][对齐字节...][data...]。
    // 对齐字节数由 AG 决定（原始 PDF 第 142 页 Table 82："Used for alignment,
    // only if AG >2"；"If AG = DWORD, 2 alignment bytes must be used"），
    // AG=BYTE/WORD 时为 0 —— 容量必须把它扣掉，否则 AG=DWORD 时按旧口径
    // 算出 (MAX_CTO-2)/AG 会令整帧超出 MAX_CTO。
    const auto alignment = static_cast<ElementCount>(
        DownloadAlignmentBytes(m_session_.GetAddressGranularity()));
    const auto header = 2U + alignment;
    if (ag == 0U || max_cto < header + ag) {
        // CTO 头 [CMD][SIZE] + 对齐字节后，连一个元素都放不下
        return 0U;
    }
    return (max_cto - header) / ag;
}

ElementCount MemoryAccess::MaxShortDownloadElements() const {
    const auto max_cto = static_cast<ElementCount>(m_session_.MaxCto());
    const auto ag = static_cast<ElementCount>(
        AgToBytes(m_session_.GetAddressGranularity()));
    // SHORT_DOWNLOAD 固定头 8 字节：MAX_CTO=8 时无处放数据（docs L2026）
    if (ag == 0U || max_cto < 8U + ag) {
        return 0U;
    }
    return (max_cto - 8U) / ag;
}

ElementCount MemoryAccess::ValidateWrite(Address address,
                                         BytesView data) const {
    if (!m_session_.IsConnected()) {
        LIBXCP_LOG_ERROR("Memory write rejected: XCP session is not connected");
        throw detail::MakeInvalidState("写入内存前必须先建立 XCP 连接");
    }
    if (data.empty()) {
        LIBXCP_LOG_ERROR("Memory write rejected: data is empty");
        throw detail::MakeInvalidArgument("写入数据不能为空");
    }
    const auto ag = m_session_.GetAddressGranularity();
    const auto ag_bytes = static_cast<std::uint64_t>(AgToBytes(ag));
    if (ag_bytes == 0U) {
        LIBXCP_LOG_ERROR("Memory write rejected: address granularity is zero");
        throw detail::MakeInvalidArgument("地址粒度为 0，Session 参数非法");
    }
    if ((data.size() % ag_bytes) != 0U) {
        LIBXCP_LOG_ERROR(
            "Memory write rejected: byte_count={} is not aligned to "
            "ag_bytes={}",
            data.size(), ag_bytes);
        throw detail::MakeInvalidArgument(
            "写入字节数 " + std::to_string(data.size()) +
            " 不能被 Address Granularity (" + std::to_string(ag_bytes) +
            " Byte/Address) 整除");
    }
    const auto element_count =
        static_cast<ElementCount>(data.size() / ag_bytes);
    const auto end_exclusive =
        static_cast<std::uint64_t>(address) +
        static_cast<std::uint64_t>(element_count) * ag_bytes;
    if (end_exclusive > 0x100000000ULL) {
        LIBXCP_LOG_ERROR(
            "Memory write rejected: 32-bit address overflow address={} bytes={} ag_bytes={}",
            address, data.size(), ag_bytes);
        throw detail::MakeInvalidArgument(
            "写入范围导致 32 位地址溢出: address=0x" + std::to_string(address) +
            ", bytes=" + std::to_string(data.size()) +
            ", AG=" + std::to_string(ag_bytes));
    }
    return element_count;
}

void MemoryAccess::DownloadChunked(Address address, AddressExtension extension,
                                   BytesView data) {
    const ElementCount element_count = ValidateWrite(address, data);
    const auto chunk_limit = MaxDownloadElements();
    if (chunk_limit == 0U) {
        LIBXCP_LOG_ERROR(
            "DOWNLOAD rejected: MAX_CTO/AG leaves no chunk capacity");
        throw detail::MakeInvalidArgument(
            "当前 Session 的 (MAX_CTO-2)/AG 过小，无法使用 DOWNLOAD 分块写入");
    }
    const auto ag = m_session_.GetAddressGranularity();
    const auto ag_bytes = static_cast<std::size_t>(AgToBytes(ag));

    XcpAddress40 cursor{address, extension};
    ElementCount remaining = element_count;
    ElementCount completed = 0;
    std::size_t offset_bytes = 0;

    while (remaining > 0U) {
        const auto chunk = (remaining < chunk_limit) ? remaining : chunk_limit;
        const BytesView part = data.subspan(
            offset_bytes, static_cast<std::size_t>(chunk) * ag_bytes);
        try {
            // 每块都显式 SET_MTA 到该块起点：不依赖 Slave 的 MTA 自增语义，
            // 因而超时恢复（重放最后一次 SET_MTA）后重试落在正确地址上
            m_executor_.ExecuteSetMta(cursor.extension, cursor.address);
            m_executor_.ExecuteDownload(chunk, part);
        } catch (const XcpException& e) {
            // 写回不静默降级：任一块失败即整个写操作视为未完成，
            // 并把"已完成 x/y 元素"的上下文附在原异常上（docs L1985
            // 的原子性只对单块成立，跨块必须由调用方重新发起）
            throw XcpException(
                e.Category(),
                "DOWNLOAD 分块写入失败：已完成 " + std::to_string(completed) +
                    "/" + std::to_string(element_count) +
                    " 个元素（起始地址 0x" + std::to_string(address) + "）。" +
                    std::string(e.what()),
                e.GetCommandCode(), e.GetErrorCode(), e.RetryCount(),
                std::string(e.TransportError()));
        }

        remaining -= chunk;
        completed += chunk;
        offset_bytes += static_cast<std::size_t>(chunk) * ag_bytes;
        if (remaining == 0U) {
            break;
        }
        // 推进到下一块起点（与 UploadChunked 同一语义）：
        // chunk 是元素数，XCP 地址按元素计数，故地址推进量为 chunk 本身；
        // AG 只用于把元素数换算成字节数（见上一行的 offset_bytes）。
        const auto next = cursor.Advance(chunk, ag);
        if (!next) {
            LIBXCP_LOG_ERROR(
                "DOWNLOAD rejected: chunk address advance overflow");
            throw detail::MakeInvalidArgument(
                "DOWNLOAD 分块推进时 32 位地址溢出");
        }
        cursor = *next;
    }
}

// ---------------------------------------------------------------------------
// B1：块传输写入（DOWNLOAD 首帧 + DOWNLOAD_NEXT 续帧）
// ---------------------------------------------------------------------------

bool MemoryAccess::CanUseBlockMode() const {
    // 默认不启用：块序列丢掉中间帧会静默把后续数据写错位置，只有调用方
    // 明确承担该风险时才允许（见 SetBlockModeEnabled 的说明）
    if (!m_block_mode_enabled_) {
        return false;
    }
    // 块下载是 Master→Slave 方向，规范要求查 GET_COMM_MODE_INFO 的
    // COMM_MODE_OPTIONAL.MASTER_BLOCK_MODE(bit0)。CONNECT 的 COMM_MODE_BASIC
    // bit6（SLAVE_BLOCK_MODE）只描述 Slave→Master 上传块，与下载无关：
    // 用它判定会两个方向都判错（漏降级 / 误启用），因此不得回落到该位。
    const SessionParameters params = m_session_.Parameters();
    if (!params.comm_mode_info.has_value()) {
        return false;
    }
    if ((params.comm_mode_info->comm_mode_optional &
         kCommModeOptionalMasterBlockMode) == 0U) {
        return false;
    }
    return params.comm_mode_info->max_bs >= 1U;
}

void MemoryAccess::SetBlockModeEnabled(bool enabled) {
    m_block_mode_enabled_ = enabled;
}

void MemoryAccess::DownloadBlock(Address address, AddressExtension extension,
                                 BytesView data, ElementCount element_count) {
    const auto ag = m_session_.GetAddressGranularity();
    const auto ag_bytes = static_cast<std::size_t>(AgToBytes(ag));
    const auto frame_capacity = MaxDownloadElements();
    if (frame_capacity == 0U) {
        // 单帧装不下任何元素时无法编排块序列，交由调用方选择其他通路
        throw detail::MakeInvalidArgument(
            "当前 Session 的 (MAX_CTO-2)/AG 过小，无法使用块传输写入");
    }

    // 块长度上限：规范 block mode 的 size 上限为
    // min(MAX_BS * 单帧元素数, 255)（原始 PDF §7.5.2.9）
    const auto max_bs = static_cast<std::uint32_t>(
        m_session_.Parameters().comm_mode_info->max_bs);
    std::uint64_t block_limit = static_cast<std::uint64_t>(max_bs) *
                                static_cast<std::uint64_t>(frame_capacity);
    if (block_limit > 255U) {
        block_limit = 255U;
    }
    if (block_limit == 0U) {
        throw detail::MakeInvalidArgument("块传输上限为 0，无法编排块序列");
    }

    // 本次块的总元素数（首帧 DOWNLOAD 的 size 字段就是这个数）
    ElementCount remaining_total = element_count;
    std::size_t offset_bytes = 0;
    XcpAddress40 cursor{address, extension};

    while (remaining_total > 0U) {
        ElementCount block_elements = remaining_total;
        if (block_elements > static_cast<ElementCount>(block_limit)) {
            block_elements = static_cast<ElementCount>(block_limit);
        }
        const BytesView block_data =
            data.subspan(offset_bytes,
                         static_cast<std::size_t>(block_elements) * ag_bytes);

        // 每块起点显式 SET_MTA：块之间虽然 MTA 已被 Slave 推进到块尾，
        // 但显式设置让"超时后重放 MTA"仍然落在正确位置
        m_executor_.ExecuteSetMta(cursor.extension, cursor.address);

        if (block_elements <= frame_capacity) {
            // 一帧就能装完：不构成块传输，走普通 DOWNLOAD（需要应答）
            m_executor_.ExecuteDownload(block_elements, block_data);
        } else {
            // 首帧：携带一帧容量的数据，但 size 字段报**整块**元素数
            const ElementCount first_chunk = frame_capacity;
            m_executor_.ExecuteDownloadBlockHead(
                block_elements,
                block_data.subspan(
                    0, static_cast<std::size_t>(first_chunk) * ag_bytes));

            ElementCount remaining = block_elements - first_chunk;
            std::size_t block_offset_bytes =
                static_cast<std::size_t>(first_chunk) * ag_bytes;
            while (remaining > frame_capacity) {
                // 中间帧：无应答；size 字段是"剩余元素数（含本帧）"
                m_executor_.ExecuteDownloadNextFrame(
                    remaining,
                    block_data.subspan(
                        block_offset_bytes,
                        static_cast<std::size_t>(frame_capacity) * ag_bytes));
                remaining -= frame_capacity;
                block_offset_bytes +=
                    static_cast<std::size_t>(frame_capacity) * ag_bytes;
            }
            // 末帧：size 字段等于末帧元素数，并等待应答
            m_executor_.ExecuteDownloadNextTail(
                remaining,
                block_data.subspan(
                    block_offset_bytes,
                    static_cast<std::size_t>(remaining) * ag_bytes));
        }

        remaining_total -= block_elements;
        offset_bytes += static_cast<std::size_t>(block_elements) * ag_bytes;
        if (remaining_total == 0U) {
            break;
        }
        const auto next = cursor.Advance(block_elements, ag);
        if (!next) {
            throw detail::MakeInvalidArgument("块传输推进时 32 位地址溢出");
        }
        cursor = *next;
    }
}

void MemoryAccess::WriteBytes(Address address, AddressExtension extension,
                              BytesView data) {
    const ElementCount element_count = ValidateWrite(address, data);

    // SHORT_DOWNLOAD 优先（一帧装得下时省一条 SET_MTA）
    if (element_count <= MaxShortDownloadElements()) {
        try {
            m_executor_.ExecuteShortDownload(element_count, extension, address,
                                             data);
            return;
        } catch (const XcpException& e) {
            // 与 SHORT_UPLOAD 同口径：只有 Slave 明确回 ERR_CMD_UNKNOWN 且
            // 错误来自 SHORT_DOWNLOAD 本身，才允许回落 SET_MTA+DOWNLOAD
            const bool fallback =
                e.GetErrorCode() &&
                *e.GetErrorCode() == ErrorCode::CmdUnknown &&
                e.GetCommandCode() &&
                *e.GetCommandCode() == CommandCode::ShortDownload;
            if (!fallback) {
                throw;
            }
        }
    }
    // 数据一帧装不下：若 Slave 已证实支持块模式，用块序列省掉每块的 SET_MTA
    // 往返；否则走保守的逐块 SET_MTA + DOWNLOAD（行为与历史版本一致）。
    if (CanUseBlockMode() && element_count > MaxDownloadElements()) {
        DownloadBlock(address, extension, data, element_count);
        return;
    }
    DownloadChunked(address, extension, data);
}

// ---------------------------------------------------------------------------
// B1：标定闭环——写后校验与持久化请求
// ---------------------------------------------------------------------------

CalibrationVerificationResult MemoryAccess::VerifyBytes(
    Address address, AddressExtension extension, BytesView expected) {
    // 复用写入侧的参数校验：空数据、非 AG 整数倍、地址溢出都在这里拦住
    const ElementCount element_count = ValidateWrite(address, expected);
    CalibrationVerificationResult result;

    // 首选 Slave 侧校验和：不必把整段标定数据搬回主机
    m_executor_.ExecuteSetMta(extension, address);
    const auto checksum = m_executor_.ExecuteBuildChecksum(element_count);
    if (checksum.has_value() && checksum->checksum_type.has_value()) {
        const auto local =
            ComputeXcpChecksum(*checksum->checksum_type, expected);
        if (local.has_value()) {
            result.method = CalibrationVerificationMethod::SlaveChecksum;
            result.checksum_type = checksum->checksum_type;
            result.slave_checksum = checksum->checksum;
            result.local_checksum = *local;
            result.status = (*local == checksum->checksum)
                                ? CalibrationVerificationStatus::Match
                                : CalibrationVerificationStatus::Mismatch;
            return result;
        }
        // 算法本地无法复现：不能把"算不出来"当成不匹配，降级为读回
        LIBXCP_LOG_WARNING(
            "BUILD_CHECKSUM 类型 {} 本地无法复现，降级为读回比对",
            ChecksumTypeName(*checksum->checksum_type));
    }

    // 读回比对：BUILD_CHECKSUM 不可用、或算法无法本地复现时的通用后备
    result.method = CalibrationVerificationMethod::Readback;
    try {
        const Bytes readback =
            ReadBytes(address, extension,
                      static_cast<ByteCount>(expected.size()));
        const bool identical = std::equal(readback.begin(), readback.end(),
                                          expected.begin(), expected.end());
        result.status = identical ? CalibrationVerificationStatus::Match
                                  : CalibrationVerificationStatus::Mismatch;
    } catch (const XcpException& e) {
        // 读不回来就是没有证据：既不能确认也不能否认，交调用方决定
        LIBXCP_LOG_WARNING("标定写后读回失败，结论为 Indeterminate: {}",
                        e.what());
        result.status = CalibrationVerificationStatus::Indeterminate;
    }
    return result;
}

CalibrationStoreResult MemoryAccess::RequestCalibrationStore(
    std::chrono::milliseconds completion_timeout) {
    if (completion_timeout.count() <= 0) {
        throw detail::MakeInvalidArgument(
            "RequestCalibrationStore 的 completion_timeout 必须大于 0");
    }
    if (!m_session_.IsConnected()) {
        throw detail::MakeInvalidState("请求保存标定数据前必须先建立 XCP 连接");
    }

    CalibrationStoreResult result;
    // SET_REQUEST 正响应无参数（规范无响应字段表），只表示请求被接受；
    // ERR_CMD_UNKNOWN 由执行器转成 UnsupportedFeature 抛出，不在此静默降级
    const SessionParameters params = m_session_.Parameters();
    const std::uint16_t session_config_id =
        params.status.has_value()
            ? params.status->session_config_id
            : static_cast<std::uint16_t>(0U);
    m_executor_.ExecuteSetRequest(SetRequestModeBit::kStoreCalReq,
                                  session_config_id);
    result.request_accepted = true;

    // 观察 GET_STATUS：STORE_CAL_REQ 位被 Slave 清除才算完成
    const auto deadline = std::chrono::steady_clock::now() + completion_timeout;
    constexpr auto kPollInterval = std::chrono::milliseconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        GetStatusResponse status;
        try {
            status = m_executor_.ExecuteGetStatus();
            ++result.polls;
        } catch (const XcpException& e) {
            // 请求可能已经生效，重发会造成重复持久化：只报告"不可知"
            LIBXCP_LOG_WARNING("保存标定数据期间 GET_STATUS 失败，结论 Unknown: {}",
                            e.what());
            result.status = CalibrationStoreStatus::Unknown;
            return result;
        }
        if (!status.store_cal_req) {
            result.status = CalibrationStoreStatus::Completed;
            return result;
        }
        std::this_thread::sleep_for(kPollInterval);
    }
    result.status = CalibrationStoreStatus::Pending;
    return result;
}

// ---------------------------------------------------------------------------
// 变量标定批次：MODIFY_BITS（docs §7.5.2.5）——SET_MTA + MODIFY_BITS 序列
// ---------------------------------------------------------------------------

void MemoryAccess::ModifyBits(Address address, AddressExtension extension,
                              std::uint8_t shift, std::uint16_t and_mask,
                              std::uint16_t xor_mask) {
    if (!m_session_.IsConnected()) {
        LIBXCP_LOG_ERROR("MODIFY_BITS rejected: XCP session is not connected");
        throw detail::MakeInvalidState("位修改前必须先建立 XCP 连接");
    }
    if (shift > kModifyBitsMaxShift) {
        LIBXCP_LOG_ERROR("MODIFY_BITS rejected: shift={} exceeds 16", shift);
        throw detail::MakeInvalidArgument(
            "MODIFY_BITS Shift Value 超出范围 0..16: " +
            std::to_string(shift));
    }
    // 32-bit 操作对象按 DWORD 校验地址溢出（与 ValidateRead/Write 同口径）
    const auto end_exclusive =
        static_cast<std::uint64_t>(address) + 4ULL;
    if (end_exclusive > 0x100000000ULL) {
        LIBXCP_LOG_ERROR(
            "MODIFY_BITS rejected: target address {} overflows 32-bit range",
            address);
        throw detail::MakeInvalidArgument(
            "MODIFY_BITS 目标地址 0x" + std::to_string(address) +
            " + 4 Byte 越过 32 位边界");
    }

    // MODIFY_BITS 不带地址，操作对象是隐含 MTA 指向的 32-bit 位置；
    // SET_MTA 成功后 CommandExecutor 记录 m_last_mta_，超时恢复自动重放。
    m_executor_.ExecuteSetMta(extension, address);
    m_executor_.ExecuteModifyBits(shift, and_mask, xor_mask);
    // docs §7.5.2.5：执行后 MTA 不变，无需调整本端记录
}

}  // namespace calmcar::xcp