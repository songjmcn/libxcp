/**
 * @file memory_access.cpp
 * @brief MemoryAccess 实现：AG 换算、分块、降级与溢出检查。
 */

#include "libxcp/memory_access.hpp"
#include "libxcp/log.hpp"

#include <string>
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
    DownloadChunked(address, extension, data);
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