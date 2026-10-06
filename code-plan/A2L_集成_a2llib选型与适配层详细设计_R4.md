# A2L 能力集成：a2llib 选型评估与 a2lbridge 适配层详细设计（R4）

> 状态：**设计待评审**（本文档不含任何代码实现，按 AGENTS.md「设计方案经用户批准之前禁止写代码」执行）
> 配套清单：`A2L_集成_未定义项清单_R4审核.md`　|　修订史见 §9.1
> 当前收敛点：构建形态（§5.3）已定稿为 `liba2l.dll` + C++ 抽象接口 + `A2L_INTERFACE`，pin main HEAD；
> **B-1～B-20 接口语义已于 R4 全部批准**（见 `A2L_接口语义_B类决策_R4.md`）；PoC 等 submodule 源码落地。
> 关联里程碑：`code-plan/XCP_1.3.0_最小协议核心实现计划.md` §12 第 1~3 项（最小 A2L / DAQ-ODT-DTO）
> 本轮范围（用户已确认）：引入 a2llib + 独立适配层；Boost 用 `find_package` + `LIBXCP_BOOST_ROOT` 推导；
> 首个验收里程碑 = 变量名→地址→SHORT_UPLOAD 读值 + IF_DATA XCP 自动建链 + DAQ/ODT/DTO 解包；测试含黄金样本回归。

---

## 1. 选型结论

### 1.1 结论

**可以采用 [ihedvall/a2llib](https://github.com/ihedvall/a2llib) 作为 libxcp 的 A2L（ASAP2）解析引擎**，
但必须满足两个前提：

1. **不直接进 XcpCore**：以独立静态库 target（本设计称 `a2lbridge`）封装，`libxcp` 保持"仅依赖标准库 + Threads(+ws2_32)"的既有架构不变量。
2. **版本固定 + 不改 thirdparty**：上游源码以 git submodule 固定在具体 commit，本工程不修改其任何文件。

### 1.2 证据（均来自实际抓取的上游文件，非记忆推断）

| 维度 | 事实 | 来源 |
|---|---|---|
| 许可证 | MIT（`SPDX-License-Identifier: MIT`），第三方声明为 Boost BSL-1.0 / GTest BSD-3 | `LICENSE`、`LICENSE-3RD-PARTY.md` |
| 语言标准 | 上游强制 `set(CMAKE_CXX_STANDARD 23)` → **必须隔离，见 §5.3** | 根 `CMakeLists.txt` |
| 解析器 | Flex/Bison，**生成产物已入库**（`src/a2lparser.cpp` 381 KB、`src/xcp/xcpdataparser.cpp` 222 KB、各 flexer/scanner），默认 `A2L_FLEX=OFF` → 编译不需要装 flex/bison | `CMakeLists.txt`、仓库 tree |
| 必需依赖 | `find_package(Boost CONFIG COMPONENTS locale filesystem process)` + `target_link_libraries(a2l PUBLIC uchardet::libuchardet)` | `script/boost.cmake`、`script/uchardet.cmake`、根 `CMakeLists.txt` |
| 可选依赖 | wxWidgets/expat/utillib（`A2L_TOOLS=OFF` 关闭）、GTest（`A2L_TEST=OFF` 关闭） | 根 `CMakeLists.txt` |
| CI 覆盖 | 只有 `cmake-linux.yml`（ubuntu-latest, Release, vcpkg）与 `cmake-windows.yml`；**macOS 无 CI** | `.github/workflows/` |
| 编码处理 | `CheckBom()` 识别 UTF-32 BE/LE、UTF-16 BE/LE、UTF-8 BOM、ASCII；非 ASCII 走 `boost::locale::conv::to_utf<char>()`；另有 `ConvertAllStrings(encoding)` 做逐符号编码归一 | `src/a2lfile.cpp` |
| 异步解析 | `AsynchParseFile(A2lReadyFunction)` + `ProgressInfo()`（按行号百分比）+ `LineNo()` | `src/a2lfile.cpp` |
| 部分解析 | `A2lParserType::PARSE_MODULE_INFORMATION_ONLY` 可只取 Module 信息（大文件快速列表） | `src/a2lfile.cpp` |
| include 合并 | `Merge(A2lFile&)` 支持 ASAP2 `INCLUDE` 语义 | `src/a2lfile.cpp` |

### 1.3 能力对照（AGENTS.md 推荐架构的 A2L 分支）

| libxcp 需要的能力 | a2llib 是否提供 | 关键 API（实测签名） |
|---|---|---|
| Parser / Database | ✅ | `A2lFile::{Filename, ParseFile, Project}`、`A2lProject::Modules()`、`Module::{Measurements, Characteristics}` |
| Measurement | ✅ | `Measurement::{EcuAddress() uint64, DataType() A2lDataType, AddressType(), Layout(), ArraySize(), BitMask(), Conversion(), ReadWrite(), LowerLimit/UpperLimit}` |
| Characteristic | ✅ | `include/a2l/characteristic.h` |
| CompuMethod | ✅ | `compumethod.h` / `compuvtab.h` / `computab.h` / `compuvtabrange.h` / `unit.h` |
| RecordLayout | ✅ | `recordlayout.h` |
| EventChannel | ✅ | `xcp/event.h`(8.3 KB)、`xcp/daqevent.h` |
| IfDataXcp | ✅ | `Module::GetXcpDataBlock()` → `XcpDataBlock::{GetVersion, GetCommonParameters, GetXcpOnCans/TcpIps/UdpIps/Sxis/Usbs/Flxs}` |
| XCP on UDP/IP（对接现有 `udp_transport`） | ✅ | `XcpOnUdpIp::{GetPort, GetAddress, GetIpv6, GetHostName, GetMaxBusLoad, GetMaxBitRate, GetPacketAlignment, GetSubCmds}` |
| t1..t7 / MAX_CTO / MAX_DTO / ByteOrder / AG / 可选命令 / Seed&Key 函数名 | ✅ | `ProtocolLayer::{GetTimer(T1..T7), GetMaxCto, GetMaxDto, GetByteOrder, GetAddressGranularity, GetOptionalCommands, GetSeedAndKeyFunction, GetEcuStates}` |
| DAQ 分配约束（Allocator 需要） | ✅ 很全 | `Daq::{GetType STATIC/DYNAMIC, GetMaxDaq, GetMinDaq, GetMaxEvent, GetOptimisationType ODT_TYPE_16/32/64/ALIGNMENT/MAX_ENTRY_SIZE, GetAddressExtension FREE/ODT/DAQ, GetIdentificationFieldType, GetGranularityOdtEntrySizeDaq, GetMaxOdtEntrySize, GetOverloadIndicator, GetPrescalerSupported, GetResumeSupported, GetStoreDaqSupported, GetDtoCtrSupported, GetPidOffSupported, GetMaxDaqTotal/OdtTotal/OdtDaqTotal/DtoEntries*, GetStim, GetTimestamp, GetDaqList, GetEventList}` |
| MemoryAccess / ECU_STATE | ✅ | `xcp/memoryaccess.h`、`xcp/ecustate.h` |
| Programming（SEGMENT/PAGE/PAG/PGM） | ✅ | `xcp/segment.h`、`page.h`、`pag.h`、`pgm.h` |
| TimeCorrelation | ✅ | `xcp/timecorrelation.h`、`clock.h`、`timestamp.h`、`timestampresolution.h`、`timestampcharacteristic.h` |
| IF_DATA **XCPplus** | ✅ **提供**（修正见下） | `A2lMemoryLayout::GetXcpPlusDataBlock()` / `GetXcpDataBlock()`、`A2lMemorySegment::GetXcpPlusDataBlock()` / `GetXcpDataBlock()`（均在 `include/a2l/a2lstructs.h`，返回 `const xcp::XcpDataBlock*`） |

> **勘误（第二轮评审）**：本文档初稿曾把 XCPplus 列为"❌ 缺口"，理由是只看 `XcpDataBlock` 类自身没有 XCPplus 容器。
> 实读 `a2lstructs.h` 后确认：XCPplus 是在 **MEMORY_LAYOUT / MEMORY_SEGMENT 层**通过独立的
> `GetXcpPlusDataBlock()` 暴露的，与规范 §8.2「同一参数若同时存在 XCPplus 与 XCP，Master 优先使用 XCPplus」
> 的语义正好对应。原结论撤销，改为按 §6.3-A 实现优先级选择逻辑。
| A2L 解压/解密（GET_ID Type=4） | ❌ 不在解析器范围 | 规范定义 `XCP_DecompressA2L()` 外部函数，需另立里程碑 |
| Seed&Key 算法 DLL/SO 加载 | ❌ 只给函数名字符串 | `GetSeedAndKeyFunction()`；现有 `xcp_master.hpp` 已明确该边界 |

### 1.4 与备选方案的比较（为什么不自写 parser）

| 方案 | 工作量 | 风险 | 判定 |
|---|---|---|---|
| **采用 a2llib + 适配层** | 适配层约 5 个模块；语法零成本 | 上游单维护者、C++23、Boost/uchardet 依赖 | **选定** |
| 自写最小 A2L parser | ASAP2 语法极大（MEASUREMENT/CHARACTERISTIC/RECORD_LAYOUT/FUNCTION/GROUP/MODULAR…），纯文法即数周至数月 | 长期兼容性由我们独担 | 否 |
| 其他开源（asap2parser 等） | 需重新调研授权与 XCP IF_DATA 支持度 | 多数无 XCP IF_DATA 二次解析 | 暂不 |

---

## 2. 本机环境实测（决定构建方案的事实）

| 检查项 | 实测结果 |
|---|---|
| CMake | 4.0.2 |
| 编译器 | Visual Studio 2022（工具集 vc143），x64 |
| Boost | `C:\boost`：`include/boost-1_86`、`lib/cmake/Boost-1.86.0/BoostConfig.cmake`；组件 `locale`/`filesystem`/`process` 均有 release+debug 双配置静态库（MT） |
| boost::locale 体积 | release 4.7 MB / debug 26 MB → **推断为 Windows-API 后端（未捆绑 ICU）**，`C:\boost` 下无 `icu*.dll`；系统有 `C:\Windows\System32\{icuuc,icuin}.dll` 但缺 `icuio.dll` |
| uchardet | **本机不存在**（全盘常见目录搜索无果） |
| git | 2.55.0，`C:\Program Files\Git\cmd\git.exe` |
| 网络 | `github.com:443` TCP **不通**；`codeload.github.com:443`/`gh-proxy.com:443` TCP 通但 TLS 失败：`curl (35) schannel: AcquireCredentialsHandle failed: SEC_E_NO_CREDENTIALS`；`Invoke-WebRequest` 同报 SSL 失败；无代理环境变量。**与工程 `CMakeLists.txt` L48 记录的 googletest 拉取失败是同一根因** |

> 后果：本会话内无法自行 clone 上游；`thirdparty/a2llib` 需由用户在本地终端执行 `git submodule add` 落地。

---

## 3. 架构设计

### 3.1 分层与依赖方向

```
Application / Qt UI（未来）
        │  只用 a2lbridge 的自有类型，永远看不到 liba2l:: / a2l::* / Boost
        ▼
┌───────────────────────────────────────────────┐
│ a2lbridge (静态库 target: libxcp_a2lbridge)     │
│   IA2lDatabase / SymbolInfo / DaqLayout ...    │
│   ├── A2lDatabaseImpl      符号表 DTO → SymbolInfo                   │
│   ├── IfDataXcpImpl        传输配置 + t1..t7 + AG/BO                 │
│   ├── DaqLayoutImpl        DAQ/EVENT DTO → ODT/DTO 映射              │
│   ├── RecordLayoutImpl     FNC_* 位布局 → 字节解码                    │
│   └── CompuMethodEval      IDENTICAL/LINEAR/TAB_* → raw↔phys         │
└───────────────────────────────────────────────┘
        │ 只 #include "liba2l/liba2l_api.hpp"（C++ 抽象接口，§5.3.3）
        ▼
liba2l.dll  ← 导出层独占上游 include；上游藏在 DLL 内部
        ▼
thirdparty/a2llib (submodule, pinned commit c3105749 = main HEAD, 不可修改)
        ▼
Boost(locale,filesystem,process) + uchardet(shim)   ← 全部封在 DLL 内
        ▲
libxcp (XcpCore)  ← 不依赖 a2lbridge，也不依赖 liba2l（保持零第三方依赖）
```

关键点：**`libxcp` 与 `a2lbridge` 是平级的两个库**，由上层（应用/QML 层或未来的集成层）同时链接。
`a2lbridge` 可以 `PUBLIC` 依赖 `libxcp`（复用 `Bytes`、`XcpAddress40`、`CommandTimeouts`、`UdpTransportConfig` 等已有类型），反向依赖禁止。
这样 code-plan 批次 1~7 反复验证过的"核心库不依赖 Qt/A2L/CAN 厂商库"这条不变量继续成立。

**R3 变更提示**：本图初稿写的是"桥接层是唯一允许 `#include <a2l/...>` 的地方"。
按 §5.3.3 定稿后，该位置改为 **`liba2l_export.cpp`（DLL 内部）**，桥接层连 `liba2l::` 上游类型也看不到，
只看到本工程自写的 C++ 抽象接口。

### 3.2 目录规划

> **批次9 勘误**：`a2lbridge/` 与 `uchardet-shim/` 已并入 `thirdparty/a2l-sdk/`，
> 主树顶层不再有 `a2lbridge/` 目录；终态布局见
> `A2L_集成_R4_实施记录_批次9_SDK工程合并.md` §1。

```
thirdparty/
  a2llib/                    # git submodule，pin main HEAD c3105749（固定 SHA），不修改（仅 SDK 构建时消费）
  uchardet-shim/             # 本工程新增：仅 uchardetConfig.cmake（见 §5.2，P0 阻塞项）
  a2l-sdk/                   # 本工程新增：独立 project，产出 liba2l.dll（见 §5.3.7）
    include/liba2l/liba2l_api.hpp   # 唯一对外头（抽象接口 + 自有 DTO）
    src/liba2l_export.cpp           # 全工程唯一 #include <a2l/...> 的文件
    build-sdk.ps1                   # Windows-only 一键构建并输出 prepared root
a2lbridge/                          # ★ target = libxcp_a2lbridge
  include/libxcp/a2l/               # 主树自有类型（§4），不外泄 liba2l 接口
    a2l_types.hpp            # 自有数据类型（SymbolKind/AsamDataType/ByteOrder...）
    ia2l_database.hpp        # 查询接口
    if_data_xcp.hpp          # IF_DATA XCP 提取结果
    daq_layout.hpp           # DAQ/ODT/DTO 布局
    record_layout.hpp        # 记录布局解码
    compu_method.hpp         # raw↔phys 换算
    a2l_bridge.hpp           # 门面：LoadFromFile()/LastError()/Progress()
  src/                       # 只依赖 liba2l/liba2l_api.hpp，禁止出现 <a2l/...>
    a2l_database_impl.cpp
    if_data_xcp_impl.cpp
    daq_layout_impl.cpp
    record_layout_impl.cpp
    compu_method_eval.cpp
    a2l_bridge.cpp
tests/
  a2l_golden_test.cpp        # 黄金样本回归（§7）
  a2l_gen/                   # ★ A-6：测试内生成 A2L（§7.1-A）
    gen_a2l.py               #   Python 标准库实现，零第三方依赖
    golden_spec.json         #   样本描述（符号/COMPU_METHOD/IF_DATA/DAQ）
    expected/*.json          #   期望值基线（由 spec 推导）
```

命名遵循工程既有约定（批次 4/5 已落地）：`struct` 成员无 `m_` 前缀、无尾下划线；`class` 私有成员 `m_xxx_`；命名空间 `calmcar::xcp::a2l`；Doxygen 中文注释。

**命名统一表（回答 A-3「liba2l 前缀」）**：

| 对象 | 名称 |
|---|---|
| SDK 交付产物 | `liba2l.dll` / `liba2l.lib`（import lib） |
| SDK 内 CMake target | `liba2l` |
| SDK 顶层 project | `liba2l_sdk`（位于 `thirdparty/a2l-sdk/`） |
| 导出接口命名空间 / 头文件 | `liba2l::` / `liba2l/liba2l_api.hpp` |
| 主树 imported target | `liba2l::liba2l` |
| 主树桥接库 target / 产物 | `libxcp_a2lbridge` / `libxcp_a2lbridge.lib` |
| 桥接层命名空间 | `calmcar::xcp::a2l`（不变） |
| 构建开关 / SDK 路径变量 | `LIBXCP_BUILD_A2L` / `LIBXCP_LIBA2L_ROOT` |

---

## 4. 对外接口设计（草案，评审通过后才写入头文件）

> 以下为**设计意图表达**，不是最终头文件内容；参数顺序与是否 `optional` 在评审时定稿。

### 4.1 基础类型（`a2l_types.hpp`）

```cpp
namespace calmcar::xcp::a2l {

/// @brief 符号类别
/// @details Structure 本体仅保留元数据，不执行读写（B-12）；成员级可执行能力由批次18
///          STRUCTLEAF 提供：限定路径展开为 kind=Measurement 的可寻址叶子（见 R4.9），本体整块拒绝边界不变。
enum class SymbolKind : std::uint8_t { Measurement, Characteristic, Structure };

/// @brief CHARACTERISTIC 五种标准类型（B-11）
enum class CharacteristicType : std::uint8_t { None, Value, Curve, Map, ValBlk, Ascii };

/// @brief ASAM 数据类型的本工程归一化枚举
/// @details 最终成员及宽度必须由 main HEAD 的 a2lenums.h 逐项核对；禁止按枚举序号或 sizeof 推断（B-3）。
enum class AsamDataType : std::uint8_t {
    Unknown, UByte, SByte, UWord, SWord, ULong, SLong, ULong64, SLong64,
    Float16, Float32, Float64, Boolean, BitText, String,
};

/// @brief 内存字节序（MSB_LAST = Little Endian）
enum class ByteOrder : std::uint8_t { MsbLast, MsbFirst };

/// @brief 地址粒度（一个 XCP 地址单位对应的字节数：1/2/4）
enum class AddressGranularity : std::uint8_t { Byte = 1, Word = 2, Dword = 4 };

/// @brief 数组内存主序
enum class ArrayOrder : std::uint8_t { RowMajor, ColumnMajor };

/// @brief 数组一维的归一化描述（B-2）
struct Dimension {
    std::int64_t source_lower_bound = 0;  ///< A2L 原始下界
    std::uint64_t extent = 0;             ///< 元素数量
    std::uint64_t byte_stride = 0;        ///< 该维索引增加 1 时的实际字节跨度
};

/// @brief 不丢失 64-bit 整数与文本换算结果的物理值（B-14）
using PhysicalValue = std::variant<std::int64_t, std::uint64_t, double, std::string, bool>;

/// @brief COMPU_METHOD 的归一化类别（B-8）
enum class ConversionKind : std::uint8_t {
    Identical, Linear, RatFunc, TabIntp, TabNoIntp, TabVerb, FormulaUnsupported
};

/// @brief 数值换算系数；RAT_FUNC 最多使用六项，具体顺序由实现前的上游/规范核对确定
struct NumericCoefficients { std::array<double, 6> values = {}; };

/// @brief 数值表或文本表的一项；区间端点仅在对应 A2L 表类型存在时使用
struct ConversionTableEntry {
    double input_min = 0.0;
    double input_max = 0.0;
    PhysicalValue output;
};

/// @brief 与 ConversionKind 配套的载荷
using ConversionPayload = std::variant<std::monostate, NumericCoefficients,
                                       std::vector<ConversionTableEntry>, std::string>;

/// @brief 物理值换算描述
struct ConversionInfo {
    std::string compu_method_name;
    std::string unit;                      ///< PHYS_UNIT 优先，空时解析 UNIT_REF（B-10）
    ConversionKind kind = ConversionKind::Identical;
    ConversionPayload payload;
};

/// @brief 单个符号的地址、类型和布局信息
struct SymbolInfo {
    std::string module_name;                ///< 所属 MODULE；规范键为 module::name（B-13/B-17）
    std::string name;
    std::string description;
    SymbolKind kind = SymbolKind::Measurement;
    CharacteristicType characteristic_type = CharacteristicType::None;
    AsamDataType data_type = AsamDataType::Unknown;
    std::uint64_t xcp_address = 0;          ///< A2L ECU_ADDRESS 原值，不乘 AG（B-1）
    std::uint8_t address_extension = 0;     ///< 独立 8-bit 字段，不是 address 高位（B-1）
    std::uint8_t element_size_bytes = 0;    ///< 由显式类型映射表取得（B-3）
    std::vector<Dimension> dimensions;      ///< 空表示标量；首版执行标量/1D/规则连续2D
    ArrayOrder array_order = ArrayOrder::RowMajor;
    ByteOrder byte_order = ByteOrder::MsbLast;
    std::optional<std::uint32_t> daq_bit_offset; ///< ODT entry 位定位，与 BIT_MASK 分离（B-9）
    std::uint64_t bit_mask = 0;             ///< 数值提取；首版只接受连续掩码
    std::optional<std::uint64_t> error_mask;///< 仅保存；首版不由它自动推导 valid
    bool read_write = false;
    ConversionInfo conversion;
};

}  // namespace calmcar::xcp::a2l
```

### 4.2 数据库接口（`ia2l_database.hpp`）

```cpp
class IA2lDatabase {
public:
    virtual ~IA2lDatabase() = default;

    /// @brief 按 module::symbol 精确查找；无模块前缀时仅全库唯一名称可成功（B-13/B-17）
    virtual Result<SymbolInfo> Find(std::string_view qualified_or_unique_name) const = 0;

    /// @brief 大小写无关通配符检索；稳定排序后再按 max_count 截断（B-13）
    virtual Result<std::vector<SymbolInfo>> Search(std::string_view pattern,
                                                   std::size_t max_count = 200) const = 0;

    /// @brief 已发布不可变快照中的符号数量；加载中返回 NotReady（B-20）
    virtual Result<std::size_t> Count() const = 0;

    /// @brief 计算连续存储所占实际字节数；返回值不除以 AG（B-1）
    virtual Result<std::size_t> ByteSizeOf(std::string_view qualified_name) const = 0;

    /// @brief raw 字节 → tagged 物理值（B-8/B-14）
    virtual Result<PhysicalValue> ToPhysical(std::string_view qualified_name,
                                             BytesView raw) const = 0;

    /// @brief 物理值 → raw 字节；默认严格拒绝越界、NaN/Inf 和不可逆映射（B-15）
    virtual Result<Bytes> FromPhysical(std::string_view qualified_name,
                                       const PhysicalValue& physical_value) const = 0;
};
```

> `Result<T>` / `Error` 为本工程 C++20 自有类型，字段至少包含 `ErrorCode`、`Severity`、`Phase`、
> 文件/行列、MODULE、symbol、message/cause（B-19）。`LastError()` 只保留为 UI 格式化视图，
> 不允许业务逻辑匹配错误字符串。
> **批次12 追加（B-18 结构化载体）**：`Error.include_chain`（`std::vector<std::string>`）承载 include 链，
> 元素为 **canonical 全路径（UTF-8）**，顺序 = 主文件 → 触发点，首元素恒为主文件；非 include 类错误恒空。
> `message` 里的 `a -> b -> a` 文本维持原样（仅文件名，B-19 显示契约），二者互不替代。
> 具名失败码与实现归并码的对应关系见 `A2L_接口语义_B类决策_R4.md` §5。

### 4.3 IF_DATA XCP 提取（`if_data_xcp.hpp`）

```cpp
/// @brief 一种 transport 实例的连接参数（当前只填 UDP，其余留待后续里程碑）
struct TransportEndpoint {
    enum class Kind : std::uint8_t { UdpIp, TcpIp, Can, Sxi, Usb, Flx };
    Kind kind = Kind::UdpIp;
    std::uint16_t version = 0x0100;         ///< IF_DATA XCP 段版本号
    std::string remote_host;                ///< IPv4/IPv6 字符串；CAN 时为通道名
    std::uint16_t remote_port = 0;
    std::string local_host;                 ///< 由 A2L 描述时填入，否则由调用方决定
    std::uint16_t local_port = 0;
    std::uint8_t packet_alignment = 8;      ///< **位宽** 8/16/32 bit（批次12 修正：原默认值 1 与注释矛盾）
    std::vector<std::uint8_t> sub_commands; ///< 传输层子命令码（A2L 关键字 `OPTIONAL_TL_SUBCMD <IDENT>`）
};

/// @brief Protocol Layer 级参数（等价于现有 CommandTimeouts + SessionParameters 的来源）
struct ProtocolLayerInfo {
    std::uint16_t version = 0x0100;
    std::uint16_t timeout_ms[7] = {};       ///< t1..t7，索引 0 = t1
    std::uint8_t max_cto = 0;
    std::uint16_t max_dto = 0;
    ByteOrder byte_order = ByteOrder::MsbLast;
    AddressGranularity address_granularity = AddressGranularity::Byte;
    std::vector<std::uint8_t> optional_commands;       ///< A2L 声明支持的命令码
    std::string seed_and_key_function;                 ///< 外部函数文件名（不含路径）
    bool has_ecu_states = false;
};

/// @brief MODULE 级 IF_DATA XCP 汇总结果
struct IfDataXcpInfo {
    bool ok = false;
    std::string last_error;
    ProtocolLayerInfo protocol_layer;
    std::vector<TransportEndpoint> transports;
    std::optional<DaqInfo> daq;                        ///< 见 §4.4
    std::vector<EventChannelInfo> event_channels;      ///< 见 §4.4
};
```

### 4.4 DAQ / ODT / DTO（`daq_layout.hpp`）

```cpp
/// @brief DAQ 处理器能力（Allocator 的输入约束）
struct DaqInfo {
    bool static_supported = false;                      ///< 首里程碑唯一启用的执行路径（B-5）
    bool dynamic_supported = false;                     ///< 仅保留能力；首版返回 DynamicDaqNotImplemented
    std::uint16_t max_daq = 0;                          ///< MAX_DAQ
    std::uint16_t max_event_channel = 0;
    std::uint8_t min_daq = 0;                           ///< MIN_DAQ
    std::uint8_t odt_entry_min_size_bytes = 1;          ///< GRANULARITY_ODT_ENTRY_SIZE_DAQ
    std::uint8_t max_odt_entry_size = 0;                ///< MAX_ODT_ENTRY_SIZE_DAQ
    enum class OdtType : std::uint8_t { Default, Odt16, Odt32, Odt64, Alignment, MaxEntrySize }
        odt_type = OdtType::Default;                     ///< OPTIMISATION_TYPE → ALLOCATOR 打包对齐
    enum class AddrExtMode : std::uint8_t { Free, PerOdt, PerDaq }
        address_extension_mode = AddrExtMode::Free;      ///< ADDRESS_EXTENSION
    enum class IdFieldType : std::uint8_t { Absolute, RelativeByte, RelativeWord, RelativeWordAligned }
        identification_field_type = IdFieldType::Absolute;
    bool dto_counter_supported = false;                  ///< DTO_CTR_SUPPORTED
    bool pid_off_supported = false;                      ///< PID_OFF_SUPPORTED
    bool prescaler_supported = false;
    bool resume_supported = false;
    bool overflow_flag_supported = false;                ///< OVERLOAD_INDICATION != NONE
    std::optional<std::uint32_t> timestamp_max_size_bits;///< TIMESTAMP_SIZE（DTO 内含时间戳时影响可用载荷）
};

/// @brief Event Channel 描述（含 TIMING 与 DAQ_LIST 引用）
struct EventChannelInfo {
    std::string name;                                    ///< EVENT_CHANNEL 的名字（DAQ_EVENT 引用它）
    std::uint16_t channel_number = 0;
    double cycle_time_us = 0.0;                          ///< MIN_CYCLE_TIME / CYCLE_TIME
    std::vector<std::uint16_t> daq_list_numbers;         ///< 该事件下要 START 的 DAQ LIST
    bool has_consistency = false;
};

/// @brief 一条 DTO 的解析结果（供 DtoDecoder 回调使用）
struct DecodedDtoSample {
    std::string symbol_name;                             ///< 来自 WRITE_DAQ/STATIC 映射
    std::uint64_t address = 0;
    std::uint8_t address_extension = 0;
    Bytes raw;                                           ///< DTO 中的实际原始字节；AG 不改变字节内容
    PhysicalValue physical_value;                        ///< 已套用 COMPU_METHOD 的 tagged value（B-14）
    std::optional<bool> valid;                           ///< 质量状态；首版不由 ERROR_MASK 猜测（B-9）
};

/// @brief DTO envelope 的运行时冻结布局（B-7）
struct DtoFrameLayout {
    DaqInfo::IdFieldType identification_field_type = DaqInfo::IdFieldType::Absolute;
    bool first_odt = false;
    bool counter_enabled = false;
    bool timestamp_enabled = false;
    std::uint8_t timestamp_size_bits = 0;  ///< 时间戳位宽（TIMESTAMP_SIZE）
    bool overflow_indicator = false;       ///< OVERLOAD 指示位
    bool pid_off = false;                  ///< PID_OFF（批次11 补齐；true 时 Decode 显式拒绝）
    std::uint8_t header_bytes = 1;         ///< envelope 固定头（PID）字节数，冻结后不可变
};

/// @brief 实际 DAQ 配置的不可变快照（B-5/B-6；批次14 已落地为真实类型）
/// @details PREDEFINED STATIC 优先来自 READ_DAQ；可配置 STATIC 来自本端 WRITE_DAQ 账本。
///          A2L ELEMENT_LIST 只作候选和一致性约束，不能充当实际 ODT entry 顺序的唯一依据。
///          落地形状（`daq_layout.hpp`）：
///            enum class DaqLayoutSource { A2lPredefined, LocalLedger, EcuReadback };
///            struct DaqOdtRoute { uint8_t pid; uint16_t daq_list; uint8_t odt_number; };
///            struct DaqLayoutSnapshot {
///                DaqLayoutSource source; uint32_t generation;
///                std::vector<DaqListLayout> lists;   // EPK → ODT/Entry
///                std::vector<DaqOdtRoute>  routes;   // PID → 单个 ODT（账本/回读才有）
///            };
///          `OdtEntryLayout` / `DecodedDtoSample` 另带 `symbol_aliases`：歧义地址
///          不选归属（`symbol_name` 留空）但候选全留（B-6"不猜"与"留痕"并存）。
///          解码权威顺序：有 `routes` → 按 PID 定位**单个 ODT**（docs L2225
///          `绝对 ODT 号 = FIRST_PID + 相对 ODT 号`）；来源要求账本/回读却无
///          `routes` → `InvalidLayout`；纯 A2L PREDEFINED → 维持"EPK == 列表号"。
///          构建入口：`A2lBridge::CreateDaqLayout`（A2L）、
///          `CreateDaqLayoutFromLedger`（本端账本）、
///          `CreateDaqLayoutFromEcuReadback`（READ_DAQ 取证）。
struct DaqLayoutSnapshot;   // 定义见 a2lbridge/include/libxcp/a2l/daq_layout.hpp

/// @brief 基于实际配置快照解析 DTO；首里程碑只启用 STATIC DAQ
class IDaqLayout {
public:
    virtual ~IDaqLayout() = default;

    /// @brief 先按 frame_layout 解 envelope，再按冻结快照解 entry（B-7）
    virtual Result<std::vector<DecodedDtoSample>> Decode(
        const DtoFrameLayout& frame_layout, BytesView dto) const = 0;

    /// @brief 估算符号集合的实际字节数；AG 仅用于 XCP 元素计数/对齐，不改变字节数（B-1）
    virtual Result<std::size_t> PackedByteSize(
        const std::vector<std::string>& qualified_symbol_names) const = 0;
};
```

### 4.5 门面（`a2l_bridge.hpp`）

```cpp
/// @brief 一次加载的选项
struct LoadOptions {
    bool module_information_only = false;   ///< 超大文件先只列 MODULE（映射上游 PARSE_MODULE_INFORMATION_ONLY）
    bool require_if_data_xcp = true;        ///< 无 IF_DATA XCP 时是否判为失败
    std::uint32_t progress_notify_percent = 5;
};

class A2lBridge {
public:
    /// @brief 加载并解析 A2L（含 INCLUDE 递归合并）；失败返回结构化 Error（B-18/B-19）
    static Result<std::unique_ptr<A2lBridge>> Load(const std::string& file_path,
                                                   const LoadOptions& options = {});

    /// @brief 异步 one-shot 加载（B-20）
    /// @details 回调在工作线程触发；不可变快照发布后恰好调用一次。调用方负责切回 UI 线程。
    ///          加载中查询返回 NotReady；取消/析构必须安全停止并 join。
    Result<void> LoadAsync(const std::string& file_path, const LoadOptions& options,
                           std::function<void(Result<void>)> ready);

    /// @brief 0..100，取自上游 ProgressInfo()
    int Progress() const;

    [[nodiscard]] const IA2lDatabase& Database() const;
    [[nodiscard]] const IfDataXcpInfo& XcpInfo() const;
    [[nodiscard]] std::unique_ptr<IDaqLayout> CreateDaqLayout() const;
    [[nodiscard]] std::string LastError() const;   ///< 含上游行号（LineNo()）
};
```

### 4.6 R4 接口语义决策汇总（B-1～B-20 已批准）

完整决策、失败行为和测试门禁见 `A2L_接口语义_B类决策_R4.md`。本节列出实现边界：

| 范围 | R4 决策 |
|---|---|
| 地址 | A2L ECU_ADDRESS 原值 + 独立 extension；AG 只用于元素计数、地址步进和对齐 |
| 数组 | 执行标量、1D、规则连续 2D；复杂布局只保留元数据 |
| RECORD_LAYOUT | 执行 VALUE、连续 VAL_BLK、标准连续 FNC_VALUES；复杂布局拒绝 |
| 换算 | 执行 IDENTICAL/LINEAR/RAT_FUNC/TAB_INTP/TAB_NOINTP/TAB_VERB；FORM 只保留原文 |
| CHARACTERISTIC | 五型均建模；执行 Value/连续 ValBlk/定长 Ascii；Curve/Map 仅元数据 |
| STRUCTURE | 仅识别和保存元数据，不展开、不读写 |
| DAQ | 首里程碑只启用 STATIC；实际 READ_DAQ/WRITE_DAQ 账本优先于 A2L 候选顺序 |
| DTO | 使用冻结的 `DtoFrameLayout + DaqLayoutSnapshot`；布局冲突时整帧拒绝 |
| 数据库 | `module::symbol` 规范键；多 MODULE 显式选择；不可变快照并发读 |
| 错误 | C++20 `Result<T>/Error`；异常不跨 DLL；`LastError()` 仅展示 |
| 一致性 | 运行时为真值，按受影响功能分级阻断；t1～t7 不作为 Slave runtime 比对项 |

**明确不允许的静默降级**：未知类型默认宽度、地址乘 AG、extension 拼高位、不规则布局扁平化、
没有实际 entry manifest 时猜 DTO→symbol、不可唯一逆换算、结构体当 byte array、Curve/Map 猜测写入。

---

## 5. 构建集成设计

### 5.1 libxcp 顶层最小新增内容（R4：仅开发验证）

> 当前只保证 Windows/MSVC 的最小开发验证集，完整边界见 `A2L_CMake最小开发验证集_R4.md`。
> **批次9 勘误**：末行 `add_subdirectory(a2lbridge)` 已废除——桥接层改由 SDK 工程编译，
> 主树追加 `find_library(libxcp_a2lbridge)` + `IMPORTED STATIC libxcp::a2lbridge`
> （见批次9记录 §3.3）；本节末"当前约束"中的"主树不查 Boost、不编 thirdparty"
> 自此在构建图层面真正成立。

```cmake
option(LIBXCP_BUILD_A2L "Build the A2L bridge" OFF)
set(LIBXCP_LIBA2L_ROOT "" CACHE PATH "Prepared liba2l root")

if(LIBXCP_BUILD_A2L)
  if(NOT LIBXCP_LIBA2L_ROOT)
    message(FATAL_ERROR "LIBXCP_LIBA2L_ROOT is required")
  endif()
  find_path(LIBXCP_LIBA2L_INCLUDE_DIR NAMES liba2l/liba2l_api.hpp
            PATHS "${LIBXCP_LIBA2L_ROOT}/include" NO_DEFAULT_PATH REQUIRED)
  find_library(LIBXCP_LIBA2L_IMPORT_LIBRARY NAMES liba2l
               PATHS "${LIBXCP_LIBA2L_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
  find_file(LIBXCP_LIBA2L_RUNTIME NAMES liba2l.dll
            PATHS "${LIBXCP_LIBA2L_ROOT}/bin" NO_DEFAULT_PATH REQUIRED)
  add_library(liba2l::liba2l SHARED IMPORTED GLOBAL)
  set_target_properties(liba2l::liba2l PROPERTIES
    IMPORTED_IMPLIB "${LIBXCP_LIBA2L_IMPORT_LIBRARY}"
    IMPORTED_LOCATION "${LIBXCP_LIBA2L_RUNTIME}"
    INTERFACE_INCLUDE_DIRECTORIES "${LIBXCP_LIBA2L_INCLUDE_DIR}")
  add_subdirectory(a2lbridge)
endif()
```

当前约束：`LIBXCP_LIBA2L_ROOT` 必须显式指定；主树不查 Boost、不编 thirdparty；不生成或消费发布型 `liba2lConfig.cmake`；不设计环境变量/注册表/`.deps-cache` 回退；A2L 开关关闭时原构建图不变；CRT 自动校验按 A-10 暂不实施。
### 5.2 uchardet 依赖处置（**R3c：升 main 后重新生效，且为 P0 阻塞项**）

> **批次9 勘误（两点）**：
> ① 方案 A 的落地形态变更：不再需要"假 Config 包 + CMAKE_PREFIX_PATH"，
>   `uchardet-shim/` 已成为 `thirdparty/a2l-sdk/` 的子工程（`set(uchardet_FOUND TRUE)`
>   命中上游 `script/uchardet.cmake:6` 的 if 守卫 + 工程内 `uchardet::libuchardet`
>   INTERFACE IMPORTED 目标）。实现细节与两个 CMake 硬错误教训见批次9记录 §3.1。
> ② 下文 P1 的 a2lobject.cpp "6 个符号"失实：`src/a2lobject.cpp:12-44` 的
>   `DetectEncoding()`（连同 `#include <uchardet.h>`）在上游当前版本整体被注释，
>   实际生效引用仅 `src/a2lhelper.cpp:16` 的 5 个符号（批次8收口时 grep 实测）。

> **更正我上一轮的错误暗示**：我曾写"v1.0 不依赖 uchardet"，容易让人以为这是版本差异。
> 实际两版 `CMakeLists.txt` **都无条件执行 `find_package(uchardet CONFIG REQUIRED)`**，
> 且都以 `target_link_libraries(a2l PUBLIC uchardet::libuchardet)` 链入 —— 本机都没有 uchardet。
> 所以无论选哪个版本，本节都必须解决；v1.0 只是让我误判了它是否存在。

上游无条件 `find_package(uchardet CONFIG REQUIRED)` + `PUBLIC` 链接，本机无 uchardet、pypi/vcpkg 通道被 TLS 拦死。两案：

- **方案 A（A-8 已选，仍适用）**：`thirdparty/uchardet-shim/uchardetConfig.cmake` 提供
  `add_library(uchardet::libuchardet INTERFACE IMPORTED GLOBAL)` 空目标。
  **前置条件 P1（R4.2 已实测通过）**：grep 全量上游源码中 uchardet 符号分布：
  - `src/a2lhelper.cpp`：`DetectCharset()` 函数调用 `uchardet_new/handle_data/data_end/get_charset/delete`（5 个符号）
  - `src/a2lobject.cpp`：编码检测调用 `uchardet_new/handle_data/data_end/get_n_candidates/get_encoding/delete`（6 个符号）
  - `src/xcp/` 目录下 **38 个文件全部不引用 uchardet**
  - `include/a2l/xcp/` 下 **29 个头文件全部不引用 uchardet**
  - `include/a2l/` public 头文件中 **无任何 uchardet 引用**

  **P1 结论**：uchardet 仅被 ASAP2 主解析器的编码归一化路径使用（`A2lFile::ParseFile()` →
  `ConvertAllStrings()` → `DetectCharset()`），**IF_DATA XCP 子解析器（`XcpDataBlock` 构造函数
  直接吃 `std::string`）完全不调用 uchardet**。因此：
  - ✅ **链接闭包安全**：shim 空目标满足 `find_package(REQUIRED)` + `PUBLIC` 链接，`liba2l.dll`
    可正常生成；xcp 解析路径运行期不缺符号。
  - ⚠️ **完整 A2L 加载路径有连带风险**：`ParseFile()` 走 `ConvertAllStrings()` 时 `DetectCharset`
    因 shim 返回空编码 → boost::locale 转换可能失败。**此风险已记入 §6.3 已知缺口表**。
    缓解：首里程碑要求 A2L 样本统一 UTF-8/ASCII；若需 UTF-16/32 支持则回到方案 B。
- **方案 B（回退）**：真装 uchardet。本机网络受限，需你本地构建或提供二进制。

> shim 目录属本工程自建，不违反"不改 thirdparty"。
> ✅ R4.2 确认：P1 已从"头号不确定源"降级为"已验证、有明确边界条件的已知约束"。

### 5.3 隔离形态定稿（**R3c：`liba2l.dll` + C++ 抽象接口 + `A2L_INTERFACE`，pin main HEAD**）

> **标题沿革**：本节初稿题为"C++ 标准冲突处置"。按 A-13 pin main HEAD 后，上游自身即 C++23，
> 标准分治重新成为必需；现存的四个理由是"上游头不外泄 / 依赖不进主树 / C++ 标准分治 / 编译时间隔离"。

#### 5.3.0 决策记录（R3 用户裁决，R3c 更新 A-8/A-13）

| 编号 | 决策 | 落地位置 |
|---|---|---|
| A-1 | 直接选 A：独立 SDK 构建，不走 `add_subdirectory` | §5.3.1 |
| A-2 | 交付形态 = **DLL** | §5.3.1 / §5.3.4b |
| A-3 | 统一前缀 **`liba2l`**（target `liba2l`、别名 `liba2l::liba2l`、产物 `liba2l.dll`/`liba2l.lib`） | §3.2 / §5.1 / §5.3.1 |
| A-5 | **不使用 `extern "C"`，统一 C++ 导出接口** → POD 折叠层取消 | §5.3.3 |
| A-6 | 测试样本在测试例程内生成（不依赖外部真实文件） | §7.1-A |
| A-7 | **暂不支持 Linux/macOS**，Windows-only | §5.4 |
| A-8 | uchardet 走方案 A（shim）—— **R3c 复活为 P0**：两版上游都无条件 `find_package(uchardet REQUIRED)`，本机没有；shim 是硬需求，前置 P1 grep 必做 | §5.2 |
| A-9 | 引入 **`A2L_INTERFACE` 宏**做导出标注 | §5.3.3b |
| A-10 | CRT 一致性校验 **先 pass**（本里程碑不做自动拦截，仅 Windows + 人工核对） | §5.3.4b |
| A-11 | **允许 `std::function` 回调** | §5.3.3-R6 / §4.5 |
| A-12 | **Boost 用静态库** —— R3b 实测证实成立（上游 `boost.cmake` 显式 `Boost_USE_STATIC_LIBS ON`，两版一致） | §5.3.6 |
| A-13 | ~~pin tag v1.0~~ → **R3c 改判：pin main HEAD `c3105749…`，固定 SHA + 手工升级**（`.gitmodules` 不写 branch） | §5.3.9 ⚠️ 使 C++23/uchardet 两项风险回升 |

> **R3c 追加说明（重要）**：用户原话是"直接使用 main HEAD 不直接使用 commit 号"。
> 技术上不可实现 —— submodule 只能以 gitlink(SHA) 形式存在，`branch` 字段仅对 `--remote` 生效。
> 已按"pin main 的当前 SHA + 人工升级"落地，详见 §5.3.9。
> 该改判的净效果：**换来 XCPplus 与最新 lexer 修复，重新背上 C++23 分治、uchardet shim、MSVC 对 C++23 支持度三项不确定**。
| A-4 | 未答。随 POD 层取消而变形：所有权契约改由「分配/释放同侧」承担（§5.3.3 R2） | §5.3.3 |

> **A-10 的连带后果（必须知情）**：跳过 CRT 一致性自动校验后，Debug 主树误链 Release DLL
> 会跨模块堆释放，表现为随机崩溃且难定位。既然不做拦截，唯一防线是流程约定：
> 「`LIBXCP_BUILD_A2L=ON` 时，主树构建类型必须与 `LIBXCP_LIBA2L_ROOT` 指向的 cfg 目录一致」。
> 该约定写入 §7.3 命令示例与 PoC 检查点，作为人工核对项；将来恢复跨配置需求时再补校验。

> 说明：A-5 的选择使"彻底隔离"的含义从「标准 + STL + ABI 三重隔离」收缩为
> **「标准隔离 + 上游头不外泄」**；STL/CRT 一致性不再由边界类型保证，改由构建校验保证。
> 在 A-7（单平台单工具集）前提下这是自洽的取舍，但**不再是无条件彻底的隔离**，请勿如此对外描述。

### 5.3.1 最终交付形态（R3c）

```
thirdparty/a2l-sdk/dist/msvc-x64-release/     # 与 msvc-x64-debug 平行两份
   bin/liba2l.dll          # 上游 main(方式① STATIC) + Boost 静态 + 薄壳导出层
   lib/liba2l.lib          # import library
   include/liba2l/liba2l_api.hpp   ★ 唯一对外头：抽象接口 + 自有 DTO，零上游类型
   # 当前不产出 package config；发布型 install/export 留待后续
```

当前最小开发验证集不调用 `find_package(liba2l CONFIG)`；主树按 §5.1 从显式 `LIBXCP_LIBA2L_ROOT` 查找头文件、import library 和 DLL，并创建本地 `liba2l::liba2l` imported target。发布型 package config 留待后续。

**R3c 更新（A-13 改判 pin main）**：main 的 `CMAKE_CXX_STANDARD` 是 **23**，与 libxcp 主树的 20 不一致 ——
本节为"隔离 C++23/C++20 冲突"而设计的复杂度**重新成为必需**（R3b 曾因拟 pin v1.0 而下调，现恢复）。
上游的 `set(CMAKE_CXX_STANDARD 23)` 只影响 SDK 自己的 build tree，libxcp 主树永远 C++20。
独立 SDK 构建的四条理由：上游头不外泄、Boost/uchardet 不进主树、**C++ 标准分治**、编译时间隔离。详见 §5.3.9。

### 5.3.2 论证依据：为什么"仅预编译静态库 + 桥接层直接 include 上游头"不够

（本节保留 R2 的实测证据。R3 通过 §5.3.3 的「导出层独占上游 include」消解这三条路径，
故本节结论仍成立，只是解法从 POD 换成了接口隔离。）

实测证据表明上游公开头文件里存在**会进入消费方 TU 的代码**：

| 位置 | 内容 | 后果 |
|---|---|---|
| `include/a2l/module.h` 末尾 | `template <typename T> std::string Module::GetUnitString(const T&) const` | 模板定义在头文件 → 桥接层实例化时按**桥接层的标准**编译 |
| `include/a2l/a2lfile.h` | `void Filename(std::wstring f){...}`、`FilenameW()`、`NumberOfLines()`、`ParserType()` 等 inline | 同上 |
| `include/a2l/a2lstructs.h` | `Asap2Version::FromString()`、`A2lSiExponents::HasExponent()` inline；`A2lMemoryLayout/A2lMemorySegment` 含 `mutable std::optional<xcp::XcpDataBlock>` 成员 | inline + **需要 XcpDataBlock 完整类型** → 桥接层必然 include `xcpdatablock.h`，其全部 inline 成员一并带入 |

因此若桥接层直接 `#include <a2l/...>`，则：
1. 仍需保证这些头能在 `/std:c++20` 下编译（未验证，属风险）；
2. 跨界对象（`std::optional<XcpDataBlock>`、`std::deque<A2lMemorySegment>`、`std::unordered_map<std::string,std::unique_ptr<Measurement>>`）两侧布局一致性依赖同工具集同 CRT —— **这才是真正的脆弱点**。

### 5.3.3 R3 定稿：C++ 抽象接口边界（取消 POD 折叠层）

> **A-5 用户裁决：不使用 `extern "C"`，统一 C++ 导出接口。**
> 这直接推翻了本节初稿的 POD 方案。取舍如下，必须知情：
> - **失去**：POD + extern "C" 带来的「STL/CRT 版本无关」保证。
> - **换来**：接口自然（可用 `std::string`/`std::vector`/`std::function`），少一层 8~10 个结构体的手工映射，开发效率高。
> - **前提成立**：A-7 已锁定 Windows-only + MSVC vc143 单工具集，CRT 一致性可控（但需硬校验，见 §5.3.4b）。

```
┌── SDK 构建树（独立 project liba2l_sdk，C++23，vc143，/MD 或 /MDd）──┐
│ thirdparty/a2llib (submodule, pin main HEAD c3105749, 不修改)          │
│ thirdparty/a2l-sdk/                                                  │
│   include/liba2l/liba2l_api.hpp  ← 唯一对外头：抽象接口 + 自有 DTO    │
│   include/liba2l/liba2l_export.h ← A2L_INTERFACE 宏定义（§5.3.3b）     │
│   src/liba2l_export.cpp          ← 全工程唯一 #include <a2l/...>      │
│   src/dll_main.cpp               ← DllMain                            │
└──────────────────────────────────────────────────────────────────────┘
        ↓ 交付 bin/liba2l.dll + lib/liba2l.lib + include/liba2l/
┌── libxcp 主树（C++20）───────────────────────────────────────────────┐
│ a2lbridge/src/*.cpp   只 #include "liba2l/liba2l_api.hpp"             │
│                       把 liba2l DTO → §4 的 SymbolInfo/IfDataXcpInfo  │
└──────────────────────────────────────────────────────────────────────┘
        ↑
libxcp (XcpCore)  ← 依旧零第三方依赖，不链接 liba2l
```

§5.3.2 列出的三条上游 inline/模板泄漏路径**自动消解**：因为桥接层不再 include 任何 `a2l/*` 头，
上游类型全部藏在 `liba2l_export.cpp` 内。

#### 5.3.3b `A2L_INTERFACE` 导出宏（回答 A-9）

定义在 **本工程自写的** `include/liba2l/liba2l_export.h`（不改 thirdparty），三段式写法：

```cpp
// 三态：编译 DLL 时定义 LIBA2L_MAKE_SHARED；使用 DLL 时两者都不定义；静态集成时定义 LIBA2L_STATIC_LINK
#if defined(LIBA2L_STATIC_LINK)
#  define A2L_INTERFACE            // 静态集成：空
#elif defined(_WIN32) || defined(__CYGWIN__)
#  if defined(LIBA2L_MAKE_SHARED)
#    define A2L_INTERFACE __declspec(dllexport)
#  else
#    define A2L_INTERFACE __declspec(dllimport)
#  endif
#  define A2L_LOCAL
#else                              // GCC / Clang 可见性（代码留能力，本里程碑不实机验证）
#  if defined(LIBA2L_MAKE_SHARED) && __GNUC__ >= 4
#    define A2L_INTERFACE __attribute__((visibility("default")))
#  else
#    define A2L_INTERFACE
#  endif
#  define A2L_LOCAL __attribute__((visibility("hidden")))
#endif
```

用法与注意：

| 要点 | 说明 |
|---|---|
| 标注位置 | 加在**导出的类**与**工厂函数**上：`class A2L_INTERFACE IDoc { ... }`、`A2L_INTERFACE std::unique_ptr<IDoc> CreateDoc() noexcept;` |
| 与 A-10 的关系 | 宏本身跨平台 ≠ 支持跨平台。**只在 Windows 编译验证**，Linux/macOS 分支为"未测试代码"，不得对外宣称可用 |
| MSVC 整类导出 | `__declspec(dllexport)` 标在 class 上即导出其全部成员（含 inline），这正是我们需要的，且**规避了 §5.3.8 担心的 `WINDOWS_EXPORT_ALL_SYMBOLS` 不可靠问题** —— 因为我们只导出自己的薄层，不需要导出上游类 |
| 不要标在含 STL 成员的 struct 上？ | 需要标。DTO 是纯数据 + `std::string/vector`，构造/拷贝由调用方 TU 生成即可；但**跨界的 vector 必须由 DLL 侧分配**（R2） |
| CMake 侧 | `target_compile_definitions(liba2l PRIVATE LIBA2L_MAKE_SHARED)`；消费方通过 imported target 的 INTERFACE 什么都不定义（默认走 dllimport 分支） |

#### 导出面纪律（选 C++ 接口后必须遵守，否则风险原样回来）

| # | 规则 | 理由 |
|---|---|---|
| R1 | 跨界只用本工程自有类型 + `std::string`/`std::vector`/`std::optional`；**禁止任何 `a2l::` 类型出现在 `liba2l_api.hpp`** | 防止上游头被间接拉进主树 |
| R2 | 导出接口全为抽象类（纯虚析构）；对象由 DLL 侧工厂创建、DLL 侧 `Release()` 销毁，**主树永不 free DLL 分配的内存** | 这就是新的 A-4 答案：分配与释放同侧 |
| R3 | 一次调用返回完整快照（如 `std::vector<SymbolDto>`）；不返回迭代器、不返回指向 DLL 内部的引用 | 对应 §6.3-A 的裸指针生命周期问题 |
| R4 | 所有导出方法 `noexcept`，错误经 `LastError()`/返回码传出 | 上游 `ReadAndConvertFile()` 会 `throw std::runtime_error`，必须在导出层捕获转换；异常跨 DLL 边界不可靠 |
| R5 | `liba2l_api.hpp` 顶部定义 `kLibA2lAbiVersion`，工厂函数运行时校验 | 防 DLL 与头文件不同步（回答 C-3） |
| R6 | **异步回调用 `std::function<void(int)>`（A-11 已允许）**，但须声明回调触发线程；回调期间禁止在主线程调用同一 Doc 的其他方法 | A-5+A-11 的直接收益；线程契约归 B-20 |

#### 5.3.4 ~~两档落地选项~~（R3 作废）

原 A/B 二选一已由用户裁决为 A；B 档（桥接层直接 include 上游头）随之废弃，理由见 §5.3.2。

#### 5.3.4b C++ 接口边界的残余风险（R3b 按 A-10 调整）

| 风险 | 后果 | 缓解（现状） |
|---|---|---|
| **CRT 混链（主树 /MDd 链 Release /MD 的 DLL）** | 堆跨模块释放，运行期随机崩溃且难定位 | ⚠️ **A-10 决定本里程碑不做自动校验**。降级为人工约定：主树构建类型必须与 `LIBXCP_LIBA2L_ROOT` 所指 cfg 目录一致（写入 §7.3 与 PoC P6）。**此风险已知且被接受，非遗漏** |
| STL 版本漂移（VS 小升级改 `std::string` 布局） | 静默内存破坏 | SDK 目录名带 `<toolset>`；ABI 版本号兜底；SDK 与主树要求同机同 VS |
| `_ITERATOR_DEBUG_LEVEL` / `/GL` / `/RTCc` 不一致 | 静默改变 STL 布局 | 固定 IDL（Release=0 / Debug=2）；导出层禁 `/GL`；写进 SDK 构建脚本而非口头约定（此项**不受 A-10 影响**，属 SDK 内部一致性） |
| 异常穿越 DLL 边界 | 缺 unwind 信息时终止进程 | 导出方法一律 `noexcept`（R4） |
| 部署产物翻倍 | release/debug × x64 各一份 dll/lib/pdb | `dist/<cfg>/` 分目录；测试运行前拷贝到 exe 目录（C-11） |


### 5.3.5 上游 install/export 缺陷（已知事实；当前最小集不处理）

`cmake/a2lConfig.cmake.in` 内容：

```cmake
include("${CMAKE_CURRENT_LIST_DIR}/dbcTargets.cmake")   # ← 文件名写死为 dbcTargets
@PACKAGE_INIT@                                          # ← 位置在所有语句之后，无效
set(dbc_DIR "@PACKAGE_SOME_INSTALL_DIR@")               # ← 残留自 dbclib 模板
```

而 `install(EXPORT a2lTargets ... NAMESPACE A2l::)` 生成的是 `a2lTargets.cmake` → **两者文件名不匹配，`find_package(a2l CONFIG)` 必然失败**。
v1.0 的 `CMakeLists.txt` 里这段同样存在（我已解码核对），且 v1.0 还有一处 Doxygen 配置写 `include/dbc`（也是从 dbclib 抄的残留）。
→ 属上游问题，我们不改，自写 wrapper 规避。

**R3c 更正**："`add_library` 列表里 `src/labelscanner.cpp` 出现两次"确实是 **main** 的情况（我一度以为选 v1.0 可回避，现 R3c 已定 main，该问题回来了）。
→ 属上游问题，我们不改；SDK 构建脚本里容忍该告警即可。

→ 当前最小开发验证集不生成 package config；主树按 §5.1 从显式 root 创建 imported target。正式发布 SDK 时再评估自写 config。按 A-10，不做 CRT 自动校验；仅保留 ABI 版本常量供运行时自检（R5）。

### 5.3.6 交付包所需的链接闭包（本机已核实可得）

| 来源 | 内容 | 状态 |
|---|---|---|
| Windows SDK 10.0.26100.0 | `um\x64` 453 个 .lib、`ucrt\x64\libucrt.lib` | ✅ 实测存在 |
| MSVC 17 (vc143) | `libcpmt.lib` / `libccpmts.lib` / `libvcruntime.lib` / `legacy_*` | 随 VS 安装 |
| Boost 1.86 | `locale`/`filesystem`/`process`(+传递依赖 `charconv`/`container`/`thread`/`atomic`…) vc143-mt-x64 **静态** | ✅ 实测存在；上游 `script/boost.cmake`（v1.0 与 main 同）显式 `set(Boost_USE_STATIC_LIBS ON)` → **印证 A-12** |
| uchardet | ❌ **本机缺失，且 pin main 后为硬依赖**（两版都无条件 `find_package(REQUIRED)` + PUBLIC 链接） | ⚠️ **P0 阻塞项**：走 §5.2 shim，前置 P1 grep 必做 |

**A-12 的落地结论**：Boost 走静态（`Boost_USE_STATIC_LIBS ON`，上游已默认如此）→
`liba2l.dll` 把 Boost 一并吞入，交付面只有一个 DLL + import lib，无需随包分发 Boost/ICU 二进制。
若将来发现 boost::locale 实际动态依赖 ICU（D-3 待验），再单独处理。

### 5.3.7 liba2l SDK 的独立构建入口（新增，不属于 libxcp 主树）

> **批次9 扩展**：本工程已成为 A2L 栈**唯一**构建入口——新增子工程 `uchardet-shim/`
> （见 §5.2 勘误）与 `a2lbridge/`（桥接层源码整体迁入，`add_subdirectory` 顺序：
> shim → ../a2llib → liba2l → bridge）；prepared root 增出
> `lib/libxcp_a2lbridge.lib` + `include/libxcp/a2l/*.hpp`。批次9记录 §3.2/§3.4。

```
thirdparty/a2l-sdk/
  CMakeLists.txt        # project(liba2l_sdk LANGUAGES CXX)；set(CMAKE_CXX_STANDARD 23)
                        #   ↑ R3c：pin main HEAD，上游自身即 C++23 → SDK 树用 23，
                        #     主树保持 20（AGENTS.md 要求）。标准分治重新必需，见 §5.3.9。
                        #   add_subdirectory(../a2llib ...) 取上游 a2l STATIC target（方式 ①）
                        #   A2L_TOOLS/A2L_TEST/A2L_DOC/A2L_FLEX = OFF
  include/liba2l/liba2l_api.hpp   # 唯一对外头（抽象接口 + 自有 DTO）
  include/liba2l/liba2l_export.h  # A2L_INTERFACE 宏（§5.3.3b）
  src/liba2l_export.cpp           # 全工程唯一 #include <a2l/...> 的文件
  src/dll_main.cpp                # DllMain
  build-sdk.ps1                   # Windows-only：校验 submodule SHA → configure → build
                                  #   → 复制头文件/import lib/DLL 到显式 prepared root（不 install）
```

关键点：
- **R3c 回退**：因 pin main，SDK 树必须用 `CMAKE_CXX_STANDARD 23`，与主树的 20 不一致 ——
  这正是本节初稿设计的"标准隔离"要解决的问题，隔离理由恢复为四条：
  **上游头不外泄、Boost/uchardet 不进主树、C++ 标准分治、编译时间隔离**。
- `Boost` 的 `find_package` 只需在这个子 project 里满足（`LIBXCP_BOOST_ROOT` 推导 `Boost_DIR`）。
- `uchardet` 的 `find_package` 同样只在此树满足（走 §5.2 shim）。
- 上游 `add_library(a2l ...)` 未指定类型，受 `BUILD_SHARED_LIBS` 控制（其默认被上游设为 OFF）
  → 按 §5.3.8 **方式 ①** 让它保持 STATIC，只有我们的薄壳是 SHARED。
- `dist/` 布局：`dist/msvc-x64-{release,debug}/{bin,lib,include,cmake}`。

### 5.3.8 两种 DLL 组织方式（R3b：不再是"退路"，而是并列可选）

由于 §5.3.3b 我们**只导出自己的薄层类**（带 `A2L_INTERFACE`），并不需要将上游类导出，
所以原先担心的"`WINDOWS_EXPORT_ALL_SYMBOLS` 无法导出 inline/模板"问题**只在方式 ② 下才存在**：

| 方式 | 做法 | 优点 | 缺点 | 建议 |
|---|---|---|---|---|
| **① 上游 STATIC + 薄壳 SHARED** | `a2l.lib`(STATIC，上游原样) + `liba2l.dll`(仅含 `liba2l_export.cpp`，标 `A2L_INTERFACE`) | 导出面最小最可控；不依赖任何自动导出机制；上游完全不改 | 两个产物要一起装 | ✅ **首选** |
| ② 全部编进一个 SHARED | 上游源文件直接进 `liba2l` 目标 | 单一产物 | 上游类未被标注，若导出层误引用上游 inline 函数会链接失败；需 `WINDOWS_EXPORT_ALL_SYMBOLS` 兜底 | 备选 |

PoC 的 P4/P4b 两个检查点分别对应这两种方式，先做 ① 即可判定可行性。

### 5.3.9 ⚠️ 版本 pin 决策（A-13 R3c：**pin main HEAD**）

#### 决策记录

| 项 | 值 |
|---|---|
| 选定版本 | **main HEAD = `c31057498555cbb29ab48e718329ca3163c10221`**（2026-09-24，"Fixed bug when large floating point are written as integer with many zeros."）<br>⚠️ **本 SHA 是我在 PoC 之前经 GitHub API 查到的快照**。submodule 落地时若上游已前进，以你实际 checkout 的 SHA 为准并回报，我更新本表 —— **不要求你去回退到一个更旧的 commit**，因为你要的就是 main |
| 维护方式 | **固定 gitlink SHA + 手工升级**（`.gitmodules` 不写 `branch` 字段） |
| 备选（未选） | tag v1.0 = `5b8cc45a24bc184db0d9fa8012786cc768a21c90` |

#### 关于"用 main HEAD 但不写 commit 号"——技术上无法实现（已纠正）

submodule 在父仓库中**只能以 gitlink（40 位 SHA）形式存在**。`.gitmodules` 的 `branch` 字段仅对
`git submodule update --remote` 生效；普通 clone/update 永远 checkout 父仓库记录的 SHA。
故浮动引用不可行，本设计采用「固定 SHA + 人显式 `--remote` 升级 + review diff」。

⚠️ 这条纪律是**硬性的**：每次刷新上游 SHA，必须同步重建 `liba2l.dll` 并核对 `liba2l_api.hpp`。
因为 A-5 选了 C++ 接口、A-10 又 pass 掉了 ABI 校验，DLL 与头文件不同步时我们只有
`kLibA2lAbiVersion` 一道兜底 —— 浮动引用等于把它也削掉。

#### 选 main 换来的收益

| 收益 | 证据 |
|---|---|
| **XCPplus 支持回归** → §6.3-A 的优先级逻辑可用，满足规范 §8.2 | `a2lstructs.h`(main) 有 `GetXcpPlusDataBlock()` |
| Label(.lab) / ECU Container 能力（范围外但可用） | main 有 `labelparser/labelscanner` |
| **数字字面量解析修复**：unsigned/signed number 先试整数、失败再试 `double`，都失败才报错。直接影响 COMPU_METHOD 系数与大 `ECU_ADDRESS` 的正确性 | commit `c3105749` 对 `src/a2lflexer.l` case 210/211/212 的 patch |
| BOM/编码转换 API 明确存在（`ReadAndConvertFile()`/`CheckBom()`） | main `a2lfile.h` 4,741 B，已逐行读到 |

#### 选 main 重新背上的代价（R3b 曾以为已消除，现全部回来）

| 代价 | 影响 | 处置 |
|---|---|---|
| **上游 `CMAKE_CXX_STANDARD 23`** | 与主树 C++20 不一致 → §5.3 的分治设计**重新必需** | SDK 独立 project 用 23，主树保持 20；靠 DLL 边界隔离 |
| **uchardet 硬依赖** | 本机无 uchardet → 直接 configure 失败。⚠️ **注意：这不是 main 独有的**，v1.0 同样 `find_package(uchardet CONFIG REQUIRED)` + PUBLIC 链接（我上轮误判为"v1.0 无此依赖"，已更正）。选 main 并没有新增这项，只是让它无法再被回避 | §5.2 方案 A shim，**前置 P1 grep 必做** |
| MSVC 17 对 C++23 的支持度未知 | 上游 CI 的 windows job 用的具体工具集未核实 | PoC P3 实测；失败则回退讨论 v1.0 |
| TU 集合比 v1.0 大（新增 logstream/label 等） | 编译时间增加 | PoC P3 记录实际 TU 数，替换我此前误引的 "~150" |

> **勘误**：我上一轮给的两个 SHA 有误 —— 当时写的 "main HEAD `1274fa4a…`" 其实是 main 的**父 commit**
> （`c3105749` 的 parents 字段可证）。本轮 pin 用的是真正的 HEAD `c3105749`。

#### 附：v1.0 vs main 差异表（R3b 存档，供日后重评时参考）

⚠️ **本表首版有一处错误已更正**：曾记为"v1.0 不依赖 uchardet"。实际两版 `CMakeLists.txt`
都无条件执行 `find_package(uchardet CONFIG REQUIRED)` 并 PUBLIC 链接进核心库 —— 该依赖与选哪个版本无关。

| 维度 | v1.0（未选） | main（**已选**） |
|---|---|---|
| `CMAKE_CXX_STANDARD` | 20 | **23** |
| uchardet 硬依赖 | 有（PUBLIC 链接） | 有（PUBLIC 链接）—— **两版相同** |
| `GetXcpPlusDataBlock()`（IF_DATA XCPplus） | ❌ 无 | ✅ **有** → §6.3-A 优先级逻辑可用，满足规范 §8.2 |
| Label(.lab) / ECU Container 支持 | ❌ 无 | ✅ 有（本里程碑范围外） |
| `a2lfile.h` 体积 | 2,497 B | 4,741 B（多 `CheckBom`/`ReadAndConvertFile`/`ConvertAllStrings`/编码列表 API） |
| lexer 数字字面量处理 | 旧行为 | ✅ `c3105749` 修复："大浮点被当成整数写入" |
| `xcp/*.h` 核心组件 | 齐全 | 齐全（`daq.h` d7b9946、`event.h` 554fd5f、`protocollayer.h` cef94d2、`xcponudpip.h` 8bc14ec 两版同 sha） |
| `measurement.h` | 2,949 B，字段与 main 一致（解码核对） | 同内容 |

> R3b 曾据本表推荐 pin v1.0（理由：构建风险低）。用户 R3c 改判选 main，取代表述见本节上方的"决策记录"与"收益/代价"两表。
> 若 PoC P3 证明 MSVC 17 编不过上游 C++23，本表即为回退评估的起点（登记为未定义项 A-17）。


### 5.4 平台承诺（R3：Windows-only）

| 平台 | 本里程碑承诺 |
|---|---|
| **Windows x64 / MSVC vc143** | 唯一支持目标。PoC 必须实测通过 configure/build/link/运行 |
| Linux / macOS | **A-7 裁决：暂不支持**。SDK 构建脚本只提供 `.ps1`；相关讨论从设计中移除 |

后续若要恢复跨平台，需重开决策项：各平台独立构建 SDK、导出层宏适配（`__declspec` vs `__attribute__((visibility))`）、
以及上游在 GCC/Clang 下的 C++23 兼容性验证（上游 CI 仅覆盖 ubuntu + windows）。

---

## 6. 数据流与错误处理

### 6.1 加载流程

```
【在 liba2l.dll 内部（§5.3.3：上游类型不外泄）】
  a2l::A2lFile::Filename(path) + ParseFile()   ← 上游负责编码/BOM/include
  失败 → catch 后转成错误码 + LastError 字符串（R4：异常不跨界）

【DLL 导出层返回给桥接层的快照】
  DocumentDto { modules[], symbols[], compu_methods[], if_data_xcp, daq, events }

【a2lbridge 侧转换流程】
  ├─ 遍历 module（A-7 后仍可能有多个 MODULE，见 B-17）
  │    ├─ SymbolDto[] → SymbolInfo 表（module::name → info）
  │    │     └─ xcp_address = EcuAddress() 原值；address_extension 独立保存
  │    │        element_size_bytes 来自显式 ASAM 类型映射表（B-3），禁止 sizeof(enum)
  │    │        AG 只用于元素计数、地址步进与对齐，不乘入基地址（B-1）
  │    ├─ CompuMethodDto → tagged ConversionInfo（B-8）
  │    │     IDENTICAL/LINEAR/RAT_FUNC/TAB_INTP/TAB_NOINTP/TAB_VERB 可执行；FORM 仅保留原文
  │    ├─ RecordLayoutDto → 位布局表（字段清单未定，见 B-4）
  │    └─ IfDataXcpDto → IfDataXcpInfo
  │          ├─ ok==false → 记 warning，不致命（若无 IF_DATA 且 require=false）
  │          ├─ ProtocolLayer → t1..t7 / MAX_CTO / MAX_DTO / BO / AG
  │          ├─ TransportEndpoint[]（UDP/TCP/CAN…）
  │          ├─ DaqDto → DaqInfo
  │          └─ EventDto[] → EventChannelInfo[]
  └─ 一致性检查（见 §6.2）
```

### 6.1-A 地址语义（B-1，R4 已批准）

```text
xcp_address       = A2L ECU_ADDRESS 原值
address_extension = 独立 8-bit 字段
byte_offset       = element_index * element_size_bytes
前置条件          = byte_offset % AG == 0
element_address   = xcp_address + byte_offset / AG
```

约束：

1. `ADDRESS_GRANULARITY` 不乘入 `xcp_address`；它只影响 XCP 命令元素计数、地址步进和对齐。
2. `address_extension` 独立编码，不与 address 拼成 40-bit 整数。
3. `ByteSizeOf()` 返回实际字节数，不除以 AG；DTO `raw` 同样保持实际字节语义。
4. 未对齐、乘法溢出或地址加法溢出时返回结构化错误，且不得向 ECU 发包。
5. `ECU_CALIBRATION_OFFSET`、`INDIRECT`、`SUB_ADDRESS` 首版不猜测，返回 `UnsupportedAddressingMode`。

黄金测试：AG=1/2/4 下基地址不变；8-byte 数据的 XCP 元素数为 8/4/2；extension 独立；AG=2 的奇数字节偏移失败；溢出失败且不发包。

### 6.2 A2L 与 Slave 运行时参数一致性（规范要求，规范 §8.4）

XCP 规定：同一参数既写在 A2L `IF_DATA` 又能运行时查询时，Master **必须检查一致性**并报告。
R4 决策是**运行时值为当前会话真值**，不再让 UI 逐项选择来源；但所有差异都保留供 UI 展示和诊断。

```cpp
/// @brief 单个 A2L 声明与运行时值的差异（B-16）
struct ParamDiscrepancy {
    std::string parameter_name;
    std::string a2l_value;
    std::string runtime_value;
    enum class Severity : std::uint8_t { Info, Warning, Error } severity = Severity::Warning;
    std::string affected_feature;    ///< MemoryAccess / Daq / Transport 等
};
```

分级策略：

- **Error**：protocol major、通信 ByteOrder、AG、不兼容的 DAQ identification/entry granularity/address-extension mode；只阻断受影响功能。
- **Warning**：MAX_CTO/MAX_DTO、protocol minor、资源位和 optional command 差异；采用运行时值及两端更保守的有效限制。
- **Info**：只有 A2L 才提供的静态配置。
- `t1..t7` 是 Master 超时配置，不属于 Slave runtime 可比参数；PAGING/PROGRAMMING 不在本里程碑比较。

比对函数留在 `a2lbridge`，避免 XcpCore 感知 A2L。

### 6.3 已知缺口与降级策略

| 缺口 | 影响 | 本里程碑策略 |
|---|---|---|
| ~~IF_DATA XCPplus 未被建模~~ **已撤销：上游提供 `GetXcpPlusDataBlock()`**（见 §1.3 勘误） | — | 按 §6.3-A 实现优先级选择 |
| COMPU_METHOD `FORMULA` 表达式求值 | 无法执行该非线性换算 | 第一版保留原文并返回结构化 `UnsupportedConversion`（B-8），不猜公式 |
| 变长数组下界 `/* expr */` | 无法证明数组布局 | 保留元数据并返回结构化 `UnsupportedArrayLayout`（B-2），不降级成一维 |
| A2L 压缩/加密（GET_ID Type=4） | 无法从 ECU 上传 A2L | 本里程碑只做本地文件；外部函数接口另立设计 |
| Seed&Key 外部函数加载 | 只能拿到函数名字符串 | 保持现有 `xcp_master.hpp` 的注入式设计不变 |
| boost::locale 无 ICU 后端 | UTF-16/32 BOM 的 A2L 可能抛异常 | PoC 用一个小 UTF-16 文件实测；失败则要求样本统一 UTF-8/ASCII |
| uchardet shim 下完整解析路径编码检测失效（R4.2 新增） | `A2lFile::ParseFile()` → `ConvertAllStrings()` → `DetectCharset()` 因 shim 返回空编码，非 UTF-8/ASCII 文件可能解析失败或内容损坏 | 首里程碑 A2L 样本强制 UTF-8/ASCII；IF_DATA XCP 子解析器（`XcpDataBlock`）不受影响；需 UTF-16/32 时回退 §5.2 方案 B |
| **ARRAY_DIMENSION / SOURCE_LOWER_BOUNDS 上游无语法（批次11 实测）** | `a2lflexer.l`+`a2lparser.y` grep 均 0 命中：含该属性的 A2L 会在上游直接 ParseFailed；B-2 的 `source_lower_bound` 无数据源、恒 0 | 接受限制（记入 B-2 注记）；规则 2D 以 MATRIX_DIM 表达；真实需求出现时向上游提语法 PR，不自研预处理 |
| **PID_OFF DTO envelope（B-7，批次11 定口径）** | 关闭识别字段后无法按 EPK 路由帧；XCP 要求唯一标识由 Transport 层保证（docs/XCP_1.3.0 §7.5，UDP 通道无此关联） | `DtoFrameLayout::pid_off` 成员已补齐（对齐 §4.4）；Decode 对 `pid_off==true` 显式拒绝 `UnsupportedOperation`；真实支持待 CAN/传输上下文需求 |
| **EVENT TIME_UNIT 码表缺失（批次10 核证）** | 仓库内无 Event 时间单位数值表（docs 仅述"周期=TIME_CYCLE×TIME_UNIT、低 nibble 编码"；§2362 的表属 TIMESTAMP 高 nibble，不可套用）→ `cycle_time_us` 无法换算 | raw 码（time_cycle/time_unit）透传、换算恒 0（不臆造）；用户提供 XCP Part 1 权威表后补换算与 T4 正向断言 |
| ~~异步进度未透传（批次12 登记，原 ③ 延后）~~ **批次13 已收口（改判为阶段进度）** | 复核推翻批次13 §0 的原核证结论：① `number_of_lines_` **只在 UTF-16/32 分支赋值**（`a2lfile.cpp:239-254`），ASCII/UTF-8（我们全部样本）恒 0 → 行级比例在分母上就不可算；② `A2lFile::LineNo()` 内部 `scanner_.load()->lineno()` 读非原子 `yylineno`（`a2lfile.cpp:416-421` + `FlexLexer.h:96/104`），且 `scanner` 是 `ParseFile()` 的**栈对象**（`:256-277`，结束即置空析构）→ 跨线程轮询存在访问已销毁对象的窗口 | **不接通行级进度**，改由 SDK 内部 `std::atomic<int>` 发布**阶段进度**（受理 5 → include 预扫描 25 → `ParseFile()` 返回 70 → 快照完成 100）；桥接层发布前上限 99、发布后恒 100（`ready` 改 `std::atomic<bool>` + release/acquire）；不调上游 `ProgressInfo()/LineNo()`、不用上游 `AsynchParseFile`、不加监控线程。`LoadOptions::progress_notify_percent` 语义降为"调用方轮询节奏提示" |
| ~~STRUCTURE/INSTANCE 无数据源（批次12 登记，原 ④ 延后）~~ **批次13 已闭合（B-12 首版边界）** | SDK 导出层此前只采 MEASUREMENT/CHARACTERISTIC，`SymbolKindDto::kStructure` 分支恒不触发 | `IDoc::ListStructures`（ABI v4）采集 `TypedefStructures()`+`Instances()`，成员只存名/偏移/引用 TYPEDEF 名与 MATRIX_DIM 原码，**不递归展开**；桥接层映射为 `kind=Structure` 元数据符号（`data_type=Unknown`/`element_size=0`），`ByteSizeOf/ToPhysical/FromPhysical` 一律 `UnsupportedOperation/Query`，且不参与基址反查；样本 `ST_ENGINE/ST_WRAPPER/I_ENGINE` + 用例 `StructureMetadataOnly`。**批次18（R4.9）：leaf 级展开器已落地**——桥接层 `structure_layout.cpp::BuildStructureLeaves` 把 TYPEDEF 成员（标量/固定数组/嵌套/结构数组实例逐元素/CalSeg TYPEDEF_CHARACTERISTIC 事实并入）展开为限定路径可执行叶子；本体整块拒绝与本行边界不变 |
| ~~`record_layout_impl.cpp` 已写未接线~~ **批次13 已接线（R3 闭合）** | 此前 `CheckRecordLayoutExecutable`/`RecordLayoutInfo` 全仓 0 调用点 | `SymbolDto` 随 v4 增 `record_layout_name` + `record_layout_kind`（`kNotProvided/kPlainScalar/kAxisPts/kComplex`，判定只用上游既有 getter）；桥接层在 `ByteSizeOf/ToPhysical/FromPhysical` 三处 Resolve 之后统一准入：AXIS_PTS→`UnsupportedOperation`、复合→`InvalidLayout`（Phase::Layout）；`kNotProvided`（无 DEPOSIT/查不到版式）**不进判定器**，仍由 B-3 宽度门拒绝——"无信息"不等于"不可执行" |
| ~~VAL_BLK / ASCII 执行未做（批次12 登记，原 ⑤ 延后）~~ **批次13 半边闭合 + 半边事实化** | RL 推导此前只覆盖 CHARACTERISTIC VALUE | **VAL_BLK 已可执行**：元素类型由单段 `FNC_VALUES`(Position=0/DIRECT/纯标量版式) 证明，元素数由 `MATRIX_DIM` 乘积证明（含 0/溢出即整体退回 `kUnknown`，不半证半猜）→ 单维 extent 进 `dimensions`，`ByteSizeOf`/`ComputeElementAddress` 走既有 B-1/B-2 通路（样本 `C_VALBLK_EXEC`，无 MATRIX_DIM 的 `C_VALBLK` 仍拒）。**ASCII 判定不改**：上游 `MAX_LENGTH` 无词法/语法、`Characteristic` 无长度 getter（逐条 grep 实测）→ 字节数不可证，维持 `UnsupportedDataType` 显式拒绝，不臆造长度、不自研预处理（与 B-2/ARRAY_DIMENSION 同策略） |
| ~~T9/T10/Debug SDK 三项门禁延后（批次12 登记，原 ⑥）~~ **批次13 三项全落地** | ① 无 ≥5 MB 性能基线；② T10 只是 configure 期 P9 扫描；③ prepared root 只有 Release | ① `gen_a2l.py --perf 5`（产物落 build tree + `.count` 侧车）+ ctest 条目 `ParseLargeFileUnderBudget`：**实测 5 MB = 264 ms**，按 3× 取整锁 `A2L_PERF_BUDGET_MS=900`，只记基线不设 SLA；② 新 `tests/a2l_isolation_check.cmake` + ctest 条目 `A2lIsolation`（桥接层禁含上游头 / 主树禁引用 A2L 栈，大小写敏感，命中即非零退出）；③ `build-sdk.ps1 -Config Debug` 产出 `msvc-x64-debug`，A-10"cfg 同侧"核对自此双侧可执行 |
| **同一 XcpMaster 上 Disconnect→Connect 不可用（批次14 实测的既有缺口）** | `CommandExecutor::m_transport_closed_` 只在析构与 `OnTransportClosed` 处置 `true`，**全仓无复位点**（grep 实测）→ 重连时 `PerformAttempt` 立即判超时并落入"Connecting 不允许恢复"的 InvalidState | 本批**不改**（超出批次14 范围且会影响既有恢复语义）；批次14 记录已登记为遗留项，测试侧显式写明"不测重连路径"的理由 |

| **上游 UDP 选项非法值静默降级（批次12 实测）** | `PACKET_ALIGNMENT IDENT` 仅接受 `PACKET_ALIGNMENT_8/16/32` 三个字面量，不匹配时 `SetPacketAlignment` **静默保持默认 8**；`OPTIONAL_TL_SUBCMD IDENT` 未识别名同样静默丢弃 → 消费侧无法区分"未声明"与"声明了非法值" | 本批只透传上游已识别值并在注释/DTO 说明该限制，不伪造检测能力；真实需求出现时向上游提语法/报错 PR，不改 thirdparty |


### 6.3-A IF_DATA XCP / XCPplus 优先级（规范 §8.2，新增）

```
取 MEMORY_LAYOUT / MEMORY_SEGMENT 级：
  blk_plus  = GetXcpPlusDataBlock()
  blk_plain = GetXcpDataBlock()

if (blk_plus && blk_plus->IsOk())      → 主用 XCPplus
else if (blk_plain && blk_plain->IsOk()) → 用 XCP
else                                   → 记 warning「A2L 未描述 IF_DATA XCP」

若两者同时存在且同一参数取值不同：
  → 以 XCPplus 为准，并生成一条 ParamDiscrepancy{Severity::Info,
      "IF_DATA XCP vs XCPplus: <param>"}，让用户知情而非静默。

批次12 起比对项清单（`DiffIfData` 实测覆盖面）：
  PROTOCOL_VERSION / MAX_CTO / MAX_DTO / BYTE_ORDER / ADDRESS_GRANULARITY
  / TRANSPORT / UDP_PORT / UDP_HOST / SEED_AND_KEY
  / PACKET_ALIGNMENT（批次12 新增）/ SUB_CMDS（批次12 新增）
```

注意 `GetXcpPlusDataBlock()` 返回的是指向 `mutable std::optional<XcpDataBlock>` 内部的裸指针，
**生命周期依附于所属 `A2lMemoryLayout`/`A2lMemorySegment`**。导出层（`liba2l_export.cpp`）必须在 DLL 内一次性把值拷进自有 DTO 返回，
桥接层不得跨调用持有指向 DLL 内部的引用 —— 这是 §5.3.3 纪律 R3「一次调用返回完整快照」的直接动因。


---

## 7. 测试方案（用户已确认：含黄金样本回归）

### 7.1-A 测试样本生成方案（回答 A-6）

**先回答"Windows 平台是否有生成 A2L 的能力"：有，但不能用现成的开源库。**

| 候选 | 能否生成 A2L | 证据 | 结论 |
|---|---|---|---|
| pyA2L ([christoph2/pyA2L](https://github.com/christoph2/pyA2L)) | ✅ 能，功能完整 | `pya2l/imex/a2l_exporter.py`（70,610 B），导出 `export_db()` / `open_database()`；另有 `json_exporter.py`(46 KB) | ❌ **许可证 GPL-2.0**（GitHub API `"license":{"key":"gpl-2.0"}`）。本工程为 MIT 闭源可发布取向，不引入 GPL 工具进构建/测试链 |
| a2llib 自身 | ❌ 只读 | 全仓库无 writer/exporter | 不可用 |
| 商业工具（CANape/INCA/a2lgen） | ✅ | — | 不可自动化、不可进 CI，排除 |
| **自研极简生成器** | ✅ 够用 | A2L 是纯文本 + 块语法，我们只需 5 类块 | ✅ **采用** |

**实现定位（关键认知）**：生成器的目的不是产出"合规 A2L"，而是产出
**「a2llib 能解析、且期望值由 spec 独立算出」的输入文件**。因此不需要覆盖 ASAP2 全语法。

```
tests/a2l_gen/gen_a2l.py        # Python 3 标准库（json + pathlib + str.format），零依赖
  输入：golden_spec.json        # 声明式描述：符号表 / COMPU_METHOD / RECORD_LAYOUT /
                                #         IF_DATA XCP(UDP) / DAQ / EVENT_CHANNEL
  输出：<tmp>/golden_basic.a2l  # 以及 expected/*.json（含 raw↔phys 期望值，由 spec 反算）
  调用：ctest 里作为 fixture 先跑，或 CMake add_test(NAME gen_a2l ...) 前置
```

需支持的块（最小集，超出即报错而非静默跳过）：
`ASAP2_VERSION` / `PROJECT` / `MODULE` / `MOD_COMMON` / `MOD_PAR` /
`MEASUREMENT` / `CHARACTERISTIC` / `COMPU_METHOD` / `COMPU_VTAB` / `UNIT` /
`RECORD_LAYOUT` / `IF_DATA XCP`（含 `PROTOCOL_LAYER` + t1..t7 + MAX_CTO/MAX_DTO +
BYTE_ORDER + ADDRESS_GRANULARITY + OPTIONAL_COMMAND + SEED_AND_KEY_EXTERNAL_FUNCTION）、
`DAQ` / `DAQ_LIST` / `EVENT_CHANNEL` / `DAQ_EVENT` / `MEMORY_SEGMENT`。

**这个方案的固有弱点必须记录**：循环自证 —— 我们生成的文件里，恰好只包含我们想到要测的语义。
真实工具的怪癖（超长 INCLUDE 链、混合编码、非标准空白、注释嵌套、关键字大小写混用）不会自然出现。
缓解措施：
1. 在 `golden_spec.json` 里显式加"畸形但合法"变体开关（如多空格、`\r\n` vs `\n`、大小写混用关键字、BOM 组合）；
2. 保留一条**手工编写**的 `golden_handwritten.a2l`（不走生成器），用于交叉验证生成器本身没写错；
3. 一旦将来拿到真实项目 A2L，直接作为额外样本挂进来，不改测试逻辑。

### 7.1-B 静态样本清单（仍保留少量手写的破坏性用例）

```
tests/data/a2l/                  # 仅存放"生成器造不出来"的文件
  golden_handwritten.a2l         # 手工编写，交叉验证生成器
  broken_truncated.a2l           # 截断 → 必须失败且给行号
  broken_bad_keyword.a2l         # 语法错误 → 必须失败
  golden_utf16le.a2l             # UTF-16 LE BOM（探索 boost::locale 后端能力，见 T6）
```

### 7.2 用例分组（`tests/a2l_golden_test.cpp`，GTest）

| 组 | 用例 | 断言要点 |
|---|---|---|
| T1 加载/错误 | `LoadGoldenBasicOk` | `Result` 成功、快照发布、`Count()>0` |
| T1 加载/错误 | `LoadMissingFileFails` / `LoadTruncatedReportsLocation` | 稳定 ErrorCode；文件/行列完整；不靠字符串判断 |
| T1 INCLUDE | `IncludeRelativeCycleDepthAndRoot` | 相对当前文件；循环检测；32/33 深度；默认禁止越根（B-18） |
| T2 地址 | `AddressGranularityDoesNotRewriteBase` | AG=1/2/4 时 ECU_ADDRESS 不变；8-byte 元素数为8/4/2；extension 独立（B-1） |
| T2 地址 | `UnalignedOrOverflowAddressRejected` | AG 不整除、乘法/地址加法溢出均返回错误且不发包 |
| T2 类型/数组 | `AllA2lTypesHaveExplicitWidth` | 实际枚举全集显式映射；未知类型拒绝（B-3） |
| T2 类型/数组 | `Scalar1dAndRegular2dLayout` | 0-based；原始下界/stride；row/column；复杂布局拒绝（B-2） |
| T2 位字段 | `BitMaskAndDaqBitOffsetAreIndependent` | 连续 BIT_MASK、DAQ BIT_OFFSET 不重复移位；ERROR_MASK 不推 valid（B-9） |
| T2 搜索 | `QualifiedNameAndStableSearch` | `module::symbol`；跨模块歧义；通配符；稳定排序后截断（B-13/B-17） |
| T3 换算 | `SupportedConversionMatrix` | IDENTICAL/LINEAR/RAT_FUNC/TAB_INTP/TAB_NOINTP/TAB_VERB 正向；可逆方法往返（B-8） |
| T3 换算 | `ConversionFailureIsStructured` | 分母零、多解、表外、FORM、越界、NaN/Inf 均返回稳定错误（B-8/B-15） |
| T3 精度 | `PhysicalValuePreservesInt64` | 2^53±1、UINT64_MAX、INT64_MIN 不经 double 丢精度（B-14） |
| T3 单位 | `PhysUnitAndUnitRefPolicy` | PHYS_UNIT 优先；UNIT_REF 回退；SI 元数据保存但不换算（B-10） |
| T4 IF_DATA | `UdpEndpointAndProtocolLayer` | endpoint、t1..t7、MAX_CTO/DTO、BO、AG、命令能力逐字段 |
| T4 一致性 | `RuntimeTruthAndSeverity` | major/BO/AG/DAQ约束为Error；容量/能力为Warning；t1..t7不误报（B-16） |
| T5 CHARACTERISTIC | `CharacteristicKindsAndScope` | 五型识别；Value/ValBlk/Ascii 可执行；Curve/Map 操作拒绝（B-11） |
| T5 RECORD_LAYOUT | `SupportedAndRejectedLayouts` | VALUE/连续VAL_BLK/FNC_VALUES；复杂布局明确拒绝（B-4） |
| T5 STRUCTURE | `StructureMetadataOnly` | 可查询元数据；本体读写返回 UnsupportedStructuredType（B-12） |
| T5.5 叶子路径（批次18） | `StructLeafRealCorpusFourForms` / `StructLeafBrokenCorpusWarnsEveryRejection` | 真实语料四形态+CalSeg 叶子可 Find/寻址/换算/读写（42 符号 0 告警基线）；不可证形态恰 7 条 `InvalidLayout/Load` 告警、坏路径 Find→NotFound；本体整块仍拒（B-12 边界不变） |
| T6 DAQ | `StaticDaqFromActualLedger` | PREDEFINED READ_DAQ、可配置 WRITE_DAQ；A2L顺序与ECU顺序相反仍按账本解码（B-5/B-6） |
| T6 DTO | `DtoEnvelopeMatrix` | ID类型×首/非首×counter×timestamp×PID_OFF；全部截断边界（B-7） |
| T6 DAQ | `DynamicDaqRejectedExplicitly` | 返回 DynamicDaqNotImplemented，不伪装成功（B-5） |
| T7 线程 | `ImmutableSnapshotConcurrentReads` | 加载中 NotReady；发布后并发读；回调一次；取消/析构安全（B-20） |
| T8 编码 | `Utf8AndUtf16Descriptions` | 中文 UTF-8 不损坏；UTF-16 行为与 main 的 BOM API 一致 |
| T9 性能 | `ParseLargeFileUnderBudget` | ≥5 MB 合成文件先记录基线，之后再定 SLA |
| T10 隔离 | `CoreLibHasNoA2lSymbols` | `libxcp` include/link INTERFACE 不出现 `a2l`/`Boost` |

### 7.3 构建与运行

```bash
# 开启 A2L 桥接（SDK 路径由本机 liba2l 构建产物位置决定，不写死进仓库）
cmake -B cmake-build-release -S . -DLIBXCP_BUILD_A2L=ON -DLIBXCP_LIBA2L_ROOT=<LIBA2L_SDK_ROOT>
cmake --build cmake-build-release --config Release
ctest --test-dir cmake-build-release -C Release -R "A2l.*" --output-on-failure
```

默认（不带 `-DLIBXCP_BUILD_A2L=ON`）时现有测试集行为与产物**必须逐字节不变**，这是回归门禁。

> **A-7 后记**：本里程碑只承诺 Windows x64 / MSVC vc143。上面命令中的 `<cfg>` 需与主树构建类型一致，
> `liba2l.dll` 由 `POST_BUILD copy_if_different` 复制到 `$<TARGET_FILE_DIR:A2lSmoke>`；C-11 已关闭。

---

## 8. PoC 步骤与验收（**R3c：`liba2l.dll` + C++ 接口 + pin main HEAD，需你批准**）

### 8.1 两阶段划分

| 阶段 | 内容 | 产出 |
|---|---|---|
| **S1：SDK 构建** | 独立 project 编上游 STATIC + 薄壳 SHARED，输出到显式准备目录（不执行 install） | `include/`、`lib/liba2l.lib`、`bin/liba2l.dll` |
| **S2：主树消费** | 显式指定 `LIBXCP_LIBA2L_ROOT`，构建桥接层和单个 `A2lSmoke` | `libxcp_a2lbridge.lib` + `A2lSmoke.exe`；POST_BUILD 复制 DLL |

### 8.2 检查点

| # | 动作 | 通过标准 | 失败时的处置 |
|---|---|---|---|
| **P0** | ✅ **已完成（R4.3 实测）**：`thirdparty/a2llib` 已落地，`git ls-files -s` 显示 gitlink `160000 c31057498555cbb29ab48e718329ca3163c10221`，与设计 pin 值**完全一致**；`git rev-parse HEAD` 同值。`.gitmodules` 仅含 path/url 两字段、**无 `branch`** ✅；`thirdparty/a2llib/CMakeLists.txt` 确认 `set(CMAKE_CXX_STANDARD 23)` ✅ | 全部通过标准满足，无需处置 | — |
| **P1** | ✅ **已完成（R4.2 grep + R4.3 复核）**：uchardet 符号命中 `src/a2lhelper.cpp`（`DetectCharset()`）与 `src/a2lobject.cpp`（编码检测），**核心库确有调用**。但 `src/xcp/` 38 文件 + `include/a2l/xcp/` 29 头文件均无引用 → 按 §5.2 R4.2 结论执行：**shim 对链接闭包与 XCP 子解析器安全**，完整 `ParseFile()` 路径的编码检测失效风险已记入 §6.3，首里程碑以 UTF-8/ASCII 样本约束缓解。**A-8 方案 A 维持有效，无需重开方案 B** | 见左列结论 | 若后续需支持 UTF-16/32 A2L → 回 §5.2 方案 B 真装 uchardet |
| ~~P1b~~ | ~~核实 v1.0 的 BOM 能力~~ —— R3c 改 pin main 后作废：main 的 `ReadAndConvertFile()`/`CheckBom()` 已逐行读到，能力明确 | — | — |
| **P2-前置**（R4.3 实测） | S1 configure 的环境前提核查（非动作本身）：① VS 2022 / vc143 ✅；② CMake 4.0.2 ✅；③ `C:\boost\lib` 下 locale/filesystem/process 三组件 **Release(mt) 与 Debug(mtd) 双套齐全**（`*_vc143-mt[-gd]-x64-1_86.lib`），印证 A-12 静态库假设 ✅；④ uchardet 全盘搜索确认不存在 → shim 必要性成立 ✅。⚠️ 新发现：**CMake 4.x 移除了 <3.5 的 compatibility**，若上游或 Boost CONFIG 包声明低版本 `cmake_minimum_required` 可能在 configure 报错——P2 实测时留意，必要时 `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` 兜底 | 全部前提满足，P2 可执行 | 若 CMake 4.x 拒绝低版本声明 → 加 policy 兜底参数重试 |
| P2 | S1 configure：`liba2l_sdk` project，**`CMAKE_CXX_STANDARD 23`**，Boost_DIR 由 `LIBXCP_BOOST_ROOT` 推导，uchardet 走 shim，`A2L_TOOLS/TEST/DOC/FLEX=OFF`。**前置：需先创建 `thirdparty/uchardet-shim/` 与 `thirdparty/a2l-sdk/CMakeLists.txt`（属实现代码，待批准后落地）** | Boost 1.86 三组件命中（静态，印证 A-12）；uchardet CONFIG 命中 shim | 逐项排查 |
| P3 | S1 build 上游 `a2l`（STATIC，§5.3.8 方式 ①） | 全部 TU 在 MSVC 17 `/std:c++23` 下编过。**须记录实际 TU 数**（main 含 label/logstream，比我此前误引的 "~150" 更多） | 若 C++23 编译失败 → 试 `/std:c++latest`；仍失败则**回报并重启 v1.0 讨论**（这是选 main 的主要代价） |
| **P4** | S1 build `liba2l.dll`（**薄壳 SHARED + `A2L_INTERFACE`**） | 生成 `liba2l.dll` + `liba2l.lib`；`dumpbin /EXPORTS` 可见工厂函数与导出类成员 | 缺符号 → 导出层引用了未导出的上游 inline，改在 `liba2l_export.cpp` 内闭环 |
| P4b | （可选）验证 §5.3.8 方式 ② 全量 SHARED | 记录结论即可，不作门禁 | 失败则确认采用方式 ① |
| P5 | 把头文件/import lib/DLL 输出到显式 prepared root（不执行 install/export） | §5.1 的 find_path/find_library/find_file 成功；ABI 常量可读 | 修输出目录或显式 root |
| P6 | S2 configure + build `libxcp_a2lbridge`（**`/std:c++20`**，只 include `liba2l_api.hpp`）；**人工核对主树 cfg == SDK cfg 目录**（A-10 pass 后无自动拦截） | 桥接层零上游头泄漏；C++20 树能消费 C++23 编出的 DLL（证明标准隔离有效） | 说明导出接口还漏了类型或标准泄漏进边界 |
| P7 | 构建并运行唯一 `A2lSmoke`；POST_BUILD `copy_if_different` 复制 DLL 到 exe 目录 | exe 生成、DLL 可加载、退出码 0 | 排查 imported target 与复制命令 |
| P8 | 关开关重编主树 | 原 libxcp + 全部既有测试照常通过，产物不变 | 视为阻塞缺陷 |
| P9 | 隔离性断言 | `grep -r "#include <a2l/" a2lbridge/` **必须为空**；`libxcp` 目标 INTERFACE 不含 `liba2l`/Boost | 设计违规，回退修正 |
| P10 | `find_package(Python3 REQUIRED COMPONENTS Interpreter)`；CTest fixture 先生成 build-tree A2L，再运行 `A2lSmoke` | 零第三方 Python 包；生成文件不入库；冒烟解析通过 | Python 缺失时 configure 明确失败 |

### 8.3 本 PoC 不做

install/export package、CI、缓存、性能 SLA、完整测试标签体系、Linux/macOS 均不在当前最小开发验证集。
此外，不实现 §4 桥接层完整业务逻辑（除支撑 `A2lSmoke` 的最小路径），不做 CRT 混链自动校验（A-10 pass）。

### 8.4 风险登记（R3c 更新：pin main 后三项回升）

| 风险 | 说明 | 缓解 |
|---|---|---|
| ⬆️ **上游 C++23 vs 主树 C++20** | R3b 曾因 v1.0 判为"已消除"，**pin main 后重新成为主要矛盾** | §5.3 标准分治恢复为主方案；PoC P6 专门验证"C++20 树消费 C++23 DLL" |
| ⬆️ **uchardet 硬依赖** | 两版都无条件 `find_package(REQUIRED)`+PUBLIC 链接，本机没有。✅ R4.3：P1 已查明引用范围（仅 `a2lhelper/a2lobject`，xcp 子集零引用），风险降级为"已界定约束" | §5.2 shim；完整解析路径编码失效风险见 §6.3；UTF-16/32 需求出现时回方案 B |
| ⬆️ **MSVC 17 对 C++23 的支持度未知** | 上游 CI windows job 的具体工具集未核实；main 的 TU 集合也比 v1.0 大 | PoC P3 实测；失败则回退讨论 v1.0 |
| **DLL 与头文件不同步** | 固定 SHA 已规避日常漂移；人工升级仍可能只刷新 submodule | 当前最小集仅要求人工重建 SDK + `kLibA2lAbiVersion` 运行时检查；升级自动化移出范围 |
| **CRT 混链无自动拦截（A-10 pass）** | Debug 主树误链 Release DLL → 跨模块堆释放崩溃，难定位 | 流程约定：主树 cfg 必须与 `LIBXCP_LIBA2L_ROOT` 所指 cfg 目录一致；写入 §7.3，PoC P6 人工核对。**已知并被接受，非遗漏** |
| 上游类无 dllexport | 仅在 §5.3.8 方式 ② 下成问题 | 首选方式 ①（薄壳 SHARED），导出面自带 `A2L_INTERFACE` |
| STL 版本漂移 | VS 小升级改布局 | SDK 目录带 `<toolset>`；`kLibA2lAbiVersion` 兜底 |
| DLL 部署负担 | 交付需带 `liba2l.dll`(+pdb) | release/debug × x64 各一份；拷贝规则写入 tests/CMakeLists（C-11） |
| 异常穿越 DLL 边界 | 上游解析会 throw | 导出方法全 `noexcept`（R4） |
| **A-6 循环自证** | 生成的样本只含我们想到的语义 | §7.1-A 三条缓解：畸形开关 + 手写交叉样本 + 预留真实样本挂载点 |
| 上游 export 模板损坏 | `a2lConfig.cmake.in` 引用不存在的 `dbcTargets.cmake` | §5.3.5 自写 wrapper，绕开 |
| ✅ 已消除：放弃 XCPplus | R3b 曾作为 pin v1.0 的代价 | **pin main 后 XCPplus 可用，§6.3-A 优先级逻辑保持有效** |


---

## 9. 自我审查（占位符 / 矛盾 / 边界）

- ✅ 未写死绝对路径：`LIBXCP_LIBA2L_ROOT` / `LIBXCP_BOOST_ROOT` 走缓存变量；`C:\boost`、Windows SDK 版本只作为本文档实测记录出现，不进 CMake 默认值。
- ✅ 未擅自改 thirdparty：`thirdparty/a2llib` 保持原样；SDK 工程、`A2L_INTERFACE` 宏、export wrapper 全部在本工程自建目录内。
- ✅ **uchardet 引用范围已查明（R4.2 P1 实测）**：仅 `src/a2lhelper.cpp` + `src/a2lobject.cpp` 引用 uchardet 符号；`src/xcp/` 38 个文件 + `include/a2l/xcp/` 29 个头文件均无引用。shim 链接安全，但完整 `ParseFile()` 路径在非 UTF-8/ASCII 输入下有编码检测失效风险（已记入 §6.3）。
- ⚠️ 有意保留的不确定项（显式标注，不当作已知事实）：① **MSVC 17 在 `/std:c++23` 下能否编过上游全部 TU**（P3 定，选 main 的主要代价）；② boost::locale 是否有 ICU 后端（T6 探索用例定）。
- ⚠️ **A-9 与 A-7/A-10 存在内在矛盾**（要跨平台宏但只做 Windows）：已按"代码留能力、只验 Windows"化解（§5.3.3b）。Linux/macOS 分支属**未测试代码**，不得对外宣称可用。
- ⚠️ **"用 main HEAD 但不写 commit 号"技术上不可实现**：submodule 在父仓库只能以 gitlink(SHA) 存在，`.gitmodules` 的 `branch` 仅对 `--remote` 生效。已按固定 SHA + 人工升级落地（§5.3.9）——这不是拒绝执行，是 git 机制限制。
- ⚠️ **§1.3 的能力矩阵是按 main 核对的**，R3c 选定 main 后该矩阵继续有效（XCPplus / Label 均可用）。R3b 曾为 v1.0 记录的差异表作废；§6.3-A 的 XCPplus 优先级逻辑恢复为主路径。
- ⚠️ 范围边界：本设计只覆盖"读取 A2L → 驱动 XCP 读值/DAQ 解包"。刷写（PROGRAMMING/SEGMENT）、ECU State 切换、SET_REQUEST、时间同步精度校准虽有能力，但**不在本里程碑**。
- ⚠️ §4 全部接口为草案，评审定稿前不落头文件（遵守"未经指示不添加新接口"）。
- ✅ **B-1～B-20 已于 R4 全部批准**，接口与算法语义基线见 `A2L_接口语义_B类决策_R4.md`。
- ⚠️ B-3 的实际枚举全集、B-4 的具体上游字段名、B-7 的 DTO envelope 精确字节规则仍需源码/规范实测；它们是事实验证，不再是设计决策缺口。

### 9.1 修订记录

| 轮次 | 变更 | 依据 |
|---|---|---|
| R1 | 初稿：`add_subdirectory` 源码级集成 + C++ 标准复位兜底 | 当时未评估 ABI 面 |
| R2 | ① 采纳"预编译静态库彻底隔离"，废弃方案 C；新增 inline/模板跨界证据、POD 折叠双层边界、上游 export 缺陷、SDK 独立构建入口、两阶段 PoC、风险登记。<br>② **撤销 XCPplus 缺口结论**（§1.3 勘误），新增 §6.3-A 优先级逻辑与裸指针生命周期约束。<br>③ 更新 §3.2 目录规划。 | `include/a2l/{module.h,a2lfile.h,a2lstructs.h}`、`cmake/a2lConfig.cmake.in`、本机 Windows SDK/Boost 实测 |
| **R3c** | **A-13 改判：pin main HEAD，不 pin v1.0**。落地内容：<br>① §5.3.9 重写为版本决策记录（含"submodule 无法浮动引用"的机制说明与收益/代价双向表）；<br>② C++23 分治与 MSVC 对 C++23 支持度重新成为活跃风险；§5.3.7 SDK 标准改回 23；<br>③ **更正 uchardet 判断**：两版都 REQUIRED，该 P0 从未因 v1.0 消失，§5.2 恢复准确表述；<br>④ XCPplus 可用 → §6.3-A 优先级逻辑生效，满足规范 §8.2；main 的 BOM API 明确存在；<br>⑤ lexer 修复（`c3105749`）记为实质利好；PoC 增 P11 升级纪律演练；<br>⑥ 勘误：旧 "main HEAD `1274fa4a…`" 实为父 commit。 | GitHub API `/commits/main`（sha `c31057498555cbb29ab48e718329ca3163c10221`，parents=`1274fa4a…`）、v1.0/main CMake 对照 |
| **R4** | 用户逐项批准 B-1～B-20：<br>① 修正地址语义（ECU_ADDRESS 原值、extension 独立、AG 只作元素计数/步进/对齐）；<br>② 数据类型显式映射、数组/RECORD_LAYOUT/CHARACTERISTIC/STRUCTURE 首版边界定稿；<br>③ 换算改 tagged `PhysicalValue`，六类方法执行，FORM 暂不执行，越界默认 Reject；<br>④ DAQ 首版只启用 STATIC，以实际 READ_DAQ/WRITE_DAQ 账本为真值，并用冻结 DTO 布局；<br>⑤ `module::symbol`、安全 INCLUDE、结构化 Result/Error、不可变快照线程模型定稿；<br>⑥ A2L/Slave 差异按 Error/Warning/Info 分级且运行时为真值。 | 用户逐项确认；完整基线见 `A2L_接口语义_B类决策_R4.md` |
| **R4.1** | CMake 范围收缩为最小开发验证集：仅 Windows/MSVC 下构建 `liba2l.dll`、`libxcp_a2lbridge` 和 `A2lSmoke`；主树由显式 root 创建 imported target；POST_BUILD 复制 DLL；Python3 CTest fixture；install/export/CI/cache/SLA 暂不做。 | 用户明确“只考虑实现代码，只保证最小调试集”；选择解释为最小开发验证集且默认 Release |
| **R4.2** | P1 uchardet grep 实测落地 + 嵌入可行性确认：<br>① §5.2 P1 从"待实测"更新为"已实测通过"，附完整符号分布证据（xcp 子集 0 引用、完整解析路径有连带风险）；<br>② §6.3 已知缺口表新增"uchardet shim 下完整解析路径编码检测失效"条目及缓解策略；<br>③ §9 自我审查将 uchardet 从"不确定项"移至"已查明约束"；<br>④ 整体嵌入可行性确认：架构隔离正确、XCP 协议调用链路闭环、libxcp 不变量不被破坏、无需修改核心库接口。 | 工具实测：grep `thirdparty/a2llib/src/xcp/` + `include/a2l/xcp/` 全量扫描；`protocol_types.hpp` / `session.hpp` / `command_executor.hpp` / `xcp_master.hpp` 接口核对 |
| **R4.3** | PoC 检查点状态刷新（§8.2 / §8.4）：<br>① **P0 判定完成**：submodule 已落地且 gitlink = pin 值 `c3105749…`，`.gitmodules` 无 branch 字段，上游 C++23 确认——全部通过标准满足；<br>② **P1 判定完成并修正结论表述**：核心库确有 uchardet 调用（原通过标准"无→shim"不成立），但按 R4.2 界定的范围（xcp 子集零引用）维持 A-8 方案 A，不触发回退问话；<br>③ 新增 **P2-前置** 行记录环境核查：VS2022/vc143 ✅、CMake 4.0.2 ✅、Boost 三组件 Release+Debug 双套齐全 ✅、uchardet 确认不存在 ✅；登记 CMake 4.x 低版本 compatibility 移除的新风险及 policy 兜底预案；<br>④ §8.4 uchardet 风险行同步降级；⑤ 明确下一步 P2/P3 需先创建 shim 与 SDK 工程文件（实现代码，待用户批准后执行）。 | `git ls-files -s` / `rev-parse HEAD` / `.gitmodules` / 上游 CMakeLists 实读；`C:\boost\lib` 目录枚举；vswhere；全盘 uchardet 搜索 |
| **R4.4** | 批次10（数据补全 + 黄金回归套件 + 端到端 10.3）落地后的口径修订：<br>① **ABI bump 1→2**：IDoc +SetActiveModule/AllowIncludeOutsideRoot/LastErrorLine；IfDataXcpDto +timers/命令码/Seed&Key/ECU状态/module_name/ambiguous/plus_conflicts，新 DTO DaqCapsDto/EventChannelDto/XcpPlusConflictDto；<br>② **B-17 落地口径**：多 MODULE 同时声明 IF_DATA → GetIfDataXcp 返回 kAmbiguousName，LoadOptions::active_module 显式选择，绝不静默取首个；<br>③ **B-18 落地口径**：实测上游 `/include`（FixIncludeFile）无循环/深度/越根防护（.a2l 递归 ParseFile 会栈溢出），防护由 SDK 解析前预扫描承担（≤32 层、默认禁越根、带行号），LoadOptions::allow_include_outside_root 可放开越根；<br>④ **B-16 修正**：MAX_CTO/MAX_DTO 为 Warning（容量取保守值即可协商），protocol **MAJOR** 不兼容为 Error、仅 MINOR 为 Warning；<br>⑤ **B-14 修正**：64 位整型 IDENTICAL 全字路径正逆算不经 double（精度口径）；<br>⑥ **B-2 补充**：MATRIX_DIM stride 按低维元素数递推（bridge 同递推二次校验）；<br>⑦ **B-4 子集**：CHARACTERISTIC VALUE + 单段 FNC_VALUES(Position=0/DIRECT/纯标量版式) 才推导 data_type；VAL_BLK/ASCII 执行延后（B-11 部分完成）；<br>⑧ B-16 的 DAQ 侧 Error 项与 B-6 实际账本**推迟到 10.4**（XCP 核心 DAQ 命令组两端均未实现，仓库实测）；<br>⑨ 事件 TIME_UNIT 仓库内无码表 → cycle_time_us 恒 0（raw 码透传不臆造换算）；§7.2 T8 探索实测 UTF-16 BOM 报 `Invalid or unsupported charset: UTF-16`（§6.3 缺口实证，样本统一 UTF-8/ASCII 约束维持）。 | `code-plan/A2L_集成_R4_实施记录_批次10_数据补全与黄金套件.md`（核证清单 + 4 处测试倒逼缺陷修正 + 291/288 测试证据） |
| **R4.5** | 批次11（偏差收口）修订：<br>① **`DtoFrameLayout::pid_off` 补齐**（对齐 §4.4:400）：`Decode` 对 `pid_off==true` 显式拒绝 `UnsupportedOperation/Phase::Layout`（PID 缺席后无法按 EPK 路由，XCP 要求 Transport 层保证唯一标识——docs §7.5，UDP 通道无此关联）；T6 envelope 矩阵补 PID_OFF 维度负例；真实支持（按 Transport 上下文路由无 PID 帧）待 CAN 需求；<br>② **B-18 include chain 文本级交付**：循环/深度错误 message 升级为完整链 `a -> b -> a`（B-19 显示契约，业务仍只判 ErrorCode）；结构化 `Error.include_chain` + IDoc 通道**随 10.4 ABI v3**，不单独 bump；<br>③ **B-2 `source_lower_bound` 事实化**：上游词法+语法 grep 均无 `ARRAY_DIMENSION/SOURCE_LOWER_BOUNDS`（本轮复测）→ 字段恒 0 且含该属性 A2L 会在上游直接 ParseFailed；已在 §6.3 新增缺口行 + B-2 行注记，策略=接受限制（真实需求向上游提语法 PR，不自研预处理）；<br>④ **EVENT TIME_UNIT 码表**：已确认 docs 仅给出"周期=TIME_CYCLE×TIME_UNIT/低 nibble"而无数值表（2362 行的表属 TIMESTAMP 高 nibble 不可套用）→ §6.3 新增缺口行，`cycle_time_us` 维持 raw 码透传/0，待用户提供 XCP Part 1 权威表后补换算。 | `code-plan/A2L_集成_R4_实施记录_批次11_偏差收口.md`（实施+测试证据；批次10/11 改动已入库 commit 580bdb0 / 51381a8） |
| **R4.6** | 批次12（ABI v3：include 链结构化 + UDP 端点透传）修订：<br>① **B-18 结构化载体落地**：`Error.include_chain`（canonical 全路径，主文件→触发点，首元素恒为主文件，非 include 类错误恒空）+ `IDoc::LastErrorChain(std::vector<std::string>*)`（out-param 风格，与 `ListSymbols` 同惯例；`nullptr` 返回 `kBadArgument` 且不改写错误通道）；链填充四个失败点（入口超深 / 父层超深 / 循环 / 越根，越根链含目标文件）；`message` 文本一字不改（B-19 显示契约与既有 `" -> "` 断言保持）；<br>② **liba2l ABI 2→3**（同批承载 ①与③，避免二次重建/回归）；<br>③ **UDP 端点元数据落地**：`IfDataXcpDto` +`udp_packet_alignment`（上游原始码 0/1/2）+`udp_sub_commands`；桥接层按 `8<<raw` 转**位宽 8/16/32**（>2 防御值 0=未知），`DiffIfData` 追加 `PACKET_ALIGNMENT`/`SUB_CMDS` 两条 Info 冲突；**本批零传输行为变化**——`UdpTransportConfig` 实测无 alignment/sub-command 概念，纯元数据；<br>④ §4.3 `packet_alignment` 默认值 `1 → 8`（原行"=1 ///< 8/16/32 bit"自相矛盾）；§6.3 新增上游 `OPTIONAL_TL_SUBCMD`/`PACKET_ALIGNMENT` 非法值静默降级行；⑤ 异步失败分支 message 由硬编码"异步解析失败"改为 SDK `LastError()` 文本 + `cause`（与同步分支对称）；<br>⑥ 错误码口径按用户裁决采**方案 B**：不补具名码，映射表写入 `A2L_接口语义_B类决策_R4.md` §5（逐条实测取证，含"B-12 当前无对应代码路径"与"`record_layout_impl` 已写未接线"两项如实登记）；<br>⑦ 原候选 ③异步进度 / ④STRUCTURE / ⑤VAL_BLK-ASCII / ⑥T9-T10-Debug SDK 经用户裁决**登记延后**（§6.3 四行），10.4 核心 DAQ 与写回（P0）另批立项。<br>⑧ **§4.4 `DtoFrameLayout` 草案块按已验证实现回写**（`timestamp_size_bytes` → `timestamp_size_bits`，补 `overflow_indicator` / `header_bytes`），**改文档不改代码**——该结构字段集已由 T6 DTO 矩阵与 292/292 验证，改名代码无功能收益；同时确立引用纪律：跨文档定位改用**字段名锚点**，不再引用绝对行号（批次11 记录里的 `§4.4:400` 已因文档增删发生漂移）。 | `code-plan/A2L_集成_R4_实施记录_批次12_ABIv3_include链与UDP端点透传.md`（写码前 10 项核证 / 3 处问题实录 / 292·288 门禁 / P9 隔离 0 命中） |
| **R4.7** | 批次13（A2L 侧收尾：R1/R2/R3/R4/R5/R6/R7 + ABI v4）修订：<br>① **B-12 首版闭合**：`IDoc::ListStructures` + `StructInfoDto/StructMemberDto`（TYPEDEF_STRUCTURE 与 INSTANCE 元数据，成员**不递归展开**）；桥接层映射为 `kind=Structure` 的元数据符号，读写必拒 `UnsupportedOperation/Query`，且不进基址反查表；同名冲突时保留 MEASUREMENT/CHARACTERISTIC（B-13 不猜）。<br>② **B-11 半边落地 + 半边事实化**：RL 推导放宽到连续 VAL_BLK（元素类型 `FNC_VALUES` + 元素数 `MATRIX_DIM` 乘积，两者皆可证才执行，压单维 extent 走 B-1/B-2）；ASCII 因上游无 `MAX_LENGTH` 而维持显式拒绝（§6.3 行已改判）。<br>③ **B-4/B-12 判定器接线**：`SymbolDto` 增 `record_layout_name/record_layout_kind`，桥接层在数值读写前调 `CheckRecordLayoutExecutable`；`kNotProvided`（无版式信息）不进判定器以免把"不知道"当成"不可执行"。<br>④ **§4.5 `Progress()` 语义改判**：由"取自上游 ProgressInfo"改为 **SDK 阶段进度**（5/25/70/100，桥接层发布前上限 99）——原口径经复核不可实现（行数只对 UTF-16/32 有效、`LineNo()` 解引用 `ParseFile` 栈上的 scanner），详见 §6.3 与批次13 记录 §0.2；`progress_notify_percent` 语义降为轮询节奏提示。<br>⑤ **B-20 线程口径补注**：`A2lBridge::Impl::ready` 改 `std::atomic<bool>`（release/acquire），使"异步加载中轮询 Progress()/Database"这条契约真正无数据竞争。<br>⑥ **门禁补齐**：T9 `ParseLargeFileUnderBudget`（实测 264 ms → 预算锁 900 ms，不设 SLA）、T10 `A2lIsolation` ctest 化、Debug prepared root。<br>⑦ ABI **3→4** 一次承载 ①③④；libxcp 侧零改动，OFF 门禁仍全绿（339/339；计数较 288 上升系核心新增用例，与 A2L 无关）。 | `code-plan/A2L_集成_R4_实施记录_批次13_A2L侧收尾.md`（§0.2 复核证据：`a2lfile.cpp:239-254/416-421/256-277`、`FlexLexer.h:96/104`、`a2lparser.y:1226-1238/1355-1369/855-877`、`a2lflexer.l:196`；ON 346/346、OFF 339/339、clang-format 0、P9+`A2lIsolation` 双保险） |
| **R4.8** | 批次14（XCP 核心 DAQ + 写回 + B-6 账本 + B-16 DAQ 项 + DTO 帧边界）修订：<br>① **R12 帧边界定稿**：`DtoPacket::data` 改为**完整 DTO 帧**（`data[0]==pid`），`IEventListener::OnDto` 注释与本节 §4.4 附注同步；RES/ERR/EV/SERV 仍剥首字节——只有 DTO 例外，理由是下游 `IDaqLayout::Decode` 的入参契约就是含头整帧。<br>② **§4.4 `DaqLayoutSnapshot` 由不透明前置声明落地为真实类型**：`source`(A2lPredefined/LocalLedger/EcuReadback) + `generation` + `lists` + `routes`(PID→(daq,odt))；`OdtEntryLayout`/`DecodedDtoSample` 增 `symbol_aliases`（B-6"不猜归属"与"保留候选"同时成立）；新增入口 `A2lBridge::CreateDaqLayoutFromLedger` 与 `CreateDaqLayoutFromEcuReadback`。<br>③ **B-6 落地口径**：可配置 STATIC 以本端 `WRITE_DAQ` 账本为权威（`XcpMaster::DaqLedger()`），PREDEFINED 以 `READ_DAQ` 回读为权威；账本缺失/PID 无路由/同 PID 被两个 (daq,odt) 声称 → 一律 `InvalidLayout`，调用方保留 raw；A2L 顺序只作候选（E2E 用"A2L 序与账本序相反"证明按账本解码）。<br>④ **B-16 DAQ 侧三项比对落地**：`RuntimeXcpParams` 增 identification/address-extension/granularity 三项（`nullopt`=未查则跳过），不一致记 **Error**。<br>⑤ **B-5 边界不变**：DYNAMIC 仍 `UnsupportedOperation`；`ALLOC_*` 未实现故不进 `CommandCode`（只登记已实现的 0xE3/0xE2/0xE1/0xE0/0xDE/0xDD/0xDB/0xD9/0xDA/0xD8 + 写回用到的 DOWNLOAD 组）。<br>⑥ **字节偏移权威**：`docs/XCP_1.3.0_document.md` 无 Position 字节表（实测），偏移一律交叉参考只读 `thirdparty/XCPlite/src/xcp.h`；**SET_MTA 例外不可照抄**（XCPlite 是 1.4 的 8 字节变体，本库维持 7 字节）。<br>⑦ **隐含状态恢复扩到写回侧**：SYNCH 恢复后 UPLOAD/**DOWNLOAD** 重放 `SET_MTA`、**WRITE_DAQ/READ_DAQ** 重放 `SET_DAQ_PTR`（docs L2783）；配置编排对每个 Entry 都显式 `SET_DAQ_PTR`，避开"写过末位指针未定义"（docs L2172）。<br>⑧ 计划文档 T14-04 原文"GET_DAQ_LIST_INFO 的 MAX_ODT/MAX_ODT_ENTRY/**FIRST_PID**"经 docs L2452-2457 与 L2222 复核为**误记**：FIRST_PID 只出现在 `START_STOP_DAQ_LIST` 响应，已按规范实现并在此登记纠正。 | `code-plan/A2L_集成_R4_实施记录_批次14_XCP核心DAQ与写回.md`（核心侧新增 51 条用例 + A2L 侧 9 条；ON 346/346、OFF 339/339、clang-format 0、`A2lIsolation` 自动跑通过） |
| **R4.9** | 批次18（XCPlite 结构叶子展开 STRUCTLEAF + CalSeg 标量事实并入，接管闭合批次16）修订：<br>① **B-12 leaf 展开器落地**：SDK 导出 TYPEDEF 标量类型事实（`TypedefMeasurementDto` + `IDoc::ListTypedefMeasurements`，**ABI v5→v6**；`StructInfoDto` 增 INSTANCE 级 `matrix_dim`），桥接层新增 `a2lbridge/src/structure_layout.{hpp,cpp}::BuildStructureLeaves`——四形态（标量成员/定长数组/嵌套结构/结构数组实例逐元素 `path[i]`）+ 深度≤32/DFS 环检测/尺寸界内/ProductDims 防乘溢出/每实例预算 4096；不可证一律 `LoadWarning(InvalidLayout, Load, module::path)`，不猜测（B-3）。叶子以 `kind=Measurement` 符号注册（基址+Σ偏移+索引×步长、extension 透传、B-1 不乘 AG），可 Find/ByteSizeOf/ComputeElementAddress/ToPhysical/FromPhysical/读写；**本体整块仍拒且不进基址反查**（B-12 边界不变，`m_leaf_positions_` 排除）。<br>② **TYPEDEF_CHARACTERISTIC 并入标量事实注册表（18-A′，探针驱动）**：CalSeg 校准成员在 XCPlite 语料只以 TYPEDEF_CHARACTERISTIC 引用；元素类型经 Deposit→RECORD_LAYOUT FNC_VALUES 与 CHARACTERISTIC 符号同一可证口径（Position 0/1+DIRECT+标量），不可证→宽度 0 交 resolver 拒绝；同名测量先入 + `stable_sort` first-wins 确定化；真实语料 InvalidLayout **2→0**（42 符号基线）。<br>③ **CalSeg 实例 A2L 只读实然钉住（F15）**：XCPlite `src/a2l.c:1092-1116/:1156-1169` 仅对 `XcpAddrIsAbs||IsApp||IsDyn` 寻址打印 `READ_WRITE` → CalSeg 分段实例缺该位，叶子 `read_write=false`，`FromPhysical` 由只读门拒 `UnsupportedOperation/Conversion`（`compu_method_eval.cpp:571`，先于限值）——**不伪造位**；物理可写性证据维持在批次17 裸地址通路。<br>④ 回写：§4.1 `SymbolKind` 注释、§6.3 B-12 行 leaf 展开器状态改"已落地"、§7.2 新增 T5.5 用例行、B 类决策文档 B-12 行更新。批次16 的 16-0001 语料门由批次17 运行时 A2L 解除并入库 `tests/data/a2l/xcplite_structleaf_corpus.a2l`（+ 手写负例 `xcplite_leaf_broken.a2l`）。 | `code-plan/XCPlite_Slave_协议调试_实施记录_批次18_结构叶子展开.md`（F13–F17 核证表 + §6 批次16 映射；ON 377·OFF 352 双门禁 / A2lGolden 38+1SKIP / Xcplite 25/25 / Release+Debug prepared 同批 / clang-format 0） |
