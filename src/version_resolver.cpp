/**
 * @file version_resolver.cpp
 * @brief 协议层与传输层版本裁决实现（批次 B1）。
 */

#include "libxcp/version_resolver.hpp"

namespace calmcar::xcp {

namespace {

/// @brief 判断「版本上报值」与「支持上限」的关系并渲染冲突描述
/// @param layer_name 层名称，用于描述文本
/// @param reported 对端上报的版本
/// @param cap 本库支持上限
/// @return 对端版本高于上限时的描述文本；否则返回空
std::string DescribeCapConflict(const std::string& layer_name,
                                const XcpProtocolVersion& reported,
                                const XcpProtocolVersion& cap) {
    if (!(cap < reported)) {
        return {};
    }
    // 方案 D1：同 Major 且高于支持上限 → 封顶运行并告警；跨 Major 同样封顶，
    // 但描述中明确标出，便于调用方判断风险等级。
    const bool same_major = (reported.major == cap.major);
    const std::string kind = same_major ? "同主版本、次版本高于上限" : "主版本高于支持范围";
    return layer_name + "上报版本 " + reported.ToString() + " 高于支持上限 " +
           cap.ToString() + "（" + kind + "），已按上限封顶运行";
}

}  // namespace

VersionEvidence VersionResolver::ResolveLayer(
    const std::string& layer_name, std::uint8_t connect_major,
    std::optional<XcpProtocolVersion> probe_version,
    std::optional<XcpProtocolVersion> configured,
    std::optional<XcpProtocolVersion> a2l, XcpProtocolVersion support_cap,
    std::vector<std::string>& conflicts) {
    VersionEvidence evidence;

    // 优先级（方案 §4.2）：GET_VERSION 运行时 > 调用方配置 > A2L 静态 >
    // CONNECT 主版本。CONNECT 只提供主版本，永远是置信度最低的一档。
    if (probe_version.has_value() && probe_version->IsValid()) {
        // 运行时确认：Minor 可信，置信度最高
        evidence.reported = *probe_version;
        evidence.source = VersionSource::GetVersion;
        evidence.confidence = VersionConfidence::RuntimeConfirmed;
        evidence.minor_confirmed = true;
    } else if (configured.has_value() && configured->IsValid()) {
        // 调用方显式配置：Minor 由调用方负责，视为已知
        evidence.reported = *configured;
        evidence.source = VersionSource::Configured;
        evidence.confidence = VersionConfidence::Configured;
        evidence.minor_confirmed = true;
    } else if (a2l.has_value() && a2l->IsValid()) {
        // A2L 静态证据：Minor 由描述文件给定，但弱于运行时
        evidence.reported = *a2l;
        evidence.source = VersionSource::A2l;
        evidence.confidence = VersionConfidence::StaticEvidence;
        evidence.minor_confirmed = true;
    } else if (connect_major != 0U) {
        // 仅 CONNECT 主版本：Minor **未知且不可猜测**（方案 D1 硬约束）
        evidence.reported = XcpProtocolVersion{connect_major, 0U};
        evidence.source = VersionSource::ConnectMajor;
        evidence.confidence = VersionConfidence::MajorOnly;
        evidence.minor_confirmed = false;
    } else {
        // 无任何证据：不得伪造版本，保持 Unknown
        evidence.source = VersionSource::Unknown;
        evidence.confidence = VersionConfidence::Unknown;
        evidence.minor_confirmed = false;
        return evidence;
    }

    // 冲突记录（只记录、不覆盖）：运行时与配置不一致，或运行时与 A2L 不一致。
    // 用户仲裁（m03674）：**不提供静默模式**，冲突必须始终被记录在裁决结果中。
    if (probe_version.has_value() && probe_version->IsValid()) {
        if (configured.has_value() && configured->IsValid() &&
            !(*configured == *probe_version)) {
            conflicts.push_back(layer_name + "GET_VERSION 上报 " +
                                probe_version->ToString() + " 与调用方配置 " +
                                configured->ToString() +
                                " 不一致，采用运行时值");
        }
        if (a2l.has_value() && a2l->IsValid() && !(*a2l == *probe_version)) {
            conflicts.push_back(layer_name + "GET_VERSION 上报 " +
                                probe_version->ToString() + " 与 A2L 声明 " +
                                a2l->ToString() + " 不一致，采用运行时值");
        }
    }

    // 封顶判定：上报值高于支持上限时保留 reported 原值、置 capped 标志。
    // Capped 是**状态标志而非伪造的来源**（方案 §4.2）。
    if (support_cap < evidence.reported) {
        evidence.capped = true;
        conflicts.push_back(
            DescribeCapConflict(layer_name, evidence.reported, support_cap));
    }

    return evidence;
}

VersionResolver::Result VersionResolver::Resolve(const Inputs& inputs) {
    Result result;
    result.get_version_available = inputs.probe.IsAvailable();

    // GET_VERSION 可用时才有精确的 PL/TL 版本；不可用时两侧都为 nullopt，
    // 让各层各自回退到配置 / A2L / CONNECT 主版本。
    std::optional<XcpProtocolVersion> probe_pl;
    std::optional<XcpProtocolVersion> probe_tl;
    if (result.get_version_available) {
        probe_pl = XcpProtocolVersion{inputs.probe.response->protocol_major,
                                      inputs.probe.response->protocol_minor};
        probe_tl = XcpProtocolVersion{inputs.probe.response->transport_major,
                                      inputs.probe.response->transport_minor};
    }

    const VersionEvidence pl_evidence = ResolveLayer(
        "协议层", inputs.connect_protocol_major, probe_pl,
        inputs.policy.protocol_version, inputs.a2l_protocol_version,
        kProtocolVersion15, result.conflicts);
    const VersionEvidence tl_evidence = ResolveLayer(
        "传输层", inputs.connect_transport_major, probe_tl,
        inputs.policy.transport_version, inputs.a2l_transport_version,
        kTransportVersion15, result.conflicts);

    result.protocol.evidence = pl_evidence;
    result.transport.evidence = tl_evidence;

    // A2L 静态声明与最终采用的证据不一致时置标志位（快照消费方据此阻断
    // 依赖布局的功能）。仅比较"是否可用"的 A2L 值，不参与取值优先级。
    result.protocol.conflicts_with_a2l =
        inputs.a2l_protocol_version.has_value() &&
        inputs.a2l_protocol_version->IsValid() &&
        pl_evidence.IsAvailable() &&
        !(pl_evidence.reported == *inputs.a2l_protocol_version);
    result.transport.conflicts_with_a2l =
        inputs.a2l_transport_version.has_value() &&
        inputs.a2l_transport_version->IsValid() &&
        tl_evidence.IsAvailable() &&
        !(tl_evidence.reported == *inputs.a2l_transport_version);

    // 生效版本：Minor 未确认时进入 1.x-conservative——按 Major.0 作为能力
    // 判据标识，并置 conservative 标志。**这代表"能力集按保守交集执行"，
    // 不代表对端就是该 Minor**（方案 D1 明确定义）。
    if (!pl_evidence.IsAvailable()) {
        result.protocol.effective = XcpProtocolVersion{};
        result.protocol.conservative = false;
    } else if (!pl_evidence.minor_confirmed) {
        // 保守能力集：以支持范围下限作为能力口径。
        // B-T1 §4.3：下限自 V1.3 下调至 V1.2，故保守口径同步下移——
        // 保守模式的语义是"按支持范围内最低版本的能力集执行"，
        // 下限改变后若仍固定按 V1.3，会在对端实为 V1.2 时高估其能力。
        result.protocol.effective = kProtocolVersion12;
        result.protocol.conservative = true;
    } else if (kProtocolVersion15 < pl_evidence.reported) {
        result.protocol.effective = kProtocolVersion15;
        result.protocol.conservative = false;
    } else {
        result.protocol.effective = pl_evidence.reported;
        result.protocol.conservative = false;
    }

    if (!tl_evidence.IsAvailable()) {
        result.transport.effective = XcpProtocolVersion{};
        result.transport.conservative = false;
    } else if (!tl_evidence.minor_confirmed) {
        // 传输层保守化：Minor 未知时不得假定布局能力，按下限口径运行。
        result.transport.effective = XcpProtocolVersion{1, 0};
        result.transport.conservative = true;
    } else if (kTransportVersion15 < tl_evidence.reported) {
        result.transport.effective = kTransportVersion15;
        result.transport.conservative = false;
    } else {
        result.transport.effective = tl_evidence.reported;
        result.transport.conservative = false;
    }

    return result;
}

}  // namespace calmcar::xcp