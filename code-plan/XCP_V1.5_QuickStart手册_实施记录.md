# XCP V1.5 Quick Start 手册 实施记录

日期：2026-10-23 ｜ 类型：文档 ｜ 代码变更：无（仅新增文档 + README 索引一行）

## 1. 交付物

- 新增 `docs/XCP_V1.5_QuickStart_标定与测量.md`（约 930 行）：XCP V1.5 标定与测量最小闭环 Quick Start 手册。
- `README.md` §7 文档索引增加该手册链接。

## 2. 定位（用户确认）

- 本手册是**规范总结，不是 libxcp 实现总结**（用户原话：「根据规范补齐规范，这个只是规范的总结非实现的总结」）。
- 正文纯规范内容，不带 libxcp 实现痕迹；TCP 与 UDP 双承载均按规范讲解（libxcp 当前 TCP 未实现不构成障碍，亦不构成手册内容裁剪理由）。
- 范围裁剪：只做"最基本的标定 + 测量"。刷写/编程、时间相关、块传输/交错模型、CAN/FlexRay/LIN、DBG/USER 命令、SET/GET_REQUEST 细节等明确排除（见手册 0.2 表）。

## 3. 依据文档

| 引用缩写 | 文件 | 备注 |
|---|---|---|
| PL | docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.md（9029 行） | Markdown 转换版存在图文错位（README §7），图表类结论均经原文行逐条直读核对；权威件为同目录 PDF |
| TL | docs/ASAM-AE-MCD-1-XCP-AS-Ethernet-Transport-Layer-V1-5-0.md（767 行） | 全文通读 |

写作纪律：手册中每个协议结论都有 PL/TL 节号或表号依据；无法直读核对的推断一律删除或软化措辞（见 §5 纠错清单）。

## 4. 手册结构

- 第 0 章 导读（用途/范围裁剪表/阅读路线/记法约定：位置从 0 计数、HEX、AG=Address Granularity）
- 第 1 章 分层模型（PL/TL 解耦、拓扑单主单从、版本协商 CONNECT Major + GET_VERSION 完整版本）
- 第 2 章 协议骨架（标准通信模型一问一答、状态机、Message/Packet 分层、CTO/DTO 识别字段 5 型与 DAQ_KEY_BYTE 宣告、时间戳、命令分层与 optional 规则、CONNECT/GET_STATUS/DISCONNECT 字节表、事件与服务码、错误码表与严重级 S0–S3、超时与 SYNCH）
- 第 3 章 标定最小闭环（Sector/Segment/Page 概念、SET_MTA/UPLOAD/DOWNLOAD、SHORT_UPLOAD/SHORT_DOWNLOAD、MODIFY_BITS、页切换、Seed&Key 与外部函数文件、端到端时序图、6 条常见坑）
- 第 4 章 测量最小闭环（ODT/DAQ list/事件通道概念栈、静态 vs 动态配置、FREE/ALLOC 四步曲、SET_DAQ_PTR/WRITE_DAQ、SET_DAQ_LIST_MODE、启停、DTO 解码、DAQ PACKED MODE（V1.5 新）、端到端时序图、6 条常见坑）
- 第 5 章 以太网 TL（TCP/UDP 寻址与连接语义对比、通信模型、Header LEN/CTR + Tail PACKET_ALIGNMENT、MAX_CTO/MAX_DTO 值域、TL 专用命令 GET_SLAVE_ID/EXT/SET_SLAVE_IP/GET_DAQ_CLOCK_MULTICAST、总线负载核算、选型建议）
- 第 6 章 A2L 接口（IF_DATA/AML 文件族、overrule 规则、在线一致性、V1.5 要点回顾）
- 附录 A 术语表（20 条，带首现节号）；附录 B 命令码速查（按组，Support 列照录 PL Tables 24–28 + TL Table 2）

## 5. 校对轮发现并修正的主要错误（溯源清单摘要）

定稿前对全文做了逐条溯源核对，修正的重大/易错点：

1. CTO 识别码：EV=0xFD、SERV=0xFC（旧稿误写 0xFB/0xFA）；事件码全集为 Table 22 的 0x00–0x0C + 0xFC–0xFF 体系（旧稿误用 0xE0 系）。
2. 命令必选性按 Mnemonic Category + §7.4 Tables 24–31 定稿：V1.5 中 SET_MTA/UPLOAD/SHORT_UPLOAD 均为 Standard optional；Calibration 组唯 DOWNLOAD mandatory；FREE/ALLOC 系存在"码表 mandatory for DAQ/STIM vs 节头 dynamic, optional"双重口径，手册如实注明。
3. SHORT_DOWNLOAD 响应误述删除（规范无特殊响应，"返回新 MTA"无据）。
4. Seed&Key 全表按 §7.5.1.9/10 逐字重写：UNLOCK 无 Mode 字节、首包 Length=密钥总长、key 校验失败→ERR_ACCESS_LOCKED 且 Slave 进 disconnected、re-CONNECT 后保护位复位须重新 GET_SEED；补 PL §9 外部函数接口与"字节序原样传递"纪律。
5. 版本兼容删除"同 Major 向后兼容"无据承诺，改为 Compatibility Matrix（§3.1.2）表述；CONNECT 版本字节仅 Major。
6. 识别字段型定为 5 型（Figures 49–53），DTO 型由 DAQ_KEY_BYTE bit7:6 宣告（Table 143）；PID_OFF 仅 absolute ODT 型且 PID_OFF_SUPPORTED=1。
7. ERR_RESOURCE_TEMPORARY_NOT_ACCESSIBLE(0x33) 语义改为规范原文（Slave 内 XCP Handler 暂时无资源），全文四处统一。
8. 超时机制按 §7.6.2 重写：t1–t7 来自 A2L；(Pre-Action, Action) 共试 2 轮；SYNCH 恒回 FE+0x00；严重级定义按 Table 227。
9. 删除"GET_STATUS 任何时候可发"、"UDP RESUME 按记录地址继续投递"等无原文依据的推断。
10. 交叉引用修正（0.3 阅读路线指向 5.7 等）。

## 6. 验证方式

- 非代码变更，无编译/测试项；docs/ 与 code-plan/ 均不在 CMake 构建路径内，不影响任何目标。
- 正确性验证=上述逐条溯源核对（每条结论对应 PL/TL 原文行或表号）；遗留不确定措辞已删除或标注"规范未规定"。
- 已知规范瑕疵照录并注明：TL §7.1 带宽算例存在 byte/s 与 bit 混单位表述（手册 5.7 已注）。
