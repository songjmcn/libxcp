/**
 * @file checksum.hpp
 * @brief 本地校验和计算：用于校验 BUILD_CHECKSUM 的 Slave 侧结果。
 *
 * 依据 ASAM MCD-1 XCP V1.5.0（原始 PDF §7.5.2.11 BUILD_CHECKSUM、Table 75
 * 校验和类型、Table 77 CRC 参数）。
 */

#ifndef CALMCAR_XCP_CHECKSUM_HPP_
#define CALMCAR_XCP_CHECKSUM_HPP_

#include <cstdint>
#include <optional>
#include <span>

#include "libxcp/protocol_types.hpp"

namespace calmcar::xcp {

/**
 * @brief 在本地计算指定类型的 XCP 校验和
 * @param type Slave 在 BUILD_CHECKSUM 正响应中回报的校验和类型
 * @param data 参与校验的原始字节（与 Slave 侧 MTA 起的 block size 字节一致）
 * @return 计算结果；**本地无法复现该算法时返回 std::nullopt**
 *
 * @par 仅支持参数在规范中完全确定的算法
 *   以下算法在本库内可实现，参数取自规范 Table 75 / Table 77：
 *   - `XCP_ADD_11` / `XCP_ADD_12` / `XCP_ADD_14`：按字节累加，结果分别
 *     模 2^8 / 2^16 / 2^32 后被零扩展到 DWORD。单字节输入无字节序歧义。
 *   - `XCP_CRC_16`：width 16、poly 0x8005、init 0x0000、refin TRUE、
 *     refout TRUE、xorout 0x0000。
 *   - `XCP_CRC_16_CITT`：width 16、poly 0x1021、init 0xFFFF、refin FALSE、
 *     refout FALSE、xorout 0x0000。
 *
 * @note 以下类型返回 `std::nullopt`，调用方必须改走读回比对而不能把
 *       "算不出来"当成"不匹配"：
 *       - `XCP_ADD_22` / `XCP_ADD_24` / `XCP_ADD_44`：多字节数据单元的
 *         累加结果依赖 Slave 的字节序，规范未在 Table 77 中给出对齐/端序
 *         约定，本地复现有歧义。
 *       - `XCP_CRC_32`：**Table 77 只定义了 CRC_16 与 CRC_16_CITT 的参数**，
 *         规范未给出 CRC_32 的 poly/init/refin/xorout，无法可靠复现。
 *       - `XCP_USER_DEFINED`：算法在外部文件（ASAM MCD-2 MC 描述）中定义，
 *         本库不加载该外部实现。
 */
[[nodiscard]] std::optional<std::uint32_t> ComputeXcpChecksum(
    CalibrationChecksumType type, std::span<const std::uint8_t> data);

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_CHECKSUM_HPP_