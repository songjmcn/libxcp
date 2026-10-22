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
#include <limits>
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

/// @brief 将桥接层的同形物理值 variant 复制到核心类型。
MeasurementValue ToCorePhysicalValue(const a2l::PhysicalValue& value) {
    MeasurementValue result;
    if (std::holds_alternative<std::int64_t>(value)) {
        result = std::get<std::int64_t>(value);
    } else if (std::holds_alternative<std::uint64_t>(value)) {
        result = std::get<std::uint64_t>(value);
    } else if (std::holds_alternative<double>(value)) {
        result = std::get<double>(value);
    } else if (std::holds_alternative<std::string>(value)) {
        result = std::get<std::string>(value);
    } else {
        result = std::get<bool>(value);
    }
    return result;
}

/// @brief 计算并验证首期支持的 A2L 数组元素数；不截断不支持的布局。
MeasurementResult<std::uint8_t> ElementCountOf(const a2l::SymbolInfo& symbol) {
    if (symbol.dimensions.empty()) {
        return std::uint8_t{1};
    }
    const auto invalid = [&symbol](std::string message) {
        return MeasurementResult<std::uint8_t>{MeasurementError{
            MeasurementErrorCode::InvalidLayout, std::move(message), symbol.name}};
    };
    if (symbol.kind != a2l::SymbolKind::Measurement) {
        return invalid("首期 DAQ 数组只支持 A2L MEASUREMENT");
    }
    if (symbol.dimensions.size() != 1U) {
        return invalid("首期 DAQ 数组只支持一维连续布局");
    }
    const a2l::Dimension& dimension = symbol.dimensions.front();
    if (dimension.extent == 0U || dimension.extent > 255U) {
        return invalid("DAQ 数组元素数必须在 1 到 255 之间");
    }
    if (symbol.element_size_bytes == 0U ||
        dimension.byte_stride != symbol.element_size_bytes) {
        return invalid("DAQ 数组必须具有固定宽度且元素连续排列");
    }
    if (symbol.daq_bit_offset.has_value() || symbol.bit_mask != 0U) {
        return invalid("首期 DAQ 数组不支持位字段");
    }

    const std::uint64_t total_bytes =
        dimension.extent * symbol.element_size_bytes;
    constexpr std::uint64_t kMaxAddress =
        std::numeric_limits<std::uint32_t>::max();
    if (symbol.xcp_address > kMaxAddress || total_bytes == 0U ||
        total_bytes - 1U > kMaxAddress - symbol.xcp_address) {
        return invalid("DAQ 数组字节范围超出 32 位 XCP 地址空间");
    }
    return static_cast<std::uint8_t>(dimension.extent);
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
    const MeasurementResult<std::uint8_t> element_count = ElementCountOf(symbol);
    if (!element_count.HasValue()) {
        return element_count.ErrorInfo();
    }

    MeasurementSymbolInfo info;
    // 地址：A2L 的 xcp_address 在大面是 32 位 XCP 地址；显式收窄到核心 Address。
    info.address = static_cast<Address>(symbol.xcp_address & 0xFFFFFFFFULL);
    info.extension = symbol.address_extension;
    // 事件通道：SymbolInfo 不含事件通道；v0.5 默认 0（不由特定事件触发），
    // 运行时事件绑定由上层 QueryDaqEventInfo 取证后另行补充。
    info.event_channel = 0;
    info.element_size_bytes = symbol.element_size_bytes;
    info.element_count = element_count.Value();
    info.is_array = !symbol.dimensions.empty();
    return detail::MakeMeasurementOk(info);
}

MeasurementResult<MeasurementValue> A2lMeasurementDatabase::ToPhysical(
    std::string_view name, BytesView raw) const {
    const a2l::Result<a2l::PhysicalValue> res =
        m_database_.ToPhysical(name, a2l::BytesView{raw});
    if (!res.HasValue()) {
        return ToCoreError<MeasurementValue>(res.ErrorInfo());
    }
    // a2l::PhysicalValue 与核心 MeasurementValue 是同形 variant。
    return detail::MakeMeasurementOk(ToCorePhysicalValue(res.Value()));
}

MeasurementResult<MeasurementValue> A2lMeasurementDatabase::ToPhysicalElement(
    std::string_view name, std::uint64_t element_index, BytesView raw) const {
    const a2l::Result<a2l::PhysicalValue> res =
        m_database_.ToPhysicalElement(name, element_index, a2l::BytesView{raw});
    if (!res.HasValue()) {
        return ToCoreError<MeasurementValue>(res.ErrorInfo());
    }
    return detail::MakeMeasurementOk(ToCorePhysicalValue(res.Value()));
}

}  // namespace calmcar::xcp