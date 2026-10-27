# TCP/UDP 平台隔离重构——基于现有实现的修订计划

- 版本：v3，2026-10-27；本版替代 2026-10-26 的 v1/v2 **尚未执行的重构步骤**。历史 CI 排障和用户已裁决事项保留在本节及末节，不把历史行号/统计当成当前事实。
- 状态：**评审完成，修订方案待用户批准；未实施本计划中的重构**。
- 范围：`src/socket_util.hpp/.cpp`、`src/event_poller.hpp/.cpp`、`src/tcp_transport.cpp`、`src/udp_transport.cpp`、`CMakeLists.txt`、相关 `tests/`、三平台 workflow、隔离检查脚本及必要文档。
- 不改：`include/libxcp/**` 的公共接口/ABI、framing/CTR、`thirdparty/**`；只支持 Windows/Linux/macOS 的现有 IPv4 能力。

## 1. 评审结论与证据（当前工作树）

**原计划的方向正确，但不能照 v2 直接实施。** 已完成的 A1/A2 改造使几个前提失效，原验收门甚至无法通过；先修正接口与范围，再申请实施批准。

| 优先级 | 原计划的问题 | 当前代码证据 | 修订决定 |
| --- | --- | --- | --- |
| 阻断 | 原 §4.2、P1 拟新增 `SocketOps::wait_readable_multi`，与已有能力重叠 | `src/event_poller.hpp:33-75,98-106` 已有 `WaitItem`、`WaitStatus`、`EventPoller::Wait`、`WaitAnySocket`；`src/udp_transport.cpp:687-748` 已用于数据/唤醒双句柄 | **不新增**第二套多句柄等待；继续用 `EventPoller` 与既有注入点 |
| 阻断 | 原 G2/P4 声称除 `socket_ops_win.cpp` 外 `src/` 无 `_WIN32`，按计划拆完仍失败 | `src/event_poller.cpp:20-33,38-81,85-91,108-188` 有 Windows 分支；`src/event_poller.hpp:29` 间接依赖含平台头的 `src/socket_util.hpp:27-37` | **纳入 EventPoller 平台实现拆分**；所有非平台实现文件一并检查 |
| 高 | 原 §1 把 `SocketHandle` 与 UDP 双句柄等待列为未完成，统计“29 处/约 −250 行”不再有效 | `src/socket_util.hpp:41-52` 已使用 `std::intptr_t`/`-1`；`src/udp_transport.cpp:45-49,687-748` 已复用句柄别名与 `WaitAnySocket` | 保留已完成成果；删除旧数量、行号与减少行数的承诺 |
| 高 | 单纯把 `sockaddr` 变成 POD 仍会在传输文件泄漏 `AF_INET`、`SOCK_STREAM`、`SOL_SOCKET`、`TCP_NODELAY` 等平台常量 | `src/tcp_transport.cpp:239,303-368,378-385,391-418,508-511`；`src/udp_transport.cpp:432-505` | 创建种类、socket 选项改为内部语义枚举/专用操作；原生常量只留平台实现 |
| 高 | 原方案只拆 `socket_util.cpp` 的 WSA 生命周期，但目前 EventPoller 另有 Winsock 守卫 | `src/socket_util.cpp:56-93,150-159`；`src/event_poller.cpp:40-79,185-187`；`src/udp_transport.cpp:108-137,198-225` | 平台实现需验证 WSAStartup/WSACleanup 配平与初始化失败；不得宣称已有唯一守卫；有必要再共用内部守卫 |
| 高 | 原 T4 写成 UDP 注入的“接收失败被报告”，没有说明保持既有错误语义 | `src/udp_transport.cpp:721-734,790-805`：等待错误警告/重试；recvfrom 超时重试、失效退出、其余警告/重试 | 用例分别覆盖错误文本、分支及继续/退出；不把原有“警告后继续”暗改成关闭 |
| 中 | 原 §4.2 计划把 `NameResolver` 改为 `std::function` 并调整 `getaddrinfo`/`freeaddrinfo`，未核实使用者 | `src/socket_util.hpp:94-99,227-242`、`src/socket_util.cpp:334-335,392-396`；对 `src/*.cpp` 的检索只发现定义，未发现传输调用 | 不凭空设计 DNS 功能或强加 `std::function`；先检查其真实使用/测试，再决定保留、迁移或单独清理；既有仅 IPv4 字面量行为不变 |
| 中 | 原 CI 文本写死统一 `--timeout 600`、本机 macOS 等，与当前实际不符 | `.github/workflows/linux.yml:73-77` 为 600；`.github/workflows/macos.yml:73-79` 为 180；`.github/workflows/msbuild.yml:78-87` 为 90 且有看门狗；`code-plan/Windows平台CTest挂死修复_实施记录.md:83-91,139-154` 有本机 Windows 验证 | 以仓库中各平台现行命令为准，不强制把超时统一，也不把旧 PR 成功误写为本重构已验证 |

历史决策继续有效：POSIX 不因单项 `SO_NOSIGPIPE` 差异拆 Linux/macOS；句柄继续用 `std::intptr_t`；IPv4 解析迁入无平台头内部接口；同一次接口迁移完成 TCP/UDP 所需调用能力。**原“P1 扩成 30 个虚方法”以及“新增多句柄方法”取消**：现有 `SocketOps` 方法数不能作为必须达到的目标。

## 2. 修订目标与边界

1. `src/` 的非平台内部头和 TCP/UDP 传输实现不包含原生 Socket 头，不使用 `sockaddr`、`socklen_t`、`in_addr`、原生地址/选项常量及平台分支；公共头完全不变。
2. `SocketOps` 负责 Socket 生命周期、IPv4 地址、TCP/UDP I/O 与 socket 选项；**`EventPoller` 独占等待能力**（单/多句柄共用已有接口），不得再在 `SocketOps` 增加 `wait_readable_multi`。
3. 平台实现按 Windows/POSIX 分源；Windows 代码必须由 Windows CI 编译并执行有关测试。允许 POSIX 实现内为 macOS `SO_NOSIGPIPE` 保留局部编译条件。
4. UDP 保留现有唤醒对、超时兜底、先 join 再 close、零长包/CTR/告警与异常语义；TCP 保留 100 ms 接收轮询片与“不能依赖 Windows shutdown 唤醒阻塞 recv”的约束（`src/tcp_transport.cpp:58-70,531-549`）。
5. 此次不是“通用网络库”：不新增 IPv6/DNS 能力，不修改协议包处理，不清理无关测试/示例/工具的平台分支。

## 3. 组件、接口与数据流（拟议，实施前确认）

```text
src/detail/socket/socket_ops.hpp              平台无关 SocketHandle/Ipv4Address/SocketOps 声明
src/detail/socket/socket_ops_win.cpp            Winsock 原生操作及错误分类
src/detail/socket/socket_ops_posix.cpp          Linux/macOS 原生操作及错误分类
src/event_poller.hpp                            现有 WaitItem/WaitStatus/EventPoller；改为只依赖无平台头接口
src/detail/socket/event_poller_win.cpp          WSAPoll 与错误文本
src/detail/socket/event_poller_posix.cpp        poll 与错误文本
src/socket_util.hpp                             必要时保留仅 include 新头的过渡转发层
```

- 使用原有 `SocketHandle = std::intptr_t`、`kInvalidSocketHandle = -1`；Windows 原生 `SOCKET` 与句柄的双向转换限制在平台实现并做尺寸、无效哨兵及往返验证；不把 `-1` 哨兵的跨编译器转换当成无需验证的假设。
- `Ipv4Address` 只携带 IPv4 的 **4 字节网络序**、**主机序端口**；IPv6 继续在传输配置校验时显式拒绝。原 v2 `family=2/10 + ip[16]` 与当前“不实现 IPv6”矛盾，故不照搬；解析/格式化、bind/connect/accept/getsockname、sendto/recvfrom 的转换与字节序在平台边界集中实现。明确区分 TCP listener 的本地地址与 accept 的对端地址，不把对端误写进 `bound_local`（现有 `src/tcp_transport.cpp:508-522` 有此赋值，须核实是否可观测后单独评估）。
- `create` 的 TCP/UDP 种类、`set_int_option` 的 reuse/keepalive/nodelay 与 SIGPIPE、接收超时改用内部**语义操作**；不要将 `AF_INET`、`IPPROTO_TCP`、`SOL_SOCKET` 等原生常量从新头漏回传输文件。保留现有失败致命/仅警告的分级（例如 TCP_NODELAY 失败只告警）。
- TCP 的 `wait_readable` / `wait_writable` 签名与三态不变；真实实现继续委托 `EventPoller` 单项等待。UDP 双项调用既有 `EventPoller::Wait`；`WaitStatus::Ready()` 的 failed 位仍属于就绪，由后续 I/O 判定。等待失败时**直接使用 `Wait` 的 `error_out`**记录错误，不能再先经丢弃错误文本的 `WaitAnySocket` 然后读取可能已被其它调用覆盖的 `errno`/`WSAGetLastError`（当前 `src/event_poller.cpp:214-217` 与 `src/udp_transport.cpp:721-733` 的组合风险）；TCP 单项委托目前也丢掉 `error_out`（`src/socket_util.cpp:135-139`），应一并确保错误有来源。错误分类需要明确区分“等待中断”和“Socket 失效”，避免错误持续发生时忙循环告警，并保持 UDP 现有关闭兜底。Windows EventPoller 还须保持独立调用 `RealEventPoller()` 时的 Winsock 初始化保证（`src/event_poller.cpp:40-79,185-188`）。
- `recvfrom` 注入应经 `SocketOps`，但等待的注入仍由 `SetEventPollerForTesting()` 承担。`tests/tcp_fake_socket.hpp:56-61,141-163` 现有 `ScriptReceive` 只拦截 `recv`，不能覆盖 UDP 的 `recv_from`：须新增独立的线程安全脚本/计数和可控就绪触发。两套进程全局注入在测试中必须串行、生命周期覆盖接收线程与 Close/join，假操作未脚本化时转发真实实现。
- 名称解析目前未见 TCP/UDP 生产调用，故**不将 `std::vector<SocketAddress>`/`std::function` 视为既定签名**；如果发现其他用户需要 `NameResolver`，先给出真实调用点及兼容测试再定迁移方式。

## 4. 分阶段实施与出口条件

| 阶段 | 内容 | 阶段出口 |
| --- | --- | --- |
| P0：接口契约确认 | 核对所有 `SocketOps`/EventPoller 调用点及 `NameResolver` 用户；审定 IPv4 POD 字节序、API 语义、错误/注入与 WSA 生命周期，确认下列用例；取得用户对扩大的 EventPoller 范围批准 | 架构、组件、数据流、错误处理、测试五项确认；不得提前修改产品代码 |
| P1：去头泄漏 | 增加纯内部 POD/语义接口，迁移 `socket_util.hpp`、TCP、UDP 的必要调用点及假对象；保持现有平台实现仍可编译；不添加多句柄虚方法 | Release 增量构建及针对性 TCP/UDP/EventPoller 测试 + Windows/Linux/macOS CI Build/Test 通过；过渡期不宣称 G1/G2 达标 |
| P2：平台分文件 | 将 SocketOps 与 EventPoller 分别拆为 Windows/POSIX 实现，CMake 按平台选**各一份**，移除旧实现 TU；校验初始化与错误报告 | 非平台头和传输文件零原生类型/常量/分支；三平台编译、端到端和失败注入测试通过 |
| P3：UDP 最终复用 | 移除 UDP 的第二套 WSA/原生调用；唤醒对、sendto、recvfrom、超时与格式化均走既有抽象；不重复替换已迁入 EventPoller 的等待 | UDP 回环、唤醒失败兜底、Close 延迟、零长包、收发异常和 Xcplite 互操作回归均通过；TCP 全量回归不退化 |
| P4：自动门禁 | CI 加跨平台静态隔离检查；更新 README 与每阶段 `code-plan/` 实施/测试记录 | 三平台 Build + Test + 隔离门均绿；记录明确的提交/运行证据后才能将状态改为“已完成” |

每阶段修改代码前按 `AGENTS.md` 流程获得方案批准；只改文档不是实施。Windows/Linux/macOS CI 均为硬门，不以本机单平台结果替代；**完整编译先 clean**，使用 Release，不运行 install 树全量测试。不做提交或回滚 Git 写操作，除非用户另行授权。

## 5. 测试设计与验收规则

| 分类 | 必测行为 |
| --- | --- |
| POD/边界 | IPv4 `127.0.0.1`、`0.0.0.0`、端口 0/非零、无效文本、地址/端口字节序；Windows/POSIX 原生句柄往返及无效句柄 |
| TCP | 原有 `TcpFakeSocketTest` 短写/EINTR/EAGAIN/超时、TCP 回环、Slave accept、静默对端 Close ≤ 既有容差；SIGPIPE 相关行为在支持平台验证 |
| EventPoller | 单/双句柄顺序、readable/failed、超时/失败文本、注入；Windows `WSAPoll` 与 POSIX `poll` 真机行为；避免重复等待 API |
| UDP | 成功收发/端口回填、唤醒可用与创建失败兜底、关闭前不释放句柄、空 Datagram、recvfrom 失败三类分支（超时/中断/其他警告并继续）、发送失败、错误文本可观测性；复用 `tests/udp_transport_test.cpp:947-1001,1014-1068,1093-1107` 的兜底、快速关闭、200 次开关与关闭后无回调测试 |
| 生命周期 | 多实例 TCP/UDP/EventPoller 同时存在；Winsock 初始化失败、创建线程失败、重复 Open/Close、注入撤销后的无悬挂引用 |
| 回归 | 已有 `UdpBoundaryFraming`、`XcpliteIntegration`、全部 TCP 测试和三平台 CTest；CTest 串行，保留 workflow 已有性能用例排除项和各平台超时/Windows 看门狗 |

原 v2 T3“T3 立即唤醒”应改为**保留并回归已存在的机制**，而不是声称本计划新建多句柄能力；T4 必须测试警告和继续处理，不预设一律关闭。测试目标中为新内部接口新增单元测试，真实 socket 回环与注入测试分开；进程级注入的 ctest 目标标记 `RUN_SERIAL`。现有 CI 排除的是性能门禁，不应把其他功能用例移出测试。

隔离脚本应 **先采集命中，再判断退出码**，避免 `grep ... && exit 1` 在组合脚本中受 `set -e`/管道语义影响：

1. 扫描 `src/**/*.hpp` 的原生 Socket 头，以及对旧转发头的传递污染；纯内部头还须禁止 `sockaddr`/`socklen_t`/`in_addr`、原生常量。
2. 扫描 `src/` 的非平台实现 `.cpp/.hpp` 的 `_WIN32`、原生 Socket include/调用与平台常量；排除的只应是**明确列出的四个** `socket_ops_{win,posix}.cpp`、`event_poller_{win,posix}.cpp`。若新增平台实现，须显式审议检查白名单，不能用通配符吞掉任意文件。
3. 检查 CMake 各平台仅编入本平台两个实现，旧 `socket_util.cpp`/`event_poller.cpp` 不再参与编译；三个 workflow 运行同一脚本，Windows 用 bash。静态检查是辅助门，不能代替三平台 Build/Test。

## 6. 风险、批准与历史记录

- **阻断性待确认项**：扩大范围包含已存在的 `EventPoller` 平台文件拆分是否获批准；这是保留“`src/` 非平台文件零 `_WIN32`”目标的必要条件。如不批准，必须明确降低该目标并在门禁白名单中列出现有 `src/event_poller.cpp` 例外，不得继续宣称全隔离。
- **实施前确认**：内部 POD/选项接口的最终签名、错误映射与 Windows WSA 生命周期；确认是否保持未使用的名称解析钩子；不假设历史虚表方法数或负行数承诺。设计自检：无 IPv6 功能承诺；不重复 EventPoller；不把 P1 过渡态误判为最终达标。
- **历史已完成但非本次重构**：A1 `SocketHandle` POD 与 A2 EventPoller 多句柄抽象已实现（`code-plan/Platform接口分层_实施计划_EventPoller与SocketHandle.md`）；三平台 CI Test 现已设置硬门；Windows 接收轮询与挂死排障记录在 `code-plan/Windows平台CTest挂死修复_实施记录.md`。原 v2 的 PR #14 与 2026-10-26 统计仅为当时基线，**不证明本版 P1–P4 已实施或已通过**。
- **回滚原则**：按阶段保留小步可验证的源码/测试/配置改动；任一步三平台未绿先修或回退该步，勿机械执行旧 v2 的 `git checkout -- ...` 路径清单（旧清单未涵盖 EventPoller、workflow 和新增测试）。
- **批准状态**：原 Q1（POSIX 不拆）、Q2（`intptr_t`）、Q3（IPv4 解析纳入内部接口）、Q4（接口变更集中一次）沿用；修订后的 P0–P4 **尚未批准**，不得将“用户曾裁决 Q1–Q4”解释为同意新增 EventPoller 范围。是否顺带接格式门禁为独立选择，不列入本次完成条件。
