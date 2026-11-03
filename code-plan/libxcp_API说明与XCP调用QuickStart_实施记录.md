# libxcp API 说明与 XCP 调用 Quick Start 文档记录

日期：2026-10-31

## 1. 交付文件

| 文件 | 内容 |
|---|---|
| `docs/API_说明.md` | 面向 C++ 集成者，概述 CMake targets、XcpMaster、TCP/UDP、A2L 数据库、measurement API、错误模型与对象生命周期。 |
| `docs/XCP_调用_QuickStart.md` | 展示本地 A2L 加载与符号搜索、精确查找、UDP/TCP Transport 配置、连接后读取原始字节及转换物理值。 |

## 2. 文档依据与注意事项

- 接口核对依据为 `include/libxcp/xcp_master.hpp`、`include/libxcp/{tcp_transport.hpp,tcp_transport_config.hpp,tcp_wire_profile.hpp,udp_transport.hpp,udp_transport_config.hpp,xcp_error.hpp}`、`include/libxcp/measurement/measurement_session.hpp`，以及 A2L bridge/database 的公开声明头文件。
- 核对现有 UDP 主站示例 `examples/xcp_master_udp/xcp_master_udp.cpp` 与 README；没有引入代码接口或修改第三方代码。
- 特别说明 A2L `Result<T>` 与 XCP `XcpException` 是不同错误模型，数据库/适配器的非拥有生命周期，raw bytes 与 A2L 物理换算分离，UDP 端点配置，以及 TCP `AsamV14`/`AsamV15` profile 的显式选择限制。
- Quick Start 只展示标量 `ReadMemoryBytes`；数组/曲线、DAQ、跨块写入和 ECU 权限不作为简单变量读取示例，避免暗示这些能力不需单独规划或验证。

## 3. 后续补充：TCP 版本探测与 wire profile 试连

日期：2026-11-01；根据用户反馈在两份文档中补充“TCP 协议版本嗅探”说明。

- 明确区分 XCP `GET_VERSION (0xC0, 0x00)` 和 TCP `AsamV14` / `AsamV15` wire profile 选择：前者默认 CONNECT 后探测 PL/TL 版本，失败默认不致连接失败、版本 minor 保持未确认；它不决定 TCP 帧布局。
- 说明库的严格 framing 自动化是连接级候选验证，不是在同一 TCP 字节流里嗅探/切换。候选由 A2L 版本与对齐声明排序，逐个用新连接进行 CONNECT/GET_COMM_MODE_INFO/GET_STATUS 及 QueryStatus 校验，关闭试连后再建立正式会话；验证阶段禁用 GET_VERSION，防止两类探测相互影响。
- 补充 `xcp_master --tcp-profile auto` 用法、Accepted/Rejected/Inconclusive 的区别与 exit code，以及可供 C++ 集成复用的候选生成、试连、协商 API。
- 注明 8-bit 下两种 profile 的字节布局等价，试连结果只证明候选可用，不保证 profile 唯一；16/32-bit A2L alignment 仅把对应 V1.5 候选前置，其余默认候选仍保留作回退；`auto-legacy` 仅为不推荐的旧模式。

## 4. 验证

- 已复读新增 Markdown，检查两个文档之间及仓库示例/头文件的相对链接；检查时发现 Measurement README 链接目标不存在，已改为现存的 `docs/modules/测量子系统.md`，并确认 `examples/xcp_master_udp/README.md`、TCP 配置头文件和 wire profile 头文件目标存在。
- API 名称、主要参数类型、默认 A2L `require_if_data_xcp`、TCP wire profile 选择、地址扩展与 AG 说明均对照公开头文件核验。
- 原有 Quick Start 片段仍是分节组合的集成示例，需替换 ECU/A2L 配置；本次新增的 TCP 候选协商 C++ 片段已用 C++20 `-fsyntax-only` 对公开头文件完成语法编译检查，不执行实际 ECU 网络试连。未运行测试：本次仅改文档且未增加测试代码。
