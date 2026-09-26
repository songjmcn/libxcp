// =============================================================================
// a2l_bridge.cpp —— 桥接层门面（§4.5 / B-16/B-19/B-20）
//
// 职责：SDK 生命周期（CreateDoc/Release + ABI 校验）、同步/异步装载编排、
// 快照发布（B-20）、运行时参数比对（B-16）。异常绝不越出本层。
// =============================================================================

#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "a2l_database_impl.hpp"
#include "a2l_dto_map.hpp"
#include "libxcp/a2l/a2l_bridge.hpp"

namespace calmcar::xcp::a2l {
namespace {

Error BridgeError(ErrorCode code, Phase phase, std::string message) {
    Error e;
    e.code = code;
    e.phase = phase;
    e.severity = Severity::Error;
    e.message = std::move(message);
    return e;
}

}  // namespace

/**
 * @struct A2lBridge::Impl
 * @brief 门面私有实现（Pimpl；持有 SDK 文档与已发布快照）
 */
struct A2lBridge::Impl {
    /// @brief 释放 SDK 文档对象（对称 Release，禁止跨 DLL delete）
    ~Impl() {
        if (doc != nullptr) {
            doc->Release();
        }
    }

    /// @brief 创建 SDK 文档并做 ABI 运行时校验（R5）
    bool Create(std::string* error_out) {
        doc = liba2l::CreateDoc(liba2l::kLibA2lAbiVersion);
        if (doc == nullptr) {
            *error_out = "liba2l ABI 版本不匹配或工厂失败（期望 " +
                         std::to_string(liba2l::kLibA2lAbiVersion) + "）";
            return false;
        }
        return true;
    }

    /**
     * @brief 从已加载的 SDK 文档构建并发布不可变快照（B-20）
     * @return 成功返回 true；失败填充 last_error 且保持未发布状态
     */
    bool PublishSnapshot(bool require_if_data) {
        std::vector<liba2l::SymbolDto> symbols;
        const liba2l::ErrorCode rc = doc->ListSymbols(&symbols);
        if (rc != liba2l::ErrorCode::kOk) {
            last_error = detail::MapSdkError(rc, Phase::Load, "枚举符号失败");
            return false;
        }
        database = detail::MakeDatabase(std::move(symbols));

        Result<IfDataXcpInfo> xcp =
            detail::BuildIfDataXcp(*doc, require_if_data);
        if (!xcp.HasValue()) {
            last_error = xcp.TakeError();
            database.reset();
            return false;
        }
        xcp_info = std::move(xcp.Value());
        detail::ResolveDaqListSymbols(&xcp_info->static_daq_lists, *database);
        // 快照至此完整：database/xcp_info 同时可见（无半发布窗口——
        // 本函数仅在 Load 成功路径末尾被调用一次）
        ready = true;
        return true;
    }

    liba2l::IDoc* doc = nullptr;                        ///< SDK 文档（拥有）
    std::unique_ptr<detail::A2lDatabaseImpl> database;  ///< 不可变快照（B-20）
    std::optional<IfDataXcpInfo> xcp_info;              ///< IF_DATA 快照
    bool ready = false;                                 ///< 快照是否已发布
    int progress = 0;                                   ///< 最近进度百分比
    Error last_error{};                                 ///< 最近错误
    std::function<void(Result<void>)> user_cb;          ///< 异步完成的用户回调
    bool require_if_data_xcp = true;  ///< 装载选项缓存（回调内用）
};

Result<std::unique_ptr<A2lBridge>> A2lBridge::Load(const std::string& file_path,
                                                   const LoadOptions& options) {
    std::unique_ptr<Impl> impl(new (std::nothrow) Impl());
    if (impl == nullptr) {
        return BridgeError(ErrorCode::Internal, Phase::Load, "内存分配失败");
    }
    std::string create_error;
    if (!impl->Create(&create_error)) {
        return BridgeError(ErrorCode::AbiMismatch, Phase::Load, create_error);
    }
    // 批次10：B-17 active_module 与 B-18 include 越根开关必须在 Load 前生效
    impl->doc->SetActiveModule(options.active_module);
    impl->doc->AllowIncludeOutsideRoot(options.allow_include_outside_root);
    const liba2l::ErrorCode rc =
        impl->doc->Load(file_path, options.module_information_only);
    if (rc != liba2l::ErrorCode::kOk) {
        // SDK 侧 noexcept：LastError() 返回内部缓冲的 const char*，拷贝即安全
        const char* sdk_text = impl->doc->LastError();
        std::string sdk_message = sdk_text != nullptr ? sdk_text : "";
        Error e = detail::MapSdkError(
            rc, Phase::Load,
            sdk_message.empty() ? "SDK 装载失败" : std::move(sdk_message));
        e.path = file_path;
        // B-19：解析失败时结构化行号（SDK 无信息时为 0 → 保持 nullopt）
        if (const std::uint32_t line = impl->doc->LastErrorLine(); line > 0) {
            e.line = line;
        }
        return e;
    }
    impl->progress = 100;
    if (!impl->PublishSnapshot(options.require_if_data_xcp)) {
        return Error(impl->last_error);
    }

    std::unique_ptr<A2lBridge> bridge(new A2lBridge());
    bridge->m_impl_ = std::move(impl);
    return bridge;
}

Result<std::unique_ptr<A2lBridge>> A2lBridge::LoadAsync(
    const std::string& file_path, const LoadOptions& options,
    std::function<void(Result<void>)> ready_callback) {
    // SDK 契约：LoadAsync 受理时即拷贝路径与选项进内部线程状态，引用参数
    // 无需在此额外续命（见 Doc::LoadAsync 的 async_path_ 赋值）
    std::unique_ptr<Impl> impl(new (std::nothrow) Impl());
    if (impl == nullptr) {
        return BridgeError(ErrorCode::Internal, Phase::Load, "内存分配失败");
    }
    std::string create_error;
    if (!impl->Create(&create_error)) {
        return BridgeError(ErrorCode::AbiMismatch, Phase::Load, create_error);
    }

    // 指针借用给回调线程；所有权在 move 进 bridge 后仍指向同一 Impl 对象
    Impl* raw = impl.get();
    std::unique_ptr<A2lBridge> bridge(new A2lBridge());
    bridge->m_impl_ = std::move(impl);

    // SDK 契约：LoadAsync 立即受理并在内部线程完成解析；completed_cb 可能
    // 运行在 SDK 线程。桥接层在回调里发布快照（B-20）后转发用户回调。
    raw->user_cb = std::move(ready_callback);
    raw->require_if_data_xcp = options.require_if_data_xcp;
    // 批次10：B-17/B-18 选项在受理前生效（SDK 受理时拷贝配置进线程态）
    raw->doc->SetActiveModule(options.active_module);
    raw->doc->AllowIncludeOutsideRoot(options.allow_include_outside_root);
    const liba2l::ErrorCode start = raw->doc->LoadAsync(
        file_path, options.module_information_only, nullptr, [raw](int code) {
            Result<void> outcome = [&]() -> Result<void> {
                if (code != static_cast<int>(liba2l::ErrorCode::kOk)) {
                    Error e = detail::MapSdkError(
                        static_cast<liba2l::ErrorCode>(code), Phase::Load,
                        "异步解析失败");
                    // B-19：异步失败同样带出结构化行号
                    if (const std::uint32_t line = raw->doc->LastErrorLine();
                        line > 0) {
                        e.line = line;
                    }
                    return e;
                }
                raw->progress = 100;
                if (!raw->PublishSnapshot(raw->require_if_data_xcp)) {
                    return Error(raw->last_error);
                }
                return Result<void>{};
            }();
            if (raw->user_cb) {
                raw->user_cb(std::move(outcome));
            }
        });
    if (start != liba2l::ErrorCode::kOk) {
        raw->user_cb = nullptr;
        return Error(
            detail::MapSdkError(start, Phase::Load, "启动异步解析失败"));
    }

    return bridge;
}

A2lBridge::~A2lBridge() = default;

int A2lBridge::Progress() const noexcept { return m_impl_->progress; }

const IA2lDatabase* A2lBridge::Database() const noexcept {
    return m_impl_->ready ? m_impl_->database.get() : nullptr;
}

const IfDataXcpInfo* A2lBridge::XcpInfo() const noexcept {
    return m_impl_->ready && m_impl_->xcp_info.has_value()
               ? &m_impl_->xcp_info.value()
               : nullptr;
}

Result<std::unique_ptr<IDaqLayout>> A2lBridge::CreateDaqLayout() const {
    if (!m_impl_->ready || !m_impl_->xcp_info.has_value()) {
        return BridgeError(ErrorCode::NotReady, Phase::Runtime,
                           "快照未发布，无法冻结 DAQ 布局（B-20）");
    }
    const std::vector<DaqListLayout>& lists =
        m_impl_->xcp_info->static_daq_lists;
    if (lists.empty()) {
        return BridgeError(ErrorCode::NotFound, Phase::Layout,
                           "A2L 中不存在 STATIC DAQ_LIST");
    }
    // B-5：DYNAMIC 只解析能力不执行 —— 显式拒绝，不伪装成功
    if (m_impl_->xcp_info->daq.has_value() &&
        !m_impl_->xcp_info->daq->static_supported) {
        return BridgeError(
            ErrorCode::UnsupportedOperation, Phase::Layout,
            "动态 DAQ 首里程碑不支持（B-5 DynamicDaqNotImplemented）");
    }
    return detail::MakeDaqLayout(lists, m_impl_->database.get());
}

Result<std::vector<ParamDiscrepancy>> A2lBridge::CompareWithRuntime(
    const RuntimeXcpParams& runtime) const {
    if (!m_impl_->ready || !m_impl_->xcp_info.has_value()) {
        return BridgeError(ErrorCode::NotReady, Phase::Compare,
                           "IF_DATA 快照缺失，无法比对（B-20）");
    }
    const ProtocolLayerInfo& a2l = m_impl_->xcp_info->protocol_layer;
    std::vector<ParamDiscrepancy> out;

    auto add = [&](const char* name, std::string a2l_value,
                   std::string runtime_value, Severity sev,
                   const char* feature) {
        ParamDiscrepancy d;
        d.parameter_name = name;
        d.a2l_value = std::move(a2l_value);
        d.runtime_value = std::move(runtime_value);
        d.severity = sev;
        d.affected_feature = feature;
        out.push_back(std::move(d));
    };
    auto fmt = [](auto v) { return std::to_string(v); };

    // B-16：运行时为真值；差异只影响对应功能，Error 也只阻断该功能
    // B-16 分级：容量/能力取两端更保守值即可协商 → Warning（不是 Error）。
    // Error 仅限破坏协商的项（protocol major、ByteOrder、AG、DAQ 约束）。
    if (runtime.max_cto != a2l.max_cto) {
        add("MAX_CTO", fmt(a2l.max_cto), fmt(runtime.max_cto),
            Severity::Warning, "传输分片");
    }
    if (runtime.max_dto != a2l.max_dto) {
        add("MAX_DTO", fmt(a2l.max_dto), fmt(runtime.max_dto),
            Severity::Warning, "DTO 组包");
    }
    if (runtime.byte_order != a2l.byte_order) {
        add("BYTE_ORDER",
            a2l.byte_order == ByteOrder::MsbLast ? "MSB_LAST" : "MSB_FIRST",
            runtime.byte_order == ByteOrder::MsbLast ? "MSB_LAST" : "MSB_FIRST",
            Severity::Error, "多字节数值解释");
    }
    if (runtime.address_granularity != a2l.address_granularity) {
        add("ADDRESS_GRANULARITY", fmt(AgToBytes(a2l.address_granularity)),
            fmt(AgToBytes(runtime.address_granularity)), Severity::Error,
            "地址计算");
    }
    // B-16：protocol major 不兼容 → Error（阻断协议协商）；仅 minor 差异 →
    // Warning（同 major 可协商兼容）。t1..t7 不参与比对（Master 超时配置）。
    const std::uint16_t a2l_major =
        static_cast<std::uint16_t>(a2l.version >> 8);
    const std::uint16_t rt_major =
        static_cast<std::uint16_t>(runtime.protocol_version >> 8);
    if (a2l_major != rt_major) {
        add("PROTOCOL_VERSION_MAJOR", fmt(a2l_major), fmt(rt_major),
            Severity::Error, "协议协商");
    } else if (runtime.protocol_version != a2l.version) {
        add("PROTOCOL_VERSION_MINOR", fmt(a2l.version),
            fmt(runtime.protocol_version), Severity::Warning, "协议协商");
    }
    if (runtime.has_daq !=
        (a2l.max_dto > 0 && m_impl_->xcp_info->daq.has_value() &&
         m_impl_->xcp_info->daq->static_supported)) {
        add("DAQ_SUPPORT",
            m_impl_->xcp_info->daq && m_impl_->xcp_info->daq->static_supported
                ? "true"
                : "false",
            runtime.has_daq ? "true" : "false", Severity::Warning, "DAQ 通道");
    }
    // t1..t7 明确不参与比对（§6.2）
    return out;
}

std::string A2lBridge::LastError() const {
    if (!m_impl_->ready && m_impl_->last_error.message.empty()) {
        return {};
    }
    std::string text = ToString(m_impl_->last_error.code);
    text += ": ";
    text += m_impl_->last_error.message;
    if (!m_impl_->last_error.symbol.empty()) {
        text += " [";
        text += m_impl_->last_error.symbol;
        text += "]";
    }
    return text;
}

A2lBridge::A2lBridge() : m_impl_(new Impl()) {}

}  // namespace calmcar::xcp::a2l
