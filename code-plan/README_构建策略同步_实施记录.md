# README.md 构建策略同步记录

- **日期**：2026-07-18（第三轮收尾批次）
- **任务来源**：用户要求"修改了 cmake 的构建策略，现在来更新 README.md 文件"——把三轮 CMake 改造（A2L add_subdirectory 化 → A2L 必编化去开关 → EXAMPLES 自动跟随 SLAVE）的最终构建语义完整回写进 README。此前各轮只做了局部措辞修正，本次为全文对齐。

## 变更点（README.md）

1. §1 能力表：A2L 集成状态 "✅ 可选组件" → "✅ 必编组件"。
2. §2 结构树：根脚本注释 "4 个可选开关" → "核心库 + A2L 栈（必编）+ 3 个可选开关"；a2l-sdk 注释改为"主树经 add_subdirectory 直接编译（必编）"，build-sdk.ps1 标注"独立 SDK 开发入口"。
3. §2.1 分层架构表：A2L 组件行 "（可选）" → "（必编）"；依赖隔离段重写——保留"核心不依赖 A2L、A2lIsolation 门禁校验"语义，删除过时表述"主工程不编译任何 thirdparty 源码，只以 IMPORTED target 消费准备根产物"，替换为"A2L 栈源码经 add_subdirectory(thirdparty/a2l-sdk) 随主工程直接编译，不再消费预构建准备根"。
4. §2.2 模块文档索引：构建系统行描述补"3 个构建开关与 Boost 硬依赖"。
5. §3.1 环境要求：删除过时的"构建准备根需 Boost（经 -BoostRoot 传入）"；新增 **Boost 硬依赖**条目（CONFIG 包 locale/filesystem/process；自动发现优先级 `-DBoost_DIR` > `C:\boost\lib\cmake\Boost-*` > `BOOST_ROOT`；vcpkg 全局集成干扰与 `VCPkgLocalAppDataDisabled=true` 注意事项）；Python3 需求改挂在 A2L 专项测试下。
6. §3.2 Release 编译：命令前加说明"A2L 栈必编，常规配置无需额外开关"；新增两条注意——全新 build 目录首编要编译上游 a2llib（数分钟）、子模块未初始化时 configure 明确报错需 `git submodule update --init`。
7. §3.5 构建开关：消除上一批遗留的重复段落——"唯一新增依赖是 Boost…常规配置即最简形式"一段与 §3.2 命令重复，改为 Boost 自动发现规则的完整表述 + 手工指定示例；开关表维持 TESTS ON / XCPLITE_SLAVE OFF / EXAMPLES OFF（含自动跟随说明）。

## 验证

- 残留 grep：README 内不再有"可选组件/IMPORTED/准备根消费/-BoostRoot/4 个开关"等旧机制表述（仅存"不再消费预构建准备根"的历史对照句与 .deps-cache 无关"可选"）。
- 实测 README 所述裸命令 `cmake -B <dir> -S . -DLIBXCP_BUILD_TESTS=OFF` configure exit 0（Boost 自动发现 C:/boost/lib/cmake/Boost-1.86.0），验证目录已清理。
