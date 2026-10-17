# cpp_demo_V201 A2L 支持补全实施记录

> 状态：实施中。原始 `cpp_demo_V201.a2l` 保持只读；用户明确不修改 `thirdparty/a2llib`。完整标定验收基于经消歧的独立测试语料，不把原始重复 `C_delay_us` 的限制误报为已解决。

## 本轮修改

- SDK ABI 升至 8，新增 `TypedefCharacteristicDto` / `ListTypedefCharacteristics` 与 `MemorySegmentDto` / `ListMemorySegments`，分别导出 VALUE/CURVE 类型事实及静态 MEMORY_SEGMENT、XCP SEGMENT/PAGE 属性。
- bridge 将 TYPEDEF_CHARACTERISTIC 按 VALUE/CURVE 保留类型语义并展开结构叶子；提供固定轴、维度/步长和唯一段关联，增加元素物理值转换。只有固定轴 CURVE 单点进入写入候选，整条曲线及不规则/歧义布局保持拒绝。
- 新增经消歧 A2L fixture，供 TYPEDEF_CHARACTERISTIC、INSTANCE 权限、内存段/PAGE 和曲线事实回归；原始 A2L 中重复 typedef 仍不作范围/权限保证。
- `tools/xcp_master` 增加一维 MEASUREMENT 数组详情的 0 基索引及 8 项分页读取、固定轴 CURVE 单点读写；边界不合法时拒绝。标定写入前要求 A2L 与活动 XCP PAGE 均允许，预读/确认后再次核验活动页，写后回读期间继续检查页状态；不自动切页、不重试写入。
- 新增严格0基索引解析及策略测试；下一页起点使用饱和钳位并覆盖接近 `UINT64_MAX` 的索引，避免分页导航加法回绕。Python 3.9 不支持 `Path.write_text(..., newline=...)`，`tests/a2l_gen/gen_a2l.py` 改用 `Path.open(..., newline="")` 保持换行行为。

## 验证结果

- AppleClang Release 增量构建通过：`xcp_master`、`XcpMasterSymbolPolicyTest`、`A2lGolden`。
- 命令：`ctest --test-dir cmake-build-release-a2l --output-on-failure -R 'XcpMaster|A2lGolden|a2l_gen'`；结果 **18/18 通过**。覆盖 XCP Master 策略（含8项分页边界）、标定策略、A2L 生成器、A2L 黄金测试、CLI 帮助和参数拒绝用例。
- 构建仍有 GoogleTest 的 `char8_t` 转换警告及重复静态库链接提示，不影响上述构建/测试结果。

## 尚未验证

- 全仓库 Release 完整构建与 CTest；Windows/Linux 构建。
- FTXUI 真实终端中的索引编辑、分页、焦点、resize 事件集成。
- 真实 XCP Slave 联测：活动页查询、数组读取、VALUE/CURVE 点写入及回读。
- 为读写工作流补齐 mock 级零发送/页切换并发测试；现有策略单测不等价于协议端到端验证。
- 剩余跨主站/ECU页切换 TOCTOU 风险：本地 `command_mutex_` 不能锁住 ECU 或其他 XCP master。写入前后多次核页可发现部分变化，但若外部在最后一次检查与WRITE之间切页，错误页写入不可撤销；写后检查只能报告风险/不确定回读。
