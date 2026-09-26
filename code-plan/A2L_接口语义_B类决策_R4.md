# A2L 接口语义 B 类决策（R4）

> 配套设计：`A2L_集成_a2llib选型与适配层详细设计_R4.md`  
> 配套清单：`A2L_集成_未定义项清单_R4审核.md`  
> 状态：**B-1～B-20 已由用户逐项批准；本文是实现前语义基线，不代表整体设计已批准。**

## 1. 总原则

1. 运行时 XCP 能力是当前会话真值，A2L 是静态声明；冲突必须结构化报告。
2. 无法证明地址、布局或换算正确时明确拒绝，不进行猜测性兼容。
3. 原始字节、逻辑地址、Address Extension、物理值分别建模。
4. DLL 内异常一律转换为结构化错误，不跨 ABI 边界。

## 2. 已批准决策

| 编号 | 最终决策 | 首版失败/降级行为 |
|---|---|---|
| **B-1 地址** | `xcp_address = A2L ECU_ADDRESS` 原值；`address_extension` 为独立 8-bit 字段；`byte_offset = element_index * element_size_bytes`；要求 `byte_offset % AG == 0`；`element_address = xcp_address + byte_offset / AG`。AG 不乘入基地址，extension 不拼高位 | 未对齐、溢出、INDIRECT/SUB_ADDRESS → 明确错误且不发包；不自动叠加 ECU_CALIBRATION_OFFSET |
| **B-2 数组** | API 使用 0-based 索引；内部保存 source lower bound、extent、byte stride 和显式主序；首版执行标量、1D、可证明连续的规则 2D | 冲突、溢出、不规则布局 → `UnsupportedArrayLayout`。**事实注记（批次11）**：上游 a2llib 词法+语法 grep `ARRAY_DIMENSION/SOURCE_LOWER_BOUNDS` 均 0 命中 → `source_lower_bound` 恒 0，且含该属性的 A2L 会在上游直接 ParseFailed；规则 2D 以 `MATRIX_DIM` 表达，stride 按低维元素数递推 |
| **B-3 类型** | 按实际 `A2lDataType` 枚举逐项建立固定宽度映射；禁止 `sizeof(enum)`、ordinal 推断和未知类型默认宽度 | 未知、厂商扩展、长度不明 → `UnsupportedDataType` |
| **B-4 RECORD_LAYOUT** | 首版执行标量 VALUE、连续 VAL_BLK、标准连续 FNC_VALUES；CURVE/MAP 只保存轴和布局元数据 | 复杂 block order、轴点交错、重定位、厂商扩展 → `UnsupportedRecordLayout` |
| **B-5 DAQ 范围** | 首里程碑只启用 STATIC DAQ；DYNAMIC 只解析能力，待 Allocator 和 WRITE_DAQ 账本完成后再启用 | `DynamicDaqNotImplemented` |
| **B-6 DTO 映射** | 使用不可变 `DaqLayoutSnapshot`。PREDEFINED STATIC 优先 READ_DAQ；可配置 STATIC 使用本端 SET_DAQ_PTR/WRITE_DAQ 账本；A2L ELEMENT_LIST 只作候选/一致性约束；按 `{extension,address,size,bit}` 关联并保留 aliases | 无实际账本或可信 entry → raw DTO + `LayoutUnknown`，不猜符号/顺序 |
| **B-7 DTO 偏移** | 先解析 DTO envelope；冻结布局包含 ID 类型、首/非首 ODT、counter、timestamp 当前模式/宽度、PID_OFF，再解 entry；精确字节规则经规范与源码验证后固化 | 帧短、未知 PID、非法 PID_OFF、模式冲突 → 丢弃整帧并返回结构化错误 |
| **B-8 换算** | tagged variant；首版执行 IDENTICAL、LINEAR、RAT_FUNC、TAB_INTP、TAB_NOINTP、TAB_VERB；FORM/FORMULA 只保存原文；逆换算仅在唯一、可逆、范围内时开放 | 分母零、表外无默认、逆解多值、FORM → 明确 Conversion 错误 |
| **B-9 位字段** | BIT_MASK 仅用于值提取；DAQ BIT_OFFSET 仅用于 ODT entry 定位；ERROR_MASK 独立保存。首版只执行连续 BIT_MASK，不由 ERROR_MASK 自动推导 valid | 冲突、越界、非连续 mask → `InvalidBitLayout` |
| **B-10 单位** | 优先 PHYS_UNIT；为空时解析 UNIT_REF；保存 SI exponent 元数据但不自动单位换算；冲突时显示 PHYS_UNIT 并 warning | 悬空 UNIT_REF 不阻断 raw 读取 |
| **B-11 CHARACTERISTIC** | 显式五型：Value、Curve、Map、ValBlk、Ascii；首版执行 Value、连续 ValBlk、明确长度 Ascii；Curve/Map 只查询轴与布局元数据 | 不支持操作 → `UnsupportedCharacteristicOperation`；引用缺失 warning |
| **B-12 STRUCTURE** | 首版识别并保存 STRUCTURE/INSTANCE/TYPEDEF 元数据，但不展开、不读写；后续独立展开器按 AddressOffset 生成 qualified leaf 并检测递归 | `UnsupportedStructuredType`，不得静默当 byte array |
| **B-13 Search** | 规范键 `module::symbol`；精确 Find 大小写敏感；未限定 module 仅全库唯一时成功；Search 可大小写无关并支持 `* ?`，稳定排序后截断 | 歧义 → `AmbiguousName`；非法模式 → `InvalidSearchPattern` |
| **B-14 物理值** | `PhysicalValue = variant<int64_t,uint64_t,double,string,bool>`；IDENTICAL 整数保精度，数值换算用 double，TAB_VERB 用 string；raw bytes 独立保留 | 精度损失或类型错误 → `PrecisionLoss/TypeMismatch` |
| **B-15 逆换算越界** | 默认严格 Reject；未来可增加显式 ClampPolicy，但默认仍拒绝并必须返回 clamped 状态 | 越界、NaN/Inf、不可逆 → 不产生 raw、不下发 ECU |
| **B-16 参数比对** | 运行时为真值。Error：protocol major、通信 ByteOrder、AG、不兼容 DAQ ID/entry/address-extension；Warning：MAX_CTO/MAX_DTO、minor、资源/可选命令差异，采用运行时和更保守限制；Info：A2L-only。t1～t7 不作为 Slave runtime 差异项 | Error 只阻断受影响功能，保留数据库、诊断及其他安全功能 |
| **B-17 多 MODULE** | 保存全部 MODULE；对象和 IF_DATA 均带 module scope；仅恰好一个 MODULE 时自动设 active module，多 MODULE 必须显式选择 | `ModuleRequired/AmbiguousName` |
| **B-18 INCLUDE** | 相对当前包含文件目录解析；canonical path 活动栈检测循环并去重；最大深度 32；默认禁止越出主 A2L 根目录，可显式放开 | 返回完整 include chain，不发布半成品数据库。**实现注记（批次11 分步）**：上游 FixIncludeFile 无防护，由 SDK 解析前预扫描拦截；循环/深度错误的 message 携带完整链文本（`a -> b -> a`，B-19 显示契约，业务仍只判 ErrorCode）；结构化 `Error.include_chain` 字段与 IDoc 通道随 10.4 ABI v3 交付 |
| **B-19 错误** | C++20 自建 `Result<T>/Error`，含 ErrorCode、Severity、Phase、path、line、column、module、symbol、message/cause；`LastError()` 仅作展示兼容 | DLL 内捕获所有异常；多错误稳定排序 |
| **B-20 线程** | 私有 mutable builder 完成后一次性发布 immutable snapshot；发布后 const 查询并发安全；加载中返回 NotReady；Bridge one-shot；工作线程回调在发布后恰好一次；析构/取消安全 join | Busy/NotReady/Cancelled 结构化返回，失败不发布半成品 |

## 3. 接口草案约束

1. `SymbolInfo::address` 语义改为 `xcp_address`，Address Extension 始终独立。
2. `Dimension` 表达 source lower bound、extent、byte stride 和主序。
3. `ConversionInfo` 改 tagged variant。
4. `ToPhysical()` 返回 `Result<PhysicalValue>`；`FromPhysical()` 接收 `PhysicalValue` 并返回 `Result<Bytes>`。
5. `Find()` 必须能表达未找到和歧义，不能只使用 `optional`。
6. `DecodedDtoSample::raw` 是 DTO 实际字节，不存在“按 AG 换算”；physical 使用 `PhysicalValue`；首版不由 ERROR_MASK 推导 valid。
7. `IDaqLayout` 依赖不可变 `DaqLayoutSnapshot + DtoFrameLayout`。
8. `LoadAsync` 明确 one-shot、回调线程、状态发布顺序、取消和析构语义。

## 4. 测试门禁摘要

- 地址：AG=1/2/4 下基地址不变；8-byte 数据元素计数为 8/4/2；extension 独立；非整除和溢出不发包。
- 类型：实际枚举全覆盖；大小端；64 位边界；NaN/Inf；未知类型拒绝。
- 布局：标量、1D、规则 2D；VALUE/VAL_BLK/FNC_VALUES；复杂布局拒绝。
- 换算：六类正向；可逆方法反向；分母零、多解、表外、FORM 拒绝。
- DAQ：STATIC；READ_DAQ/WRITE_DAQ 账本；A2L 与 ECU 顺序相反；DTO envelope 组合与截断。
- 数据库：多 MODULE 重名、限定名、稳定排序、INCLUDE 循环和深度。
- 错误/线程：结构化错误黄金样本；加载中 NotReady；并发只读；取消/析构；回调恰好一次。
