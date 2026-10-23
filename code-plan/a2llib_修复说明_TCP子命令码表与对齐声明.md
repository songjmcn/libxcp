# a2llib 修复说明：TCP 子命令码表漏逗号 + PACKET_ALIGNMENT 声明标志

> 用途：向上游 `ihedvall/a2llib` 或内部审批提交的变更申请说明。
> 基线 commit：`c31057498555cbb29ab48e718329ca3163c10221`（"Fixed bug when large floating point are written as integer with many zeros."，2026-09-24）
> 修复 commit：`c4999aaf6657f8a0fe9462024b85c3cbfa9fffcd`（分支 `feat_xcp_tcp_transport`）
> 影响文件：4 个，+39 / -1
>   - `include/a2l/xcp/xcpontcpip.h`
>   - `src/xcp/xcpontcpip.cpp`
>   - `include/a2l/xcp/xcponudpip.h`
>   - `src/xcp/xcponudpip.cpp`

---

## 一、问题概述

本次包含两个相互独立的缺陷修复，均在 XCP on Ethernet 传输层 IF_DATA 解析部分。其中**问题 1 是功能性 Bug（错误编码），问题 2 是信息丢失（API 无法表达已声明事实）**。两者都不改变既有正确路径的行为，只修正错误路径与补齐可观测性。

---

## 二、问题 1：TCP 可选子命令码表因漏逗号导致索引整体前移（严重度：高）

### 2.1 现象

`XcpOnTcpIp::AddSubCmd()` 用一张字符串表把 A2L 中的子命令名映射为规范规定的 1 字节码值，码值计算方式为 **`0xFA + 表项索引`**（表项位置即子命令码）。原代码该表第二个保留槽 `""` 与 `"SET_SLAVE_IP_ADDRESS"` 之间**漏写了一个逗号**。

### 2.2 根因

C++ 中相邻的字符串字面量会被**自动拼接成一个元素**（string literal concatenation）：

```cpp
// 错误写法（原代码）
constexpr std::array<std::string_view, 6> kCmdList = {
  "GET_DAQ_CLOCK_MULTICAST",
  ""                    // ← 此处缺逗号
  "SET_SLAVE_IP_ADDRESS",   // 上一行与本行被拼成 "" + "SET_SLAVE_IP_ADDRESS"
  ...
};
```

`""` 与 `"SET_SLAVE_IP_ADDRESS"` 拼接后变成单个元素 `"SET_SLAVE_IP_ADDRESS"`，于是原本设计的 6 项有效布局塌缩为 **5 个有效项**，其后所有项的索引**整体前移一位**。由于码值 = `0xFA + index`，前移直接导致后续子命令全部算出错误的码值。

### 2.3 实测错误结果（修复前）

| 子命令 | 规范码值 | 枚举声明 | 修复前实际得到 | 偏差 |
|---|---|---|---|---|
| `GET_DAQ_CLOCK_MULTICAST` | 0xFA | 0xFA | 0xFA | 正确（首项不受影响） |
| `SET_SLAVE_IP_ADDRESS` | 0xFC | 0xFC | **0xFB** | −1 |
| `GET_SLAVE_ID_EXTENDED` | 0xFD | 0xFD | **0xFC** | −1 |
| `GET_SLAVE_ID` | 0xFF | 0xFF | **0xFE** | −1 |

即：除首项外，三个 TCP 可选子命令的码值全部比规范和同库头文件 `TcpSubCmd` 枚举声明小 1。这构成三重矛盾——与 ASAM 规范码表矛盾、与本头文件自身枚举声明矛盾、与同库 UDP 侧实现矛盾。

### 2.4 对照证据：UDP 侧是正确的

`XcpOnUdpIp::AddSubCmd()` 的同名表逗号齐全、6 项完整，码值正确。这证明 TCP 侧纯属手误，而非设计意图差异。修复后即与 UDP 表逐字一致。

### 2.5 修复

在 `""` 与 `"SET_SLAVE_IP_ADDRESS"` 之间补上逗号，恢复 6 项布局，使索引重新与规范码值对齐。修复后 TCP 表与 UDP 表完全相同：

```cpp
constexpr std::array<std::string_view, 6> kCmdList = {
  "GET_DAQ_CLOCK_MULTICAST",   // idx0 → 0xFA
  "",                          // idx1 → 0xFB 保留槽（未分配）
  "SET_SLAVE_IP_ADDRESS",      // idx2 → 0xFC
  "GET_SLAVE_ID_EXTENDED",     // idx3 → 0xFD
  "",                          // idx4 → 0xFE 保留槽（未分配）
  "GET_SLAVE_ID"               // idx5 → 0xFF
};
```

### 2.6 影响面与风险

- **影响**：任何解析含 TCP 可选子命令（`SET_SLAVE_IP_ADDRESS` / `GET_SLAVE_ID_EXTENDED` / `GET_SLAVE_ID`）的 A2L 的场景，都会向下游产出错误码值。若这些码值参与实际 XCP 请求构造或对端校验，会导致静默的功能错误。
- **回归风险极低**：改动仅是「让错误路径变正确」，不影响本就正确的首项；且与 UDP 侧行为对齐，一致性提升。
- **建议补充测试**：对四个 TCP 子命令逐一断言 `GetSubCmds()` 返回的码值等于规范值（当前上游若无此覆盖，应新增）。

---

## 三、问题 2：PACKET_ALIGNMENT「未声明」与「声明为 8bit」不可区分（严重度：中）

### 3.1 现象

`SetPacketAlignment(const std::string&)` 只在传入字符串精确匹配 `PACKET_ALIGNMENT_8/16/32` 三者之一时才赋值；不匹配时**静默保持成员默认值 `PACKET_ALIGNMENT_8 (=0)`**。

因此从外部看，以下三种情况在 `GetPacketAlignment()` 返回值上完全无法区分：

1. A2L 中**根本没有** `PACKET_ALIGNMENT` 关键字（未声明）；
2. A2L 中显式声明了 `PACKET_ALIGNMENT_8`；
3. A2L 中声明了一个**非法字面量**（被静默吞掉，落到默认 8）。

### 3.2 为什么这是缺陷而非可接受行为

「未声明」和「显式声明为 8」在协议语义上是两回事：前者表示对端未承诺对齐方式，后者表示明确选择了 8-bit 对齐。上层（本项目 libxcp 的 A2L 端点 ABI，批次 B-T4）需要据此做 fail-closed 判定与冲突比对，无法容忍二者混淆。同理，非法字面量本应被发现，却被静默降级为合法默认值，掩盖了解析错误。

### 3.3 修复

在 `XcpOnTcpIp` 与 `XcpOnUdpIp` 各新增一个私有布尔成员 `alignment_declared_` 与公开只读访问器 `IsAlignmentDeclared()`：

- `SetPacketAlignment()` 一旦被调用，**在做字面量匹配之前无条件置位** `alignment_declared_ = true`——因为该函数被调用本身就证明 A2L 中存在 `PACKET_ALIGNMENT` 关键字；
- `Reset()` 中将其复位为 `false`，与其余状态清理保持一致。

这样上层即可通过 `IsAlignmentDeclared()` 恢复「是否显式声明」这一事实，而不必依赖有歧义的 `GetPacketAlignment()` 数值。

### 3.4 API 兼容性

纯增量：新增只读方法与新私有成员，不改动任何既有方法签名与返回类型，不改变既有正确解析路径的行为。对现有调用者向后兼容。

---

## 四、变更清单（diff 摘要）

| 文件 | 变更 |
|---|---|
| `include/a2l/xcp/xcpontcpip.h` | 新增 `IsAlignmentDeclared()` 访问器 + `alignment_declared_` 成员（doxygen 中文注释） |
| `src/xcp/xcpontcpip.cpp` | `SetPacketAlignment` 无条件置位声明标志；`AddSubCmd` 补漏逗号 + 规范码表注释；`Reset` 复位标志 |
| `include/a2l/xcp/xcponudpip.h` | 同 TCP 头文件，新增访问器与成员 |
| `src/xcp/xcponudpip.cpp` | `SetPacketAlignment` 置位标志；`Reset` 复位（UDP 表本就正确，无逗号修复） |

---

## 五、验证建议

1. **单元级**：
   - TCP/UDP 各断言四个子命令码值 == `{0xFA, 0xFC, 0xFD, 0xFF}`（问题 1）。
   - 分别以「不调用 SetPacketAlignment」「调用传 PACKET_ALIGNMENT_8」「调用传非法串」三种输入，断言 `IsAlignmentDeclared()` == `{false, true, true}`（问题 2）。
2. **一致性**：断言 TCP 与 UDP 的 `kCmdList` 及派生码值完全相同。
3. **回归**：确认既有 A2L golden 测试仍通过（本子命令码值变化可能体现在依赖它生成的期望文件中，需一并核对）。

---

## 六、提交申请注意事项（重要）

- a2llib 是外部 fork（remote = `https://github.com/ihedvall/a2llib.git`）。若贵方无该仓库 push 权限，请将本分支推送到贵方可控的镜像/fork，并相应更新主仓 `.gitmodules` 中 a2llib 的 url，否则主仓记录的子模块指针将指向仅存在于本机的 commit，他人 `submodule update` 会失败。
- 推送顺序：**先推 a2llib 分支到其远端，再推主仓**，确保子模块 commit 在父仓引用前已在远端可达。
