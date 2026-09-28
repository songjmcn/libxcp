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

    /**
     * @brief DOWNLOAD 单块最大元素数（= (MAX_CTO - 2) / AG）
     * @details CTO 头占 [CMD][SIZE] 两字节（xcp.h:578-581）。docs L2010-2012
     *          的 "MAX_CTO/AG - 1" 说的是**无长度字段**的 DOWNLOAD_MAX，本库
     *          不使用该 Optional 命令，因此按 DOWNLOAD 自身布局取整。
     */
    [[nodiscard]] ElementCount MaxDownloadElements() const;

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
