/**
 * @file memory_access.hpp
 * @brief 高层内存读取：SHORT_UPLOAD 优先，必要时降级 SET_MTA+UPLOAD 分块。
 *
 * 依据 code-plan/XCP_1.3.0_详细设计_类接口与头文件.md 第 13 节实现。
 */

#ifndef CALMCAR_XCP_MEMORY_ACCESS_HPP_
#define CALMCAR_XCP_MEMORY_ACCESS_HPP_

#include "libxcp/command_executor.hpp"
#include "libxcp/session.hpp"

namespace calmcar::xcp {

/**
 * @brief 内存访问接口
 *
 * 封装轮询读取逻辑：优先 SHORT_UPLOAD，必要时降级到 SET_MTA+UPLOAD；
 * 自动处理 AG 换算、分块和地址溢出检查。
 *
 * @par 长度上限（docs/XCP_1.3.0_document.md §7.5.1.11 / §7.5.1.12）
 *   UPLOAD:       1 <= NumberOfElements <= MAX_CTO / AG - 1
 *   SHORT_UPLOAD: 1 <= NumberOfElements <= MAX_CTO / AG
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

    [[nodiscard]] Bytes ReadBytes(Address address, AddressExtension extension,
                                  ByteCount byte_count);

    [[nodiscard]] Bytes ShortUpload(Address address, AddressExtension extension,
                                    ElementCount element_count);

    [[nodiscard]] Bytes UploadChunked(Address address,
                                      AddressExtension extension,
                                      ElementCount element_count);

private:
    CommandExecutor& m_executor_;
    Session& m_session_;

    [[nodiscard]] bool CanUseShortUpload(ElementCount element_count) const;
    [[nodiscard]] ElementCount MaxUploadElements() const;
    [[nodiscard]] ElementCount MaxShortUploadElements() const;
    void ValidateRead(Address address, ElementCount element_count) const;

    /// @brief 校验读取参数（已连接、元素数非 0、地址不溢出）
    /// @throws XcpException(InvalidArgument)
    void validateRead(Address address, ElementCount element_count) const;
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_MEMORY_ACCESS_HPP_
