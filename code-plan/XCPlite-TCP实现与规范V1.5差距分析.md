# XCPlite TCP 传输实现 vs ASAM XCP on Ethernet TL 规范 V1.5.0 —— TCP 专项差距分析

- 范围：**仅 XCP over TCP/IP**（`useTCP=true` 时 `XcpEthServerInit(addr,port,true,queueSize)` 起的收发链）。UDP/多播条款见另一篇《XCPlite与ASAM_XCP以太网传输层规范差距分析.md》。
- 规范：`docs/ASAM-AE-MCD-1-XCP-AS-Ethernet-Transport-Layer-V1-5-0.pdf`（全文提取 /tmp/xcp_eth_spec.txt，页号按之）。
- 代码：`/Users/alex/workspace/project/cvf/utils/XCPlite`，行号均指该仓库 `src/` 文件。
- 方法：源码逐行核对 + 对真实 ECU（192.168.2.137:5559）报文级实测（本会话 m00198–m00304、m00440 区间）。所有判定均有 file:line 或报文证据；推断处明示。

---

## 0. TCP 数据通路地图

```
socketOpen/bind/listen(backlog=5)        platform.c:667 · xcpEthTl.c:504-509
 └ ReceiveThread (xcpEthServer.c:160-187)
    └ XcpEthTlHandleCommands (xcpEthTl.c:302-363)
       ├ Sock==INVALID_SOCKET 才 socketAccept → setTcpKeepAlive(5,1,3)   :319-325
       ├ recv 4B 头 [dlc|ctr]（MSG_WAITALL, platform.c:742）             :332-333
       ├ recv dlc B payload                                              :335
       │   （不消费 FILL 填充——见 §2 偏差 D2）
       ├ 完整 → handleXcpCommand (:220-284)
       │   ├ connected → XcpCommand(packet,(uint8_t)dlc) 同步分发 (:259-261)
       │   └ !connected：仅收 dlc==2&&CC_CONNECT（QueueClear 后分发 :267-277）
       │                 其余包 DBG warning 后静默丢弃 (:280-282)
       │     └→ XcpAsyncCommand (xcpLite.c:2422+)
       │         ├ CONNECT 特判 CRO_LEN==2（ETH 配置精确匹配 :2440-2442，宏定义 xcp_cfg.h:24）
       │         ├ 非 CONNECT：!isConnected() && cmd!=0xF2 → CRC_CMD_IGNORED 静默 return (:2481-2484)
       │         ├ CRO_LEN<1||>MAX_CTO → ERR CMD_SYNTAX (:2486-2487)
       │         └ switch default → error(CRC_CMD_UNKNOWN) → ERR(0xFE,0x20) (:3308-3312)
       │             响应经 XcpSendResponse(async=false) → XcpTlSendCrm 直发 (:2366-2377; xcpEthTl.c:180-197)
       └ FIN(n==0)/错误 → XcpDisconnect + sleepMs(100) + closeTcpConnection (:341-363)
 └ TransmitThread → XcpTlHandleTransmitQueue (xcpEthTl.c:611-690)
      仅服务 DAQ DTO/EVENT/SERV/async 响应；TCP 命令应答不经队列
```

核心认知：TCP 模式下命令响应在接收线程内**同步直发**，不依赖发送队列健康度；队列只承载流式数据。多数"响应丢失"类怀疑在此模式下可以排除。

---

## 1. §4.1.1 TCP 寻址与会话模型

| 条款 | 规范原文要点 | XCPlite 实现 | 判定 |
|---|---|---|---|
| 从机为监听方 | slave is the listener | bind+listen（示例绑 0.0.0.0），✅ | ✅ |
| **同时只接受一条连接** | "It will only accept one connection at the time." | 单全局 `gXcpTl.Sock`，仅当 INVALID_SOCKET 才 accept（xcpEthTl.c:319-328）。但 listen backlog=5（platform.c:667）⇒ 第二/三…条连接完成三次握手后躺在 accept 队列，**既不拒绝也不服务**，直到第一条关闭才被取走。实测吻合：早期测试 socket 未显式 close 期间新 CONNECT 超时；脚本每端口显式 close 后恢复 | ⚠️ **偏差 D1**：语义近似达成但手段偏松（排队挂起 ≠ 拒绝）。上位机必须保证失败路径也 close socket |
| connected 态 socket 关闭 → XCP disconnect、停 DAQ | §4.1.1 | n==0 → XcpDisconnect（内部 isDaqRunning→XcpStopDaq，清 SS_CONNECTED，xcpLite.c:2340-2356）+ 关 socket | ✅ |
| RESUME Note（master 反复重连、slave 缓冲数据） | §4.1.1 Figure1 | `XcpStart(queue, resumeMode)`：`(void)resumeMode; // not implemented yet`（xcpLite.c:3519-3522） | ❌ 未实现（Note 级非 MUST，列功能缺失） |

## 2. §4.3 头与尾（TCP 流分帧）

| 条款 | 规范 | XCPlite | 判定 |
|---|---|---|---|
| Control Field = LEN WORD Intel + CTR WORD Intel；TCP 沿用 UDP 格式以便解码 | §4.3.1 | packed `{u16 dlc; u16 ctr; …}`（xcpEthTl.c:51-57），双向加头（发送 :188-191） | ✅（ECU 实测决定性验证：raw `[Len][CMD]` 被读成 dlc=0x01FF 阻塞凑包；dlc+ctr LE16 则 CONNECT 正常应答） |
| LEN = 原始 XCP Packet 字节数 | §4.3.1.1 | rx 按 dlc 收 payload | ✅ |
| master CTR 逐包递增；multicast CTR=0；slave 生成第二个独立 CTR 覆盖 RES/ERR/EV/SERV/DAQ 全部类型 | §4.3.1.2 | slave 单一 `gXcpTl.Ctr++`：CRM 直发 :190、队列段 QueuePeek 盖戳（xcpQueue64.c:707）、丢包补偿 `Ctr+=lost`（xcpEthTl.c:651）。实测 CONNECT RES ctr 跨连接单调 03→13 ⇒ 服务器级计数器持续健康 | ✅ 字面合规（规范未要求 RES 与 DAQ 分计数；源码注释的 "design flaw" 是 CANape 私有期望，超出规范文本） |
| tail 0-3 FILL；**单消息成帧时 tail 可选** | §4.3.2.1 | CRM 精确 `size+4` 无 FILL（xcpEthTl.c:195） | ✅ 发送侧合规 |
| 多消息拼接按 PACKET_ALIGNMENT_x 用 FILL 对齐 | §4.3.2.2 | 队列条目 `(msg_len+3)&~3`（xcptl_cfg.h:46 ALIGNMENT=4；xcpQueue64.c QueueAcquire），ODT 4 字节头含 0xAA 对齐字节（xcpLite.c:1490/1999-2001，因 XCP_MAX_DAQ_COUNT=1024>256 选 4B 头），QUEUE_ACCUMULATE_PACKETS 拼段 | ✅ 符合 ALIGNMENT_32 |
| **接收方按整帧推进（头+包+尾）** | §4.3.2 隐含 | TCP rx 只读 `4+dlc`（xcpEthTl.c:332-335），**不消费 FILL**。自洽前提是自己从不发 FILL；一旦对端（第三方 master 按 §4.3.2.2 在 TCP 上拼段发 CMD，或改版固件带 FILL）发来填充，后续 4 字节头读取即错位 | ⚠️ **偏差 D2（潜在流错位缺陷）**：接收侧无 tail 消化逻辑。本地假服务器实验证明：发送方补 FILL 而接收方不吞 → 下一帧解析错乱 |

## 3. §4.2 通信模型

标准命令/响应模型 ✅（同步分发）。block transfer、interleaved 为 optional：XCPlite 明文不支持 interleaved（busy_response TODO xcpLite.c:3316-3318）——不算违规，列功能缺失。

## 4. §5 专有命令落在 TCP 单播上的行为

规范 Table2(p12)：五个子命令全 optional，*"might not even send a negative response"*。

| 子命令 | TCP 收到时行为 | 源码 | 判定 |
|---|---|---|---|
| GET_SLAVE_ID F2 FF | 未连接：TL 静默丢弃（xcpEthTl.c:280-282）；已连接：switch `case CC_TL_GET_SERVER_ID: goto no_response`（xcpLite.c:3212-3213） | 同左 | ✅ 规范授权静默（实测 J/K 沉默吻合） |
| EXTENDED F2 FD | 实现体在 `#ifdef XCPTL_ENABLE_MULTICAST`（默认注释 xcptl_cfg.h:53）且响应只走多播口 XcpSendMulticastResponse | xcpLite.c:3214-3236 | ✅ 允许不实现 |
| SET_SLAVE_IP_ADDRESS F2 FC | 无 case ⇒ switch default 回 ERR(0xFE, CRC_CMD_UNKNOWN=0x20)（比静默友好，可接受；Table20 BUSY 重试语义因此 N/A） | xcp.h:1123 有码无处理 | ✅ 边缘合规 |
| GET_DAQ_CLOCK_MULTICAST F2 FA | handler 存在但 `XCP_ENABLE_DAQ_CLOCK_MULTICAST` 注释关闭（xcp_cfg.h:390）⇒ 落入 unknown→ERR 路径（若可达） | xcpLite.c:3174-3209 | ✅ 允许不实现 |

前提提醒：以上"会回 ERR"都建立在 stock 流程可达 switch 之上。**ECU 实测连未知命令 0x55 都不回**（实验 M）——矛盾见第 7 节。

## 5. CONNECT 及基础命令在 TCP 侧的细节

| 项 | 依据 | XCPlite | 判定 |
|---|---|---|---|
| CONNECT CRO 长度 | 基础标准 CRO=2 | ETH 编译下 `CRO_LEN == CRO_CONNECT_LEN(2)` 精确匹配（xcpLite.c:2440-2442；宏 xcp.h:476） | ✅ |
| CONNECT MODE 位 | 基础标准 bit0=LEGACY 等 | mode 取出后原样传 ApplXcpConnect(mode)，协议层不解析位；随后**无条件**置 SS_LEGACY_MODE（xcpLite.c:2458），仅 GET_COMM_MODE_INFO 后才可能转 advanced（:3116 一带） | ⚠️ **偏差 D3**：不按 MODE 协商，恒 legacy 起步 |
| ApplXcpConnect 返回 false | — | `return CRC_CMD_OK; // Application not ready, ignore`——**无任何响应**（xcpLite.c:2452-2455） | ⚠️ **偏差 D4**：应用拒绝 CONNECT 时静默超时而非负响应（规范无强制，但显著增加黑盒误诊成本——正是本次踩过的坑形态） |
| CONNECT RES 内容 | §3.1.1(p7)：只回 TL 版本 MSB | 版本两字节取 `>>8` 主版本（xcpLite.c:2465-2466）✅；RESOURCE 硬编码 `RM_DAQ|RM_CAL_PAG`=0x05（:2469）、COMM_BASIC 硬编码 `CMB_OPTIONAL`=0x80（:2471，Intel/AG=BYTE 定死） | ⚠️ **偏差 D5**：格式合规、内容不随固件能力（libxcp/上位机不可据此开通 PGM/STIM 或改字节序假设） |
| 重复 CONNECT（connected 分支） | 基础标准：复位 DAQ 重新应答 | 正常走 connected 分发，DBG warning "Already connected! DAQ setup cleared!"，XcpClearDaq 生效 | ✅（ECU 不回 ⇒ 集成异常，第 7 节） |
| 未连接收到非 CONNECT 命令 | 基础标准应回 CRC_CMD_IGNORED(0x02) 负响应 | **双重静默**：TL 层丢弃（xcpEthTl.c:280-282）即使侥幸进协议层也有 gate 静默 return（xcpLite.c:2481-2484） | ⚠️ **偏差 D6**：以太网 TL 文档只对 UDP 错误来源授权"不响应"（§4.1.2），TCP 未授权；此处连 ERR 都不发，偏离基础标准精神（工程上防风暴常见，但诊断不友好） |
| CRO_LEN 越界守卫 | — | `CRO_LEN<1 || >MAX_CTO → ERR CMD_SYNTAX`（xcpLite.c:2486-2487）；TL 层 `dlc>MAX_CTO → return false` 静默断会话（xcpEthTl.c:259-260） | ✅ 基本健全（注意后者静默且不断线，属 D6 家族） |

## 6. TCP 工程质量（非规范条款，影响可用性）

1. **无 TCP_NODELAY**（全库 grep 空）：Nagle×delayed-ACK 交互可给小包响应引入 ~40ms 粒度延迟；低负载不明显，高命令吞吐/压测放大。
2. **socketSend 单次 send() 无短写循环**（platform.c:778-784，int16_t）：`r!=size` 即 WOULD_BLOCK(-1 重试) 或错误(closeTcpConnection 断连)（xcpEthTl.c:197-215）。大聚合段遇缓冲区边界有整体断连风险。
3. keepalive idle=5s/intvl=1s/cnt=3（xcpEthTl.c:325）：拔网线最坏 ~8s 释放 Sock 供新连接，方向正确。
4. assert 在 NDEBUG 下失效但均有运行期兜底（见 §5 守卫行）。
5. 部分头读取 n∈(0,4) 落兜底断连分支（xcpEthTl.c:358-363）；MSG_WAITALL 下理论不可达。
6. 队列 commit 一致性门 `data[1]==0xAA || data[0]>=0xFC`（xcpQueue64.c:694/750）：以首字节 PID≥0xFC 或 ODT 对齐字节区分 DTO/CRM 段—— clever 但与 2 字节 ODT 头配置互斥（当前 1024 DAQ 固定 4 字节头，安全）。

## 7. 实测重大异常：ECU 在 CONNECT 之后对所有后续命令沉默（集成层问题，非库规范差距）

事实矩阵（192.168.2.137:5559，xcplite 帧，脚本 PID 判定修正为 0xFE=ERR 后复测不变）：
- 每条**新**连接的第一个 CONNECT 必得 RES（服务器级 ctr 单调 03→13）；
- 同连接第二包起：GET_VERSION(0x00)/GET_STATUS(0xFD)/GET_COMM_MODE_INFO(0xFB)/SYNCH(0xFC)/DISCONNECT(0xFE)/二次 CONNECT(0xFF)/未知 0x55/TL F2 族 —— 一律零响应、不断线；
- 快速断开重连三轮全活（ctr 0a/0b/0c）⇒ accept 循环与 TX 健康。

与 stock 代码路径推演矛盾：CONNECT RES 发出前 SS_CONNECTED 必已置位（xcpLite.c:2458 早于组 CRM :2464-2471），第二包理应进 connected 分支 → switch default 至少回 ERR(0xFE,0x20)。可收窄至三种解释（按可能性排序）：
(a) ECU 集成替换/旁路了 handleXcpCommand 之后的链路（例如网关只在 TL 层代理 CONNECT 探活）；
(b) 接收线程在第一包处理后阻塞（Appl 回调链或自有泵送）；
(c) ECU 使用修改版 XCPlite 固件。

裁决手段（推荐顺序）：
1. ECU 侧 `XcpSetLogLevel(≥4)`（dbg_print.h:76 运行时级别；cal_test main.cpp:183 用法）复现 CONNECT→GET_VERSION：出现 `RX: CTR..DATA` 而无后续 ⇒ (b)/(c)；出现 `Command ignored because not in connected state` ⇒ SS_CONNECTED 未保持（查 their ApplXcpConnect/看门狗）；全无输出 ⇒ (a)。
2. ECU 侧 tcpdump 确认第二包确实到达内核（排除中间链路设备过滤）。
3. 比对固件符号表与本仓库版本。

## 8. 结论分级汇总（TCP 视角）

**符合**：帧头 LEN/CTR LE16、TCP=UDP 同格式、从机独立 CTR 全类型递增、单消息无 tail 合法、拼段 32bit 对齐、MAX_CTO/DTO 范围与如实上报、单连接+FIN 即 XCP disconnect、CONNECT CRO 精确长度校验、重复 CONNECT 复位语义、§5 可选命令静默权限、CRO 越界负响应守卫。

**偏离/缺陷**（按影响排序）：
1. 【集成级·待裁决】CONNECT 后全命令沉默（第 7 节）——不是库的规范差距，是 ECU 集成异常。
2. 【中·D2】TCP rx 不消费 FILL：依赖"自己从不发 FILL"的脆弱自洽，异端发送方即刻流错位。
3. 【中·D1】第二连接挂 accept 队列不拒绝不服务（backlog=5）："only accept one connection" 的宽松实现；上位机未 close 会造成新连接假死。
4. 【低-中·D6/D4】未连接非 CONNECT 命令、应用拒绝 CONNECT 均为静默——偏离基础标准 CMD_IGNORED 应发负响应的精神，黑盒诊断成本高（本次实证）。
5. 【低-中·D5/D3】CONNECT RES RESOURCE/COMM_BASIC 硬编码；MODE 位不协商恒 legacy。
6. 【低】RESUME 未实现、interleaved 未实现（均 optional）。
7. 【工程】无 TCP_NODELAY、send 无短写循环（第 6 节）。

## 9. 对 libxcp TcpTransport 的设计输入（由本报告推导）

1. 帧 = `struct.pack('<HH', len(payload), ctr)` + payload，payload 补齐到 4 字节（ECU 实测接受；亦对齐友好）。
2. 本地 CTR 每命令递增（同一连接内，不分命令类型）；重连重置与否按基础标准逐连接语义（建议每连接从 0 起并记录）。
3. 接收：读 4B 头 → 读 dlc B → **再吞掉 `(4-dlc%4)%4` 填充字节**（不吞则流错位，本地假服务器实测教训）。
4. PID 空间：0x00–0xFB DTO / 0xFC SERV / 0xFD EV / **0xFE ERR(code=payload[1])** / 0xFF RES。
5. 单连接语义：析构与所有失败路径显式 close；重连前先确保旧 socket 关闭（否则 D1 排队假死）。
6. 超时语义要宽容：对端"忽略"不发负响应（D6），Timeout 分类不能反推丢包。
7. 不要依赖 §5 探测命令做 slave discovery；直连已知 IP:port。
8. CONNECT RES 的 RESOURCE/COMM_MODE_BASIC 仅作参考位（D5），能力以实际命令试错为准。
