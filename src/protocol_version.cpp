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

CapabilitySet CapabilitySetFor(const XcpProtocolVersion& effective) noexcept {
    CapabilitySet caps;

    // 无效版本（Major 为 0，即"未知"占位）：不得假定任何能力。
    // 这与 VersionPolicy 的"未配置"语义一致——宁可拒绝，不可静默发错报文。
    if (!effective.IsValid()) {
        return caps;
    }

    // V1.2 基线：GET_VERSION 与 Level-1（0xC0）命令族。
    // 由 thirdparty/XCPliteV1.2 实测确认该对端具备这两项。
    if (!(effective < kProtocolVersion12)) {
        caps.get_version = true;
        caps.level1_commands = true;
    }

    // V1.3 追加按 DAQ List / Event 逐项查询的扩展接口。
    // V1.2 对端不保证具备，必须由运行时探测或上层显式尝试决定。
    if (!(effective < kProtocolVersion13)) {
        caps.extended_daq_queries = true;
    }

    // V1.5 追加 DAQ Packed 模式（§4.1.10，本库尚未提供编码路径，
    // 此处只登记协议层可用性，不代表本库已实现）。
    if (!(effective < kProtocolVersion15)) {
        caps.daq_packed_mode = true;
    }

    // Optional TL Subcommand 本库基础 Profile 一律不实现，恒为 false
    // （见表中语义：false 表示"不得使用"）。
    caps.optional_tl_subcommands = false;
    return caps;
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
    // 下限自 B-T1 §4.3 起为 V1.2：AsamV14 兼容 Profile 的对端
    // （thirdparty/XCPliteV1.2）上报 PL 0x0102，必须被接受。
    if (protocol_version.has_value() &&
        protocol_version->IsValid() && *protocol_version < kProtocolVersion12) {
        return "VersionPolicy.protocol_version " + protocol_version->ToString() +
               " 低于支持下限 " + kProtocolVersion12.ToString();
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