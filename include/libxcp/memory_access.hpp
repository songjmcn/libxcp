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

private:
    /// @brief 命令执行器（非拥有）
    CommandExecutor& m_executor_;
    /// @brief 会话（非拥有），提供 MAX_CTO / AG / SHORT_UPLOAD 可用性
    Session& m_session_;

    /// @brief 是否可走 SHORT_UPLOAD：能力可用且元素数不超过单包上限
    [[nodiscard]] bool CanUseShortUpload(ElementCount element_count) const;
    /// @brief UPLOAD 单块最大元素数（= MAX_CTO/AG - 1，为 CTO 自身留出余量）
    [[nodiscard]] ElementCount MaxUploadElements() const;
    /// @brief SHORT_UPLOAD 单包最大元素数（= MAX_CTO/AG）
    [[nodiscard]] ElementCount MaxShortUploadElements() const;

    /// @brief 校验读取参数（已连接、元素数非 0、地址不溢出）
    /// @throws XcpException(InvalidArgument)
    void ValidateRead(Address address, ElementCount element_count) const;
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_MEMORY_ACCESS_HPP_
