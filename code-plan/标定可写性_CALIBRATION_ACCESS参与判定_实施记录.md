# 标定可写性：`CALIBRATION_ACCESS` 参与判定（方案 A）实施记录

- 日期：2026-10-23
- 触发：客户 A2L（`apa_complex.a2l`）里部分标定量被拒写；排查中发现根因之一是 `CALIBRATION_ACCESS` 被整条忽略，见 `code-plan/标定写入被拒_MEMORY_SEGMENT缺失分析与P2修复.md` §8。
- 用户批准：「批准方案」（方案 A：只改 a2l-sdk 判定，不动 a2llib、不动 ABI）。

---

## 0. 结论速览

| 项 | 结果 |
| --- | --- |
| 缺陷 | `read_write` 只看 `READ_ONLY`/`READ_WRITE`，忽略 V1.4 起取代它的 `CALIBRATION_ACCESS` |
| 修复 | `liba2l_export.cpp` 的 CHARACTERISTIC 与 INSTANCE 两条赋值都串上 `CalibrationAccessAllowsWrite()` |
| 行为变化面 | 仅「A2L 显式声明了 `CALIBRATION_ACCESS`」时；未声明（`UNKNOWN`）完全不变 |
| 兼容性 | XCPlite V1.2 / V2.2 生成器都不输出该关键字 → 现有演示流程逐位不变（已实测） |
| 测试 | 新增 `tests/a2l_calibration_access_test.cpp` 10 用例；`ctest -E "^Example"` **972/972 通过** |
| ABI | 不变（仍是 v9，未加字段）；未改 `thirdparty/a2llib` |

---

## 1. 规范依据

取自 `docs/ASAM_AE_MCD-2_MC_BS_V1-7-0.pdf`：

| 条款 | 内容 |
| --- | --- |
| §4.7.32 CHARACTERISTIC（p.73） | 原型只有 `[-> READ_ONLY]`，**没有** `READ_WRITE` |
| §4.7.85 INSTANCE（p.145） | 可选属性为 `[-> CALIBRATION_ACCESS]` + `[-> READ_WRITE]`，没有 `READ_ONLY` |
| §4.7.115 READ_WRITE（p.188） | 把测量对象标记为可写 |
| §4.7.28 CALIBRATION_ACCESS（p.69） | `NO_CALIBRATION` 不可改、只能读；`OFFLINE_CALIBRATION` 可刷写但不可标定；原文 **“It substitutes the READ_ONLY attribute since ASAM-MCD-2 MC V1.4”** |

即：**CHARACTERISTIC 缺关键字本来就是可写的**，所以 `!c.ReadOnly()` 的方向没错；错的是只认 `READ_ONLY`、不认取代它的 `CALIBRATION_ACCESS`。

---

## 2. 改动清单

### 2.1 `thirdparty/a2l-sdk/src/liba2l_export.cpp`（唯一产品代码改动）

新增判定函数（匿名命名空间内，`thirdparty/a2l-sdk/src/liba2l_export.cpp:745`，紧邻 `BuildCharacteristicSymbol`）：

```cpp
[[nodiscard]] bool CalibrationAccessAllowsWrite(
    a2l::A2lCalibrationAccess access) {
    switch (access) {
        case a2l::A2lCalibrationAccess::NO_CALIBRATION:      // 只能读
        case a2l::A2lCalibrationAccess::OFFLINE_CALIBRATION: // 不可在线标定
        case a2l::A2lCalibrationAccess::NOT_IN_MCD_SYSTEM:   // MCD 不可读写
            return false;
        case a2l::A2lCalibrationAccess::CALIBRATION:  // 沿用原判据
        case a2l::A2lCalibrationAccess::UNKNOWN:      // 未声明 → 沿用原判据（兼容）
        default:
            return true;
    }
}
```

两处赋值改为「原判据 且 访问级别未额外禁止」：

| 位置 | 修复前 | 修复后 |
| --- | --- | --- |
| `thirdparty/a2l-sdk/src/liba2l_export.cpp:841-842`（CHARACTERISTIC） | `s.read_write = !c.ReadOnly();` | `s.read_write = !c.ReadOnly() && CalibrationAccessAllowsWrite(c.CalibrationAccess());` |
| `thirdparty/a2l-sdk/src/liba2l_export.cpp:930-931`（INSTANCE） | `d.read_write = in.ReadWrite();` | `d.read_write = in.ReadWrite() && CalibrationAccessAllowsWrite(in.CalibrationAccess());` |

`a2l::Characteristic` 与 `a2l::Instance` 都继承 `a2l::A2lObject`，访问器在 `thirdparty/a2llib/include/a2l/a2lobject.h:36-40`，默认值 `UNKNOWN`（同文件 `:137`）——这是「未声明就不改行为」的支点。

### 2.2 `tests/a2l_calibration_access_test.cpp`（新增，10 用例）

内联一份极小 A2L 写到临时目录（`std::filesystem::temp_directory_path()/libxcp_calibration_access/`），逐条断言桥接层发布的 `read_write`：

| 用例 | 声明 | 期望 |
| --- | --- | --- |
| `CharacteristicWithoutKeywordStaysWritable` | 无关键字 | 可写 |
| `CharacteristicReadOnlyStaysReadOnly` | `READ_ONLY` | 不可写 |
| `CharacteristicExplicitCalibrationIsWritable` | `CALIBRATION_ACCESS CALIBRATION` | 可写 |
| `CharacteristicNoCalibrationIsRejected` | `CALIBRATION_ACCESS NO_CALIBRATION` | 不可写 |
| `CharacteristicOfflineCalibrationIsRejected` | `CALIBRATION_ACCESS OFFLINE_CALIBRATION` | 不可写 |
| `CharacteristicNotInMcdIsRejected` | `CALIBRATION_ACCESS NOT_IN_MCD_SYSTEM` | 不可写 |
| `InstanceWithReadWriteStaysWritable` | INSTANCE `READ_WRITE` | 可写 |
| `InstanceWithNoCalibrationIsRejected` | `READ_WRITE` + `NO_CALIBRATION` | 不可写 |
| `InstanceWithOfflineCalibrationIsRejected` | `READ_WRITE` + `OFFLINE_CALIBRATION` | 不可写 |
| `InstanceWithoutReadWriteIsRejected` | 未声明 `READ_WRITE` | 不可写 |

两条实测踩坑（已固化在文件注释里）：

1. **断言目标必须是叶子**（`m::i_plain.v`）而不是 INSTANCE 本身。INSTANCE 在快照里是「类型节点」（`kind=Type`、`size=0`、`read_write` 恒 false），数据由展开后的叶子承载。首版断言容器节点导致 1 个假失败。
2. 加载选项必须 `require_if_data_xcp = false`：语料刻意不带 `IF_DATA XCP`，否则报「A2L 中未找到 IF_DATA XCP 段」。

### 2.3 `tests/CMakeLists.txt`

新增目标 `A2lCalibrationAccessTest`（放在 `A2lLogTimestampTest` 之前）：链接 `libxcp::a2lbridge` + `GTest::gtest` + `libxcp_test_main`，`POST_BUILD` 复制 `liba2l`，`gtest_discover_tests(... DISCOVERY_TIMEOUT 120)`。不依赖 golden 夹具。

---

## 3. 验证

### 3.1 新增用例

```
cd cmake-build-release/tests && ./A2lCalibrationAccessTest
[==========] 10 tests from 1 test suite ran.
[  PASSED  ] 10 tests.
```

### 3.2 全量回归（macOS Release）

```
cmake --build cmake-build-release -j 4            # 成功
ctest -E "^Example" -j 4                          # 972 项 / 0 失败 / 2 跳过（跳过为既有）
ctest -R "^Example" -j 1                          # 13/14，唯一失败 ExampleXcpIntegrationDemoCppDemo（既有）
```

> `Example` 系必须串行：并行时多个用例抢默认端口 5555，会产生「端口已被占用」的假失败。

### 3.3 兼容性实测（本轮重点）

用临时探针 `.tmp/probe/dump_rw.cpp`（未入库）对比修复后的 `read_write`：

| 样本 | 修复后 |
| --- | --- |
| `.tmp/v12a2l/xcp_slave_demo_v12.a2l` 的 `cal.gain/offset/limit/run/corrections` | 全 `rw=1`（与修复前一致） |
| 同上 `fast.counter` / `slow.basic.u8` | `rw=1`（与修复前一致） |
| `.tmp/v22a2l/xcp_slave_demo.a2l` 的 `cal.corrections` / `meas_slow.basic.*` | 全 `rw=1`（与修复前一致） |
| 客户 `~/Downloads/apa_complex.a2l` 的 `algo_status_info.apa_fusion_status.period_switch` | `rw=1`（与修复前一致） |

源级依据（为什么必然一致）：`thirdparty/XCPliteV1.2/src/a2l.c` 全文 `CALIBRATION_ACCESS` 出现 **0** 次；`READ_WRITE` 只在 `:1256`（INSTANCE）与 `:1295`（MEASUREMENT）输出，且仅在绝对/动态寻址模式下。V2.2（`thirdparty/XCPlite/src/*.c`）同样为 0。两份生成物实测 `CALIBRATION_ACCESS=0`。

---

## 4. 未做项

| 项 | 原因 |
| --- | --- |
| 方案 B（把 `CALIBRATION_ACCESS` 透到 `SymbolInfo`，由 `tools/xcp_master/symbol_policy.hpp` 给出更准的拒绝文案） | 需要 ABI v10，收益仅是文案；本次未批准 |
| `NOT_IN_MCD_SYSTEM` 的语义细分（是「不可读」还是「不可写」） | 规范只写 MCD 系统不可读写，本项目按不可写处理；若将来要区分读/写需另立条目 |
| MEASUREMENT 的 `CALIBRATION_ACCESS` | 规范 §4.7.52 里 MEASUREMENT 的可写性只由 `READ_WRITE` 表达，不接 `CALIBRATION_ACCESS`，故 `:700` / `:978` 两处未改 |
| 客户 A2L 的端到端写入复测 | 需要现场 ECU（`192.168.2.137:5559`）在位；本次只做静态加载与既有探针核对 |

### 4.1 复核结论：`a2llib` 的 `read_only` 缺省值**不是缺陷，明确不改**

早期分析里曾把 `thirdparty/a2llib/src/a2lparser.y:679`（只在遇到 `READ_ONLY` 时置只读、缺省可写）记为缺陷，理由是「ASAP2 缺省是 READ_ONLY」。**该理由错误，已在本文与
`code-plan/标定写入被拒_MEMORY_SEGMENT缺失分析与P2修复.md` §4 更正。**

规范原文（`docs/ASAM_AE_MCD-2_MC_BS_V1-7-0.pdf`）：

- §4.7.32 CHARACTERISTIC（p.73）：可选属性含 `[-> CALIBRATION_ACCESS]`、`[-> READ_ONLY]`，**没有 `READ_WRITE`**；
- §4.7.114 READ_ONLY（p.187）：“indicate that an **adjustable object** cannot be changed” —— 是给「本来可改的对象」打的例外标记；
- §4.7.115 READ_WRITE（p.188）：“mark a **measurement** object to be writeable”；
- §4.7.85 INSTANCE（p.145）：可选属性为 `[-> CALIBRATION_ACCESS]` + `[-> READ_WRITE]`，**没有 `READ_ONLY`**。

三类对象的缺省方向相反，本仓解析器三条路径各自正确：

| 对象 | 缺省 | 改判关键字 | 解析器实现 |
| --- | --- | --- | --- |
| CHARACTERISTIC | **可写** | `READ_ONLY` / `CALIBRATION_ACCESS`（限制值） | `thirdparty/a2llib/src/a2lparser.y:679` + `a2lobject.h:150`（`read_only_ = false`）✅ |
| INSTANCE | 不可写 | `READ_WRITE` | `thirdparty/a2llib/src/a2lparser.y:876` + `instance.h`（`read_write_ = false`）✅ |
| MEASUREMENT | 不可写 | `READ_WRITE` | `thirdparty/a2llib/src/a2lparser.y:921` ✅ |

若改成「缺省只读」的实测影响面：`~/Downloads/apa_complex.a2l` 里 `READ_ONLY` 出现 **0** 次、2,558 个 CHARACTERISTIC 全都没有访问关键字 → 全部变不可写；本仓两份 demo A2L（`READ_ONLY=0`、`CALIBRATION_ACCESS=0`）的 `cal.*` 同样全部失效。

**用户裁决（2026-10-24）**：保持现状，不改。本条目留档，避免后续再次被误当作缺陷修改。

---

## 5. 回滚

```bash
git checkout -- thirdparty/a2l-sdk/src/liba2l_export.cpp tests/CMakeLists.txt
rm -f tests/a2l_calibration_access_test.cpp
```

（`thirdparty/a2llib` 本次未改，无需回滚。）