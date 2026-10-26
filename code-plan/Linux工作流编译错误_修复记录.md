# Linux workflow 编译错误修复记录

## 问题

GitHub Actions 运行 `40078265415` 的 `Build` 步骤在编译
`tests/xcp_master_transport_selection_test.cpp` 时失败：

```text
tools/xcp_master/transport_selection.hpp:477:36:
error: ‘any_of’ is not a member of ‘std’
```

`transport_selection.hpp` 调用了 `std::any_of`，但没有直接包含声明该算法的
标准头文件 `<algorithm>`。其他平台或翻译单元的传递包含可能掩盖此问题，Linux
构建则暴露了头文件依赖不完整。

## 修改

- 在 `tools/xcp_master/transport_selection.hpp` 添加 `<algorithm>`。
- 不改传输选择逻辑、运行时行为或 Linux workflow 配置。
- 复用已有多实例去重测试，不增加重复测试用例。

## 验证

- GCC 13 使用 C++20 对 `transport_selection.hpp` 执行独立语法编译：通过。
- GCC 13 使用 C++20 对 `tests/xcp_master_transport_selection_test.cpp` 执行语法检查：通过。
- Release CMake 配置未能完成：本地缺少 `thirdparty/a2llib/CMakeLists.txt` 子模块内容，
  且 Boost `process` CMake package 未找到，因此无法运行完整目标构建及单元测试。
