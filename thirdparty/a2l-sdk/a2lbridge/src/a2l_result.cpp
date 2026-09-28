// =============================================================================
// a2l_result.cpp —— 枚举名称映射（B-19）
// =============================================================================

#include "libxcp/a2l/a2l_result.hpp"

namespace calmcar::xcp::a2l {

const char* ToString(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Ok:
            return "Ok";
        case ErrorCode::NotReady:
            return "NotReady";
        case ErrorCode::NotFound:
            return "NotFound";
        case ErrorCode::AmbiguousName:
            return "AmbiguousName";
        case ErrorCode::UnsupportedAddressingMode:
            return "UnsupportedAddressingMode";
        case ErrorCode::AddressOverflow:
            return "AddressOverflow";
        case ErrorCode::UnsupportedDataType:
            return "UnsupportedDataType";
        case ErrorCode::UnsupportedConversion:
            return "UnsupportedConversion";
        case ErrorCode::ConversionNotInvertible:
            return "ConversionNotInvertible";
        case ErrorCode::ConversionOutOfRange:
            return "ConversionOutOfRange";
        case ErrorCode::RawSizeMismatch:
            return "RawSizeMismatch";
        case ErrorCode::InvalidLayout:
            return "InvalidLayout";
        case ErrorCode::UnsupportedOperation:
            return "UnsupportedOperation";
        case ErrorCode::BadArgument:
            return "BadArgument";
        case ErrorCode::ParseFailed:
            return "ParseFailed";
        case ErrorCode::IoError:
            return "IoError";
        case ErrorCode::AbiMismatch:
            return "AbiMismatch";
        case ErrorCode::Internal:
            return "Internal";
    }
    return "Unknown";
}

const char* ToString(Severity severity) noexcept {
    switch (severity) {
        case Severity::Info:
            return "Info";
        case Severity::Warning:
            return "Warning";
        case Severity::Error:
            return "Error";
    }
    return "Unknown";
}

const char* ToString(Phase phase) noexcept {
    switch (phase) {
        case Phase::Load:
            return "Load";
        case Phase::Query:
            return "Query";
        case Phase::Address:
            return "Address";
        case Phase::Conversion:
            return "Conversion";
        case Phase::Layout:
            return "Layout";
        case Phase::Compare:
            return "Compare";
        case Phase::Runtime:
            return "Runtime";
    }
    return "Unknown";
}

}  // namespace calmcar::xcp::a2l
