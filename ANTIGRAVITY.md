# 方寸输入法 Windows · Antigravity 交接入口

> 合仓说明：正式源码入口为 [ZhaoHaiBln/FangCunIME](https://github.com/ZhaoHaiBln/FangCunIME)，本目录是 `windows/` 工程根。根 CI 入口与历史来源见 [根 README](../README.md) 和 [迁移记录](../MIGRATION.md)。原独立 Windows 仓库与旧 CI 链接保留为历史证据；构建、安装命令从本目录执行。

这是一份可独立打开的 Windows 源码工程。请在 Antigravity 中打开本仓库下包含 `CMakeLists.txt` 的 `windows/` 目录。

下载源码 ZIP 后可直接解压打开、修改和构建。若需要由 Antigravity 提交修改到 GitHub，建议在它的终端克隆仓库后打开该目录，保留远端和提交历史：

```powershell
git clone --branch main https://github.com/ZhaoHaiBln/FangCunIME.git
cd FangCunIME/windows
```

## 先确认工程

| 项目 | 当前事实 |
| --- | --- |
| 产品 | 方寸输入法 Windows，Windows 10/11 系统级输入法 |
| 开发与 CI 仓库 | [ZhaoHaiBln/FangCunIME](https://github.com/ZhaoHaiBln/FangCunIME)，工程根为 `windows/` |
| 历史 Windows 来源 | [FangCunIME-Windows](https://github.com/ZhaoHaiBln/FangCunIME-Windows)，保留原源码与 CI 证据 |
| 工作分支 | `main`，按用户要求在此维护 Windows 端 |
| 技术栈 | C++20 / CMake / SQLite / zlib / Windows TSF COM DLL |
| Windows UI | Win32、Direct2D、DirectWrite；候选窗与设置窗 |
| 工程和安装器版本 | `0.1.0`，分别见 `CMakeLists.txt` 与 `installer/FangCunIME.iss` |
| 整理前源码基线 | `028be5cf70bac494f0005172bfe52c491d085c6c` |
| 该基线已通过的 Windows CI | [43800795464](https://github.com/ZhaoHaiBln/FangCunIME-Windows/actions/runs/43800795464) |

原独立 Windows 仓库的交接提交只添加文档、源码校验清单和 README 入口，没有移动源文件，也没有更改输入、候选、词库、注册或安装行为。合仓保留该源码基线及上述历史 CI 证据；本轮没有运行新的 CI。Windows 工程所需三份词库位于自身 `assets/`，不依赖旁边的 Android 工程。

## 按此顺序阅读

1. [复制给 Antigravity 的提示词](docs/handoff/COPY_TO_ANTIGRAVITY.md)：可直接复制正文。
2. [源码地图](docs/handoff/PROJECT_MAP.md)：按修改目标定位文件。
3. [构建、测试与安装](docs/handoff/BUILD_AND_TEST.md)：Windows x64 的准确命令。
4. [已知问题与验收范围](docs/handoff/KNOWN_ISSUES.md)：哪些是已确认的问题，哪些只是尚未实测。
5. [源码基线清单](docs/handoff/SOURCE_BASELINE.json)：交接前应用文件 SHA256、版本和 CI 记录。

## 最短构建路线

在安装了 Visual Studio 2022 C++ 桌面开发工具、Windows SDK 和 CMake 的 Windows x64 开发环境中，从本仓库的 `windows/` 目录执行：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DFANGCUN_BUILD_TSF=ON -DFANGCUN_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

如需安装测试，在管理员 PowerShell 中从同一目录执行：

```powershell
.\installer\install.ps1 -BuildDir .\build\Release -AssetsDir .\assets
```

`-AssetsDir` 要显式填写：当前脚本的默认值仍是旧 Android 工程路径。CI 使用 Ninja，DLL 在 `build-x64` 而非 `build\Release`。两种生成器的完整流程见构建说明。

## 继续开发时的边界

- 使用现有 C++20 / TSF 工程；这里不包含 MyClash，也不是 Go 或 Android 项目。
- 保留异步查询取消、会话 generation、焦点切换清理和组合键透传行为。
- 保留 `%LOCALAPPDATA%\FangCunIME` 中的用户设置、学习和个人词典；普通升级和卸载不应擅自清空。
- 构建和 CI 都在方寸合仓 `ZhaoHaiBln/FangCunIME` 的 `main` 维护，入口为根 `.github/workflows/windows-build.yml`；旧归档分支和原公开 Windows 仓库仅保留迁移备份与历史，不在那里触发新的 Windows 构建。
- 修改后的验证应区分 C++ 测试、Windows 编译打包和实际宿主输入法操作。当前 CI 未证明所有宿主或安装交互都正常。

源码 ZIP 用于修改工程。要运行现有输入法，应使用上述 CI 中的 Setup 产物；名称包含 portable 的包也需要管理员注册 TSF，不能视为免安装应用。
