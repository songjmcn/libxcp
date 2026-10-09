# =============================================================================
# build-sdk.ps1 —— A2L 栈一键构建脚本（设计文档 §5.3 / R4.3 + 批次9 工程合并）
#
# 步骤：
#   0. 校验 a2llib 子模块 SHA（防止上游漂移导致 ABI/语义意外）
#   1. 配置 + 构建 liba2l_sdk —— 单一工程从零产出全部构件：
#      uchardet 桩 → 上游 a2l(static) → liba2l.dll → libxcp_a2lbridge.lib
#      （串行 cmake --build；本机 MSBuild 并行/-j 会触发 ZERO_CHECK 竞争故障，
#       见根 CMakeLists.txt 头部说明）
#   2. 把产物拷贝到"准备根"（prepared root）：<OutRoot>/msvc-x64-<cfg>/
#        bin/liba2l.dll
#        lib/liba2l.lib  lib/libxcp_a2lbridge.lib
#        include/liba2l/*.hpp|*.h  include/libxcp/a2l/*.hpp
#      该目录为独立 SDK 分发场景的准备根；主树现默认经 add_subdirectory 直接
#      编译 A2L 栈（必编组件），本脚本不再是主树构建的前置步骤。
#
# 用法示例（相对路径即可，脚本自行换算绝对路径；仓库内禁止写死绝对路径）：
#   pwsh -File thirdparty/a2l-sdk/build-sdk.ps1 -BoostRoot C:\boost\lib `
#        -Config Release -OutRoot build/liba2l-prepared
#
# -BoostRoot 指向含 lib/cmake/Boost-* 布局的 Boost 安装前缀（或 .../lib 本身）。
#
# 批次9 起不再需要 uchardet 假包：桩库是 SDK 工程子项目（build-shim.ps1 与
# uchardetConfig.cmake.in 已退役，CMAKE_PREFIX_PATH 注入随之删除）。
#
# 环境注意：本机 MSBuild 在同时存在 HTTP_PROXY/http_proxy 大小写重复环境变量时
# 抛 ArgumentException（MSB6001），所有 cmake 调用都经清代理的子进程封装执行。
# =============================================================================

[CmdletBinding()]
param(
    # Boost 安装根（其下应有 lib/cmake/Boost-<ver>/BoostConfig.cmake）
    [Parameter(Mandatory = $true)]
    [string]$BoostRoot,

    # 构建配置：Release（默认）或 Debug
    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release',

    # 产物输出根目录（相对路径基于仓库根）
    [string]$OutRoot = 'build/liba2l-prepared',

    # 期望的子模块提交号（不匹配时警告并继续；置空跳过检查）
    [string]$ExpectedSubmoduleSha = 'c31057498555cbb29ab48e718329ca3163c10221'
)

$ErrorActionPreference = 'Stop'

# --- 路径基准 ------------------------------------------------------------------
$RepoRoot   = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$SdkSrc     = Join-Path $RepoRoot 'thirdparty\a2l-sdk'
$A2lSrc     = Join-Path $RepoRoot 'thirdparty\a2llib'
$SdkBuild   = Join-Path $RepoRoot "build\liba2l-sdk-$Config"

Write-Host "== liba2l SDK 构建 =="
Write-Host "  仓库根   : $RepoRoot"
Write-Host "  配置     : $Config"

# --- 清代理变量的子进程调用封装 --------------------------------------------------
# 返回 hashtable：Out = cmake 退出码，Stdout = 输出文本（失败时回显用）
# 注意两点本机环境隔离：
#   1) MSBuild 对同时存在 HTTP_PROXY/http_proxy 大小写重复 env 键会抛
#      ArgumentException（MSB6001），必须清除；
#   2) 本机启用了 vcpkg 全局 user-wide MSBuild 集成，它向所有链接行注入
#      "...\installed\x64-windows\lib\*.lib" 通配符伪项（MSBuild 不展开，
#      LNK1104）与 /LIBPATH。该集成由 %LOCALAPPDATA%\vcpkg\vcpkg.user.targets
#      触发，仅当 VCPkgLocalAppDataDisabled 非空时跳过 —— 必须置位该变量。
#      注意：VS 生成器下 Boost 静态库经 a2l.lib 的 /DEFAULTLIB 指令被隐式拉入，
#      其裸文件名不在任何搜索路径中，因此构建前显式把 <BoostRoot>/lib 加入 PATH。
function Invoke-CmakeClean([string]$Arguments, [string]$ExtraPath = '') {
    $pathset = if ($ExtraPath) { "set PATH=$ExtraPath;%PATH%&& " } else { '' }
    $cmd = "$pathset" +
           "set http_proxy=&& set https_proxy=&& set HTTP_PROXY=&& set HTTPS_PROXY=&& " +
           "set no_proxy=&& set NO_PROXY=&& " +
           "set VCPkgLocalAppDataDisabled=true&& " +
           "cmake $Arguments"
    $out = & cmd /c $cmd 2>&1 | ForEach-Object { "$_" }
    return [pscustomobject]@{ Out = $LASTEXITCODE; Stdout = ($out -join "`n") }
}

# --- 0. 子模块校验 --------------------------------------------------------------
if ($ExpectedSubmoduleSha) {
    Push-Location $A2lSrc
    try {
        $sha = (& git rev-parse HEAD).Trim()
    } finally {
        Pop-Location
    }
    if ($LASTEXITCODE -ne 0 -or -not $sha) {
        throw "无法读取 thirdparty/a2llib 的提交号（子模块未初始化？）"
    }
    Write-Host "  a2llib   : $sha"
    if ($sha -ne $ExpectedSubmoduleSha) {
        Write-Warning "子模块提交号与锁定值不一致：期望 $ExpectedSubmoduleSha，实际 $sha。请人工确认后再消费。"
    }
}

# --- Boost_DIR 解析（在 lib/cmake 下找 Boost-* 目录）-----------------------------
$BoostLib = if (Test-Path (Join-Path $BoostRoot 'cmake')) { $BoostRoot }
            else { Join-Path $BoostRoot 'lib' }
$BoostCmakeDir = Join-Path $BoostLib 'cmake'
if (-not (Test-Path $BoostCmakeDir)) {
    throw "找不到 Boost cmake 目录：$BoostCmakeDir（用 -BoostRoot 指定正确安装前缀）"
}
$BoostCfg = Get-ChildItem $BoostCmakeDir -Directory -Filter 'Boost-*' |
            Sort-Object Name -Descending | Select-Object -First 1
if (-not $BoostCfg) {
    throw "$BoostCmakeDir 下没有 Boost-* 配置目录"
}
$BoostDir = $BoostCfg.FullName
Write-Host "  Boost_DIR: $BoostDir"

# --- 1. liba2l_sdk（含 uchardet 桩子工程 + 上游 a2l + liba2l.dll + a2lbridge） ----
New-Item -ItemType Directory -Force -Path $SdkBuild | Out-Null
$r = Invoke-CmakeClean "-S `"$SdkSrc`" -B `"$SdkBuild`" -G `"Visual Studio 17 2022`" -A x64 -DCMAKE_BUILD_TYPE=$Config `"-DBoost_DIR=$BoostDir`""
if ($r.Out -ne 0) { Write-Host $r.Stdout; throw "liba2l_sdk 配置失败（exit $($r.Out)）" }
$r = Invoke-CmakeClean "--build `"$SdkBuild`" --config $Config" "$BoostRoot\lib"    # 串行，不加 -j；PATH 补 Boost 库目录供 /DEFAULTLIB 裸名解析
if ($r.Out -ne 0) { Write-Host $r.Stdout; throw "liba2l_sdk 构建失败（exit $($r.Out)）" }

# VS 生成器按配置分子目录，Ninja 单配置直接落根 —— 两处都找一遍；
# 子工程（如 a2lbridge）产物落 <子目录构建树>/<Config>/，根目录找不到时递归兜底。
function Find-Artifact([string]$Name) {
    foreach ($dir in @((Join-Path $SdkBuild $Config), $SdkBuild)) {
        $hit = Join-Path $dir $Name
        if (Test-Path $hit) { return (Resolve-Path $hit).Path }
    }
    $rec = Get-ChildItem $SdkBuild -Recurse -Filter $Name -ErrorAction SilentlyContinue |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($rec) { return $rec.FullName }
    return $null
}

$DllPath = Find-Artifact 'liba2l.dll'
$LibPath = Find-Artifact 'liba2l.lib'
$BridgeLibPath = Find-Artifact 'libxcp_a2lbridge.lib'
if (-not $DllPath -or -not $LibPath) {
    throw "未找到 liba2l.dll / liba2l.lib（DLL 导出可能为空，检查 LIBA2L_MAKE_SHARED）"
}
if (-not $BridgeLibPath) {
    throw "未找到 libxcp_a2lbridge.lib（a2lbridge 子工程未挂载？）"
}

# --- 2. 拷贝到准备根 ---------------------------------------------------------------
if ([IO.Path]::IsPathRooted($OutRoot)) { $OutAbs = $OutRoot }
else { $OutAbs = Join-Path $RepoRoot ($OutRoot -replace '/', '\') }
$Prepared = Join-Path $OutAbs "msvc-x64-$($Config.ToLower())"
foreach ($sub in @('bin', 'lib', 'include\liba2l')) {
    New-Item -ItemType Directory -Force -Path (Join-Path $Prepared $sub) | Out-Null
}
Copy-Item $DllPath (Join-Path $Prepared 'bin') -Force
Copy-Item $LibPath (Join-Path $Prepared 'lib') -Force
Copy-Item $BridgeLibPath (Join-Path $Prepared 'lib') -Force
Copy-Item (Join-Path $SdkSrc 'include\liba2l\*') (Join-Path $Prepared 'include\liba2l') -Force
# bridge 公开头树：include/libxcp/a2l/*.hpp（主树 IMPORTED libxcp::a2lbridge 用）
Copy-Item (Join-Path $SdkSrc 'a2lbridge\include\libxcp') (Join-Path $Prepared 'include') -Recurse -Force

Write-Host ""
Write-Host "== 完成 =="
Write-Host "  准备根（独立 SDK 分发场景）: $Prepared"
Write-Host "  产物：bin/liba2l.dll  lib/liba2l.lib  lib/libxcp_a2lbridge.lib"
Write-Host "        include/liba2l/*   include/libxcp/a2l/*"
