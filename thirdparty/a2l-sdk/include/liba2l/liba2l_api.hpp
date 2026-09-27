// ============================================================================
// liba2l —— a2llib 的 DLL 导出契约（设计文档 §5.3.3 / §5.3.3b，R4.1 裁决 A-5）
//
// 本文件是跨 DLL 边界的唯一接口。纪律（§5.3.3b R1–R6）：
//   R1 禁止出现任何上游 a2l:: 类型；只有本工程自有 DTO +
//   std::string/vector/optional R2 抽象类 + 纯虚析构 + DLL 侧工厂 +
//   Release()，内存分配/释放同侧 R3
//   一次调用返回完整快照（std::vector<XxxDto>），不暴露迭代器/内部引用 R4
//   全部导出方法 noexcept；错误经返回码 + LastError()（上游异常在导出层捕获）
//   R5 kLibA2lAbiVersion 常量，工厂运行时校验
//   R6 异步回调 std::function<void(int)> 允许（A-11），线程契约见 B-20
//
// 注意：本头文件被 C++20（主树 bridge）与 C++23（SDK 实现）两侧共同包含，
// 因此只允许使用 C++17/20 均合法的语言特征。
// ============================================================================

#ifndef LIBA2L_LIBA2L_API_HPP_
#define LIBA2L_LIBA2L_API_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "liba2l/liba2l_export.h"

namespace liba2l {

// ----------------------------------------------------------------------------
// ABI 版本（R5）。接口布局每次变更必须递增，消费方在 CreateDoc() 内被强制校验。
// v2（批次10）：IDoc +SetActiveModule/+AllowIncludeOutsideRoot/+LastErrorLine；
//               IfDataXcpDto
//               +timers/optional_commands/seed_and_key/has_ecu_states/
//               module_name/daq_caps/events/plus_conflicts；新增 DaqCapsDto、
//               EventChannelDto、XcpPlusConflictDto。
// v3（批次12）：IDoc +LastErrorChain（B-18 结构化 include 链，out-param 风格，
//               与 ListSymbols 同分配惯例）；IfDataXcpDto
//               +udp_packet_alignment/+udp_sub_commands（UDP 端点元数据补全）。
// v4（批次13）：IDoc +ListStructures（B-12 TYPEDEF_STRUCTURE/INSTANCE 元数据，
//               只存不展开）；新 DTO StructInfoDto/StructMemberDto；
//               SymbolDto +record_layout_kind（B-4/B-12 RL 可执行性判定接线）；
//               Progress() 由"完成即 100"改为**阶段进度**实算（无接口变化）。
// ----------------------------------------------------------------------------
inline constexpr std::uint32_t kLibA2lAbiVersion = 5u;

// ----------------------------------------------------------------------------
// 错误码（B-19：SDK 边界用整型码 + LastError 文本，不用异常跨 ABI）
// ----------------------------------------------------------------------------
/** 统一返回码。kOk 表示成功，其余为失败类别。 */
enum class ErrorCode : std::int32_t {
    kOk = 0,              ///< 成功
    kNotInitialized = 1,  ///< 文档尚未成功解析
    kNotFound = 2,        ///< 符号名不存在
    kAmbiguousName = 3,   ///< 裸名在多模块下命中多个符号（B-13）
    kUnsupportedAddressingMode =
        4,  ///< INDIRECT/SUB_ADDRESS/ECU_CALIBRATION_OFFSET≠0（B-1）
    kAddressOverflow = 5,  ///< 元素地址计算溢出或未按 AG 对齐（B-1）
    kTypeTooComplex =
        6,  ///< STRUCTURE 等无法展平的复合类型（B-1/B-8 禁止静默降级）
    kInvalidLayout = 7,  ///< DAQ/记录布局不规则，拒绝按字节数组猜测（B-8/B-15）
    kConversionNotInvertible = 8,  ///< 反变换不可数值求逆（B-8）
    kFormulaUnsupported = 9,       ///< FORM 公式仅文本、不可执行（B-8）
    kRawSizeMismatch = 10,         ///< 原始字节长度与符号宽度不符
    kBadArgument = 11,             ///< 参数非法（空名、越界计数等）
    kParseFailed = 12,             ///< A2L 解析失败
    kIoError = 13,                 ///< 文件读写错误
    kInternal = 14,                ///< 未分类内部错误（含上游抛出的任意异常）
};

// ----------------------------------------------------------------------------
// 枚举 DTO（与上游 ASAP2 枚举一一对应，值语义自解释；bridge 再映射到领域枚举）
// ----------------------------------------------------------------------------
/** 符号大类（B-13 查找结果的顶层判别）。 */
enum class SymbolKindDto : std::int32_t {
    kMeasurement = 0,     ///< MEASUREMENT
    kCharacteristic = 1,  ///< CHARACTERISTIC
    kStructure = 2,       ///< TYPEDEF_STRUCTURE（仅识别，不支持读写）
};

/** CHARACTERISTIC 的类型关键字。 */
enum class CharacteristicTypeDto : std::int32_t {
    kNone = 0,    ///< MEASUREMENT（无 CHARACTERISTIC 类型）
    kValue = 1,   ///< VALUE
    kCurve = 2,   ///< CURVE
    kMap = 3,     ///< MAP
    kCuboid = 4,  ///< CUBOID
    kCube4 = 5,   ///< CUBE_4
    kCube5 = 6,   ///< CUBE_5
    kValBlk = 7,  ///< VAL_BLK
    kAscii = 8,   ///< ASCII
};

/** ASAM 基本数据类型（A2lDataType 的显式宽度映射，B-3）。 */
enum class AsamDataTypeDto : std::int32_t {
    kUnknown = 0,       ///< 未标注或不支持（禁止猜默认宽度）
    kUByte = 1,         ///< UBYTE  (1 字节)
    kSByte = 2,         ///< SBYTE  (1 字节)
    kUWord = 3,         ///< UWORD  (2 字节)
    kSWord = 4,         ///< SWORD  (2 字节)
    kULong = 5,         ///< ULONG  (4 字节)
    kSLong = 6,         ///< SLONG  (4 字节)
    kAUint64 = 7,       ///< A_UINT64 (8 字节)
    kAInt64 = 8,        ///< A_INT64  (8 字节)
    kFloat16Ieee = 9,   ///< FLOAT16_IEEE (2 字节)
    kFloat32Ieee = 10,  ///< FLOAT32_IEEE (4 字节)
    kFloat64Ieee = 11,  ///< FLOAT64_IEEE (8 字节)
};

/** 字节序（MOD_COMMON BYTE_ORDER）。 */
enum class ByteOrderDto : std::int32_t {
    kUnknown =
        0,  ///< MSB_FIRST_MSW_LAST / MSB_LAST_MSW_FIRST 等混合序 → 明确报不支持
    kMsbLast = 1,   ///< INTEL（小端）
    kMsbFirst = 2,  ///< MOTOROLA（大端）
};

/** 多轴数组存储顺序（MEASUREMENT/CHARACTERISTIC 的 ARRAY_ORDER）。 */
enum class ArrayOrderDto : std::int32_t {
    kUnknown = 0,    ///< 未标注
    kRowDir = 1,     ///< ROW_DIR：第一维变化最快
    kColumnDir = 2,  ///< COLUMN_DIR：最后一维变化最快
};

/** COMPU_METHOD 的转换类别。 */
enum class ConversionKindDto : std::int32_t {
    kIdentical = 0,  ///< IDENTICAL：物理值 == 原始值
    kLinear = 1,     ///< LINEAR：p = f + i*C + i*O
    kRatFunc = 2,  ///< RAT_FUNC：p = (N1*i^2 + N2*i + N3)/(D1*i^2 + D2*i + D3)
    kTabIntp = 3,  ///< TAB_INTP：查表线性插值
    kTabNoIntp = 4,           ///< TAB_NOINTP：查表取阶梯
    kTabVerb = 5,             ///< TAB_VERB：文本表
    kFormulaUnsupported = 6,  ///< FORM：公式仅文本，不做数值转换（B-8）
    kNone = 7,                ///< 无 COMPU_METHOD
};

/** IF_DATA XCP 传输层标识。 */
enum class XcpTransportDto : std::int32_t {
    kNone = 0,      ///< 未声明具体传输层
    kCan = 1,       ///< XCPonCAN
    kFlx = 2,       ///< XCPonFLX
    kUsb = 3,       ///< XCPonUSB
    kSxi = 4,       ///< XCPonSPI/SCI（SXIP）
    kTcpIp = 5,     ///< XCPonTCP/IP
    kUdpIp = 6,     ///< XCPonUDP/IP
    kSimulink = 7,  ///< XCPonSIMULINK
};

/** DAQ 列表类型（IF_DATA XCP DAQ_LIST TYPE）。 */
enum class DaqListTypeDto : std::int32_t {
    kUnknown = 0,
    kDaq = 1,      ///< DAQ（采集）
    kStim = 2,     ///< STIM（激励）
    kDaqStim = 3,  ///< DAQ_STIM
};

/**
 * @brief RECORD_LAYOUT 的归一化类别（批次13，B-4/B-12）。
 * @details 判定完全来自上游 `RecordLayout` 的既有 getter（不引入新解析）：
 *          除单段 `FNC_VALUES`（Position==0 且 ADDRESS_TYPE==DIRECT）外
 *          全部布局字段均为默认的版式记为 kPlainScalar；含任一
 *          AXIS_PTS_* 记为 kAxisPts；含 RESERVE/DIST_OP/SHIFT_OP/RIP_ADDR/
 *          IDENTIFICATION/AXIS_RESCALE/FIX_NO_AXIS_PTS 等非连续要素记为
 *          kComplex；DEPOSIT 未给出或模块内查不到该 RECORD_LAYOUT 记为
 *          kNotProvided（**表示"无版式信息"，不等于"版式不可执行"**，
 *          桥接层此时不得调用可执行性判定器，仍由 B-3 元素宽度门拒绝）。
 */
enum class RecordLayoutKindDto : std::int32_t {
    kNotProvided = 0,  ///< 无 DEPOSIT 名 / 模块内查不到版式
    kPlainScalar = 1,  ///< 纯标量 FNC_VALUES 连续版式（可执行）
    kAxisPts = 2,      ///< 含 AXIS_PTS_*（仅元数据）
    kComplex = 3,      ///< 含非连续要素（拒绝扁平化猜测）
};

// ----------------------------------------------------------------------------
// 结构体 DTO
// ----------------------------------------------------------------------------
/**
 * @brief 数组维度信息（B-1/B-15）。
 *
 * extent 为该维元素个数；source_lower_bound 保留 A2L 中显式给出的下界（若有）。
 * 批次11 事实：上游 a2llib 词法+语法均无 ARRAY_DIMENSION/SOURCE_LOWER_BOUNDS
 * （grep 0 命中）→ 该字段恒 0，且含此属性的 A2L 会在上游直接 ParseFailed；
 * 规则 2D 以 MATRIX_DIM 表达（B-2 注记，§6.3 缺口表）。
 * 规则：只要任一维度的 stride 不等于"前面低维乘积 ×
 * 元素大小"，即视为不规则布局， bridge
 * 必须整体拒绝（kInvalidLayout），禁止展平猜测。
 */
struct DimensionDto {
    std::int64_t source_lower_bound =
        0;                          ///< A2L 声明的下界（上游无语法 → 恒 0）
    std::uint64_t extent = 0;       ///< 该维元素个数
    std::uint64_t byte_stride = 0;  ///< 相邻元素间实际字节步长（由源数据推导）
};

/** 线性/有理函数系数包（LINEAR: [C,O,F]；RAT_FUNC: [N1,N2,N3,D1,D2,D3]）。 */
struct NumericCoefficientsDto {
    double c = 0.0;   ///< LINEAR 比例项系数
    double o = 0.0;   ///< LINEAR 偏移项（加在 i*O 上）
    double f = 0.0;   ///< LINEAR 常数项
    double n1 = 0.0;  ///< RAT_FUNC 分子二次项
    double n2 = 0.0;  ///< RAT_FUNC 分子一次项
    double n3 = 0.0;  ///< RAT_FUNC 分子常数项
    double d1 = 0.0;  ///< RAT_FUNC 分母二次项
    double d2 = 0.0;  ///< RAT_FUNC 分母一次项
    double d3 = 0.0;  ///< RAT_FUNC 分母常数项
};

/** 数值/文本转换表的单行（TAB_*）。 */
struct ConversionTableEntryDto {
    double input_min = 0.0;  ///< 原始值区间下界
    double input_max = 0.0;  ///< 原始值区间上界（TAB_INTP 插值用）
    std::string
        output_text;  ///< 输出（文本表时为字符串，数值表为其十进制文本）
    bool numeric_output = false;  ///< 输出是否为数值（TAB_VERB 时为 false）
    double numeric_value = 0.0;   ///< numeric_output==true 时的物理值
};

/**
 * @brief 转换方法快照（B-8 tagged-variant，按 kind 选 payload）。
 */
struct ConversionDto {
    std::string name;  ///< COMPU_METHOD 名（空串 = 无转换）
    std::string unit;  ///< REF_UNIT 单位名（可能为空）
    ConversionKindDto kind = ConversionKindDto::kNone;
    NumericCoefficientsDto coeffs;  ///< kLinear / kRatFunc 有效
    std::vector<ConversionTableEntryDto>
        table;            ///< kTabIntp / kTabNoIntp / kTabVerb 有效
    std::string formula;  ///< kFormulaUnsupported 时保留原文（仅展示）
    std::string
        status_string_ref;  ///< STATUS_STRING_REF（TAB_VERB 状态串表名，可空）
};

/**
 * @brief 符号完整快照（B-13 键 module::symbol；B-1 地址三件套；B-3 类型宽度）。
 *
 * xcp_address 为 ECU_ADDRESS 原值（未经任何换算，B-1）。
 * element_size_bytes 为单元素字节宽（TYPE 显式映射，禁止 sizeof(enum)）。
 */
struct SymbolDto {
    std::string module_name;  ///< 所属 MODULE 名
    std::string name;         ///< 符号名（A2L IDENTIFIER）
    std::string description;  ///< 描述文本（可空）
    SymbolKindDto kind = SymbolKindDto::kMeasurement;
    CharacteristicTypeDto characteristic_type = CharacteristicTypeDto::kNone;
    AsamDataTypeDto data_type = AsamDataTypeDto::kUnknown;
    std::uint64_t xcp_address = 0;  ///< ECU_ADDRESS 原值（B-1）
    std::uint8_t address_extension =
        0;  ///< ECU_ADDRESS_EXTENSION（8 位独立字段，不拼高位）
    std::uint8_t element_size_bytes = 0;   ///< 单元素字节宽（B-3）
    std::vector<DimensionDto> dimensions;  ///< 维度表（标量时为空）
    ArrayOrderDto array_order = ArrayOrderDto::kUnknown;
    ByteOrderDto byte_order = ByteOrderDto::kUnknown;  ///< 来自 MOD_COMMON
    std::optional<std::uint32_t>
        daq_bit_offset;                       ///< BIT_OFFSET（DAQ 位偏，若有）
    std::uint64_t bit_mask = 0;               ///< BIT_MASK（0 = 未标注）
    std::optional<std::uint64_t> error_mask;  ///< ERROR_MASK（测量，若有）
    bool read_write = false;                  ///< READ_WRITE 标志
    ConversionDto conversion;                 ///< COMPU_METHOD 快照
    std::string compu_method_name;   ///< 引用的 COMPU_METHOD 名（可空）
    std::string phys_unit;           ///< PHYS_UNIT 原文（可空）
    std::string ref_memory_segment;  ///< REF_MEMORY_SEGMENT 名（可空）
    bool have_limit = false;         ///< 是否给出 LIMITS
    double lower_limit = 0.0;        ///< 下界（have_limit 时有效）
    double upper_limit = 0.0;        ///< 上界（have_limit 时有效）
    /// @brief DEPOSIT 指向的 RECORD_LAYOUT 名（批次13；MEASUREMENT 恒空）
    std::string record_layout_name;
    /// @brief RECORD_LAYOUT 归一化类别（批次13，B-4/B-12；判定见枚举注释）
    RecordLayoutKindDto record_layout_kind = RecordLayoutKindDto::kNotProvided;
};

/**
 * @brief DAQ 能力块快照（IF_DATA XCP DAQ 公共参数，源自 Daq::Get* 全族）。
 *
 * 各枚举存原始码值（与上游 a2l::xcp 枚举一一对应），由 bridge 映射到领域枚举：
 * type 0=STATIC/1=DYNAMIC；optimisation 0..5（DEFAULT/ODT16/ODT32/ODT64/
 * ALIGNMENT/MAX_ENTRY_SIZE）；address_extension_mode 0=FREE/1=ODT/3=DAQ；
 * identification_field_type
 * 0..3（ABSOLUTE/REL_BYTE/REL_WORD/REL_WORD_ALIGNED）； granularity
 * 1/2/4/8（BYTE/WORD/DWORD/DLONG）；overload 0=NONE/1=PID/2=EVENT。
 */
struct DaqCapsDto {
    std::uint8_t type = 0;                    ///< 0=STATIC 1=DYNAMIC
    std::uint16_t max_daq = 0;                ///< MAX_DAQ
    std::uint16_t max_event_channel = 0;      ///< MAX_EVENT_CHANNEL
    std::uint8_t min_daq = 0;                 ///< MIN_DAQ
    std::uint8_t optimisation = 0;            ///< OPTIMISATION_TYPE 原始码
    std::uint8_t address_extension_mode = 0;  ///< ADDRESS_EXTENSION 原始码
    std::uint8_t identification_field_type =
        0;                         ///< IDENTIFICATION_FIELD_TYPE 原始码
    std::uint8_t granularity = 1;  ///< GRANULARITY_ODT_ENTRY_SIZE_DAQ（字节）
    std::uint8_t max_odt_entry_size = 0;  ///< MAX_ODT_ENTRY_SIZE
    std::uint8_t overload_indicator = 0;  ///< OVERLOAD_INDICATION 原始码
    bool prescaler_supported = false;     ///< PRESCALER_SUPPORTED
    bool resume_supported = false;        ///< RESUME_SUPPORTED
    bool store_daq_supported = false;     ///< STORE_DAQ_SUPPORTED
    bool dto_ctr_supported = false;       ///< DTO_CTR_SUPPORTED
    bool pid_off_supported = false;       ///< PID_OFF_SUPPORTED
    bool odt_strict = false;              ///< OPTIMISATION_TYPE_ODT_STRICT
    std::optional<std::uint32_t>
        timestamp_size_bits;  ///< TIMESTAMP_SIZE（字节数×8；未声明为 nullopt）
};

/**
 * @brief EVENT 通道快照（IF_DATA XCP DAQ 内的 d_event 块）。
 *
 * time_cycle/time_unit 为原始码（上游 uint8 直存）；本仓库无 TIME_UNIT 码表
 * （XCP 规范仅说明周期=TIME_CYCLE×TIME_UNIT），因此 cycle_time_us 换算留待
 * 码表落地，bridge 侧 cycle_time_us 当前保持 0（不臆造换算）。
 */
struct EventChannelDto {
    std::string name;          ///< EVENT 通道名（IDENT 或 STRING）
    std::string short_name;    ///< 短名
    std::uint16_t number = 0;  ///< 通道号（DAQ_LIST 的 EVENT_FIXED 引用它）
    std::uint8_t type = 1;     ///< 1=DAQ 2=STIM 3=DAQ_STIM
    std::uint8_t max_daq_list = 0;  ///< MAX_DAQ_LIST（0xFF=无限制）
    std::uint8_t time_cycle = 0;    ///< TIME_CYCLE 原始码
    std::uint8_t time_unit = 0;     ///< TIME_UNIT 原始码
    std::uint8_t priority = 0;      ///< 优先级（0xFF 最高）
    std::optional<std::uint8_t>
        consistency;  ///< CONSISTENCY 原始码（0=DAQ 1=EVENT 2=ODT 3=NONE）
};

/**
 * @brief 同一参数在 XCP 与 XCPplus 块取值不同时的差异记录（§6.3-A）。
 */
struct XcpPlusConflictDto {
    std::string parameter;      ///< 参数名（如 MAX_CTO/BYTE_ORDER/UDP_PORT）
    std::string xcp_value;      ///< plain XCP 块取值（文本）
    std::string xcpplus_value;  ///< XCPplus 块取值（文本，以它为准）
};

/**
 * @brief IF_DATA XCP 公共参数快照（§6.3-A 优先级：XCPplus > XCP）。
 */
struct IfDataXcpDto {
    bool present = false;        ///< 是否存在有效的 IF_DATA XCP/XCPplus 块
    bool from_xcp_plus = false;  ///< true = 取自 XCPplus 块（§6.3-A）
    bool ambiguous = false;      ///< 多 MODULE 块且未指定 active_module（B-17）
    std::string module_name;     ///< 来源 MODULE 名（B-17 module scope）
    std::uint16_t protocol_version = 0;  ///< PROTOCOL_LAYER VERSION
    std::uint8_t max_cto = 0;            ///< MAX_CTO
    std::uint16_t max_dto = 0;           ///< MAX_DTO
    ByteOrderDto byte_order =
        ByteOrderDto::kUnknown;  ///< PROTOCOL_LAYER BYTEORDER
    std::uint8_t address_granularity =
        0;  ///< ADDRESS_GRANULARITY：1/2/4（0=未标注）
    std::uint8_t default_byte_order =
        0;  ///< BYTE_ORDER 数值编码（MSB_LAST=0/MSB_FIRST=1）
    XcpTransportDto transport = XcpTransportDto::kNone;  ///< 首个传输层实例类型
    std::uint16_t udp_port =
        0;                 ///< XCPonUDP/IP 端口（transport==kUdpIp 时有效）
    std::string udp_host;  ///< XCPonUDP/IP 主机名/IP（可空）
    /// @brief PACKET_ALIGNMENT 上游**原始码**（批次12，不做语义换算）
    /// @details 0/1/2 = 8/16/32 bit（上游 `UdpPacketAlignment`）。上游
    /// `SetPacketAlignment()` 对非 `PACKET_ALIGNMENT_8/16/32` 的字面量
    /// **静默保持默认 0**，故 0 无法区分“未声明”与“声明了非法值”
    /// （设计 §6.3 已知限制）。位宽换算由桥接层承担。
    std::uint8_t udp_packet_alignment = 0;
    /// @brief `OPTIONAL_TL_SUBCMD <IDENT>` 声明的子命令码（批次12）
    /// @details 原值透传（0xFA/0xFC/0xFD/0xFF）；上游对未识别名静默丢弃，
    ///          故本列表只含上游已识别项，空不代表 A2L 未声明。
    std::vector<std::uint8_t> udp_sub_commands;
    std::optional<double>
        resource_mask;  ///< 暂不透出的资源位图（保留扩展位，当前不用）
    // ---- 批次10 扩展（B-16/B-17/§6.1/§6.3-A 数据补全） ----
    std::array<std::uint16_t, 7> timers =
        {};  ///< PROTOCOL_LAYER t1..t7（毫秒；0=未声明）
    std::vector<std::uint8_t>
        optional_commands;  ///< OPTIONAL_CMD 声明的命令码（XCP 标准码）
    std::string
        seed_and_key_function;    ///< SEED_AND_KEY_EXTERNAL_FUNCTION（文件名）
    bool has_ecu_states = false;  ///< 是否声明 ECUSTATE 块
    std::optional<DaqCapsDto> daq_caps;   ///< DAQ 能力块（声明了 DAQ 才有效）
    std::vector<EventChannelDto> events;  ///< EVENT 通道列表
    std::vector<XcpPlusConflictDto>
        plus_conflicts;  ///< XCP vs XCPplus 同参数差异（以 XCPplus 为准，Info
                         ///< 级）
};

/**
 * @brief DAQ 列表中的单个 ODT 条目快照（冻结布局来源）。
 */
struct OdtEntryDto {
    std::uint8_t number = 0;             ///< 条目号（1 基）
    std::uint64_t address = 0;           ///< 条目地址（ECU 地址空间）
    std::uint8_t address_extension = 0;  ///< 地址扩展字节
    std::uint8_t size = 0;               ///< 本条目字节数
    std::uint8_t bit_offset = 0;         ///< 位偏（BIT_OFFSET，0 = 无）
};

/**
 * @brief 单个 ODT 快照。
 */
struct OdtDto {
    std::uint8_t number = 0;           ///< ODT 号（1 基）
    std::vector<OdtEntryDto> entries;  ///< ODT 条目列表
};

/**
 * @brief 预定义 DAQ_LIST 快照（IF_DATA XCP DAQ_LIST ... PREDEFINED）。
 */
struct DaqListDto {
    std::uint16_t number = 0;  ///< DAQ 通道号 EPK
    DaqListTypeDto type = DaqListTypeDto::kUnknown;
    std::optional<std::uint8_t> max_odt;          ///< MAX_ODT（若声明）
    std::optional<std::uint8_t> max_odt_entries;  ///< MAX_ODT_ENTRY（若声明）
    std::optional<std::uint8_t> first_pid;        ///< FIRST_PID（若声明）
    std::optional<std::uint16_t> event_fixed;     ///< EVENT_FIXED（若声明）
    bool daq_packed_mode_supported = false;       ///< DAQ_PACKED_MODE_SUPPORTED
    std::vector<OdtDto> predefined_odts;  ///< PREDEFINED 展开后的 ODT 列表
};

/**
 * @brief TYPEDEF_STRUCTURE 的单个成员元数据（批次13，B-12）。
 * @details 只保存声明本身给出的信息，**不递归展开** `Typedef` 指向的类型，
 *          也不据此推导可读写布局（首版 STRUCTURE 一律不可读写）。
 */
struct StructMemberDto {
    std::string name;          ///< 成员名（A2lStructureComponent::Name）
    std::string typedef_name;  ///< 引用的 TYPEDEF 名（空=非结构成员引用）
    std::uint64_t address_offset = 0;  ///< ADDRESS_OFFSET（结构起始为基准）
    std::uint8_t address_type = 0;     ///< A2lAddressType 原始码
    std::uint8_t layout = 0;           ///< A2lLayout 原始码
    std::vector<std::uint64_t> matrix_dim;  ///< MATRIX_DIM（原样透传）
};

/**
 * @brief TYPEDEF_STRUCTURE / INSTANCE 元数据快照（批次13，B-12）。
 * @details 一条记录要么是 TYPEDEF_STRUCTURE（`is_instance=false`，有
 * `size_bytes` 与 `members`），要么是 INSTANCE（`is_instance=true`，有
 * `ref_typedef` 与 `address`）。两者都只做"识别与展示"，不参与寻址与换算。
 */
struct StructInfoDto {
    std::string name;          ///< 对象名（A2lObject::Name）
    std::string description;   ///< 描述文本（A2lObject::Description）
    std::string module_name;   ///< 所属 MODULE（B-17 module scope）
    bool is_instance = false;  ///< true=INSTANCE，false=TYPEDEF_STRUCTURE
    std::string ref_typedef;   ///< INSTANCE 引用的 TYPEDEF 名；结构体自身为空
    std::uint64_t size_bytes =
        0;  ///< Structure::Size()；INSTANCE 固定 0（不可证）
    std::uint64_t address = 0;  ///< INSTANCE 的 ECU_ADDRESS 原值（B-1 口径）
    /// @brief INSTANCE 的 ECU_ADDRESS_EXTENSION 原值（批次15，F5；v5 起有值）
    /// @details 上游 A2lObject::EcuAddressExtension() 明确存在
    ///          （include/a2l/a2lobject.h:57/60），但 v4 的 DTO 没有这个字段，
    ///          于是分段地址 ECU 上实例地址只剩 32 位 —— 拿它去读会**静默指到
    ///          另一段内存**，而地址本身看起来完全合法（缺陷证据见
    ///          code-plan/A2L_集成_R4_遗留修复计划_批次15.md §1 L5）。
    ///          TYPEDEF_STRUCTURE 无地址，本字段对它恒 0。
    std::uint8_t address_extension = 0;
    std::uint8_t address_type = 0;         ///< A2lAddressType 原始码
    std::uint8_t layout = 0;               ///< A2lLayout 原始码
    bool read_write = false;               ///< INSTANCE 的 READ_WRITE 声明
    std::vector<StructMemberDto> members;  ///< 成员元数据（不展开）
};

// ----------------------------------------------------------------------------
// 抽象接口（R2：DLL 侧创建、Release() 同侧销毁；R4：全 noexcept）
// ----------------------------------------------------------------------------

/**
 * @brief 已解析 A2L 文档的只读快照视图。
 *
 * 生命周期：CreateDoc()/CreateDocFromString() 成功后由调用方持有，
 * 用毕必须调用 Release()（DLL 内 delete）。Load/LoadAsync 完成前不得调用查询。
 */
class A2L_INTERFACE IDoc {
public:
    IDoc() = default;
    virtual ~IDoc() = default;

    // 禁拷贝（跨 ABI 对象句柄语义）
    IDoc(const IDoc&) = delete;
    IDoc& operator=(const IDoc&) = delete;

    /**
     * @brief 同步加载并解析 A2L 文件。
     * @param file_path UTF-8 路径。
     * @param module_information_only 仅解析 MODULE 头（A2lParserType
     * 快速模式）。
     * @return kOk 成功；kParseFailed/kIoError 失败（详情见 LastError）。
     */
    virtual ErrorCode Load(const std::string& file_path,
                           bool module_information_only) noexcept = 0;

    /**
     * @brief 指定 IF_DATA XCP 取值的来源 MODULE（B-17，需在 Load 之前调用）。
     *
     * 空串 = 自动：恰有一个 MODULE 含有效 IF_DATA 块时取它；多个 MODULE
     * 同时声明时 `GetIfDataXcp` 返回 kAmbiguousName（不静默取首个）。
     * 非空 = 精确匹配 MODULE 名；匹配不到时 GetIfDataXcp 返回 kNotFound。
     * @param module_name MODULE 名（区分大小写）。
     */
    virtual void SetActiveModule(const std::string& module_name) noexcept = 0;

    /**
     * @brief 是否允许 `/include` 越出主 A2L 根目录（B-18，默认禁止）。
     *
     * 无论开关与否，循环 include 与深度 >32 一律在解析前拒绝（kParseFailed）。
     * @param allow true=允许绝对路径/根目录外的 include。
     */
    virtual void AllowIncludeOutsideRoot(bool allow) noexcept = 0;

    /**
     * @brief 最近一次错误的 A2L 行号（1 基；无信息返回 0）。
     * @details 解析失败（bison 错误）时可用；文件不存在等异常路径可能为 0。
     *          消费方仅在 >0 时填入 Error.line。
     */
    virtual std::uint32_t LastErrorLine() const noexcept = 0;

    /**
     * @brief 最近一次错误的 include 链（B-18 结构化载体，批次12）。
     * @param out 输出向量：由本 DLL 侧先 `clear()` 再填充（与
     *            `ListSymbols`/`GetIfDataXcp` 同一 out-param 分配惯例，
     *            §5.3.3 R2/R3）。
     * @details 元素为 **canonical 全路径（UTF-8）**，顺序 = 主文件 →
     *          触发点，首元素恒为主文件；仅循环/深度/越根三类预扫描错误
     *          非空，其余错误（含解析失败）返回空链。`LastError()` 文本里的
     *          箭头链（仅文件名）保持不变，二者互不替代（B-19：业务只判
     *          ErrorCode，不匹配文本）。
     * @return kOk（含“无链”的空输出）；out 为空指针返回 kBadArgument，
     *         且**不改写**错误通道（查询方法不得污染
     * LastError/LastErrorCode）。
     */
    virtual ErrorCode LastErrorChain(
        std::vector<std::string>* out) const noexcept = 0;

    /**
     * @brief 异步加载（后台线程解析；进度经 progress_cb 回调百分比 0..100）。
     *
     * 线程契约（B-20/A-11）：回调可能在 SDK 内部线程触发；completed_cb(int)
     * 参数 为最终 ErrorCode。回调返回前不得销毁 IDoc。
     * @param file_path UTF-8 路径。
     * @param module_information_only 快速模式开关。
     * @param progress_cb 进度回调（可空）。
     * @param completed_cb 完成回调（必填）。
     * @return kOk 表示任务已受理；kBadArgument 参数非法；kInternal
     * 线程启动失败。
     */
    virtual ErrorCode LoadAsync(
        const std::string& file_path, bool module_information_only,
        std::function<void(int)> progress_cb,
        std::function<void(int)> completed_cb) noexcept = 0;

    /** @brief 最近一次错误文本（UTF-8；成功后为空串）。 */
    virtual const char* LastError() const noexcept = 0;

    /** @brief 最近一次错误码（与 LastError 同步更新）。 */
    virtual ErrorCode LastErrorCode() const noexcept = 0;

    /**
     * @brief 解析进度 0..100（**阶段进度**，非按行百分比；批次13 修订）。
     * @details 上游 `A2lFile::NumberOfLines()` 只在 UTF-16/32 分支被赋值
     *          （`a2lfile.cpp:239-254`，ASCII/UTF-8 恒 0），而 `LineNo()` 内部
     *          要解引用 `scanner_`——该 scanner 是 `ParseFile()` 的**栈对象**
     *          （`a2lfile.cpp:256-277`，解析结束即置空并销毁），跨线程轮询存在
     *          访问已销毁对象的窗口。因此本 SDK 不调用上游进度接口，改由
     *          内部 `std::atomic<int>` 在四个阶段边界发布：
     *          受理=5 → include 预扫描完成=25 → 上游解析完成=70 →
     *          快照构建完成=100；失败/未加载为 0。`LoadAsync` 的
     *          `progress_cb`（若提供）由解析线程在这些边界回调。
     * @note 语义为"进行到哪一步"，不代表已解析的行数比例；
     *       `LoadOptions::progress_notify_percent` 仅作调用方轮询节奏提示。
     */
    virtual int Progress() const noexcept = 0;

    /** @brief 模块数量。要求 Load 已成功，否则返回 0。 */
    virtual std::size_t ModuleCount() const noexcept = 0;

    /**
     * @brief 列出全部符号快照（MEASUREMENT + CHARACTERISTIC）。
     * @param out 输出向量（调用前会被清空重建）。
     * @return kOk 成功；kNotInitialized 未加载。
     */
    virtual ErrorCode ListSymbols(
        std::vector<SymbolDto>* out) const noexcept = 0;

    /**
     * @brief 按限定名 "module::symbol" 或全局唯一裸名查符号（B-13）。
     * @param qualified_or_unique_name 名字。
     * @param out 输出快照。
     * @return kOk / kNotFound / kAmbiguousName / kNotInitialized。
     */
    virtual ErrorCode FindSymbol(const std::string& qualified_or_unique_name,
                                 SymbolDto* out) const noexcept = 0;

    /** @brief 取 IF_DATA XCP 公共参数快照（MODULE 级优先，其次 PROJECT 级）。
     */
    virtual ErrorCode GetIfDataXcp(IfDataXcpDto* out) const noexcept = 0;

    /** @brief 列出全部预定义 DAQ_LIST 快照。 */
    virtual ErrorCode ListDaqLists(
        std::vector<DaqListDto>* out) const noexcept = 0;

    /**
     * @brief 列出全部 TYPEDEF_STRUCTURE 与 INSTANCE 元数据（批次13，B-12）。
     * @param out 输出向量（DLL 侧先 clear 再填充）。
     * @details 只识别、不展开：成员保留名字/偏移/引用的 TYPEDEF 名与 MATRIX_DIM
     *          原码；不对结构体做寻址、换算或读写（消费方必须返回
     *          UnsupportedOperation）。排序稳定：先 TYPEDEF（按名升序），
     *          后 INSTANCE（按名升序）。
     * @return kOk（无结构体时输出空向量）/ kNotInitialized / kBadArgument。
     */
    virtual ErrorCode ListStructures(
        std::vector<StructInfoDto>* out) const noexcept = 0;

    /**
     * @brief 按 COMPU_METHOD 名取转换快照（含 TAB_* 引用的 COMPU_TAB）。
     * @param compu_method_name 名称。
     * @param out 输出。
     * @return kOk / kNotFound / kNotInitialized。
     */
    virtual ErrorCode GetConversion(const std::string& compu_method_name,
                                    ConversionDto* out) const noexcept = 0;

    /** @brief 释放本对象（DLL 内 delete；调用后指针失效）。 */
    virtual void Release() noexcept = 0;
};

// ----------------------------------------------------------------------------
// 工厂（R2/R5：DLL 侧创建 + ABI 版本运行时校验）
// ----------------------------------------------------------------------------

/**
 * @brief 创建空文档对象。
 * @param abi_version 调用方编译期的 kLibA2lAbiVersion；不匹配返回 nullptr。
 * @return 新对象（用毕 Release()），ABI 不匹配时 nullptr。
 */
A2L_INTERFACE IDoc* CreateDoc(std::uint32_t abi_version) noexcept;

}  // namespace liba2l

#endif  // LIBA2L_LIBA2L_API_HPP_
