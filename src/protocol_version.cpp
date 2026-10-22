/**
 * @file protocol_version.cpp
 * @brief 协议版本模型的字符串渲染实现（批次 B1）。
 */

#include "libxcp/protocol_version.hpp"

namespace calmcar::xcp {

std::string XcpProtocolVersion::ToString() const {
    // 规范 §3.1.1：高字节 Major、低字节 Minor，渲染为 "X.Y"
    return std::to_string(static_cast<unsigned>(major)) + "." +
           std::to_string(static_cast<unsigned>(minor));
}

std::string VersionPolicy::Validate() const {
    // 主版本 0 视为非法：XCP 规范自 V1.0 起，0.x 不构成合法版本声明。
    // 允许 0 会让"未配置"与"配置成 0.0"变得无法区分。
    if (protocol_version.has_value() && !protocol_version->IsValid()) {
        return "VersionPolicy.protocol_version 的主版本不能为 0"
               "（XCP 自 V1.0 起，0.x 不是合法版本声明）";
    }
    if (transport_version.has_value() && !transport_version->IsValid()) {
        return "VersionPolicy.transport_version 的主版本不能为 0"
               "（XCP 自 V1.0 起，0.x 不是合法版本声明）";
    }
    // 低于支持范围下限的显式声明会让后续能力裁决失去意义，直接拒绝。
    if (protocol_version.has_value() &&
        protocol_version->IsValid() && *protocol_version < kProtocolVersion13) {
        return "VersionPolicy.protocol_version " + protocol_version->ToString() +
               " 低于支持下限 " + kProtocolVersion13.ToString();
    }
    // 自相矛盾：要求探测却不允许探测往返。
    if (probe_get_version && !allow_version_probe_roundtrip) {
        return "VersionPolicy 自相矛盾：probe_get_version 为真但 "
               "allow_version_probe_roundtrip 为假";
    }
    return {};
}

std::string VersionEvidence::SourceName() const {
    switch (source) {
        case VersionSource::Unknown:
            return "Unknown";
        case VersionSource::GetVersion:
            return "GetVersion";
        case VersionSource::ConnectMajor:
            return "ConnectMajor";
        case VersionSource::Configured:
            return "Configured";
        case VersionSource::A2l:
            return "A2l";
    }
    return "Unknown";
}

std::string VersionEvidence::ConfidenceName() const {
    switch (confidence) {
        case VersionConfidence::Unknown:
            return "Unknown";
        case VersionConfidence::MajorOnly:
            return "MajorOnly";
        case VersionConfidence::Configured:
            return "Configured";
        case VersionConfidence::StaticEvidence:
            return "StaticEvidence";
        case VersionConfidence::RuntimeConfirmed:
            return "RuntimeConfirmed";
    }
    return "Unknown";
}

}  // namespace calmcar::xcp