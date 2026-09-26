// ============================================================================
// liba2l.dll 导出实现层（设计文档 §5.3.3b R1–R6）
//
// 本文件是 SDK 中唯一允许 #include <a2l/...> 的翻译单元：上游类型在此终结，
// 一律拷贝为自有 DTO 后跨边界返回。上游异常全部在此捕获并转为
// ErrorCode + LastError（R4）。
// ============================================================================

#include "liba2l/liba2l_api.hpp"

#include <algorithm>
#include <clocale>
#include <filesystem>
#include <memory>
#include <thread>
#include <unordered_map>
#include <utility>

// --- 上游 a2llib（仅此 TU 可见） ---
#include "a2l/a2lfile.h"
#include "a2l/a2lproject.h"
#include "a2l/a2lstructs.h"
#include "a2l/characteristic.h"
#include "a2l/compumethod.h"
#include "a2l/computab.h"
#include "a2l/compuvtab.h"
#include "a2l/measurement.h"
#include "a2l/module.h"
#include "a2l/unit.h"
#include "a2l/xcp/commonparameters.h"
#include "a2l/xcp/daq.h"
#include "a2l/xcp/daqlist.h"
#include "a2l/xcp/protocollayer.h"
#include "a2l/xcp/timestamp.h"
#include "a2l/xcp/xcpdatablock.h"
#include "a2l/xcp/xcponudpip.h"

namespace liba2l {

namespace {

// ----------------------------------------------------------------------------
// 枚举映射（B-3：显式一对一，未列出的上游值一律落 kUnknown，禁止猜默认宽度）
// ----------------------------------------------------------------------------

/** 上游数据类型 → DTO。 */
AsamDataTypeDto MapDataType(a2l::A2lDataType t) {
    switch (t) {
        case a2l::A2lDataType::UBYTE:
            return AsamDataTypeDto::kUByte;
        case a2l::A2lDataType::SBYTE:
            return AsamDataTypeDto::kSByte;
        case a2l::A2lDataType::UWORD:
            return AsamDataTypeDto::kUWord;
        case a2l::A2lDataType::SWORD:
            return AsamDataTypeDto::kSWord;
        case a2l::A2lDataType::ULONG:
            return AsamDataTypeDto::kULong;
        case a2l::A2lDataType::SLONG:
            return AsamDataTypeDto::kSLong;
        case a2l::A2lDataType::A_UINT64:
            return AsamDataTypeDto::kAUint64;
        case a2l::A2lDataType::A_INT64:
            return AsamDataTypeDto::kAInt64;
        case a2l::A2lDataType::FLOAT16_IEEE:
            return AsamDataTypeDto::kFloat16Ieee;
        case a2l::A2lDataType::FLOAT32_IEEE:
            return AsamDataTypeDto::kFloat32Ieee;
        case a2l::A2lDataType::FLOAT64_IEEE:
            return AsamDataTypeDto::kFloat64Ieee;
        case a2l::A2lDataType::UNKNOWN:
        default:
            return AsamDataTypeDto::kUnknown;
    }
}

/** ASAM 类型 → 单元素字节宽（B-3 显式表；kUnknown → 0 由上层判错）。 */
std::uint8_t TypeWidthBytes(AsamDataTypeDto t) {
    switch (t) {
        case AsamDataTypeDto::kUByte:
        case AsamDataTypeDto::kSByte:
            return 1;
        case AsamDataTypeDto::kUWord:
        case AsamDataTypeDto::kSWord:
        case AsamDataTypeDto::kFloat16Ieee:
            return 2;
        case AsamDataTypeDto::kULong:
        case AsamDataTypeDto::kSLong:
        case AsamDataTypeDto::kFloat32Ieee:
            return 4;
        case AsamDataTypeDto::kAUint64:
        case AsamDataTypeDto::kAInt64:
        case AsamDataTypeDto::kFloat64Ieee:
            return 8;
        case AsamDataTypeDto::kUnknown:
        default:
            return 0;
    }
}

/** CHARACTERISTIC 类型 → DTO。 */
CharacteristicTypeDto MapCharType(a2l::A2lCharacteristicType t) {
    switch (t) {
        case a2l::A2lCharacteristicType::VALUE:
            return CharacteristicTypeDto::kValue;
        case a2l::A2lCharacteristicType::CURVE:
            return CharacteristicTypeDto::kCurve;
        case a2l::A2lCharacteristicType::MAP:
            return CharacteristicTypeDto::kMap;
        case a2l::A2lCharacteristicType::CUBOID:
            return CharacteristicTypeDto::kCuboid;
        case a2l::A2lCharacteristicType::CUBE_4:
            return CharacteristicTypeDto::kCube4;
        case a2l::A2lCharacteristicType::CUBE_5:
            return CharacteristicTypeDto::kCube5;
        case a2l::A2lCharacteristicType::VAL_BLK:
            return CharacteristicTypeDto::kValBlk;
        case a2l::A2lCharacteristicType::ASCII:
            return CharacteristicTypeDto::kAscii;
        case a2l::A2lCharacteristicType::UNKNOWN:
        default:
            return CharacteristicTypeDto::kNone;
    }
}

/** 字节序 → DTO（混合字内/字间序明确报 kUnknown，不做近似）。 */
ByteOrderDto MapByteOrder(a2l::A2lByteOrder t) {
    switch (t) {
        case a2l::A2lByteOrder::MSB_LAST:
            return ByteOrderDto::kMsbLast;
        case a2l::A2lByteOrder::MSB_FIRST:
            return ByteOrderDto::kMsbFirst;
        default:
            return ByteOrderDto::kUnknown;
    }
}

/** 存储顺序 → DTO。 */
ArrayOrderDto MapLayout(a2l::A2lLayout t) {
    switch (t) {
        case a2l::A2lLayout::ROW_DIR:
            return ArrayOrderDto::kRowDir;
        case a2l::A2lLayout::COLUMN_DIR:
            return ArrayOrderDto::kColumnDir;
        default:
            return ArrayOrderDto::kUnknown;
    }
}

/** 转换类别 → DTO（FORM 归入 kFormulaUnsupported，B-8 仅文本不执行）。 */
ConversionKindDto MapConversionType(a2l::A2lConversionType t) {
    switch (t) {
        case a2l::A2lConversionType::IDENTICAL:
            return ConversionKindDto::kIdentical;
        case a2l::A2lConversionType::LINEAR:
            return ConversionKindDto::kLinear;
        case a2l::A2lConversionType::RAT_FUNC:
            return ConversionKindDto::kRatFunc;
        case a2l::A2lConversionType::TAB_INTP:
            return ConversionKindDto::kTabIntp;
        case a2l::A2lConversionType::TAB_NOINTP:
            return ConversionKindDto::kTabNoIntp;
        case a2l::A2lConversionType::TAB_VERB:
            return ConversionKindDto::kTabVerb;
        case a2l::A2lConversionType::FORM:
            return ConversionKindDto::kFormulaUnsupported;
        case a2l::A2lConversionType::UNKNOWN:
        default:
            return ConversionKindDto::kNone;
    }
}

/** 传输层类型判定（按 XcpDataBlock
 * 中出现的首个传输层块，检查顺序固定保证确定性）。 */
XcpTransportDto PickTransport(const a2l::xcp::XcpDataBlock& blk) {
    if (!blk.GetXcpOnCans().empty()) return XcpTransportDto::kCan;
    if (!blk.GetXcpOnFlxs().empty()) return XcpTransportDto::kFlx;
    if (!blk.GetXcpOnUsbs().empty()) return XcpTransportDto::kUsb;
    if (!blk.GetXcpOnSxis().empty()) return XcpTransportDto::kSxi;
    if (!blk.GetXcpOnTcpIps().empty()) return XcpTransportDto::kTcpIp;
    if (!blk.GetXcpOnUdpIps().empty()) return XcpTransportDto::kUdpIp;
    if (!blk.GetXcpOnSimulinks().empty()) return XcpTransportDto::kSimulink;
    return XcpTransportDto::kNone;
}

/**
 * @brief 把上游 CompuMethod（连同其引用的 COMPU_TAB / UNIT）拍平成
 * ConversionDto。
 * @param cm     上游转换方法（可为空指针 → kind=kNone）。
 * @param module 所属模块（查 COMPU_TAB / UNIT）。
 * @return 转换快照。
 */
ConversionDto BuildConversion(const a2l::CompuMethod* cm,
                              const a2l::Module& module) {
    ConversionDto out;
    if (cm == nullptr) {
        return out;  // kind 默认 kNone
    }
    out.name = cm->Name();
    out.kind = MapConversionType(cm->Type());
    out.formula = cm->Formula();
    out.status_string_ref = cm->StatusStringRef();

    // 单位：RefUnit 名直接透出（UNIT 对象在 bridge 侧不需要更多结构）
    out.unit = cm->RefUnit();

    // LINEAR：语法 COEFFS_LINEAR offset factor 两参数；兼容退化的 COEFFS
    // 前两位。
    if (out.kind == ConversionKindDto::kLinear) {
        const auto& lin = cm->CoeffsLinear();
        const auto& co = cm->Coeffs();
        if (lin.size() >= 2) {
            out.coeffs.o = lin[0];  // 偏移 O
            out.coeffs.c = lin[1];  // 比例 C
            out.coeffs.f = 0.0;     // LINEAR 无独立常数项来源
        } else if (co.size() >= 2) {
            out.coeffs.o = co[0];
            out.coeffs.c = co[1];
            out.coeffs.f = 0.0;
        }
    } else if (out.kind == ConversionKindDto::kRatFunc) {
        // COEFFS N1 N2 N3 D1 D2 D3（上游 grammar 顺序）
        const auto& co = cm->Coeffs();
        if (co.size() >= 6) {
            out.coeffs.n1 = co[0];
            out.coeffs.n2 = co[1];
            out.coeffs.n3 = co[2];
            out.coeffs.d1 = co[3];
            out.coeffs.d2 = co[4];
            out.coeffs.d3 = co[5];
        }
    } else if (out.kind == ConversionKindDto::kTabIntp ||
               out.kind == ConversionKindDto::kTabNoIntp) {
        const a2l::CompuTab* tab = module.GetCompuTab(cm->CompuTabRef());
        if (tab != nullptr) {
            for (const auto& [in_val, out_val] : tab->KeyValueList()) {
                ConversionTableEntryDto e;
                e.input_min = in_val;
                e.input_max = in_val;
                e.numeric_output = true;
                e.numeric_value = out_val;
                e.output_text = std::to_string(out_val);
                out.table.push_back(e);
            }
        }
    } else if (out.kind == ConversionKindDto::kTabVerb) {
        const a2l::CompuVtab* vtab = module.GetCompuVtab(cm->CompuTabRef());
        if (vtab != nullptr) {
            for (const auto& [in_val, text] : vtab->KeyValueList()) {
                ConversionTableEntryDto e;
                e.input_min = in_val;
                e.input_max = in_val;
                e.numeric_output = false;
                e.output_text = text;
                out.table.push_back(e);
            }
        }
    }
    return out;
}

/**
 * @brief 采集一个 MEASUREMENT 的完整快照。
 * @param m      上游测量对象。
 * @param module 所属模块。
 * @return 符号 DTO。
 */
SymbolDto BuildMeasurementSymbol(const a2l::Measurement& m,
                                 const a2l::Module& module) {
    SymbolDto s;
    s.module_name = module.Name();
    s.name = m.Name();
    s.description = m.Description();
    s.kind = SymbolKindDto::kMeasurement;
    s.characteristic_type = CharacteristicTypeDto::kNone;
    s.data_type = MapDataType(m.DataType());
    s.element_size_bytes = TypeWidthBytes(s.data_type);
    s.xcp_address = m.EcuAddress();
    s.address_extension =
        static_cast<std::uint8_t>(m.EcuAddressExtension() & 0xFF);
    s.byte_order = MapByteOrder(m.ByteOrder());
    s.array_order = MapLayout(m.Layout());
    s.bit_mask = m.BitMask();
    s.error_mask = m.ErrorMask();
    if (m.ArraySize() > 0) {
        DimensionDto d;
        d.extent = m.ArraySize();
        d.byte_stride = s.element_size_bytes;
        s.dimensions.push_back(d);
    }
    for (const auto dim : m.MatrixDim()) {
        DimensionDto d;
        d.extent = dim;
        d.byte_stride = s.element_size_bytes;
        s.dimensions.push_back(d);
    }
    // 多维规则性校验交给 bridge（B-1/B-15），SDK 只忠实透传源数据。
    s.read_write = m.ReadWrite();
    if (m.HaveLimit()) {
        s.have_limit = true;
        s.lower_limit = m.LowerLimit();
        s.upper_limit = m.UpperLimit();
    }
    s.phys_unit = m.PhysUnit();
    s.ref_memory_segment = m.RefMemorySegment();
    s.compu_method_name = m.Conversion();
    s.conversion =
        BuildConversion(module.GetCompuMethod(m.Conversion()), module);
    return s;
}

/**
 * @brief 采集一个 CHARACTERISTIC 的完整快照。
 * @param c      上游特性对象。
 * @param module 所属模块。
 * @return 符号 DTO。
 */
SymbolDto BuildCharacteristicSymbol(const a2l::Characteristic& c,
                                    const a2l::Module& module) {
    SymbolDto s;
    s.module_name = module.Name();
    s.name = c.Name();
    s.description = c.Description();
    s.kind = SymbolKindDto::kCharacteristic;
    s.characteristic_type = MapCharType(c.Type());
    // CHARACTERISTIC 的元素类型藏在 RECORD_LAYOUT/FNC_VALUES 中，上游未提供
    // 直接 DataType() 访问器；首里程碑 SDK 不做布局推导，统一留 kUnknown，
    // 由 bridge 依 B-3 显式拒绝数值读写（禁止静默猜测）。
    s.data_type = AsamDataTypeDto::kUnknown;
    s.element_size_bytes = 0;
    s.xcp_address = c.Address();
    s.address_extension =
        static_cast<std::uint8_t>(c.EcuAddressExtension() & 0xFF);
    s.byte_order = MapByteOrder(c.ByteOrder());
    s.bit_mask = c.BitMask();
    s.read_write = !c.ReadOnly();
    if (c.HaveLimit()) {
        s.have_limit = true;
        s.lower_limit = c.LowerLimit();
        s.upper_limit = c.UpperLimit();
    }
    s.phys_unit = c.PhysUnit();
    s.ref_memory_segment = c.RefMemorySegment();
    s.compu_method_name = c.Conversion();
    s.conversion =
        BuildConversion(module.GetCompuMethod(c.Conversion()), module);
    return s;
}

}  // namespace

// ----------------------------------------------------------------------------
// Doc 实现类（DLL 内部；对消费方只暴露 IDoc 抽象）
// ----------------------------------------------------------------------------

/**
 * @class Doc
 * @brief IDoc 的唯一实现：持有 A2lFile 与派生快照缓存。
 *
 * 线程模型（B-20/A-11）：Load 成功后快照不可变，查询接口可并发调用；
 * LoadAsync 期间不得调用查询接口（由消费方保证，SDK 不加锁）。
 */
class Doc final : public IDoc {
public:
    Doc() = default;

    /** @brief 析构时回收异步解析线程（若仍在运行则 join，避免悬挂）。 */
    ~Doc() override {
        if (parse_thread_.joinable()) {
            parse_thread_.join();
        }
    }

    // ------------------------------------------------------------------ 加载

    /** @copydoc IDoc::Load */
    ErrorCode Load(const std::string& file_path,
                   bool module_information_only) noexcept override {
        try {
            ResetState();
            file_ = std::make_unique<a2l::A2lFile>();
            file_->Filename(file_path);
            file_->ParserType(
                module_information_only
                    ? a2l::A2lParserType::PARSE_MODULE_INFORMATION_ONLY
                    : a2l::A2lParserType::FULL_PARSING);
            // 预读检查：给缺失文件一个明确的 IoError，而不是上游泛化异常文本
            std::error_code ec;
            if (!std::filesystem::exists(std::filesystem::u8path(file_path),
                                         ec)) {
                SetError(ErrorCode::kIoError, "file not found: " + file_path);
                return ErrorCode::kIoError;
            }
            if (!file_->ParseFile()) {
                SetError(ErrorCode::kParseFailed,
                         "parse failed at line " +
                             std::to_string(file_->LineNo()) + ": " +
                             file_->LastError());
                return ErrorCode::kParseFailed;
            }
            BuildSnapshots();
            loaded_ = true;
            progress_ = 100;
            ClearError();
            return ErrorCode::kOk;
        } catch (const std::exception& ex) {
            SetError(ErrorCode::kInternal, ex.what());
            return ErrorCode::kInternal;
        } catch (...) {
            SetError(ErrorCode::kInternal, "unknown exception");
            return ErrorCode::kInternal;
        }
    }

    /** @copydoc IDoc::LoadAsync */
    ErrorCode LoadAsync(
        const std::string& file_path, bool module_information_only,
        std::function<void(int)> /*progress_cb*/,
        std::function<void(int)> completed_cb) noexcept override {
        if (!completed_cb) {
            SetError(ErrorCode::kBadArgument, "completed callback required");
            return ErrorCode::kBadArgument;
        }
        try {
            if (parse_thread_.joinable()) {
                parse_thread_.join();
            }
            // 后台线程复用同步 Load。进度回调暂不透传（上游 ProgressInfo 属另一
            // 解析路径，本 SDK 主用 ParseFile；保留接口形状供后续接入）。
            async_path_ = file_path;
            async_module_only_ = module_information_only;
            completed_cb_local_ = std::move(completed_cb);
            parse_thread_ = std::thread([this] { RunAsyncParse(); });
            return ErrorCode::kOk;
        } catch (const std::exception& ex) {
            SetError(ErrorCode::kInternal, ex.what());
            return ErrorCode::kInternal;
        } catch (...) {
            SetError(ErrorCode::kInternal, "unknown exception");
            return ErrorCode::kInternal;
        }
    }

    // ------------------------------------------------------------------ 查询

    /** @copydoc IDoc::LastError */
    const char* LastError() const noexcept override {
        return last_error_.c_str();
    }

    /** @copydoc IDoc::LastErrorCode */
    ErrorCode LastErrorCode() const noexcept override { return last_code_; }

    /** @copydoc IDoc::Progress */
    int Progress() const noexcept override { return progress_; }

    /** @copydoc IDoc::ModuleCount */
    std::size_t ModuleCount() const noexcept override {
        if (!loaded_ || !file_) {
            return 0;
        }
        return file_->Project().Modules().size();
    }

    /** @copydoc IDoc::ListSymbols */
    ErrorCode ListSymbols(std::vector<SymbolDto>* out) const noexcept override {
        if (out == nullptr) {
            return ErrorCode::kBadArgument;
        }
        if (!loaded_) {
            return ErrorCode::kNotInitialized;
        }
        try {
            *out = symbols_;  // 快照整体拷贝（R3）
            return ErrorCode::kOk;
        } catch (...) {
            return ErrorCode::kInternal;
        }
    }

    /** @copydoc IDoc::FindSymbol */
    ErrorCode FindSymbol(const std::string& qualified_or_unique_name,
                         SymbolDto* out) const noexcept override {
        if (out == nullptr || qualified_or_unique_name.empty()) {
            return ErrorCode::kBadArgument;
        }
        if (!loaded_) {
            return ErrorCode::kNotInitialized;
        }
        try {
            const auto pos = qualified_or_unique_name.find("::");
            if (pos != std::string::npos) {
                // 限定名 module::symbol —— 精确命中
                const std::string mod = qualified_or_unique_name.substr(0, pos);
                const std::string sym =
                    qualified_or_unique_name.substr(pos + 2);
                for (const auto& s : symbols_) {
                    if (s.module_name == mod && s.name == sym) {
                        *out = s;
                        return ErrorCode::kOk;
                    }
                }
                return ErrorCode::kNotFound;
            }
            // 裸名：全局唯一才允许（B-13），多义报错不猜测
            const SymbolDto* hit = nullptr;
            int count = 0;
            for (const auto& s : symbols_) {
                if (s.name == qualified_or_unique_name) {
                    hit = &s;
                    ++count;
                }
            }
            if (count == 0) {
                return ErrorCode::kNotFound;
            }
            if (count > 1) {
                return ErrorCode::kAmbiguousName;
            }
            *out = *hit;
            return ErrorCode::kOk;
        } catch (...) {
            return ErrorCode::kInternal;
        }
    }

    /** @copydoc IDoc::GetIfDataXcp */
    ErrorCode GetIfDataXcp(IfDataXcpDto* out) const noexcept override {
        if (out == nullptr) {
            return ErrorCode::kBadArgument;
        }
        if (!loaded_) {
            return ErrorCode::kNotInitialized;
        }
        try {
            *out = if_data_xcp_;
            return out->present ? ErrorCode::kOk : ErrorCode::kNotFound;
        } catch (...) {
            return ErrorCode::kInternal;
        }
    }

    /** @copydoc IDoc::ListDaqLists */
    ErrorCode ListDaqLists(
        std::vector<DaqListDto>* out) const noexcept override {
        if (out == nullptr) {
            return ErrorCode::kBadArgument;
        }
        if (!loaded_) {
            return ErrorCode::kNotInitialized;
        }
        try {
            *out = daq_lists_;
            return ErrorCode::kOk;
        } catch (...) {
            return ErrorCode::kInternal;
        }
    }

    /** @copydoc IDoc::GetConversion */
    ErrorCode GetConversion(const std::string& compu_method_name,
                            ConversionDto* out) const noexcept override {
        if (out == nullptr || compu_method_name.empty()) {
            return ErrorCode::kBadArgument;
        }
        if (!loaded_) {
            return ErrorCode::kNotInitialized;
        }
        try {
            for (const auto& kv : conversions_) {
                if (kv.first == compu_method_name) {
                    *out = kv.second;
                    return ErrorCode::kOk;
                }
            }
            return ErrorCode::kNotFound;
        } catch (...) {
            return ErrorCode::kInternal;
        }
    }

    /** @copydoc IDoc::Release */
    void Release() noexcept override { delete this; }

private:
    /** 清空一次加载产生的全部状态。 */
    void ResetState() noexcept {
        loaded_ = false;
        progress_ = 0;
        symbols_.clear();
        daq_lists_.clear();
        conversions_.clear();
        if_data_xcp_ = {};
        file_.reset();
    }

    /** 记录错误码与文本。 */
    void SetError(ErrorCode code, const std::string& msg) {
        last_code_ = code;
        last_error_ = msg;
    }

    /** 清除错误状态。 */
    void ClearError() {
        last_code_ = ErrorCode::kOk;
        last_error_.clear();
    }

    /** 异步线程体：跑同步 Load 并把结果码回抛完成回调。 */
    void RunAsyncParse() noexcept {
        ErrorCode code = ErrorCode::kInternal;
        try {
            code = Load(async_path_, async_module_only_);
        } catch (...) {
            code = ErrorCode::kInternal;
        }
        if (completed_cb_local_) {
            completed_cb_local_(static_cast<int>(code));
        }
        completed_cb_local_ = nullptr;
    }

    /** 解析成功后构建全部 DTO 快照（排序保证确定性，B-20 不可变快照）。 */
    void BuildSnapshots() {
        a2l::A2lProject& project = file_->Project();
        for (const auto& [mod_name, module_ptr] : project.Modules()) {
            if (!module_ptr) {
                continue;
            }
            const a2l::Module& module = *module_ptr;
            // MEASUREMENT
            for (const auto& [name, m] : module.Measurements()) {
                if (m) {
                    symbols_.push_back(BuildMeasurementSymbol(*m, module));
                }
            }
            // CHARACTERISTIC
            for (const auto& [name, c] : module.Characteristics()) {
                if (c) {
                    symbols_.push_back(BuildCharacteristicSymbol(*c, module));
                }
            }
            // COMPU_METHOD 全量快照（bridge 可按名二次查询）
            for (const auto& [name, cm] : module.CompuMethods()) {
                if (cm && conversions_.find(name) == conversions_.end()) {
                    conversions_.emplace(name,
                                         BuildConversion(cm.get(), module));
                }
            }
            // IF_DATA XCP：首个含有效块的 MODULE 优先（§6.3-A 优先级在多块场景
            // 由 bridge 复核；符号级 IF_DATA 亦在 bridge 侧按需覆盖）
            if (!if_data_xcp_.present) {
                ExtractIfDataXcp(module);
            }
        }
        // 确定性排序（上游 unordered_map 迭代顺序不稳定）
        std::sort(symbols_.begin(), symbols_.end(),
                  [](const SymbolDto& a, const SymbolDto& b) {
                      if (a.module_name != b.module_name)
                          return a.module_name < b.module_name;
                      return a.name < b.name;
                  });
    }

    /** 从模块对象的 IF_DATA XCP/XCPplus 块提取公共参数与 DAQ 布局。 */
    void ExtractIfDataXcp(const a2l::Module& module) {
        // §6.3-A：XCPplus 存在且 IsOk 时优先，否则退回 XCP
        const a2l::xcp::XcpDataBlock* blk = nullptr;
        bool from_plus = false;
        const a2l::xcp::XcpDataBlock* plus = module.GetXcpPlusDataBlock();
        if (plus != nullptr && plus->IsOk()) {
            blk = plus;
            from_plus = true;
        } else {
            const a2l::xcp::XcpDataBlock* xcp = module.GetXcpDataBlock();
            if (xcp != nullptr && xcp->IsOk()) {
                blk = xcp;
            }
        }
        if (blk == nullptr) {
            return;
        }

        if_data_xcp_.present = true;
        if_data_xcp_.from_xcp_plus = from_plus;

        const a2l::xcp::CommonParameters& cp = blk->GetCommonParameters();
        if (const auto* pl = cp.GetProtocolLayer(); pl != nullptr) {
            if_data_xcp_.protocol_version = pl->GetVersion();
            if_data_xcp_.max_cto = pl->GetMaxCto();
            if_data_xcp_.max_dto = pl->GetMaxDto();
            if_data_xcp_.byte_order =
                pl->GetByteOrder() == a2l::xcp::ByteOrder::BYTE_ORDER_MSB_FIRST
                    ? ByteOrderDto::kMsbFirst
                    : ByteOrderDto::kMsbLast;
            if_data_xcp_.default_byte_order =
                static_cast<std::uint8_t>(pl->GetByteOrder());
            if_data_xcp_.address_granularity =
                static_cast<std::uint8_t>(pl->GetAddressGranularity());
        }
        if_data_xcp_.transport = PickTransport(*blk);
        if (if_data_xcp_.transport == XcpTransportDto::kUdpIp) {
            const auto& udps = blk->GetXcpOnUdpIps();
            if (!udps.empty()) {
                if_data_xcp_.udp_port = udps.front().GetPort();
                if_data_xcp_.udp_host = udps.front().GetHostName();
            }
        }

        // DAQ：预定义列表快照
        if (const auto* dq = cp.GetDaq(); dq != nullptr) {
            for (const auto& list : dq->GetDaqList()) {
                DaqListDto d;
                d.number = list.GetNumber();
                if (const auto& t = list.GetType(); t.has_value()) {
                    switch (*t) {
                        case a2l::xcp::DaqListType::DAQ:
                            d.type = DaqListTypeDto::kDaq;
                            break;
                        case a2l::xcp::DaqListType::STIM:
                            d.type = DaqListTypeDto::kStim;
                            break;
                        case a2l::xcp::DaqListType::DAQ_STIM:
                            d.type = DaqListTypeDto::kDaqStim;
                            break;
                        default:
                            d.type = DaqListTypeDto::kUnknown;
                            break;
                    }
                }
                d.max_odt = list.GetMaxOdt();
                d.max_odt_entries = list.GetMaxOdtEntries();
                d.first_pid = list.GetFirstPid();
                d.event_fixed = list.GetEventFixed();
                d.daq_packed_mode_supported = list.GetDaqPackedModeSupported();
                for (const auto& odt : list.GetPredefinedList()) {
                    OdtDto o;
                    o.number = odt.number;
                    for (const auto& e : odt.odt_entry_list) {
                        OdtEntryDto ed;
                        ed.number = e.number;
                        ed.address = e.address;
                        ed.address_extension = e.address_extension;
                        ed.size = e.size;
                        ed.bit_offset = e.bit_offset;
                        o.entries.push_back(ed);
                    }
                    d.predefined_odts.push_back(o);
                }
                daq_lists_.push_back(std::move(d));
            }
        }
    }

    /** 上游解析器实例（DLL 内拥有，绝不出边界）。 */
    std::unique_ptr<a2l::A2lFile> file_;
    /** 符号快照（Load 后不可变）。 */
    std::vector<SymbolDto> symbols_;
    /** DAQ_LIST 快照。 */
    std::vector<DaqListDto> daq_lists_;
    /** COMPU_METHOD 名 → 转换快照。 */
    std::unordered_map<std::string, ConversionDto> conversions_;
    /** IF_DATA XCP 快照。 */
    IfDataXcpDto if_data_xcp_;
    /** 是否已成功加载。 */
    bool loaded_ = false;
    /** 进度百分比。 */
    int progress_ = 0;
    /** 最近错误码。 */
    ErrorCode last_code_ = ErrorCode::kOk;
    /** 最近错误文本。 */
    std::string last_error_;
    /** 异步解析线程。 */
    std::thread parse_thread_;
    /** 异步任务输入路径。 */
    std::string async_path_;
    /** 异步任务快速模式标志。 */
    bool async_module_only_ = false;
    /** 异步完成回调。 */
    std::function<void(int)> completed_cb_local_;
};

// ----------------------------------------------------------------------------
// 工厂（R2/R5）
// ----------------------------------------------------------------------------

/**
 * @brief 创建 IDoc 实例（DLL 侧 new；用毕 Release()）。
 * @param abi_version 调用方编译期 kLibA2lAbiVersion；不一致返回 nullptr。
 * @return 新对象或 nullptr。
 */
IDoc* CreateDoc(std::uint32_t abi_version) noexcept {
    try {
        if (abi_version != kLibA2lAbiVersion) {
            return nullptr;
        }
        return new Doc();
    } catch (...) {
        return nullptr;
    }
}

}  // namespace liba2l
