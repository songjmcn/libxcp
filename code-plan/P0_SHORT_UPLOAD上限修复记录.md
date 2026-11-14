# P0：SHORT_UPLOAD / UPLOAD 单包上限 off-by-one 修复记录

- 批次：P0（`code-plan/Poll模式多Event架构实施计划.md` §9 第一批）
- 目的：修复 `MemoryAccess` 单包读取上限比规范多算 1 个元素的缺陷，为后续多 event 轮询架构（P1–P4）提供正确的基础读取通路
- 结论：代码与测试已完成，全量回归通过，未引入新告警

---

## 1. 缺陷说明

### 1.1 规范依据

原始 PDF `docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.pdf`（Markdown 转换版对应
`docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.md:3636-3650`）§7.5.1.13 Table 72：

```
SHORT_UPLOAD 的 NumberOfElements 取值范围是 [1 .. MAX_CTO/AG - 1]
```

§7.5.1.12 Table 70/71 的 UPLOAD 同样是 `[1 .. MAX_CTO/AG - 1]`。

理由是两者的正响应结构都是 `[PID][n×AG]`：PID（0xFF）固定占 1 字节，因此
`n×AG` 必须 ≤ `MAX_CTO - 1`，即 `n ≤ MAX_CTO/AG - 1`。

### 1.2 修复前的错误行为

`src/memory_access.cpp` 修复前：

```cpp
ElementCount MemoryAccess::MaxShortUploadElements() const {
    // ...
    return max_cto / ag;                      // 多算了 1，应为 max_cto/ag - 1
}

ElementCount MemoryAccess::MaxUploadElements() const {
    const auto limit = MaxShortUploadElements();   // 派生依赖
    return limit == 0U ? 0U : limit - 1U;
}
```

- `MAX_CTO=8, AG=Byte` 时，上限被算成 8，于是读 8 字节变量（例如 `double`、
  `int64_t`）会走 SHORT_UPLOAD 并请求 `n=8`；从机响应需要 `1+8=9` 字节，
  超过 `MAX_CTO=8`，只能回 `ERR_OUT_OF_RANGE`。
- `ReadElements` 的降级条件只覆盖 `ERR_CMD_UNKNOWN` + `CommandCode::ShortUpload`
  （`src/memory_access.cpp:135-155`），`ERR_OUT_OF_RANGE` 不满足降级条件，
  异常直接上抛给调用方。
- 连带影响：`MaxUploadElements()` 若继续按 `MaxShortUploadElements()-1` 派生，
  修复前者会让后者变成 `MAX_CTO/AG - 2`，把分块传输的上限也压小一格。

### 1.3 既有测试为何没抓到

`tests/memory_access_test.cpp:558-572` 原断言用 `n = MAX_CTO/AG` 视为成功，
而 mock slave 不校验响应长度是否超出 MAX_CTO，因此错误上限被"测成"正常行为。

---

## 2. 代码变更

| 文件 | 位置 | 变更 |
| --- | --- | --- |
| `src/memory_access.cpp` | `:21-35` `MaxShortUploadElements()` | 改为 `limit = max_cto/ag; return limit==0 ? 0 : limit-1;`，新增中文注释说明 §7.5.1.13、PID 占 1 字节、以及错误上限为何会直接上抛 |
| `src/memory_access.cpp` | `:37-53` `MaxUploadElements()` | 改为**独立计算**同一公式（不再复用 `MaxShortUploadElements()`），注释说明两者数值相同但协议依据不同，刻意不互相派生 |
| `include/libxcp/memory_access.hpp` | `:85-96` 类头 `@par 长度上限` | 依据改为"原始 PDF §7.5.1.12 / §7.5.1.13"，SHORT_UPLOAD 上限改为 `MAX_CTO/AG - 1`，新增 `@note` 说明两式数值相同但依据不同、实现中独立计算 |
| `include/libxcp/memory_access.hpp` | `:267-274` 私有声明 | `MaxUploadElements` 注释改"为 PID 留出 1 字节"；`MaxShortUploadElements` 改"= MAX_CTO/AG - 1，原始 PDF §7.5.1.13"并加 `@note` 禁止互相派生 |
| `tests/memory_access_test.cpp` | `:518-525` `AgCase` | 参数改为 `{Byte,8,7,7}/{Word,8,3,3}/{DWord,8,1,1}/{Byte,64,63,63}/{Word,16,7,7}/{DWord,8,1,1}`（`short_upload_max` 与 `upload_max` 相等） |
| `tests/memory_access_test.cpp` | `:594-661` | 新增套件 `MemoryAccessShortUploadLimit` 的 4 个用例（见 §3） |
| `tests/xcp_master_integration_test.cpp` | `:849` | 注释同步为"MAX_CTO/AG-1(=7)" |

---

## 3. 新增测试

套件 `MemoryAccessShortUploadLimit`（`tests/memory_access_test.cpp:602-661`）：

| 用例 | 场景 | 断言要点 |
| --- | --- | --- |
| `ExactMaxCtoElementsAvoidsShortUpload` | `MAX_CTO=8, AG=Byte` 读 8 字节 | 读回内容正确；`ShortUpload==0`；`Upload>0` |
| `MaxMinusOneStillUsesShortUpload` | `MAX_CTO=8, AG=Byte` 读 7 字节 | 仍走单包：`ShortUpload==1` 且 `SetMta==0` |
| `ExplicitShortUploadRejectsExactMaxCto` | 显式 `ShortUpload(addr, ext, 8)` | 抛 `InvalidArgument`，且**一帧都不下发**（`ShortUpload==0`） |
| `UploadChunkLimitNotDerivedFromShortUpload` | `MAX_CTO=16, AG=DWord` 读 7 元素 | `SetMta==3`、`Upload==3`（3+3+1）；若上限仍派生会退化为 4 块 |

其中第 4 例专门守护 §1.2 提到的"派生连带"风险：只改一个函数就会立刻失败。

---

## 4. 设计决策

1. **两处上限各自独立计算**：数值相同但协议依据不同（UPLOAD 与 SHORT_UPLOAD 各有
   自己的 CTO 表），复用一个会让未来调整其中一条时误伤另一条。
2. **失败时"拒绝并报错"而非"自动降级"**：`ReadElements` 仍只在
   `ERR_CMD_UNKNOWN + ShortUpload` 时回退 `UploadChunked`。修复后实际不再产生
   `n=MAX_CTO/AG` 的请求，无需扩大降级条件；扩大降级会把真正的协议错误静默吞掉。
3. **显式入口同样收口**：`ShortUpload` 是公开 API，直接调用时也按新上限校验，
   避免绕过 `ReadElements` 触发同样的越界请求。
4. **不改 `CanUseShortUpload` 的语义**：它只比较 `element_count <= 上限`，
   上限修正后自然生效（`src/memory_access.cpp:82`）。

---

## 5. 实测结果

### 5.1 环境与构建

- 分支 `feate_daq_all`；Release + Ninja + MSVC 19.44。
- 构建方式：`.tmp-build-tests.bat`（`call vcvars64.bat` → `cmake --build cmake-build-release -j 4 --target libxcp_tests`），
  以 `MSYS_NO_PATHCONV=1 cmd /c .tmp-build-tests.bat` 调用。
- 构建结果：`EXIT=0`，64/64 目标完成链接。
- `p0_build.log` 中 49 条告警全部来自第三方（glog `C4244`/`C4996`），本仓库源码 0 告警。

### 5.2 专项测试

```
./cmake-build-release/tests/libxcp_tests.exe --gtest_filter='MemoryAccessShortUploadLimit.*:AgMatrix/MemoryAccessAgTest.*'
[  PASSED  ] 28 tests.      （4 个新用例 + 24 个参数化用例）
```

注：`MemoryAccessAgTest` 的实例前缀是 `AgMatrix/`，用 `--gtest_filter='MemoryAccess*'`
抓不到参数化实例。

### 5.3 全量回归

```
./cmake-build-release/tests/libxcp_tests.exe
[==========] 978 tests from 166 test suites ran. (46062 ms total)
[  PASSED  ] 977 tests.
[  SKIPPED ] 1 test: AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8
EXIT=0
```

- 合计 978 例：977 通过 / 1 跳过 / 0 失败。
- 唯一跳过项 `ByteApiRequiresMultipleOfAg/Ag1_Cto8` 在本批次之前即被跳过，与本次改动无关。
- 本批次新增 4 例（`MemoryAccessShortUploadLimit` 套件）。
- **计数口径核对（遗留项）**：`code-plan/B4_Packed模式实施记录.md` 记录 B4 为 27 例 /
  全量 966 例，但当前代码中 `DaqPackedMode*` 实为 **34** 例
  （Codec 6 / Parser 6 / Master 7 / Planner 7 / Session 8），即 B4 记录写于中途版本，
  之后又补充了 7 例（Planner +4、Session +3）而记录未同步。因此 966 是 B4 **中期**
  数字，不是收尾基线；B4 记录已在本批次一并校正（见该文件 §5 与 §6）。

### 5.4 行尾与 diff 卫生

用 Python 改写文件时曾把两个原本 LF 的文件整篇写成 CRLF，导致 `git diff --stat`
出现 967/895 的假改动。已按字节把 `src/memory_access.cpp`、`tests/memory_access_test.cpp`
恢复为 LF。

当前真实改动规模（`git diff --ignore-cr-at-eol --stat` 与 `--stat` 一致）：

```
 include/libxcp/memory_access.hpp      | 12 +++--
 src/memory_access.cpp                 | 22 +++++++--
 tests/memory_access_test.cpp          | 86 ++++++++++++++++++++++++++++++++---
 tests/xcp_master_integration_test.cpp |  2 +-
 4 files changed, 107 insertions(+), 15 deletions(-)
```

---

## 6. 边界与遗留

1. **只影响上限判定，不改变往返次数**：`n ≤ 上限` 仍优先 SHORT_UPLOAD（1 次往返），
   超限走 `UploadChunked`（每块 SET_MTA+UPLOAD，2 次往返）。
2. **`MAX_CTO` 很小（如 8）时大数据只能分块**：这正是 P1+ 轮询架构要用"地址合并"
   降低往返次数的原因。
3. **未改动 DOWNLOAD 侧**：本次只修读取路径；写入侧 `MaxDownloadElements()` 的
   `MAX_CTO - 2 - 对齐` 公式依据不同（CTO 自身含命令码与 n），不在本批次范围。
4. **未新增对从机响应长度的运行期校验**：上限修正后不再产生越界请求，额外的
   响应长度校验属传输层/解析层职责，已有 `MalformedPacket` 路径覆盖。
5. **P1–P4 未开始**：poll 多 event 架构（`include/libxcp/poll/`、`PollSession`、
   调度线程、UDP/TCP 验收）仍是待实施项。

---

## 7. 自审

- [x] 规范依据给出可核对位置（§7.5.1.12 / §7.5.1.13 Table 70/71/72）并用 PID 占位解释公式来源。
- [x] 两个上限函数独立计算，附注释说明"为何不派生"，并有专门用例守护。
- [x] 显式 `ShortUpload` 入口同样收口，且断言"一帧不发"。
- [x] 参数化用例的期望值随公式同步更新，未保留旧错误期望。
- [x] 专项 28/28、全量 978 例 0 失败，数字与命令可复现。
- [x] 未引入新告警；第三方告警已区分为既有噪声。
- [x] 行尾恢复为 LF，diff 规模与内容改动一致（107/15）。
- [x] 未重构、未新增公开接口（除既有 `ShortUpload` 校验行为），符合"针对已完成代码不重构"约束。
- [x] 未触碰 `thirdparty/`。