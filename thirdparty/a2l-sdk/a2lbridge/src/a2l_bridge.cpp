// =============================================================================
// a2l_bridge.cpp —— 桥接层门面（§4.5 / B-16/B-19/B-20）
//
// 职责：SDK 生命周期（CreateDoc/Release + ABI 校验）、同步/异步装载编排、
// 快照发布（B-20）、运行时参数比对（B-16）。异常绝不越出本层。
// =============================================================================

#include <atomic>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
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

/// @brief 快照发布前对外可见的进度上限（批次13；100 只属于"已发布"）
constexpr int kProgressPublishingCap = 99;

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
        // 批次13（B-12）：STRUCTURE/INSTANCE 元数据快照。SDK 无结构体时返回
        // 空向量，不是错误——"没有结构体"与"没采集到"必须可区分地如实反映。
        std::vector<liba2l::StructInfoDto> structs;
        const liba2l::ErrorCode src_rc = doc->ListStructures(&structs);
        if (src_rc != liba2l::ErrorCode::kOk) {
            last_error = detail::MapSdkError(src_rc, Phase::Load,
                                             "枚举结构体元数据失败");
            return false;
        }
        // 批次13（R3 接线）：SDK 给出了 RECORD_LAYOUT 类别的 CHARACTERISTIC
        // 才进判定器表；无类别的符号不进（"无信息"≠"不可执行"）。
        std::unordered_map<std::string, RecordLayoutInfo> record_layouts;
        for (const liba2l::SymbolDto& dto : symbols) {
            RecordLayoutInfo info;
            if (detail::ConvertRecordLayoutInfo(dto, &info)) {
                record_layouts.emplace(
                    detail::MakeQualifiedKey(dto.module_name, dto.name),
                    std::move(info));
            }
        }
        database = detail::MakeDatabase(std::move(symbols), std::move(structs),
                                        std::move(record_layouts));

        Result<IfDataXcpInfo> xcp =
            detail::BuildIfDataXcp(*doc, require_if_data);
        if (!xcp.HasValue()) {
            last_error = xcp.TakeError();
            database.reset();
            return false;
        }
        xcp_info = std::move(xcp.Value());
        detail::ResolveDaqListSymbols(&xcp_info->static_daq_lists, *database);
        // 批次15（F6）：装配加载期告警。① 数据库构造时跳过的同名 STRUCTURE；
        // ② 未声明 FIRST_PID 的 DAQ_LIST —— 它们的解码只能按"列表号回退"，
        //    属弱权威（F1/D1），必须让用户知情而不是以为拿到了取证布局。
        load_warnings = database->LoadWarnings();
        for (const DaqListLayout& list : xcp_info->static_daq_lists) {
            if (list.first_pid.has_value()) {
                continue;
            }
            LoadWarning warning;
            warning.code = ErrorCode::UnsupportedOperation;
            warning.phase = Phase::Layout;
            warning.subject = "DAQ_LIST " + std::to_string(list.number);
            warning.message =
                "A2L 未声明 FIRST_PID，解码按列表号回退（未经实际取证，弱权威）";
            load_warnings.push_back(std::move(warning));
        }
        // 快照至此完整：database/xcp_info 同时可见（无半发布窗口——
        // 本函数仅在 Load 成功路径末尾被调用一次）
        ready.store(true, std::memory_order_release);
        return true;
    }

    liba2l::IDoc* doc = nullptr;                        ///< SDK 文档（拥有）
    std::unique_ptr<detail::A2lDatabaseImpl> database;  ///< 不可变快照（B-20）
    std::optional<IfDataXcpInfo> xcp_info;              ///< IF_DATA 快照
    /// @brief 加载期告警（批次15，F6；发布时一次性装配，之后只读）
    std::vector<LoadWarning> load_warnings;
    /**
     * @brief 快照是否已发布（B-20）
     * @details 异步加载时由 SDK 工作线程写、调用方线程读（Progress/Database
     *          轮询），故必须是 atomic 且用 release/acquire 配对——`ready`
     *          读到 true 时，`database`/`xcp_info` 的写入对该线程一定可见。
     */
    std::atomic<bool> ready{false};
    Error last_error{};                         ///< 最近错误
    std::function<void(Result<void>)> user_cb;  ///< 异步完成的用户回调
    bool require_if_data_xcp = true;            ///< 装载选项缓存（回调内用）
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
        // B-18 批次12：include 类错误带出结构化链（其余错误 SDK 返回空链，
        // 故空链时保持字段为空 vector）
        std::vector<std::string> chain;
        if (impl->doc->LastErrorChain(&chain) == liba2l::ErrorCode::kOk &&
            !chain.empty()) {
            e.include_chain = std::move(chain);
        }
        return e;
    }
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
                    // 批次12：与同步分支对称——message 取 SDK 文本（原先硬
                    // 编码"异步解析失败"丢失了定位信息），固定语义标记进
                    // cause； B-19 下业务仍只判 ErrorCode。
                    const char* sdk_text = raw->doc->LastError();
                    std::string sdk_message =
                        sdk_text != nullptr ? sdk_text : "";
                    Error e = detail::MapSdkError(
                        static_cast<liba2l::ErrorCode>(code), Phase::Load,
                        sdk_message.empty() ? "SDK 异步装载失败"
                                            : std::move(sdk_message));
                    e.cause = "异步解析失败（completed_cb 携带）";
                    // B-19：异步失败同样带出结构化行号
                    if (const std::uint32_t line = raw->doc->LastErrorLine();
                        line > 0) {
                        e.line = line;
                    }
                    // B-18 批次12：异步分支同样带出结构化 include 链
                    std::vector<std::string> chain;
                    if (raw->doc->LastErrorChain(&chain) ==
                            liba2l::ErrorCode::kOk &&
                        !chain.empty()) {
                        e.include_chain = std::move(chain);
                    }
                    return e;
                }
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

int A2lBridge::Progress() const noexcept {
    // 批次13（R4）：加载中透传 SDK 的**阶段进度**（受理 5 → include 预扫描 25
    // → 上游解析完成 70 → 快照构建完成 100）。快照未发布前上限 99——桥接层
    // 还要组装领域快照与 IF_DATA，此时对外宣称 100 会与 B-20"发布前不可见"
    // 自相矛盾；发布后恒 100。
    if (m_impl_->ready.load(std::memory_order_acquire)) {
        return 100;
    }
    if (m_impl_->doc == nullptr) {
        return 0;
    }
    const int stage = m_impl_->doc->Progress();
    return stage > kProgressPublishingCap ? kProgressPublishingCap : stage;
}

const IA2lDatabase* A2lBridge::Database() const noexcept {
    return m_impl_->ready ? m_impl_->database.get() : nullptr;
}

const std::vector<LoadWarning>& A2lBridge::ListLoadWarnings() const noexcept {
    // 加载中/失败时该向量尚未装配（PublishSnapshot 是唯一写入点）→ 空表，
    // 与 Database() 的 NotReady 语义一致：不报错，但也不给半份告警
    return m_impl_->load_warnings;
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
    // 批次14（B-6）：A2L 侧 PREDEFINED 布局 → 快照来源标记 A2lPredefined。
    // 批次15（F1/D1）：A2L **声明了 FIRST_PID 的列表**按其生成 PID 路由
    // （docs/XCP_1.3.0_document.md L2225：绝对 ODT 号 = FIRST_PID + 相对 ODT
    // 号）；未声明的列表不进路由表，解码对其仍保留"列表号回退"旧口径。
    // 关键是**路由优先于回退**：列表号与其它列表的 PID 同属一个编号空间，
    // 有声明时仍让回退参与匹配就会用错列表的布局且全程无报错（批次15 §1 L1）。
    DaqLayoutSnapshot snapshot;
    snapshot.source = DaqLayoutSource::A2lPredefined;
    snapshot.generation = 0U;  // A2L 侧无配置代际概念
    snapshot.lists = lists;
    for (const DaqListLayout& l : snapshot.lists) {
        if (!l.first_pid.has_value()) {
            continue;
        }
        for (std::size_t i = 0; i < l.odts.size(); ++i) {
            snapshot.routes.push_back(DaqOdtRoute{
                static_cast<std::uint8_t>(*l.first_pid + i), l.number,
                static_cast<std::uint8_t>(i)});
        }
    }
    return detail::MakeDaqLayout(std::move(snapshot), m_impl_->database.get());
}

Result<std::unique_ptr<IDaqLayout>> A2lBridge::CreateDaqLayoutFromLedger(
    const std::vector<DaqLedgerEntryView>& ledger,
    std::uint32_t generation) const {
    if (!m_impl_->ready || m_impl_->database == nullptr) {
        return BridgeError(ErrorCode::NotReady, Phase::Runtime,
                           "快照未发布，无法按账本冻结布局（B-20）");
    }
    // B-5 不变：DYNAMIC 仍显式拒绝（账本只覆盖"可配置 STATIC"的下发事实）
    if (m_impl_->xcp_info.has_value() && m_impl_->xcp_info->daq.has_value() &&
        !m_impl_->xcp_info->daq->static_supported) {
        return BridgeError(
            ErrorCode::UnsupportedOperation, Phase::Layout,
            "动态 DAQ 首里程碑不支持（B-5 DynamicDaqNotImplemented）");
    }
    // 账本缺失时**不**降级到 A2L 顺序：空快照在 Decode 处统一 InvalidLayout
    DaqLayoutSnapshot snapshot =
        detail::BuildSnapshotFromLedger(ledger, generation, *m_impl_->database);
    return detail::MakeDaqLayout(std::move(snapshot), m_impl_->database.get());
}

Result<std::unique_ptr<IDaqLayout>> A2lBridge::CreateDaqLayoutFromEcuReadback(
    const std::vector<DaqReadbackEntry>& readback,
    const std::vector<DaqListPid>& list_pids, std::uint32_t generation) const {
    if (!m_impl_->ready || m_impl_->database == nullptr) {
        return BridgeError(ErrorCode::NotReady, Phase::Runtime,
                           "快照未发布，无法按回读冻结布局（B-20）");
    }
    if (m_impl_->xcp_info.has_value() && m_impl_->xcp_info->daq.has_value() &&
        !m_impl_->xcp_info->daq->static_supported) {
        return BridgeError(
            ErrorCode::UnsupportedOperation, Phase::Layout,
            "动态 DAQ 首里程碑不支持（B-5 DynamicDaqNotImplemented）");
    }
    // PREDEFINED 列表的取证通路：回读为空同样只产出空快照（B-6 不猜）
    DaqLayoutSnapshot snapshot = detail::BuildSnapshotFromEcuReadback(
        readback, list_pids, generation, *m_impl_->database);
    return detail::MakeDaqLayout(std::move(snapshot), m_impl_->database.get());
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

    // ---- 批次14（T14-11）：B-16 的 DAQ 侧比对（三类均为 Error）----
    //
    // 这三项决定"PID 怎么解释、Entry 怎么对齐"，与协商结果不一致时解码会
    // 系统性错位，因此按 B-16 口径记 Error（只阻断 DAQ 功能，不影响读写）。
    // 运行时字段为 nullopt 表示调用方没查（GET_DAQ_PROCESSOR_INFO /
    // GET_DAQ_RESOLUTION_INFO 都是 Optional）→ **跳过比对**，不拿默认值凑。
    if (m_impl_->xcp_info->daq.has_value()) {
        const DaqInfo& daq = *m_impl_->xcp_info->daq;
        // 领域枚举 → A2L/XCP 原始码（ADDRESS_EXTENSION 的 3=DAQ 不是序号 2，
        // 必须显式还原后再比，否则 PerDaq 会被误判为不一致）
        const auto ext_raw = [](DaqInfo::AddrExtMode mode) {
            switch (mode) {
                case DaqInfo::AddrExtMode::Free:
                    return 0U;
                case DaqInfo::AddrExtMode::PerOdt:
                    return 1U;
                case DaqInfo::AddrExtMode::PerDaq:
                    return 3U;
            }
            return 0xFFU;  // 不可达（枚举穷尽）；越界值一律判不一致
        };
        const auto idf_raw = [](DaqInfo::IdFieldType type) {
            return static_cast<unsigned>(type);  // 0..3 与原始码同序
        };
        if (runtime.daq_identification_field_type.has_value()) {
            const unsigned a2l_value = idf_raw(daq.identification_field_type);
            if (a2l_value != *runtime.daq_identification_field_type) {
                add("IDENTIFICATION_FIELD_TYPE", std::to_string(a2l_value),
                    std::to_string(*runtime.daq_identification_field_type),
                    Severity::Error, "DTO PID 解释");
            }
        }
        if (runtime.daq_address_extension_mode.has_value()) {
            const unsigned a2l_value = ext_raw(daq.address_extension_mode);
            if (a2l_value != *runtime.daq_address_extension_mode) {
                add("ADDRESS_EXTENSION_MODE", std::to_string(a2l_value),
                    std::to_string(*runtime.daq_address_extension_mode),
                    Severity::Error, "ODT 地址扩展布局");
            }
        }
        if (runtime.daq_odt_entry_min_size_bytes.has_value()) {
            const unsigned a2l_value = daq.odt_entry_min_size_bytes;
            if (a2l_value != *runtime.daq_odt_entry_min_size_bytes) {
                add("GRANULARITY_ODT_ENTRY_SIZE_DAQ", std::to_string(a2l_value),
                    std::to_string(*runtime.daq_odt_entry_min_size_bytes),
                    Severity::Error, "ODT Entry 打包粒度");
            }
        }
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
