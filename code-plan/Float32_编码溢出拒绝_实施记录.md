# 浮点编码溢出拒绝实施记录

## 修改内容

- `FromPhysical` 编码 `FLOAT32_IEEE` 时，若有限输入超出 `float` 可表示范围，返回 `ConversionOutOfRange`，避免编码成无穷值。
- 同步拒绝 `FLOAT16_IEEE` 的溢出编码，避免生成无穷位模式。
- 在 `A2lGolden` 增加 `1e40` 的 FLOAT32 溢出回归用例。

## 验证结果

- 修改后的转换源文件通过 C++20 编译。
- 修改后的转换源文件通过 clang-format 检查。
- `git diff --check` 通过。
- A2L 黄金 fixture 生成成功。
- 未能运行 `A2lGolden`：CMake 配置因环境缺少 Boost CONFIG 包（locale、filesystem、process）而失败。
