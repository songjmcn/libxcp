# xcp_master 使用手册实施记录

## 变更范围

- 将 `tools/xcp_master/README.md` 改为中文使用手册，覆盖工具定位、构建依赖、启动参数、UDP/TCP 选择、TUI 键位、交互命令、POLL/DAQ/标定流程、组件数据流、安全边界、能力限制和常见问题。
- 增加 UDP、TCP 显式 Profile、TCP v1.5 对齐和严格自动探测启动示例，并链接项目内 UDP/TCP Master 示例及项目总览。
- 内容依据工具当前 README、`main.cpp`、策略头文件和 CMake 配置编写；未修改程序代码或接口。

## 验证结果

- 已对照源码复核启动参数和默认值、TCP `auto` 的三态验证及退出码、交互键位、命令、DAQ 的 `FREE_DAQ` 确认和写入后的回读/不回滚边界。
- `git diff --check`：通过；输出仅包含工作区既有文件的 LF/CRLF 转换警告。
- 按确认的文档校核方案，未运行构建或测试。
