# MSBuild CI 改用 Ninja 并行编译

## 背景
Windows CI 使用 `Visual Studio 17 2022` 生成器，且 `cmake --build` 未加并行参数、
未启用 `/MP`，项目级与文件级均为串行，编译很慢。

## 修改内容（仅 `.github/workflows/msbuild.yml`）
1. 新增 `ilammy/msvc-dev-cmd@v1`（x64）步骤，等价于加载 `vcvars64.bat`。
2. 新增 Ninja 检查步骤（缺失时用 choco 安装）。
3. Configure：改为 `-G Ninja`，指定 `cl` 编译器与 `CMAKE_BUILD_TYPE`。
4. Build：`cmake --build build --parallel`。
5. Test：去掉 `-C $env:BUILD_TYPE`（单配置生成器）。

## 未改动
CMake 源码、Linux/macOS 工作流。已确认 CMake 中无依赖 `Release/` 子目录的写法。
`CMakeLists.txt` 头注释中关于 VS 生成器 `-j` 的说明仅针对本地 VS 构建，保留。

## 测试结果
待推送后由 MSBuild 工作流验证（仅验证 MSBuild，不验证 Linux/macOS）。
本地为 Linux 环境，无法运行 Windows 构建。
