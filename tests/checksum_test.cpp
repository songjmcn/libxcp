/**
 * @file checksum_test.cpp
 * @brief BUILD_CHECKSUM 的本地复算测试（XCP V1.5 Table 75 / Table 77）。
 *
 * 用标准校验向量锁死实现：CRC-16/ARC 与 CRC-16/CCITT-FALSE 的 "123456789"
 * 检查值分别是 0xBB3D 与 0x29B1（规范 Table 77 给的就是这两套参数）。
 * 同时验证"本地无法复现的算法必须返回 nullopt"，避免把"算不出来"当成
 * "不匹配"。
 */

#include "libxcp/checksum.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace calmcar::xcp {
namespace {

/// @brief 把 ASCII 字符串转成字节视图
std::vector<std::uint8_t> AsciiBytes(const std::string& text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

const std::string kCheckString = "123456789";

}  // namespace

// --------------------------------------------------------------------------
// ADD 系列：单字节累加，结果零扩展到 DWORD
// --------------------------------------------------------------------------

TEST(XcpChecksum, Add11SumsBytesMod256) {
    const auto data = AsciiBytes(kCheckString);
    // '1'..'9' = 0x31..0x39，合计 477 = 0x1DD，模 2^8 得 0xDD
    const auto value = ComputeXcpChecksum(CalibrationChecksumType::Add11, data);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 0x000000DDU);
}

TEST(XcpChecksum, Add12SumsBytesMod65536) {
    const auto data = AsciiBytes(kCheckString);
    const auto value = ComputeXcpChecksum(CalibrationChecksumType::Add12, data);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 0x000001DDU);
}

TEST(XcpChecksum, Add14SumsBytesMod2Pow32) {
    const auto data = AsciiBytes(kCheckString);
    const auto value = ComputeXcpChecksum(CalibrationChecksumType::Add14, data);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 0x000001DDU);
}

TEST(XcpChecksum, AddSeriesWrapsAtItsOwnWidth) {
    // 255 个 0xFF：ADD_11 每字节进位被截断 → 255 个 0xFF 和为 0xFE01，
    // 模 2^8 = 0x01；ADD_12 模 2^16 = 0xFE01
    const std::vector<std::uint8_t> data(255U, 0xFFU);
    const auto add11 = ComputeXcpChecksum(CalibrationChecksumType::Add11, data);
    const auto add12 = ComputeXcpChecksum(CalibrationChecksumType::Add12, data);
    ASSERT_TRUE(add11.has_value());
    ASSERT_TRUE(add12.has_value());
    EXPECT_EQ(*add11, 0x00000001U);
    EXPECT_EQ(*add12, 0x0000FE01U);
}

TEST(XcpChecksum, EmptyInputYieldsZero) {
    const std::vector<std::uint8_t> empty;
    const auto value = ComputeXcpChecksum(CalibrationChecksumType::Add11, empty);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 0U);
}

// --------------------------------------------------------------------------
// CRC 系列：规范 Table 77 只定义这两个，参数必须与标准检查值一致
// --------------------------------------------------------------------------

TEST(XcpChecksum, Crc16MatchesArcCheckValue) {
    // XCP_CRC_16：poly 0x8005、init 0x0000、refin/refout TRUE → CRC-16/ARC
    const auto data = AsciiBytes(kCheckString);
    const auto value = ComputeXcpChecksum(CalibrationChecksumType::Crc16, data);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 0x0000BB3DU);
}

TEST(XcpChecksum, Crc16CittMatchesCcittFalseCheckValue) {
    // XCP_CRC16_CITT：poly 0x1021、init 0xFFFF、refin/refout FALSE
    const auto data = AsciiBytes(kCheckString);
    const auto value =
        ComputeXcpChecksum(CalibrationChecksumType::Crc16Citt, data);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 0x000029B1U);
}

// --------------------------------------------------------------------------
// 无法本地复现的算法必须返回 nullopt（不能当成"不匹配"）
// --------------------------------------------------------------------------

TEST(XcpChecksum, MultiByteAddReturnsNullopt) {
    // ADD_22/24/44 的累加结果依赖 Slave 字节序，规范未约定，不复现
    const auto data = AsciiBytes(kCheckString);
    EXPECT_FALSE(
        ComputeXcpChecksum(CalibrationChecksumType::Add22, data).has_value());
    EXPECT_FALSE(
        ComputeXcpChecksum(CalibrationChecksumType::Add24, data).has_value());
    EXPECT_FALSE(
        ComputeXcpChecksum(CalibrationChecksumType::Add44, data).has_value());
}

TEST(XcpChecksum, Crc32AndUserDefinedReturnNullopt) {
    // Table 77 未给出 CRC_32 参数；USER_DEFINED 在外部文件中定义
    const auto data = AsciiBytes(kCheckString);
    EXPECT_FALSE(
        ComputeXcpChecksum(CalibrationChecksumType::Crc32, data).has_value());
    EXPECT_FALSE(ComputeXcpChecksum(CalibrationChecksumType::UserDefined, data)
                     .has_value());
}

}  // namespace calmcar::xcp