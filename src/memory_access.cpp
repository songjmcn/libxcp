/**
 * @file memory_access.cpp
 * @brief MemoryAccess 实现：AG 换算、分块、降级与溢出检查。
 */

#include "libxcp/memory_access.hpp"

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
        throw detail::MakeInvalidState("读取内存前必须先建立 XCP 连接");
    }
    if (element_count == 0U) {
        throw detail::MakeInvalidArgument("读取元素数必须大于 0");
    }
    const auto ag = m_session_.GetAddressGranularity();
    const auto byte_count =
        static_cast<std::uint64_t>(element_count) * AgToBytes(ag);
    const auto end_exclusive = static_cast<std::uint64_t>(address) + byte_count;
    if (end_exclusive > 0x100000000ULL) {
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
        throw detail::MakeInvalidArgument("读取字节数必须大于 0");
    }
    if ((byte_count % ag_bytes) != 0U) {
        throw detail::MakeInvalidArgument(
            "读取字节数 " + std::to_string(byte_count) +
            " 不能被 Address Granularity (" + std::to_string(ag_bytes) +
            " Byte/Address) 整除");
    }
    return ReadElements(address, extension, byte_count / ag_bytes);
}

}  // namespace calmcar::xcp