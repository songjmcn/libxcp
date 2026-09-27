// =============================================================================
// a2l_dto_map.cpp —— liba2l DTO → 领域类型显式映射表（B-3/B-8/B-9）
// =============================================================================

#include "a2l_dto_map.hpp"

#include <utility>

namespace calmcar::xcp::a2l::detail {

Error MapSdkError(liba2l::ErrorCode sdk_code, Phase fallback_phase,
                  std::string_view message) {
    Error e;
    e.phase = fallback_phase;
    e.severity = Severity::Error;
    e.message = std::string(message);
    switch (sdk_code) {
        case liba2l::ErrorCode::kOk:
            e.code = ErrorCode::Internal;  // Ok 出现在错误路径只能是内部矛盾
            break;
        case liba2l::ErrorCode::kNotInitialized:
            e.code = ErrorCode::NotReady;
            e.phase = Phase::Runtime;
            break;
        case liba2l::ErrorCode::kNotFound:
            e.code = ErrorCode::NotFound;
            break;
        case liba2l::ErrorCode::kAmbiguousName:
            e.code = ErrorCode::AmbiguousName;
            break;
        case liba2l::ErrorCode::kUnsupportedAddressingMode:
            e.code = ErrorCode::UnsupportedAddressingMode;
            e.phase = Phase::Address;
            break;
        case liba2l::ErrorCode::kAddressOverflow:
            e.code = ErrorCode::AddressOverflow;
            e.phase = Phase::Address;
            break;
        case liba2l::ErrorCode::kTypeTooComplex:
            e.code = ErrorCode::UnsupportedDataType;
            break;
        case liba2l::ErrorCode::kInvalidLayout:
            e.code = ErrorCode::InvalidLayout;
            e.phase = Phase::Layout;
            break;
        case liba2l::ErrorCode::kConversionNotInvertible:
            e.code = ErrorCode::ConversionNotInvertible;
            e.phase = Phase::Conversion;
            break;
        case liba2l::ErrorCode::kFormulaUnsupported:
            e.code = ErrorCode::UnsupportedConversion;
            e.phase = Phase::Conversion;
            break;
        case liba2l::ErrorCode::kRawSizeMismatch:
            e.code = ErrorCode::RawSizeMismatch;
            e.phase = Phase::Conversion;
            break;
        case liba2l::ErrorCode::kBadArgument:
            e.code = ErrorCode::BadArgument;
            break;
        case liba2l::ErrorCode::kParseFailed:
            e.code = ErrorCode::ParseFailed;
            e.phase = Phase::Load;
            break;
        case liba2l::ErrorCode::kIoError:
            e.code = ErrorCode::IoError;
            e.phase = Phase::Load;
            break;
        case liba2l::ErrorCode::kInternal:
            e.code = ErrorCode::Internal;
            break;
        default:
            e.code = ErrorCode::Internal;
            break;
    }
    return e;
}

ByteOrder ConvertByteOrder(liba2l::ByteOrderDto dto) noexcept {
    return dto == liba2l::ByteOrderDto::kMsbFirst ? ByteOrder::MsbFirst
                                                  : ByteOrder::MsbLast;
}

std::string MakeQualifiedKey(std::string_view module_name,
                             std::string_view symbol_name) {
    std::string key;
    key.reserve(module_name.size() + 2 + symbol_name.size());
    key.append(module_name);
    key += "::";
    key.append(symbol_name);
    return key;
}

AsamDataType ConvertDataType(liba2l::AsamDataTypeDto dto) noexcept {
    using S = liba2l::AsamDataTypeDto;
    using D = AsamDataType;
    switch (dto) {
        case S::kUByte:
            return D::UByte;
        case S::kSByte:
            return D::SByte;
        case S::kUWord:
            return D::UWord;
        case S::kSWord:
            return D::SWord;
        case S::kULong:
            return D::ULong;
        case S::kSLong:
            return D::SLong;
        case S::kAUint64:
            return D::ULong64;
        case S::kAInt64:
            return D::SLong64;
        case S::kFloat16Ieee:
            return D::Float16;
        case S::kFloat32Ieee:
            return D::Float32;
        case S::kFloat64Ieee:
            return D::Float64;
        case S::kUnknown:
        default:
            // Boolean/BitText/String 等在 SDK 层已归并为 kUnknown；
            // bridge 一律按 UnsupportedDataType 处理，禁止猜宽度（B-3）
            return D::Unknown;
    }
}

CharacteristicType ConvertCharType(liba2l::CharacteristicTypeDto dto) noexcept {
    using S = liba2l::CharacteristicTypeDto;
    using D = CharacteristicType;
    switch (dto) {
        case S::kNone:
            return D::None;
        case S::kValue:
            return D::Value;
        case S::kCurve:
            return D::Curve;
        case S::kMap:
            return D::Map;
        case S::kCuboid:
            return D::Cuboid;
        case S::kCube4:
            return D::Cube4;
        case S::kCube5:
            return D::Cube5;
        case S::kValBlk:
            return D::ValBlk;
        case S::kAscii:
            return D::Ascii;
        default:
            return D::None;
    }
}

ConversionInfo ConvertConversion(const liba2l::ConversionDto& dto) {
    ConversionInfo info;
    info.compu_method_name = dto.name;
    info.unit = dto.unit;
    switch (dto.kind) {
        case liba2l::ConversionKindDto::kIdentical:
            info.kind = ConversionKind::Identical;
            break;
        case liba2l::ConversionKindDto::kLinear: {
            info.kind = ConversionKind::Linear;
            LinearCoefficients c;
            c.c = dto.coeffs.c;
            c.o = dto.coeffs.o;
            c.f = dto.coeffs.f;
            info.payload = c;
            break;
        }
        case liba2l::ConversionKindDto::kRatFunc: {
            info.kind = ConversionKind::RatFunc;
            RatFuncCoefficients r;
            r.n1 = dto.coeffs.n1;
            r.n2 = dto.coeffs.n2;
            r.n3 = dto.coeffs.n3;
            r.d1 = dto.coeffs.d1;
            r.d2 = dto.coeffs.d2;
            r.d3 = dto.coeffs.d3;
            info.payload = r;
            break;
        }
        case liba2l::ConversionKindDto::kTabIntp:
        case liba2l::ConversionKindDto::kTabNoIntp:
        case liba2l::ConversionKindDto::kTabVerb: {
            info.kind = dto.kind == liba2l::ConversionKindDto::kTabIntp
                            ? ConversionKind::TabIntp
                        : dto.kind == liba2l::ConversionKindDto::kTabNoIntp
                            ? ConversionKind::TabNoIntp
                            : ConversionKind::TabVerb;
            std::vector<ConversionTableEntry> table;
            table.reserve(dto.table.size());
            for (const liba2l::ConversionTableEntryDto& row : dto.table) {
                ConversionTableEntry entry;
                entry.input_min = row.input_min;
                entry.input_max = row.input_max;
                entry.numeric_output = row.numeric_output;
                if (row.numeric_output) {
                    entry.output = row.numeric_value;
                } else {
                    entry.output = row.output_text;
                }
                table.push_back(std::move(entry));
            }
            info.payload = std::move(table);
            break;
        }
        case liba2l::ConversionKindDto::kFormulaUnsupported:
            // FORM：仅保留原文（B-8），payload 为公式文本
            info.kind = ConversionKind::FormulaUnsupported;
            info.payload = dto.formula;
            break;
        case liba2l::ConversionKindDto::kNone:
        default:
            info.kind = ConversionKind::None;
            break;
    }
    return info;
}

SymbolInfo ConvertSymbol(const liba2l::SymbolDto& dto) {
    SymbolInfo info;
    info.module_name = dto.module_name;
    info.name = dto.name;
    info.description = dto.description;
    switch (dto.kind) {
        case liba2l::SymbolKindDto::kMeasurement:
            info.kind = SymbolKind::Measurement;
            break;
        case liba2l::SymbolKindDto::kCharacteristic:
            info.kind = SymbolKind::Characteristic;
            break;
        case liba2l::SymbolKindDto::kStructure:
            info.kind = SymbolKind::Structure;
            break;
        default:
            info.kind = SymbolKind::Measurement;
            break;
    }
    info.characteristic_type = ConvertCharType(dto.characteristic_type);
    info.data_type = ConvertDataType(dto.data_type);
    // B-1：ECU_ADDRESS 原值直传，绝不乘 AG；extension 独立字段
    info.xcp_address = dto.xcp_address;
    info.address_extension = dto.address_extension;
    // B-3：元素宽以显式类型映射为准（SDK 与 bridge 双保险，不一致时以
    // 类型映射为准并拒绝——未知类型 element_size 必为 0）
    const std::uint8_t mapped = ElementSizeOf(info.data_type);
    info.element_size_bytes = mapped;
    info.dimensions.reserve(dto.dimensions.size());
    for (const liba2l::DimensionDto& d : dto.dimensions) {
        Dimension dim;
        dim.source_lower_bound = d.source_lower_bound;
        dim.extent = d.extent;
        dim.byte_stride = d.byte_stride;
        info.dimensions.push_back(dim);
    }
    info.array_order = dto.array_order == liba2l::ArrayOrderDto::kColumnDir
                           ? ArrayOrder::ColumnMajor
                           : ArrayOrder::RowMajor;
    info.byte_order = ConvertByteOrder(dto.byte_order);
    if (dto.daq_bit_offset.has_value()) {
        info.daq_bit_offset = dto.daq_bit_offset;  // B-9：与 bit_mask 分离
    }
    info.bit_mask = dto.bit_mask;
    if (dto.error_mask.has_value()) {
        info.error_mask = dto.error_mask;  // 仅保存，不推导 valid（B-9）
    }
    info.read_write = dto.read_write;
    info.conversion = ConvertConversion(dto.conversion);
    // B-10 单位归一（批次10）：PHYS_UNIT 原文优先，为空时才保留
    // COMPU_METHOD 的 REF_UNIT 作为回退
    if (!dto.phys_unit.empty()) {
        info.conversion.unit = dto.phys_unit;
    }
    info.ref_memory_segment = dto.ref_memory_segment;
    if (dto.have_limit) {
        info.have_limit = true;
        info.lower_limit = dto.lower_limit;
        info.upper_limit = dto.upper_limit;
    }
    return info;
}

SymbolInfo ConvertStructure(const liba2l::StructInfoDto& dto) {
    SymbolInfo info;
    info.module_name = dto.module_name;
    info.name = dto.name;
    // B-12 首版边界：STRUCTURE/INSTANCE 只做"可识别的元数据"。成员清单不在
    // SymbolInfo 里展开（无 leaf 通路），但成员数/引用 TYPEDEF 名/读写声明
    // 是快照里可证的 fact，写进 description 供 UI 展示与用例断言。
    std::string derived;
    if (dto.is_instance) {
        derived = "[INSTANCE ref_typedef=" + dto.ref_typedef +
                  " read_write=" + (dto.read_write ? "1" : "0") + "]";
    } else {
        derived =
            "[TYPEDEF_STRUCTURE members=" + std::to_string(dto.members.size()) +
            " size_bytes=" + std::to_string(dto.size_bytes) + "]";
    }
    info.description =
        dto.description.empty() ? derived : dto.description + " " + derived;
    info.kind = SymbolKind::Structure;
    info.characteristic_type = CharacteristicType::None;
    // 未知类型 → element_size 必为 0（B-3 禁止猜宽度），STRUCTURE 因此
    // 天然无法参与字节数计算与换算；门面另有显式 UnsupportedOperation 门。
    info.data_type = AsamDataType::Unknown;
    info.element_size_bytes = 0;
    info.dimensions.clear();  // 成员不展开为维度（禁止扁平化猜测）
    // INSTANCE 的 ECU_ADDRESS 原值直传（B-1：绝不乘 AG）；TYPEDEF 无地址=0
    info.xcp_address = dto.address;
    // 批次15（F5）：INSTANCE 的 ECU_ADDRESS_EXTENSION 原值直传（此前恒 0，
    // 会让分段地址上的实例静默指错内存）。TYPEDEF 无地址，该值本就是 0。
    info.address_extension = dto.address_extension;
    info.read_write = false;  // B-12：结构体读写一律拒绝，不随声明放行
    return info;
}

bool ConvertRecordLayoutInfo(const liba2l::SymbolDto& dto,
                             RecordLayoutInfo* out) {
    if (out == nullptr) {
        return false;
    }
    using S = liba2l::RecordLayoutKindDto;
    if (dto.record_layout_kind == S::kNotProvided) {
        // 无版式信息（无 DEPOSIT 或模块内查不到 RECORD_LAYOUT）：
        // 不据此判定"不可执行"，交由 B-3 元素宽度门处理
        return false;
    }
    out->name = dto.record_layout_name;
    switch (dto.record_layout_kind) {
        case S::kPlainScalar:
            // 纯标量 FNC_VALUES 连续版式（VALUE 与连续 VAL_BLK 共用）
            out->kind = RecordLayoutKind::FncValues;
            out->writable = true;
            return true;
        case S::kAxisPts:
            out->kind = RecordLayoutKind::AxisPts;
            out->writable = false;
            return true;
        case S::kComplex:
            out->kind = RecordLayoutKind::Complex;
            out->writable = false;
            return true;
        case S::kNotProvided:
        default:
            // 未列出的上游取值不猜类别（B-3 同款纪律）
            return false;
    }
}

void ConvertIfDataXcp(const liba2l::IfDataXcpDto& dto, ProtocolLayerInfo* layer,
                      std::vector<TransportEndpoint>* transports) noexcept {
    if (layer != nullptr) {
        // protocol_version 是 uint16 major<<8|minor；IF_DATA 段版本首版记
        // 0x0100
        layer->version = 0x0100;
        layer->max_cto = dto.max_cto;
        layer->max_dto = dto.max_dto;
        layer->byte_order = ConvertByteOrder(dto.byte_order);
        // address_granularity：0 = A2L 未标注 → 保持默认 Byte（§6.3-A 保守值）
        switch (dto.address_granularity) {
            case 2:
                layer->address_granularity = AddressGranularity::Word;
                break;
            case 4:
                layer->address_granularity = AddressGranularity::Dword;
                break;
            case 1:
            default:
                layer->address_granularity = AddressGranularity::Byte;
                break;
        }
        // 批次10（§6.1 数据补全）：t1..t7、可选命令码、Seed&Key、ECU 状态
        for (std::size_t i = 0; i < 7; ++i) {
            layer->timeout_ms[i] = dto.timers[i];
        }
        layer->optional_commands = dto.optional_commands;
        layer->seed_and_key_function = dto.seed_and_key_function;
        layer->has_ecu_states = dto.has_ecu_states;
    }
    if (transports != nullptr &&
        dto.transport != liba2l::XcpTransportDto::kNone) {
        TransportEndpoint ep;
        switch (dto.transport) {
            case liba2l::XcpTransportDto::kCan:
                ep.kind = TransportEndpoint::Kind::Can;
                break;
            case liba2l::XcpTransportDto::kFlx:
                ep.kind = TransportEndpoint::Kind::Flx;
                break;
            case liba2l::XcpTransportDto::kUsb:
                ep.kind = TransportEndpoint::Kind::Usb;
                break;
            case liba2l::XcpTransportDto::kSxi:
                ep.kind = TransportEndpoint::Kind::Sxi;
                break;
            case liba2l::XcpTransportDto::kTcpIp:
                ep.kind = TransportEndpoint::Kind::TcpIp;
                break;
            case liba2l::XcpTransportDto::kUdpIp:
                ep.kind = TransportEndpoint::Kind::UdpIp;
                break;
            default:
                ep.kind = TransportEndpoint::Kind::UdpIp;
                break;
        }
        ep.remote_host = dto.udp_host;
        ep.remote_port = dto.udp_port;
        // 批次12（§4.3）：alignment / 子命令语义上属 XCPonUDP/IP，
        // 其它传输层不拿这两个字段凑数（保持默认值，不臆造）。
        if (ep.kind == TransportEndpoint::Kind::UdpIp) {
            // 上游原码 0/1/2 = 8/16/32 bit；>2 判为未知 → 0（防御分支，
            // 上游枚举实际只会给 0/1/2）。
            ep.packet_alignment =
                dto.udp_packet_alignment <= 2
                    ? static_cast<std::uint8_t>(8u << dto.udp_packet_alignment)
                    : 0;
            ep.sub_commands = dto.udp_sub_commands;
        }
        transports->push_back(std::move(ep));
    }
}

DaqListLayout ConvertDaqList(const liba2l::DaqListDto& dto) {
    DaqListLayout layout;
    layout.number = dto.number;
    layout.event_fixed = dto.event_fixed;  // 事件通道反查用（批次10）
    // 批次15（F1）：A2L 声明的 FIRST_PID 必须透传。此前它被丢弃，解码只能落回
    // "PID == 列表号"，而列表号与其它列表的 PID 同属一个编号空间，撞上时会用
    // 错列表的布局且**毫无报错**（docs L2225：绝对 ODT 号 = FIRST_PID + 相对
    // ODT 号）。是否据此生成 PID 路由见 A2lBridge::CreateDaqLayout。
    layout.first_pid = dto.first_pid;
    layout.odts.reserve(dto.predefined_odts.size());
    for (const liba2l::OdtDto& odt : dto.predefined_odts) {
        OdtLayout o;
        o.number = odt.number;
        o.entries.reserve(odt.entries.size());
        for (const liba2l::OdtEntryDto& entry : odt.entries) {
            OdtEntryLayout e;
            e.number = entry.number;
            e.address = entry.address;  // B-1：原值直传
            e.address_extension = entry.address_extension;
            e.size_bytes = entry.size;
            e.bit_offset = entry.bit_offset;  // B-9
            // symbol_name 由 bridge 反查地址表填充（禁止猜测，查不到留空）
            o.entries.push_back(std::move(e));
        }
        layout.odts.push_back(std::move(o));
    }
    return layout;
}

DaqInfo ConvertDaqCaps(const liba2l::DaqCapsDto& dto) {
    DaqInfo d;
    // type：0=STATIC 1=DYNAMIC（原始码显式映射，禁止序号推断）
    d.static_supported = (dto.type == 0);
    d.dynamic_supported = (dto.type == 1);
    d.max_daq = dto.max_daq;
    d.max_event_channel = dto.max_event_channel;
    d.min_daq = dto.min_daq;
    d.odt_entry_min_size_bytes = dto.granularity;  // 1/2/4/8 字节粒度
    d.max_odt_entry_size = dto.max_odt_entry_size;
    // OPTIMISATION_TYPE：0..5 → Default..MaxEntrySize（顺序显式写出）
    switch (dto.optimisation) {
        case 1:
            d.odt_type = DaqInfo::OdtType::Odt16;
            break;
        case 2:
            d.odt_type = DaqInfo::OdtType::Odt32;
            break;
        case 3:
            d.odt_type = DaqInfo::OdtType::Odt64;
            break;
        case 4:
            d.odt_type = DaqInfo::OdtType::Alignment;
            break;
        case 5:
            d.odt_type = DaqInfo::OdtType::MaxEntrySize;
            break;
        case 0:
        default:
            d.odt_type = DaqInfo::OdtType::Default;
            break;
    }
    // ADDRESS_EXTENSION：上游原始码 0=FREE 1=ODT 3=DAQ（3≠枚举序号！）
    switch (dto.address_extension_mode) {
        case 1:
            d.address_extension_mode = DaqInfo::AddrExtMode::PerOdt;
            break;
        case 3:
            d.address_extension_mode = DaqInfo::AddrExtMode::PerDaq;
            break;
        case 0:
        default:
            d.address_extension_mode = DaqInfo::AddrExtMode::Free;
            break;
    }
    // IDENTIFICATION_FIELD_TYPE：0..3 与领域枚举一一对应
    switch (dto.identification_field_type) {
        case 1:
            d.identification_field_type = DaqInfo::IdFieldType::RelativeByte;
            break;
        case 2:
            d.identification_field_type = DaqInfo::IdFieldType::RelativeWord;
            break;
        case 3:
            d.identification_field_type =
                DaqInfo::IdFieldType::RelativeWordAligned;
            break;
        case 0:
        default:
            d.identification_field_type = DaqInfo::IdFieldType::Absolute;
            break;
    }
    d.dto_counter_supported = dto.dto_ctr_supported;
    d.pid_off_supported = dto.pid_off_supported;
    d.prescaler_supported = dto.prescaler_supported;
    d.resume_supported = dto.resume_supported;
    d.overflow_flag_supported = (dto.overload_indicator != 0);  // ≠NONE
    d.timestamp_max_size_bits = dto.timestamp_size_bits;
    return d;
}

EventChannelInfo ConvertEventDto(const liba2l::EventChannelDto& dto) {
    EventChannelInfo ev;
    ev.name = dto.name;
    ev.short_name = dto.short_name;
    ev.channel_number = dto.number;
    ev.type = dto.type;
    ev.max_daq_list = dto.max_daq_list;
    ev.time_cycle = dto.time_cycle;
    ev.time_unit = dto.time_unit;
    ev.priority = dto.priority;
    if (dto.consistency.has_value()) {
        ev.consistency = dto.consistency;
        ev.has_consistency = true;
    }
    // cycle_time_us 与 daq_list_numbers 不在此转换：
    // 前者缺 TIME_UNIT 码表（见 EventChannelInfo 类注释），
    // 后者由 BuildIfDataXcp 用 DAQ_LIST 的 EVENT_FIXED 反查回填。
    return ev;
}

}  // namespace calmcar::xcp::a2l::detail
