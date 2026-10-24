# a2llib 子模块 URL 修改记录

## 修改内容

将 `.gitmodules` 中 `thirdparty/a2llib` 的 URL 从上游 HTTPS 地址改为：

`git@github.com:songjmcn/a2llib.git`

`thirdparty/XCPlite` 配置保持不变。

## 验证结果

- 执行 `git config --file .gitmodules --get submodule.thirdparty/a2llib.url`，结果为 `git@github.com:songjmcn/a2llib.git`。
- 未运行测试套件：本次仅修改 Git 子模块元数据。
