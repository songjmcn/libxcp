/**
 * @file a2l_measurement_database.cpp
 * @brief 实现 A2L 测量数据库适配器（v0.5）。
 *
 * 见头文件语义：把 a2l::IA2lDatabase 包装为核心 IMeasurementDatabase。
 * 本文件是**唯一的**核心<->桥接层接触点（S2 隔离门禁只扫 include/src，
 * adapter/ 目录不在扫描范围，见 tests/a2l_isolation_check.cmake）。
 */

#include "a2l_measurement_database.hpp"  // 本适配器类定义

#include "libxcp/measurement/measurement_database.hpp"  // IMeasurementDatabase / MeasurementSymbolInfo
#include "libxcp/measurement/measurement_result.hpp"
#include "libxcp/measurement/measurement_types.hpp"

#include "libxcp/a2l/a2l_result.hpp"
#include "libxcp/a2l/a2l_types.hpp"
#include "libxcp/a2l/ia2l_database.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace calmcar::xcp {

namespace {

/// @brief 桥接层 ErrorCode → 核心 MeasurementErrorCode 映射（计划 v0.5 表格）
MeasurementErrorCode MapErrorCode(a2l::ErrorCode code) noexcept {
    switch (code) {
        case a2l::ErrorCode::NotFound:
            return MeasurementErrorCode::NotFound;
        case a2l::ErrorCode::AmbiguousName:
            return MeasurementErrorCode::AmbiguousName;
        case a2l::ErrorCode::UnsupportedDataType:
            return MeasurementErrorCode::UnsupportedDataType;
        case a2l::ErrorCode::RawSizeMismatch:
            return MeasurementErrorCode::RawSizeMismatch;
        case a2l::ErrorCode::InvalidLayout:
            return MeasurementErrorCode::InvalidLayout;
        case a2l::ErrorCode::UnsupportedConversion:
        case a2l::ErrorCode::ConversionNotInvertible:
        case a2l::ErrorCode::ConversionOutOfRange:
            return MeasurementErrorCode::UnsupportedConversion;
        case a2l::ErrorCode::UnsupportedOperation:
            return MeasurementErrorCode::InvalidLayout;
        case a2l::ErrorCode::AddressOverflow:
            return MeasurementErrorCode::InvalidLayout;
        case a2l::ErrorCode::BadArgument:
            return MeasurementErrorCode::NotFound;
        case a2l::ErrorCode::NotReady:
            return MeasurementErrorCode::Busy;
        default:
            return MeasurementErrorCode::Fatal;
    }
}

/// @brief 构造核心侧错误
template <class T>
MeasurementResult<T> ToCoreError(const a2l::Error& err) {
    return detail::MakeMeasurementError<T>(
        MapErrorCode(err.code), err.message, err.symbol.empty() ? std::string{} : err.symbol);
}

/// @brief 计算元素总数（标量为 1；数组取维度过大乘积，超 255 截断）
std::uint8_t ElementCountOf(const a2l::SymbolInfo& symbol) noexcept {
    if (symbol.dimensions.empty()) {
        return 1;
    }
    std::uint64_t total = 1;
    for (const a2l::Dimension& d : symbol.dimensions) {
        total *= d.extent;
        if (total > 255) {
            return 255;  // 数组过大，适配层按 255 上限（核心侧对数组不展开）
        }
    }
    return static_cast<std::uint8_t>(total);
}

}  // namespace

A2lMeasurementDatabase::A2lMeasurementDatabase(
    const a2l::IA2lDatabase& database)
    : m_database_(database) {}

A2lMeasurementDatabase::~A2lMeasurementDatabase() = default;

MeasurementResult<MeasurementSymbolInfo> A2lMeasurementDatabase::Find(
    std::string_view name) const {
    const a2l::Result<a2l::SymbolInfo> res = m_database_.Find(name);
    if (!res.HasValue()) {
        return ToCoreError<MeasurementSymbolInfo>(res.ErrorInfo());
    }
    const a2l::SymbolInfo& symbol = res.Value();

    MeasurementSymbolInfo info;
    // 地址：A2L 的 xcp_address 在大面是 32 位 XCP 地址；显式收窄到核心 Address。
    info.address = static_cast<Address>(symbol.xcp_address & 0xFFFFFFFFULL);
    info.extension = symbol.address_extension;
    // 事件通道：SymbolInfo 不含事件通道；v0.5 默认 0（不由特定事件触发），
    // 运行时事件绑定由上层 QueryDaqEventInfo 取证后另行补充。
    info.event_channel = 0;
    info.element_size_bytes = symbol.element_size_bytes;
    info.element_count = ElementCountOf(symbol);
    return detail::MakeMeasurementOk(info);
}

MeasurementResult<MeasurementValue> A2lMeasurementDatabase::ToPhysical(
    std::string_view name, BytesView raw) const {
    const a2l::Result<a2l::PhysicalValue> res =
        m_database_.ToPhysical(name, a2l::BytesView{raw});
    if (!res.HasValue()) {
        return ToCoreError<MeasurementValue>(res.ErrorInfo());
    }
    // a2l::PhysicalValue 与核心 MeasurementValue 是同形 variant
    // （int64/uint64/double/string/bool），逐分支复制到核心类型。
    const a2l::PhysicalValue& pv = res.Value();
    MeasurementValue out;
    if (std::holds_alternative<std::int64_t>(pv)) {
        out = std::get<std::int64_t>(pv);
    } else if (std::holds_alternative<std::uint64_t>(pv)) {
        out = std::get<std::uint64_t>(pv);
    } else if (std::holds_alternative<double>(pv)) {
        out = std::get<double>(pv);
    } else if (std::holds_alternative<std::string>(pv)) {
        out = std::get<std::string>(pv);
    } else {
        out = std::get<bool>(pv);
    }
    return detail::MakeMeasurementOk(out);
}

}  // namespace calmcar::xcp