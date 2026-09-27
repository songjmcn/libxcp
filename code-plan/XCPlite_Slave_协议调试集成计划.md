# XCPlite Slave 协议调试集成计划（第一阶段 + 第二阶段）

## 0. 概述

将 `thirdparty/XCPlite` 作为真实 XCP Slave 对手端，与 libxcp Master 通过真实 UDP Socket 进行协议调试。分两阶段交付：

- **第一阶段**：XCP 基本协议走通 + A2L 加载 + 读取 ECU 端变量（基本类型、结构体、数组、嵌套类型）
- **第二阶段**：修改 ECU 端变量（WriteMemoryBytes 闭环验证）

## 1. 架构设计

### 1.1 整体拓扑

```text
┌─────────────────────┐     真实 UDP      ┌──────────────────────────┐
│  libxcp_tests       │ ◄═══════════════► │  xcp_test_slave          │
│  (GTest + XcpMaster)│   127.0.0.1      │  (XCPlite Slave 独立进程) │
│                     │   :5556           │                           │
│  - 启动子进程        │                   │  - XcpInit + EthServer   │
│  - 等待 Slave Ready │                   │  - A2lInit (运行时生成)   │
│  - Connect/Read/    │                   │  - 定义测试变量           │
│    Disconnect       │                   │  - 主循环 DaqTrigger     │
│  - 加载 A2L         │                   │  - SIGINT 优雅退出       │
└─────────────────────┘                    └──────────────────────────┘
```

### 1.2 构建集成

在主 `CMakeLists.txt` 中通过 `add_subdirectory(thirdparty/XCPlite)` 引入 XCPlite，配置为：

- `XCPLITE_CONFIGURATION=default`（支持 A2L 生成 + UDP）
- `XCPLITE_BUILD_EXAMPLES=OFF`
- `XCPLITE_BUILD_TESTS=OFF`
- `XCPLITE_INSTALL=OFF`

新增 CMake option：`LIBXCP_BUILD_XCPLITE_SLAVE`（默认 OFF），开启时：

1. `add_subdirectory(thirdparty/XCPlite)` 引入 xcplite 库
2. 构建 `xcp_test_slave` 可执行文件（位于 `tests/xcp_test_slave/`）
3. 构建 `xcplite_integration_tests` 测试目标

### 1.3 测试 Slave 应用程序设计

新建 `tests/xcp_test_slave/main.cpp`，定义以下测试变量：

```cpp
// === 基本类型变量 ===
uint8_t   g_basic_u8   = 0x42;
int16_t   g_basic_i16  = -1000;
uint32_t  g_basic_u32  = 0xDEADBEEF;
float     g_basic_f32  = 3.14f;
double    g_basic_f64  = 2.71828;

// === 结构体变量 ===
typedef struct {
    uint8_t  field_a;
    int16_t  field_b;
    uint32_t field_c;
} SimpleStruct_t;
SimpleStruct_t g_simple_struct = { .field_a = 10, .field_b = -200, .field_c = 0x12345678 };

// === 数组变量 ===
uint8_t  g_array_u8[8]   = {0,1,2,3,4,5,6,7};
int16_t  g_array_i16[4]  = {-10, -20, -30, -40};
float    g_array_f32[4]  = {1.0f, 2.0f, 3.0f, 4.0f};

// === 嵌套类型：结构体嵌套结构体 ===
typedef struct {
    uint8_t       inner_val;
    SimpleStruct_t nested_struct;
} OuterStruct_t;
OuterStruct_t g_outer = { .inner_val = 99, .nested_struct = {5, -50, 0xABCD} };

// === 嵌套类型：数组嵌套结构体 ===
SimpleStruct_t g_struct_array[3] = {
    {1, 100, 0x1111},
    {2, 200, 0x2222},
    {3, 300, 0x3333}
};

// === Calibration 参数（用于第二阶段写回测试）===
typedef struct {
    uint16_t cal_factor;
    float    cal_offset;
} CalParams_t;
const CalParams_t kDefaultCalParams = { .cal_factor = 100, .cal_offset = 0.5f };
```

A2L 注册使用 XCPlite 的 `A2lTypedefBegin/End`、`A2lCreateMeasurement`、`A2lCreateTypedefInstance`、`A2lCreateTypedefInstanceArray` 等宏，参照 `struct_demo` 的模式。

Slave 程序行为：

- 绑定 UDP 端口 5556（可通过命令行参数覆盖）
- 初始化 XCP + A2L 生成器（`A2L_MODE_WRITE_ONCE | A2L_MODE_FINALIZE_ON_CONNECT`）
- 主循环触发 DAQ Event（保持 Slave 活跃）
- 收到 SIGINT/SIGTERM 时优雅关闭
- A2L 输出到工作目录下的 `xcp_test_slave.a2l`

## 2. 第一阶段实施分解

### Phase1-01: CMake 构建集成

| 步骤 | 内容 | 产出 |
|------|------|------|
| 01-A | 主 CMakeLists.txt 增加 `LIBXCP_BUILD_XCPLITE_SLAVE` option 和条件 `add_subdirectory(thirdparty/XCPlite)` | 构建系统可编译 xcplite 库 |
| 01-B | 新建 `tests/xcp_test_slave/CMakeLists.txt`，定义 `xcp_test_slave` 可执行目标，链接 `xcplite::xcplite` | Slave 测试程序可编译 |
| 01-C | `tests/CMakeLists.txt` 增加 `xcplite_integration_tests` 测试目标（仅 `LIBXCP_BUILD_XCPLITE_SLAVE=ON` 时） | 集成测试可编译 |
| 01-D | 验证 Release 构建通过：`cmake -B cmake-build-release -S . -DLIBXCP_BUILD_XCPLITE_SLAVE=ON` | 编译无错误 |

### Phase1-02: XCPlite 测试 Slave 程序

| 步骤 | 内容 | 产出 |
|------|------|------|
| 02-A | 新建 `tests/xcp_test_slave/main.cpp`，实现上述变量定义 + XCP/A2L 初始化 + 主循环 | Slave 源码 |
| 02-B | 手动运行 Slave，确认 UDP 5556 端口可连接、A2L 文件正确生成 | 功能验证 |
| 02-C | 用 CANape 或 xcpclient 验证 Slave 可被标准工具识别（可选，非阻塞） | 互操作性旁证 |

### Phase1-03: 测试基础设施 —— Slave 进程管理器

| 步骤 | 内容 | 产出 |
|------|------|------|
| 03-A | 新建 `tests/xcplite_slave_fixture.hpp/cpp`：封装子进程启动/停止/等待就绪/A2L路径获取 | 测试夹具类 `XcpliteSlaveFixture` |
| 03-B | 实现端口探测逻辑：Slave 启动后轮询 UDP CONNECT 直到成功（带超时），避免固定 sleep | 可靠的就绪检测 |
| 03-C | 实现 GTest Fixture 的 SetUp/TearDown：SetUp 启动 Slave + 等待就绪；TearDown 发 SIGINT + waitpid | 自动化生命周期管理 |

### Phase1-04: XCP 基本协议走通测试

新建 `tests/xcplite_protocol_test.cpp`：

| 用例 | 验证内容 |
|------|----------|
| `ConnectDisconnect` | CONNECT → GET_STATUS → DISCONNECT 完整会话 |
| `SessionParametersMatch` | CONNECT RES 的 MAX_CTO/MAX_DTO/AG/BO 与预期一致 |
| `ReadBasicU8` | SHORT_UPLOAD 读 g_basic_u8 == 0x42 |
| `ReadBasicI16` | SHORT_UPLOAD 读 g_basic_i16 == -1000 |
| `ReadBasicU32` | SHORT_UPLOAD 读 g_basic_u32 == 0xDEADBEEF |
| `ReadBasicF32` | SHORT_UPLOAD 读 g_basic_f32，按 IEEE754 比对 |
| `ReadBasicF64` | SHORT_UPLOAD 读 g_basic_f64，按 IEEE754 比对 |
| `MultiChunkUpload` | 读取超过 MAX_CTO 的数据块，验证多块 UPLOAD 拼接 |
| `RepeatedSessions` | 连续 5 次 Connect/Read/Disconnect 稳定性 |

### Phase1-05: A2L 加载 + 变量读取测试

新建 `tests/xcplite_a2l_read_test.cpp`：

| 用例 | 验证内容 |
|------|----------|
| `LoadRuntimeGeneratedA2l` | 加载 Slave 运行时生成的 A2L 文件，无 Error |
| `FindBasicVariables` | Find("g_basic_u8"/"g_basic_i16"/...) 返回有效 SymbolInfo |
| `ReadBasicViaA2l` | A2L 符号地址 → ReadMemoryBytes → ToPhysical 闭环 |
| `ReadStructMembers` | 查找 g_simple_struct 的成员（field_a/field_b/field_c），逐个读取并验证值 |
| `ReadArrayElements` | 查找 g_array_u8，按索引读取各元素；验证 ComputeElementAddress |
| `ReadNestedStruct` | 查找 g_outer.nested_struct.field_c 等嵌套路径，读取并验证 |
| `ReadStructArray` | 查找 g_struct_array[1].field_b 等数组+结构体组合路径，读取并验证 |
| `A2lRuntimeConsistency` | CompareWithRuntime 无 Error（容量 Warning 可接受） |

> **注意**：结构体/嵌套类型的 A2L 读取能力依赖批次16 STRUCTLEAF 的实现状态。若批次16尚未完成，Phase1-05 中的结构体/嵌套用例先标记为 `GTEST_SKIP`，待批次16完成后启用。基本类型和数组的读取不依赖批次16。

### Phase1-06: 预置 A2L 回归测试

| 步骤 | 内容 |
|------|------|
| 06-A | 首次运行 Slave 后将生成的 A2L 复制到 `tests/data/a2l/xcplite_test_slave.a2l` 作为基线 |
| 06-B | 新增回归测试用例加载该预置 A2L，验证解析结果与运行时生成版本一致 |
| 06-C | 后续 Slave 变量变更时同步更新基线文件 |

## 3. 第二阶段实施分解

### Phase2-01: 写回基本变量

新建 `tests/xcplite_write_test.cpp`：

| 用例 | 验证内容 |
|------|----------|
| `WriteBasicU8` | WriteMemoryBytes(g_basic_u8, 0xFF) → ReadBack == 0xFF |
| `WriteBasicU32` | WriteMemoryBytes(g_basic_u32, 0xCAFEBABE) → ReadBack 验证 |
| `WriteFloat` | FromPhysical(2.718) → Write → Read → ToPhysical ≈ 2.718 |
| `WriteProtectedRequiresUnlock` | 未解锁写 CAL 参数 → ERR_ACCESS_LOCKED |

### Phase2-02: 写回结构体成员与数组元素

| 用例 | 验证内容 |
|------|----------|
| `WriteStructMember` | 写 g_simple_struct.field_b → 读回验证 |
| `WriteArrayElement` | 写 g_array_i16[2] → 读回验证 |
| `WriteNestedMember` | 写 g_outer.nested_struct.field_a → 读回验证 |
| `WriteCalParam` | Unlock(CAL/PAG) → 写 CalParams_t.cal_factor → 读回验证 |

### Phase2-03: Seed&Key 解锁 + 写回闭环

| 用例 | 验证内容 |
|------|----------|
| `UnlockThenWriteCalSeg` | 完整 Seed&Key → Unlock → Write → Read 闭环 |
| `WrongKeyRejected` | 错误 Key → Slave 断开 → Session Failed |

> **注意**：Phase2 的写回测试需要 XCPlite Slave 端开启 Calibration Segment 支持（`OPTION_CAL_SEGMENTS`），且测试 Slave 程序中需将 CalParams_t 注册为 CalSeg。这需要在 Phase1-02 中预留。

## 4. 关键技术约束与风险

| 风险 | 缓解措施 |
|------|----------|
| XCPlite Windows 下原子操作模拟导致性能问题 | 仅用于测试，不影响功能正确性；CLAUDE.md 已记录此限制 |
| XCPlite 默认 TCP，需确认 UDP 模式可用 | `XcpEthServerInit(addr, port, false/*UDP*/, queue_size)`；struct_demo 已有 UDP 示例 |
| 端口 5556 被占用 | Slave 支持命令行指定端口；Fixture 尝试多个端口或使用 OS 分配 + 反向发现 |
| A2L 运行时生成路径不确定 | Slave 将 A2L 写到当前工作目录；Fixture 设置明确的工作目录并传递路径 |
| 批次16 STRUCTLEAF 未完成 | Phase1-05 结构体/嵌套用例条件跳过；基本类型+数组不受影响 |
| XCPlite 作为 thirdparty 不可修改 | AGENTS.md 约束遵守；所有适配在 tests/ 侧完成 |
| 子进程管理跨平台差异 | Windows 用 CreateProcess/TerminateProcess；Linux/macOS 用 fork/exec/sigkill；Fixture 封装平台差异 |

## 5. 不做的事项

- 不修改 `thirdparty/XCPlite` 的任何源码
- 不实现 DAQ DTO 实时采集测试（属后续里程碑）
- 不实现 STIM 方向测试
- 不修改现有 UdpTestSlave 相关测试（保持既有测试不变）
- 不在本计划中处理批次16 STRUCTLEAF 的实现

## 6. 交付物

- `code-plan/XCPlite_Slave_协议调试集成计划.md`（本文件）
- `tests/xcp_test_slave/main.cpp` + `CMakeLists.txt`
- `tests/xcplite_slave_fixture.hpp/cpp`
- `tests/xcplite_protocol_test.cpp`
- `tests/xcplite_a2l_read_test.cpp`
- `tests/xcplite_write_test.cpp`（第二阶段）
- `tests/data/a2l/xcplite_test_slave.a2l`（预置基线）
- 实施记录 markdown（每次修改后按 AGENTS.md 要求输出）

## 7. 验收标准

### 第一阶段 DoD

1. `cmake -B cmake-build-release -S . -DLIBXCP_BUILD_XCPLITE_SLAVE=ON && cmake --build cmake-build-release` 编译通过
2. `ctest --test-dir cmake-build-release -R XcpliteProtocol` 全部 PASS
3. `ctest --test-dir cmake-build-release -R XcpliteA2lRead` 中基本类型+数组用例全部 PASS
4. 结构体/嵌套用例根据批次16状态要么 PASS 要么 SKIP（不 FAIL）
5. 既有 `libxcp_tests` 全量测试不受影响（`LIBXCP_BUILD_XCPLITE_SLAVE=OFF` 时构建不变）
6. 实施记录已写入 `code-plan/`

### 第二阶段 DoD

1. `ctest --test-dir cmake-build-release -R XcpliteWrite` 全部 PASS
2. Seed&Key + 写回闭环验证通过
3. 实施记录已写入 `code-plan/`

## 8. 变更记录

| 轮次 | 日期 | 内容 |
|------|------|------|
| P1 | 2025 | 初始版本：确定独立进程+真实UDP架构、add_subdirectory构建集成、专用测试Slave程序、两阶段分解 |
