# B-T4 实施记录：A2L 端点 DTO 的 ABI v9 与 TCP 端点事实

> 触发：User said (m01666)："首先检查现有进度以及未完成任务。其次：**同意 B-T4
> 一次做到 ABI v9**。"
> 计划依据：`code-plan/TCP传输_实施计划_B-T1至B-T5.md` §7.2（Profile 推断证据）、
> §7.3（多实例 fail-closed）。
> 规范依据：`docs/ASAM-AE-MCD-1-XCP-AS-Ethernet-Transport-Layer-V1-5-0.pdf`
> （子命令码表，提取文本 `.tmp/tl_v15.txt:288-291`）。

---

## 1. 问题定性：TCP 端点此前"有类别、无事实"

`IfDataXcpDto` 早在批次 12 就能正确报出 `transport == kTcpIp`，但**除了这个枚举
值之外没有任何 TCP 专属字段**：端口、主机、对齐、子命令全部落在 `udp_*` 名下。
桥接层更把这一点固化成代码——`thirdparty/a2l-sdk/a2lbridge/src/a2l_dto_map.cpp`
的旧 `:481-482`：

```cpp
ep.remote_host = dto.udp_host;   // 无条件赋值，无论 ep.kind 是什么
ep.remote_port = dto.udp_port;
```

即：**TCP 端点只能借 UDP 命名的字段对外表达**。这不只是命名难看，它掩盖了
两个真实的能力缺口——`packet_alignment` / `sub_commands` 在旧代码里
**只对 `Kind::UdpIp` 填充**：

```cpp
if (ep.kind == TransportEndpoint::Kind::UdpIp) {   // 旧实现
    ep.packet_alignment = ...;
    ep.sub_commands = dto.udp_sub_commands;
}
```

而规范对 XCPonTCP/IP 与 XCPonUDP/IP **两类传输层都定义了** `PACKET_ALIGNMENT`
与 `OPTIONAL_TL_SUBCMD`（计划 §7.2）。结论：TCP 端点拿不到这两项事实，
消费方无从判断 ECU 的 TCP 对齐。

---

## 2. ABI v9 改动

`thirdparty/a2l-sdk/include/liba2l/liba2l_api.hpp`：`kLibA2lAbiVersion` 8 → 9。

### 2.1 新增统一端点字段

| 字段 | 类型 | 语义 |
|---|---|---|
| `endpoint_port` | `uint16_t` | 端点端口（TCP/UDP 共用） |
| `endpoint_host_name` | `std::string` | `HOST_NAME` 原值 |
| `endpoint_ipv4_address` | `std::string` | `ADDRESS` 原值 |
| `endpoint_ipv6_address` | `std::string` | `IPV6` 原值 |
| `endpoint_packet_alignment` | `uint8_t` | `PACKET_ALIGNMENT` **原始码**（0/1/2） |
| `endpoint_alignment_declared` | `bool` | A2L **是否显式声明**该关键字 |
| `endpoint_sub_commands` | `vector<uint8_t>` | `OPTIONAL_TL_SUBCMD` 码值 |
| `endpoint_instance_count` | `uint32_t` | 该类传输层实例个数 |
| `endpoint_protocol_version` | `uint16_t` | Protocol Layer 版本（冗余一份） |
| `endpoint_transport_version` | `uint16_t` | **Transport Layer** 版本（`XCP_ON_*` 首字段） |
| `endpoint_profile_hint` | `std::string` | Wire Profile 诊断候选（**禁止自动生效**） |
| `endpoint_profile_hint_declared` | `bool` | 是否尝试过推断 |

旧的 4 个 `udp_*` 字段**保留**并标注 `@deprecated`（一个兼容周期）：
`UdpIp` 同时填新旧两套；**`TcpIp` 只填新字段**，不再污染 `udp_*`。

### 2.2 版本事实分离（关键）

`endpoint_transport_version` 与 `endpoint_protocol_version` 是**两个独立事实**，
不得互相冒充。回归测试用 TL=0x0104 / PL=0x0100 刻意取不同值来锁定这一点
（`tests/a2l_golden_test.cpp` 的 `TcpEndpointFactsFromA2l`）。

---

## 3. `endpoint_alignment_declared`：为什么必须改三方库

计划 §7.2 要求区分"A2L 未声明 `PACKET_ALIGNMENT`"与"A2L 声明了 8 bit"。
**该事实在解析层就被销毁了**。

### 3.1 证据（源级）

`thirdparty/a2llib/src/xcp/xcpdataparser.y:1186` 的语法规则：
```
| PACKET_ALIGNMENT IDENT { tcp.SetPacketAlignment($2); }
```
`thirdparty/a2llib/src/xcp/xcpontcpip.cpp:20-30` 的实现：
```cpp
void XcpOnTcpIp::SetPacketAlignment(const std::string& alignment) {
  constexpr std::array<std::string_view, 3> kAlignmentList = {
    "PACKET_ALIGNMENT_8", "PACKET_ALIGNMENT_16", "PACKET_ALIGNMENT_32"
  };
  for (...) { if (alignment == kAlignmentList[index]) { ...; break;; } }
}   // 未匹配 → 不改值，保持默认 PACKET_ALIGNMENT_8(=0)
```

后果：三种语义上完全不同的事实，在对象里**全是 0**——
1. A2L 根本没写 `PACKET_ALIGNMENT`（AML 默认值）；
2. A2L 写了 `PACKET_ALIGNMENT_8`；
3. A2L 写了非法字面量（被静默吞掉）。

若只看数值 0 就断言"ECU 用 8 bit 对齐"，等于**把 AML 默认值当成声明事实**，
正是计划 §7.2 明令禁止的推断。

### 3.2 处置（用户裁决）

向用户提出三个方案（m01816），User 裁决：**"在 a2llib 加 declared 标志位
（推荐）"**。据此修改三方库（`thirdparty/a2llib`，git 子模块）：

- `include/a2l/xcp/xcpontcpip.h` / `xcponudpip.h`：各加私有成员
  `bool alignment_declared_ = false;` 与公有访问器
  `[[nodiscard]] bool IsAlignmentDeclared() const;`
- `src/xcpontcpip.cpp` / `xcponudpip.cpp`：`SetPacketAlignment()` **入口处
  无条件** `alignment_declared_ = true;`（被调用即证明关键字存在），
  `Reset()` 中复位为 `false`。

SDK 侧 `thirdparty/a2l-sdk/src/liba2l_export.cpp` 据此填
`dto->endpoint_alignment_declared = udp.IsAlignmentDeclared();`（TCP 同理）。

---

## 4. 顺带修掉的两个真实缺陷

### 4.1 TCP 子命令码全部错位（缺一个逗号）

写 `TcpEndpointFactsFromA2l` 时实测到 `GET_SLAVE_ID` 得 **0xFE**、`SET_SLAVE_IP_ADDRESS`
得 **0xFB**，与头文件枚举声明的 0xFF / 0xFC 矛盾。逐字节确认根因
（`od -c` 输出，`thirdparty/a2llib/src/xcp/xcpontcpip.cpp`）：

```
"   G E T _ D A Q _ C L O C K _ M U L T I C A S T "   ,  \n
    "   "  \n   " S E T _ S L A V E _ I P _ A D D R E S S "  ,
```

`""` 与 `"SET_SLAVE_IP_ADDRESS"` 之间**没有逗号** → 相邻字符串字面量被拼接，
表退化为 5 个有效项，索引整体前移。

规范核实（`.tmp/tl_v15.txt:288-291`）：
`GET_SLAVE_ID 0xFF` / `GET_SLAVE_ID_EXTENDED 0xFD` /
`SET_SLAVE_IP_ADDRESS 0xFC` / `GET_DAQ_CLOCK_MULTICAST 0xFA`。

| 标识符 | 规范 | 修复前实测 | 修复后 |
|---|---|---|---|
| `GET_DAQ_CLOCK_MULTICAST` | 0xFA | 0xFA ✔ | 0xFA |
| `SET_SLAVE_IP_ADDRESS` | 0xFC | **0xFB** ✘ | 0xFC |
| `GET_SLAVE_ID_EXTENDED` | 0xFD | **0xFC** ✘ | 0xFD |
| `GET_SLAVE_ID` | 0xFF | **0xFE** ✘ | 0xFF |

对照：`XcpOnUdpIp::AddSubCmd` 的同名表逗号完整（6 元素，空槽在索引 1/4），
码值全部正确——即**同一份表在 TCP 侧被写坏了**。

用户裁决（m01998）：**"修 upstream AddSubCmd（推荐）"**。已补逗号，
使 TCP 表与 UDP 表逐项一致。

### 4.2 XCPplus 冲突比对对 TCP 端点完全失明

`liba2l_export.cpp` 的 `DiffIfData()` 旧实现只比对 `udp_*`：

```cpp
AppendConflict("UDP_PORT", plain.udp_port, plus.udp_port, out);
AppendConflict("UDP_HOST", plain.udp_host, plus.udp_host, out);
AppendConflict("PACKET_ALIGNMENT", align_name(plain.udp_packet_alignment), ...);
AppendConflict("SUB_CMDS", subcmd_name(plain.udp_sub_commands), ...);
```

而 SDK 从不给 TCP 端点填 `udp_*`，于是**两个 TCP 端点的端口/主机/对齐/子命令
差异一律漏报**（两侧皆空 → 判定"无差异"）。已改为比对 `endpoint_*`，
并把 UDP 专属标签改成通用标签：

- `UDP_PORT` → `ENDPOINT_PORT`（此前**无任何测试**断言该标签，改名不破坏契约）
- `UDP_HOST` → `ENDPOINT_HOST`（同上）
- `PACKET_ALIGNMENT` / `SUB_CMDS` 本就是传输层中性名，**保持不变**，
  故 `XcpPlusPriorityAndConflictReport` 的 3 条差异断言继续成立。

---

## 5. 桥接层改动

`thirdparty/a2l-sdk/a2lbridge/include/libxcp/a2l/if_data_xcp.hpp`
`TransportEndpoint` 新增两个字段：

- `bool packet_alignment_declared = false;`（区分未声明与声明 8 bit）
- `std::uint32_t instance_count = 0;`（多实例可察觉，§7.3 依据）

`thirdparty/a2l-sdk/a2lbridge/src/a2l_dto_map.cpp` 改动：

1. `remote_host` / `remote_port` 改从 `endpoint_*` 取，并恢复
   `HOST_NAME → ADDRESS → IPV6` 回退链（Vector 工具链常只写 `ADDRESS "IP"`）；
2. `packet_alignment` / `sub_commands` 的填充条件从
   `Kind::UdpIp` 放宽为 `Kind::UdpIp || Kind::TcpIp`（规范两者都定义）；
3. `ep.version` 改从 `endpoint_transport_version` 取。此前该字段恒 0，
   头注释称"桥接层无处可取"——v9 起该说法失效，注释已同步更正；
4. 新增 `ep.packet_alignment_declared` / `ep.instance_count` 透传。

---

## 6. 新增测试（`tests/a2l_golden_test.cpp`）

| 用例 | 锁定内容 |
|---|---|
| `TcpEndpointFactsFromA2l` | TCP 端点 port/host/alignment(32,declared)/subcmds(0xFF,0xFC)/TL 版本 0x0104 与 PL 版本 0x0100 分离/instance_count=1 |
| `AlignmentDeclaredFlagDistinguishesOmitted` | `golden_basic`（未声明）declared=false 且对齐仍读 8；`golden_mask`（声明 32）declared=true |
| `TcpEndpointConflictsAreReported` | XCP vs XCPplus 两个 TCP 端点差异必须报出 `ENDPOINT_PORT`/`ENDPOINT_HOST`/`PACKET_ALIGNMENT`，且胜出方取 plus 值 |

三个用例都用运行时写临时 A2L 的方式，不污染黄金生成脚本（
`tests/a2l_gen/gen_a2l.py` 本次**未改动**）。

### 6.1 用例确实抓到了缺陷（不是补绿灯）

- `TcpEndpointFactsFromA2l` 首跑**失败**：断言 0xFF/0xFC，实测 0xFE/0xFB
  → 暴露 §4.1 的逗号缺陷；修复上游后转绿。
- `TcpEndpointConflictsAreReported` 若在 §4.2 改动前运行，`find_conflict
  ("ENDPOINT_PORT")` 必为 `nullptr`（旧代码只产 `UDP_PORT` 且两侧为空）。

---

## 7. 测试结果

### 7.1 构建

```
cmake --build cmake-build-release -j 4      # exit 0，无 error
```

### 7.2 全量 CTest

```
ctest --test-dir cmake-build-release
921 tests, 99% passed, 3 failed
```

失败的 3 项**全部为既有缺陷，与本批次无关**，证据如下
（`code-plan/DAQ布局自动兼容_实施记录.md:229-236` 已用 HEAD 对照构建证伪）：

| 测试 | 现象 | 归因 |
|---|---|---|
| `a2l_perf_gen` (896) | `TypeError: write_text() got an unexpected keyword argument 'newline'` | `tests/a2l_gen/gen_a2l.py:571` 需 Python 3.10+；本会话 `git diff tests/a2l_gen/` **为空** |
| `ParseLargeFileUnderBudget` (897) | Not Run | 依赖 896 的 fixture，连带跳过 |
| `ExampleXcpIntegrationDemoCppDemo` (913) | `SET_DAQ_LIST_MODE ... event channel redefinition` | 既有缺陷，先前会话已用 HEAD 对照构建复现 |

A2L 专项（本次改动直接影响面）：

```
ctest --test-dir cmake-build-release -R "A2lGolden|A2lE2E|A2lSmoke|A2lLog|MeasurementAdapter"
100% tests passed, 0 tests failed out of 6
  #890 A2lBridgeSmoke                Passed
  #891 A2lLogTimestampTest.*         Passed
  #892 a2l_gen                       Passed   ← 黄金样本 fixture
  #893 A2lSmoke                      Passed
  #894 A2lGolden                     Passed
  #895 A2lE2E                        Passed
```

注意 `a2l_gen`(#892) 与 `a2l_perf_gen`(#896) 是**两个不同**的测试：
前者生成功能黄金样本且通过，后者只用于性能预算测试且因 Python 版本失败。

新增 3 用例单跑：

```
./tests/A2lGolden --gtest_filter="*TcpEndpoint*:*AlignmentDeclared*"
[  PASSED  ] 3 tests.
```

### 7.3 一次险些误判的记录

过程中曾先跑 `libxcp_tests` 得到 749 passed，随后核对时间戳发现
**被测二进制（11:12）早于源码改动（11:26）**，属陈旧二进制。进一步确认
`libxcp_tests` 只链 `libxcp::libxcp`、**不链 a2lbridge**，故它对本批次
天然无感；真正的验证面是 `A2lSmoke/A2lGolden/A2lE2E`（二进制 11:27:45
晚于全部源码 11:27:29，确认为本次编译产物）。

另有一次误判：直接运行 `A2lGolden` 报 29 项失败，原因是黄金样本由 CTest
fixture `a2l_gen` 生成，**绕过 ctest 直接跑二进制会缺 fixture**；改用
`ctest -R` 后全绿。

---

## 8. 已知限制（未在本批次解决）

1. **`endpoint_alignment_declared` 未参与 XCPplus 冲突比对**：若一侧显式声明
   8 bit、另一侧未声明，数值相同（都是 8）→ 不报冲突，尽管语义不同。
   纳入比对会改变既有"3 条差异"契约，留待后续批次决策。
2. **`endpoint_profile_hint` 仅占位**：本批次只定义字段并标注"禁止自动生效"，
   实际推断逻辑（及"任何组合都不足以唯一识别 Profile"的判定）未实现。
   计划 §7.2 明确要求 Profile 选择由 CLI/API/部署配置显式给出。
3. **多实例仍只取首个**：`endpoint_instance_count` 只让消费方"察觉"多实例，
   不做建模，也不做 fail-closed 拦截——拦截需 CLI 侧配合（B-T5 范围）。
4. `thirdparty/a2llib` 是 git 子模块，本次对其源码的修改**不会**出现在
   父仓库 `git diff` 中（`git diff --stat` 显示 `thirdparty/a2llib | 0`）。
   同步上游时需保留这两处改动，否则 §3.2 与 §4.1 的修复会丢失。