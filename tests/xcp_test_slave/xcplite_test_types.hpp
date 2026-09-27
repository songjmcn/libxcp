/**
 * @file xcplite_test_types.hpp
 * @brief XCPlite 协议调试测试的共享类型与常量定义。
 *
 * Slave 程序（tests/xcp_test_slave/main.cpp）与集成测试
 * （tests/xcplite_*_test.cpp）共用本头文件，保证两侧对结构体布局
 * （offsetof / sizeof）与初始值的假设完全一致。
 *
 * 依据 code-plan/XCPlite_Slave_协议调试集成计划.md §1.3。
 */

#ifndef CALMCAR_XCP_TEST_XCPLITE_TEST_TYPES_HPP_
#define CALMCAR_XCP_TEST_XCPLITE_TEST_TYPES_HPP_

#include <cstddef>
#include <cstdint>

namespace calmcar::xcp::test {

/// @brief Slave 项目名（同时是 A2L MODULE 名与 A2L 文件名前缀）
inline constexpr const char* kXcpliteSlaveProject = "xcp_test_slave";

/// @brief Slave 默认监听端口（可被命令行参数覆盖）
inline constexpr std::uint16_t kXcpliteSlaveDefaultPort = 5556;

/// @brief Slave 进程就绪探测的总超时（毫秒）
inline constexpr std::uint32_t kXcpliteSlaveReadyTimeoutMs = 15000;

/**
 * @brief 基本标量测试变量的期望初始值
 * @details Slave 端以这些值初始化全局变量，测试端以同一常量断言读回结果，
 *          单一事实来源，避免两处字面量漂移。
 */
inline constexpr std::uint8_t kExpectBasicU8 = 0x42U;          ///< g_basic_u8
inline constexpr std::int16_t kExpectBasicI16 = -1000;         ///< g_basic_i16
inline constexpr std::uint32_t kExpectBasicU32 = 0xDEADBEEFU;  ///< g_basic_u32
inline constexpr float kExpectBasicF32 = 3.14F;                ///< g_basic_f32
inline constexpr double kExpectBasicF64 = 2.71828;             ///< g_basic_f64

/**
 * @brief 简单结构体（覆盖"结构体变量"场景）
 * @details 字段名全局唯一（simple_ 前缀），避免 XCPlite 按字段名生成
 *          TYPEDEF_MEASUREMENT（M_<field>）时在多个 typedef 间冲突。
 */
struct SimpleStruct_t {
    std::uint8_t simple_u8;    ///< 字节字段（偏移 0）
    std::int16_t simple_i16;   ///< 字字段（偏移 2，前有 1 字节对齐填充）
    std::uint32_t simple_u32;  ///< 双字字段（偏移 4）
};

/// @brief SimpleStruct_t 的期望初始值（Slave 与测试两侧共用）
struct SimpleStructExpectT {
    std::uint8_t u8;    ///< 期望 simple_u8
    std::int16_t i16;   ///< 期望 simple_i16
    std::uint32_t u32;  ///< 期望 simple_u32
};

inline constexpr SimpleStructExpectT kExpectSimpleStruct = {
    .u8 = 10, .i16 = -200, .u32 = 0x12345678U};

inline constexpr SimpleStructExpectT kExpectStructArray[3] = {
    {1, 100, 0x1111U}, {2, 200, 0x2222U}, {3, 300, 0x3333U}};

/**
 * @brief 嵌套结构体（覆盖"结构体嵌套结构体 + 数组嵌套结构体"场景）
 * @details 同时包含单个结构体成员与结构体数组成员。
 */
struct OuterStruct_t {
    std::uint8_t outer_u8;           ///< 标量字段
    std::int16_t outer_i16;          ///< 标量字段（含对齐填充）
    SimpleStruct_t nested_struct;    ///< 结构体嵌套结构体
    SimpleStruct_t nested_array[2];  ///< 结构体内的结构体数组
};

/// @brief OuterStruct_t 的期望初始值（逐字段，避免 padding 参与比较）
struct OuterStructExpectT {
    std::uint8_t outer_u8;                ///< 期望 outer_u8
    std::int16_t outer_i16;               ///< 期望 outer_i16
    SimpleStructExpectT nested_struct;    ///< 期望 nested_struct
    SimpleStructExpectT nested_array[2];  ///< 期望 nested_array
};

inline constexpr OuterStructExpectT kExpectOuter = {
    .outer_u8 = 99,
    .outer_i16 = -777,
    .nested_struct = {5, -50, 0xABCDU},
    .nested_array = {{6, -60, 0x1111U}, {7, -70, 0x2222U}}};

/// @brief 字节数组 g_array_u8 长度
inline constexpr std::size_t kArrayU8Size = 8;
/// @brief 字数组 g_array_i16 长度
inline constexpr std::size_t kArrayI16Size = 4;
/// @brief 多块 UPLOAD 测试用大块变量长度（必须大于 XCPlite MAX_CTO=248）
inline constexpr std::size_t kBlobSize = 600;

/// @brief Calibration 参数结构（第二阶段写回测试；Slave 侧注册为 CalSeg）
struct CalParamsT {
    std::uint16_t cal_factor;  ///< 标定系数
    float cal_offset;          ///< 标定偏置
};

/// @brief CalParamsT.cal_factor 的期望默认值
inline constexpr std::uint16_t kExpectCalFactor = 100;
/// @brief CalParamsT.cal_offset 的期望默认值
inline constexpr float kExpectCalOffset = 0.5F;

/**
 * @brief g_blob 的第 i 个字节期望值（确定性模式，两侧共用）
 * @param i 字节下标
 * @return 期望字节值
 */
[[nodiscard]] constexpr std::uint8_t BlobPattern(std::size_t i) {
    return static_cast<std::uint8_t>((i * 31U + 7U) & 0xFFU);
}

}  // namespace calmcar::xcp::test

#endif  // CALMCAR_XCP_TEST_XCPLITE_TEST_TYPES_HPP_
