# CMake Presets 与 VS Code clangd 支持实施记录

## 修改内容

- 新增 `CMakePresets.json` 的 `clangd` Ninja configure/build preset，导出编译数据库，使用 Release，并关闭该 preset 的测试配置。
- 增加 VS Code CMake Tools 工作区设置与 CMake Tools、clangd 扩展推荐；仅将这两个共享配置文件加入版本控制。
- 更新构建系统文档，说明 preset、依赖及仅安装 Boost 静态库时的配置覆盖方式。

## 验证结果

- `cmake --preset clangd`：未完成配置。本机 Boost 仅有静态 filesystem/process 变体，而默认查找选择共享变体。
- `cmake --preset clangd -DBoost_USE_STATIC_LIBS=ON`：配置和生成成功；使用 AppleClang 与 Ninja，`LIBXCP_BUILD_TESTS=OFF`。
- `build/clangd/compile_commands.json`：生成成功，共 127 个编译数据库条目；包含 `libxcp` 的 `command_codec.cpp`、`udp_transport.cpp`、`xcp_master.cpp`。
- 按约定未构建目标、未运行测试。
