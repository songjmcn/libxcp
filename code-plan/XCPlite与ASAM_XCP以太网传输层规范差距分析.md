# XCPlite 以太网传输层 vs ASAM XCP on Ethernet TL 规范 V1.5.0 差距分析

- 规范：`docs/ASAM-AE-MCD-1-XCP-AS-Ethernet-Transport-Layer-V1-5-0.pdf`（33 页，全文提取于 /tmp/xcp_eth_spec.txt）
- 被对比实现：`/Users/alex/workspace/project/cvf/utils/XCPlite`（ECU 侧从机库；下文行号均指该仓库 `src/` 下文件）
- 分析日期：2026-10-22。所有结论均有源码行号或实测报文佐证；标注"推断"处为未直接验证项。

---

## 一、总体结论

XCPlite 的以太网传输层在**核心帧格式（§4.3 头/尾）、TCP 单连接模型（§4.1.1）、从机计数器（§4.3.1.2）上与规范一致**，属于基本合规的实现。主要差距集中在三块：

| # | 差距 | 严重度 | 与当前 ECU 沉默问题的关系 |
|---|------|--------|--------------------------|
| G1 | §5 全部以太网专有命令（GET_SLAVE_ID 等）在本编译配置下**不可达** | 中（可选命令，规范允许不实现） | **直接相关**：解释了实验 J/K/L 无响应 |
| G2 | CONNECT RES 的 RESOURCE/COMM_MODE_BASIC 为编译期硬编码常量 | 低-中 | 无关 |
| G3 | UDP 主地址变更时主动断开会话，比规范的"不响应"更激进 | 低 | 无关（TCP 模式不走此路径） |
| G4 | socketSend 无部分写处理（短写即判错断连） | 中（工程健壮性，非规范条款） | **可疑相关**：见第五节 |
| G5 | PID_ERR 取值 0xFE 与 libxcp 假设一致，但脚本曾按 0x00 判 ERR（我方 bug，已修） | — | 修正后重测仍全沉默 ⇒ 排除误判 |

另有一个**规范本身没有规定、但实测暴露的重大异常**：ECU 对同一 TCP 连接上首包 CONNECT 之后的任何命令（包括必须回 ERR 的未知命令）完全沉默——这不符合 stock XCPlite 代码路径，指向 ECU 集成层问题而非 XCPlite 库本身的规范差距（详见第五节）。

---

## 二、逐章符合性矩阵

### §4.1 寻址

| 规范条款 | XCPlite 实现 | 判定 |
|---------|-------------|------|
| §4.1.1 TCP：从机为监听方，同时只接受一条连接 | `gXcpTl.Sock` 单全局套接字，仅当 `INVALID_SOCKET` 时才 `socketAccept`（xcpEthTl.c:319-328） | ✅ 符合 |
| §4.1.1 TCP：连接在 XCP-connected 状态下关闭 → 从机执行 XCP disconnect、停 DAQ | rx 返回 0（FIN）→ `XcpDisconnect()` + sleepMs(100) + closeTcpConnection（xcpEthTl.c:347-352）；`XcpDisconnect` 内 `isDaqRunning()→XcpStopDaq()`（xcpLite.c:2340-2356） | ✅ 符合 |
| §4.1.1 RESUME 模式：从机需缓冲测量数据直到重连 | `XcpStart(queueHandle, resumeMode)` 中 `(void)resumeMode; // Start in resume mode, not implemented yet`（xcpLite.c:3519-3522） | ❌ **未实现 RESUME**（规范该段为 Note，非强制"MUST"，但功能缺失） |
| §4.1.2 UDP：connected 后只应答 CONNECT 来源 IP 的电文，其它"不响应" | XCPlite 检查 `MasterAddr`/`MasterPort` 不符时**主动 `XcpDisconnect()`**（xcpEthTl.c:240-256），随后才达到"不响应"效果 | ⚠️ 行为偏差：规范要求静默丢弃，XCPlite 额外做了断开（更严格，可能打断合法的重连尝试） |

### §4.3 头与尾

| 规范条款 | XCPlite 实现 | 判定 |
|---------|-------------|------|
| §4.3.1 Control Field = LEN(WORD Intel) + CTR(WORD Intel)，TCP/UDP 同格式 | `tXcpCtoMessage{uint16 dlc; uint16 ctr; ...}` packed（xcpEthTl.c:51-57），小端 | ✅ 符合（这正是我们 raw 裸流测试失败、dlc+ctr 成功的根因） |
| §4.3.1.2 主机 CTR 每包递增（CMD/STIM） | 由 master 负责；libxcp UdpTransport 已实现 | ✅（与本对比无关） |
| §4.3.1.2 多播 CTR=0，从机不处理 | `XcpEthTlSendMulticastCrm` 固定 `p.ctr = 0`（xcpEthTl.c:202-217）；接收侧 `handleXcpMulticastCommand` 不看 ctr | ✅ 符合（但多播整体被编译关闭，见 G1） |
| §4.3.1.2 从机生成**第二个独立** CTR，对所有 RES/ERR/EV/SERV/DAQ 递增 | 单一 `gXcpTl.Ctr++` 覆盖 CRM 直发（xcpEthTl.c:190）与队列 DTO/聚合段（QueuePeek 时盖戳 xcpQueue64.c:707；丢包补偿 `Ctr += lost` xcpEthTl.c:651） | ✅ 字面符合（一个从机计数器跨所有类型递增）。源码注释所称 "XCP design flaw"（xcpEthTl.c:614-616）指的是 CANape 期望响应与 DAQ 各自独立计数——那是工具私有期望，超出本规范文本 |
| §4.3.2 单消息成帧时 tail 可选 | CRM 直发不带 FILL：`send(size + 4)` 精确长度（xcpEthTl.c:195） | ✅ 符合（tail 可选） |
| §4.3.2.2 多消息拼接需 PACKET_ALIGNMENT_x 对齐，FILL 内容 do-not-care | 队列条目 `(msg_len+3)&~3` 补到 4 字节（xcpQueue64.c QueueAcquire，XCPTL_PACKET_ALIGNMENT=4 xcptl_cfg.h:46）；ODT 头插 0xAA 对齐字节（xcpLite.c:1999-2001）；聚合段 QUEUE_ACCUMULATE_PACKETS（xcpQueue64.c:716+） | ✅ 符合 32bit 对齐（PACKET_ALIGNMENT_32） |

### §4.4 性能极限

| 规范 | XCPlite | 判定 |
|------|---------|------|
| MAX_CTO ∈ [0x08,0xFF] BYTE；MAX_DTO ∈ [0x0008,0xFFFF] WORD | 248 / 1024（xcptl_cfg.h:29/34）；CONNECT RES 回填 b3=0xF8、b4-5=0x0400 | ✅ 范围内且上报正确 |

### §5 以太网专有命令 —— 差距 G1 的主体

规范 Table2（p12）：五个子命令**全部 optional**，超时 300ms，并明文允许 *"Optional commands might not be implemented at all. This means that a slave might not even send a negative response to an unimplemented command."*

| 子命令 | 规范要求 | XCPlite 现状 | 判定 |
|--------|---------|-------------|------|
| GET_SLAVE_ID (F2 FF) §5.1 | 从机在 239.255.0.0:**5556** 收多播，**无论连接状态**处理并按 Table4 布局多播回应 | `case CC_TL_GET_SERVER_ID: goto no_response; // Not supported`（xcpLite.c:3212-3213）；且整个多播 RX 线程被 `XCPTL_ENABLE_MULTICAST` 注释关闭（xcptl_cfg.h:53，xcpEthTl.c:464-497） | ❌ 不支持（规范允许，但注意：**unicast 发送 F2 FF 也走此 no_response 分支**——静默是"合规的不实现"） |
| GET_SLAVE_ID_EXTENDED (F2 FD) §5.2 | 同上 + MAC + GET_ID 字段 | 有实现（xcpLite.c:3214-3236）但被同一禁用宏包裹；布局用 EXT 偏移 ADDR@2 PORT@W18 STATUS@20 RES@21 IDLEN@DWORD22 ID@26 MAC@26+n（xcp.h:1153-1160），STATUS 位值与 Table11/12 一致（xcp.h:1145-1149） | ❌ 默认编译不可达；启用后布局符合 §5.2 Table10 |
| SET_SLAVE_IP_ADDRESS (F2 FC) §5.3 | 可选；ERR_CMD_BUSY→无限重试 | grep 无 `CC_TL_SET_SERVER_IP` 处理分支（xcp.h:1123 仅有码定义） | ❌ 未实现（规范允许） |
| GET_DAQ_CLOCK_MULTICAST (F2 FA) §5.4 | 从机监听 239.255.<cluster LE>:**5557**，≤2s 轮询，回 EV_TIME_SYNC | 协议层有完整 handler（xcpLite.c:3174-3209，回 PID_EV+EVC_TIME_SYNCH），端口常量正确 XCPTL_MULTICAST_PORT=5557（xcptl_cfg.h:63）；但 `XCP_ENABLE_DAQ_CLOCK_MULTICAST` 注释关闭（xcp_cfg.h:390）⇒ handler 编译不进；且它挂在 unicast 分发链路上，规范语义是多播信道 | ❌ 默认编译不可达；⚠️ 即使打开，多播入口(xcpEthTl.c handleXcpMulticastCommand)与协议层 gate(xcpLite.c:2481 豁免 F2)能通，但 spec 要求的"独立于连接状态在多播口处理"依赖同样被禁用的 MulticastSock |
| User-defined 0x01-0x0F | 可选 | `XCP_ENABLE_USER_COMMAND` 下转 ApplXcpUserCommand | ✅ 存在通道 |

**G1 小结**：本 ECU 固件的 XCPlite 编译配置（多播宏全关）下，§5 家族实际只剩 EXTENDED 的代码尸体和 clock-mcast 的 unicast 残影。实测与我们一致：实验 J（未连接直发 F2 FF）、K（CONNECT 后 F2 FF）、L（CONNECT 后 F2 FA）全部静默——对照源码，这些正是 `goto no_response` / 被禁用分支，**属于规范允许的静默**。

### §5.5 错误处理 / §6 事件 / §7 带宽 / §8 AML

- §5.5.1 Table20：SET_SLAVE_IP_ADDRESS 的 BUSY 无限重试语义——因该命令未实现而 N/A。
- §6：以太网无专有事件——XCPlite 未发明额外事件，✅。
- §7.1：MAX_BUS_LOAD 计算基于 net data（header+packet+tail），仅约束 DAQ/STIM。XCPlite 队列溢出策略为丢整 event 剩余 ODT（xcpLite.c:1985-1994 return on overrun），不做 bus-load 预检——bus load 校验本是**主机侧**职责（规范原文 "The XCP master can monitor this limit"），从机无需实现，✅ 不算差距。
- §8 AML：从机侧不适用。

---

## 三、CONNECT RES 字段核对（实测报文 × 源码）

实测（192.168.2.137:5559）：`08 00 <ctr> 00 | ff 05 80 f8 00 04 01 01`

| 字段 | 实测值 | 源码出处 | 规范评价 |
|------|--------|---------|---------|
| RESOURCE | 0x05 = RM_CAL_PAG\|RM_DAQ | **硬编码** `RM_DAQ \| RM_CAL_PAG`（xcpLite.c:2469） | ⚠️ G2：编码格式合规，但内容不随固件能力配置（若该 ECU 支持 PGM/STIM 会谎报不支持；反之亦然）。libxcp/上位机不应据此开通 PGM |
| COMM_MODE_BASIC | 0x80 = CMB_OPTIONAL, bit0=0 Intel, bit1-2=0 AG=BYTE | 硬编码常量（xcpLite.c:2471） | ⚠️ 同 G2：字节序/AG 编译期定死，无法协商 Motorola/DWORD——对本 ECU（Intel/BYTE）恰好真实 |
| MAX_CTO / MAX_DTO | 248 / 1024 | xcptl_cfg.h:29/34 | ✅ §4.4 范围内 |
| 版本两字节 | 01 / 01（仅主版本） | `XCP_PROTOCOL_LAYER_VERSION>>8`、`XCP_TRANSPORT_LAYER_VERSION>>8`（xcpLite.c:2465-2466）；cfg 实值 0x0102/0x0104（xcp_cfg.h:28, xcptl_cfg.h:24） | ✅ §3.1.1 明文"CONNECT 应答只返回 TL 版本号最高字节"——合规；完整次版本须走 GET_VERSION |

---

## 四、对我们工具链的直接要求（libxcp TcpTransport 设计输入）

1. 帧格式 = LEN(LE16)+CTR(LE16)+payload，**不是** ASAM 裸流 `[Len][CMD]`（§4.3.1 + XCPlite 双重确认）。
2. 主机 CTR 同连接内逐命令递增（§4.3.1.2）；脚本已按此实现。
3. 响应解析 PID 空间：0x00–0xFB=DTO、0xFC=SERV、0xFD=EV、**0xFE=ERR（code=payload[1]）**、0xFF=RES（xcp.h:131-134；libxcp protocol_types.hpp:133 PacketType 与此一致）。此前脚本把 0x00 当 ERR 是错的，已在 m00240 后标记待修。
4. CRM 可能带 0–3 字节 FILL（tail 可选，§4.3.2.1）：TCP 流上以 dlc 为准读取 payload 即可，FILL 可忽略但**不能不消费**（否则流错位——本地假服务器踩过此坑）。
5. 不要期待 §5 探测命令可用；slave 发现只能靠已知 IP:port 直连。
6. 单 TCP 连接语义（§4.1.1）：libxcp 重连前必须确保旧 socket 已 close，否则新 connect 会被拒/排队。

## 五、遗留异常：ECU 对 CONNECT 之后所有命令沉默（非规范差距，属集成缺陷）

stock XCPlite 流程推演：CONNECT RES 发出前 SS_CONNECTED 必已置位（xcpLite.c:2458 早于 2464-2471 组 CRM，同步路径 XcpSendResponse(async=false)→XcpTlSendCrm 直发不经队列）⇒ 同 socket 第二包必然进 connected 分支 → `XcpCommand` → 即便未知命令 switch default 也**必须**回 ERR(0xFE, CRC_CMD_UNKNOWN=0x20)。实测实验 M（CONNECT→0x55）静默，与该代码路径矛盾。结合 fresh socket 的 CONNECT 永远得到应答（accept 循环与 TX 存活），可收窄到三种解释（按可能性排序）：

1. **ECU 集成层替换了 handleXcpCommand/等价逻辑**：只在 TL 层应答 CONNECT，后续命令被丢弃（例如自研网关只转发 CONNECT 探活）。
2. **接收线程在第一包处理后阻塞**（卡在 ApplXcpConnect 回调链或其后的 XcpBackgroundTasks/自有钩子），第二包从未被 recv——与新连接 accept 前重新轮询得活路相吻合的说法不完全自洽，需要 ECU 侧日志裁决。
3. ECU 用了非本仓库版本的 XCPlite（修改版）。

**建议下一步**：在 ECU 侧把 DBG_LEVEL 提到 ≥4（dbg_print.h:76 运行时可调 `XcpSetLogLevel`，cal_test main.cpp:183 即示例），复现一次 CONNECT→GET_VERSION，看串口/控制台是否出现 `RX: CTR ... LEN ... DATA`（level5）与 `Command ignored because not in connected state`（level4 warning）。这一条日志即可在三者间定案。远程黑盒手段已穷尽。

---

## 六、证据索引

- 规范文本：/tmp/xcp_eth_spec.txt（pypdf 提取，页标记 '===== PAGE N ====='）
- 实测矩阵：本会话 m00198–m00304 区间的 python 探针输出（CONNECT RES ctr 序列 03→13 单调递增证明从机 TX/计数器健康；J/K/L/M 沉默）
- XCPlite 关键行：xcpEthTl.c:51-57/180-196/220-278/319-363/611-660；xcpLite.c:238/2340-2356/2366-2377/2419-2481/2458/2464-2471/3212-3236；xcpQueue64.c:694/707；xcptl_cfg.h:29/34/46/53/63；xcp_cfg.h:24/28/390；xcp.h:25-30/123/131-134/140-146/479-484/973-980/1121-1160；platform.c:323/742/778-784；xcpEthServer.c:105-118/160-215。
