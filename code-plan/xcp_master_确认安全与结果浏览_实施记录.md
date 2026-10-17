# XCP Master 确认安全与结果浏览实施记录

## 范围与决定

- 用户本轮要求：先修确认安全并补零写入测试，再修标定状态隔离与结果浏览；本轮不做 Windows/Linux/macOS 平台终端验收。沿用已获批的 FTXUI/A 档标定架构，不修改 Boost、A2L 子模块或协议核心公共接口。
- 安全边界以“危险操作开始下发”作为不可逆的提交点：确认许可检查和实际下发现在处于同一确认互斥临界区，提交前的取消、stop/quit、退出或标定选择变化使许可失效；已经进入网络发送的 XCP 命令不能撤回。`FREE_DAQ` 可能清空 ECU 全部动态 DAQ 表，取消时不调用 `MeasurementSession::Prepare()`。同步 XCP 发送占用该临界区期间，stop/取消 UI 调用可能等待当前请求完成，这是安全修复带来的响应性边界。

## 实施变更

- `tools/xcp_master/tui_model.hpp` 的 `ConfirmationGate` 增加单次 ticket，旧回答不能批准下一次请求；拒绝后续 Y 覆盖取消，stop/退出即使晚于 Y、早于下发也优先取消；新周期唤醒旧周期等待者。审查发现初版 `Claim` 后解锁、再调用 `send` 存在 TOCTOU，已改为 `Dispatch` 在同一互斥区完成校验及同步危险操作。`SendIfConfirmed` 是写入和 DAQ 配置共用的一次性代理。`tools/xcp_master/main.cpp` 在后台命令队列接受 stop/quit 时撤销许可，确认提示展示写入变量、当前值、目标值或 FREE_DAQ/profile/观测数量；写入串行锁发送点及 DAQ Prepare 前重新校验许可；Prepare 后若收到 stop/退出，不再启动 DAQ。已在途网络请求仍按原同步调用完成/超时，不能宣称 stop 可撤回在途操作。
- 表单提交改为后台队列中的类型化名称/目标/选择代际快照，避免用 tab 拼接文本和晚到的读写污染当前表单。`CalibrationFormModel` 在选项变化或再次搜索时清除目标草稿、最近读取值及旧状态；后台回读只有选择名称及代际均匹配才回填。标定菜单使用 FTXUI `MenuOption::on_change`，其回调为异步入队；为避免菜单索引已经更新但回调尚未运行时按钮拿到旧目标，读写按钮会先同步菜单当前选项再抓取快照；切换时在释放表单锁后撤销确认，写入发送前再复查选择。
- `list all` 结果在命令结果视图单独保留完整目录快照，与普通结果/日志的 512 行有界缓存隔离；完成构造后一次性发布。四视图的 PgUp/PgDn 各自保留 18 行窗口；完整目录从首行开始回看，普通命令返回普通结果面板。观测、普通结果、日志仍保持有界，不把结果历史误称为永久存档。`tools/xcp_master/README.md` 更新操作说明与不可逆提交边界。

## 测试与限制

- `tests/xcp_master_tui_model_test.cpp` 新增 ticket 过期/重复、取消后晚到 Y、Y 后 stop/quit 与退出优先、一次性危险操作代理的零调用断言（分别模拟 `WriteMemoryBytes` 和 `FREE_DAQ` effect），以及正常一次提交；还用跨线程测试验证新 ticket 唤醒旧等待者、下发临界区与取消互斥。测试为工具内同一发送代理的确定性假 effect，不是实车协议帧测试。
- 新增 1000 条完整结果 > 512 行的逐页可达性、有界诊断行淘汰、视图独立滚动/历史稳定/缩短夹紧、A→B→A 代际失效、切换/重新搜索清目标草稿与旧回读抑制的模型测试。未自动执行真实 FTXUI 终端事件驱动/会话生命周期联测；未经授权不向 Slave 发送实际写入或 FREE_DAQ。
- AppleClang Release 聚焦构建 `cmake --build cmake-build-release-clang --target xcp_master XcpMasterSymbolPolicyTest -j 4` 通过。原构建树的 GoogleTest 本地 FetchContent 指向已不存在的目录；将 `FETCHCONTENT_SOURCE_DIR_GOOGLETEST` 改为本仓库现有 GCC 构建树下载的 GoogleTest 源后重新 configure 成功；此路径只保留在构建缓存，不写入源码。
- 最终 `ctest --test-dir cmake-build-release-clang -R 'CalibrationFormModelTest|CalibrationSelectionTest|PanelViewportTest|ConfirmationGateTest' --output-on-failure` 13/13 通过；更宽过滤 `-R 'XcpMaster|Confirmation|PanelViewport|Calibration(Form|Selection)|BoundedLineBuffer|LatestObservation|CommandHistory|ViewSelection'` 运行 62 项、0 失败、1 项按原定义 Skipped（含核心 Master 集成与工具 CLI smoke）。`clang-format --dry-run --Werror` 检查本轮 3 个 C++ 文件及 `git diff --check` 通过。未针对本轮变更运行全量 CTest，也未执行用户暂缓的平台验收。

## 保留的后续验证

- 若要求更强的端到端证明，可用模拟 XCP transport 对取消分支断言无 `DOWNLOAD/SHORT_DOWNLOAD` 和 `FREE_DAQ` 协议帧，并用可控事件循环验证焦点/确认/退出时序；当前 effect-proxy 单测仅证明提交门控，不证明终端事件和设备行为。
- Windows/Linux 跨平台终端构建及交互验收暂不执行，依照用户当前选择保留为后续事项。
