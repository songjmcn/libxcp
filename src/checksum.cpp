/**
 * @file checksum.cpp
 * @brief 本地校验和计算实现（XCP V1.5 Table 75 / Table 77）。
 */

#include "libxcp/checksum.hpp"

#include <cstddef>

namespace calmcar::xcp {
namespace {

/// @brief 按字节累加并截断到指定宽度
/// @param data 原始字节
/// @param mask 累加结果的位宽掩码（0xFF / 0xFFFF / 0xFFFFFFFF）
/// @return 累加结果
[[nodiscard]] std::uint32_t SumBytes(std::span<const std::uint8_t> data,
                                     std::uint32_t mask) noexcept {
    std::uint32_t sum = 0U;
    for (const std::uint8_t byte : data) {
        sum = (sum + byte) & mask;
    }
    return sum;
}

/// @brief 计算反射型 CRC-16（XCP_CRC_16：poly 0x8005、init 0x0000）
/// @details refin/refout 均为 TRUE，故对每个输入字节先按位反转再进入
///          反射多项式 0xA001 的移位循环，最后整体位反转输出。
/// @param data 原始字节
/// @return 16 位校验值（零扩展到 DWORD 由调用方完成）
[[nodiscard]] std::uint16_t Crc16Reflected(
    std::span<const std::uint8_t> data) noexcept {
    std::uint16_t crc = 0x0000U;
    for (const std::uint8_t byte : data) {
        crc ^= static_cast<std::uint16_t>(byte);
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 0x0001U) != 0U) {
                crc = static_cast<std::uint16_t>((crc >> 1) ^ 0xA001U);
            } else {
                crc = static_cast<std::uint16_t>(crc >> 1);
            }
        }
    }
    return crc;
}

/// @brief 计算非反射型 CRC-16（XCP_CRC_16_CITT：poly 0x1021、init 0xFFFF）
/// @param data 原始字节
/// @return 16 位校验值
[[nodiscard]] std::uint16_t Crc16Citt(
    std::span<const std::uint8_t> data) noexcept {
    std::uint16_t crc = 0xFFFFU;
    for (const std::uint8_t byte : data) {
        crc ^= static_cast<std::uint16_t>(byte) << 8U;
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 0x8000U) != 0U) {
                crc = static_cast<std::uint16_t>((crc << 1) ^ 0x1021U);
            } else {
                crc = static_cast<std::uint16_t>(crc << 1);
            }
        }
    }
    return crc;
}

}  // namespace

std::optional<std::uint32_t> ComputeXcpChecksum(
    CalibrationChecksumType type, std::span<const std::uint8_t> data) {
    switch (type) {
        case CalibrationChecksumType::Add11:
            return SumBytes(data, 0x000000FFU);
        case CalibrationChecksumType::Add12:
            return SumBytes(data, 0x0000FFFFU);
        case CalibrationChecksumType::Add14:
            return SumBytes(data, 0xFFFFFFFFU);
        case CalibrationChecksumType::Crc16:
            return static_cast<std::uint32_t>(Crc16Reflected(data));
        case CalibrationChecksumType::Crc16Citt:
            return static_cast<std::uint32_t>(Crc16Citt(data));
        case CalibrationChecksumType::Add22:
        case CalibrationChecksumType::Add24:
        case CalibrationChecksumType::Add44:
            // 多字节累加依赖 Slave 字节序，规范未给出约定：不猜
            return std::nullopt;
        case CalibrationChecksumType::Crc32:
            // Table 77 未定义 CRC_32 参数，无法可靠复现
            return std::nullopt;
        case CalibrationChecksumType::UserDefined:
            // 算法在外部文件中定义，本库不加载
            return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace calmcar::xcp