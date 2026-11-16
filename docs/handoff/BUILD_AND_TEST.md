# 构建、测试与安装

所有命令从包含 `CMakeLists.txt` 的仓库根目录执行。Windows 功能修改以公开仓库 `ZhaoHaiBln/FangCunIME-Windows` 的 `main` 为准。

## 环境与依赖

- Windows 10/11 x64，Visual Studio 2022 的“使用 C++ 的桌面开发”组件和 Windows SDK。
- CMake 3.20 或以上，支持 C++20 的 MSVC。
- 首次 Windows 配置需要联网下载 SQLite 3.46.1 和 zlib 1.3.1。版本来源已写在 `CMakeLists.txt`。
- 使用 CI 相同路线时需要 Ninja 和已初始化的 MSVC x64 开发者环境。
- 制作 Setup 需要 Inno Setup 6；仅编译 DLL 和测试不需要 Inno Setup。

## Visual Studio 2022 路线

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DFANGCUN_BUILD_TSF=ON -DFANGCUN_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

DLL：`build\Release\FangCunTSF.dll`。测试目标：`fangcun_tests`；CTest 名称：`fangcun_core_and_keyhandler_tests`。

## 与公共 CI 相同的 Ninja 路线

在 x64 Native Tools / Developer PowerShell 环境执行：

```powershell
cmake -S . -B build-x64 -G Ninja -DCMAKE_BUILD_TYPE=Release -DFANGCUN_BUILD_TSF=ON -DFANGCUN_BUILD_TESTS=ON
cmake --build build-x64
ctest --test-dir build-x64 --output-on-failure
```

DLL：`build-x64\FangCunTSF.dll`。不要混用 Visual Studio 与 Ninja 的同一个构建目录。

## 安装、升级和卸载

用管理员 PowerShell，从仓库根目录执行 Visual Studio 产物安装：

```powershell
.\installer\install.ps1 -BuildDir .\build\Release -AssetsDir .\assets
```

如果按 Ninja 路线构建，替换为：

```powershell
.\installer\install.ps1 -BuildDir .\build-x64 -AssetsDir .\assets
```

升级（先关闭正在使用旧输入法的宿主程序，必要时切换其他输入法）：

```powershell
.\installer\upgrade.ps1 -BuildDir .\build\Release -AssetsDir .\assets
```

Ninja 产物的升级同样使用 `-BuildDir .\build-x64`。升级时 DLL 被宿主占用可能失败，现有脚本会报错，不能仅因完成了编译就宣称已安装新 DLL。

默认卸载保留用户数据：

```powershell
.\installer\uninstall.ps1
```

`-PurgeUserData` 会另外删除当前账户的学习、设置和个人词典，只在用户明确要求清除时使用。

- 程序及基础词库：`%ProgramFiles%\FangCunIME`。
- 用户数据：`%LOCALAPPDATA%\FangCunIME`，包含 `settings.ini` 和 `user.db`。
- 安装使用系统 `regsvr32` 注册 TSF/COM，完成后使用 `Win+Space` 选择输入法。
- 解压 CI portable 包后，可使用其 `installer/` 目录中的安装/升级 BAT；这依然需要管理员注册，不是免安装输入法。

## 打包 Setup

公共 CI `.github/workflows/build.yml` 已实现：构建 DLL → CTest → `dist/` 组装 → Inno Setup → 上传 Setup 和 portable 两份 artifact。可直接在 GitHub Actions 页面运行该公开仓库的工作流。

如需本机打包，在仓库根目录执行：

```powershell
New-Item -ItemType Directory -Force -Path dist/assets, dist/installer, out | Out-Null
Copy-Item build-x64/FangCunTSF.dll dist/
Copy-Item assets/dict.db, assets/pinyin-v1.dbz, assets/pinyin-v1.json dist/assets/
Copy-Item installer/*.ps1, installer/*.bat dist/installer/
Copy-Item THIRD_PARTY_NOTICES.md, licenses/rime-ice-LICENSE.txt dist/
& "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" /DMySourceDir="$PWD\dist" /O"$PWD\out" "installer\FangCunIME.iss"
if ($LASTEXITCODE -ne 0) { throw "Inno Setup 构建失败" }
```

此命令使用 Ninja DLL；使用 Visual Studio 路线时只把 DLL 的源路径改为 `build/Release/FangCunTSF.dll`。Setup 输出在 `out/`，版本由 `installer/FangCunIME.iss` 定义。

## 验证范围

源码基线 `028be5cf70bac494f0005172bfe52c491d085c6c` 对应的 [CI 43800795464](https://github.com/ZhaoHaiBln/FangCunIME-Windows/actions/runs/43800795464) 已通过 Windows x64 编译、核心/按键测试、Setup 构建和产物上传。本轮仅整理交接文件，未重跑安装或输入法宿主操作。

该 CI 不证明已经完成 COM 注册、实际候选窗输入、跨宿主切换、DPI、用户数据迁移、安装/升级/卸载交互或 x86 宿主兼容性测试。修改相应部分后需补充对应实机验收；详见已知问题文档。
