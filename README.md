# 方寸输入法 Windows 版（FangCun IME for Windows）

基于方寸输入法 Android 1.5.1（`865fade`）独立构建的 Windows 10/11 系统级输入法，采用 **C++20 + CMake + SQLite + Windows TSF (Text Services Framework)**。

## 工程结构

```
方寸输入法-FangCunIME-Windows/
├── CMakeLists.txt              # 跨平台核心与 Windows TSF 构建脚本
├── core/                       # 零 Win32 依赖的 C++20 核心引擎（小鹤/全拼/九宫格/128码组句/混输/学习/个人词典）
├── tsf/                        # Windows TSF COM DLL (ITfTextInputProcessorEx, KeyEventSink, CompositionSink, CandidateWindow)
├── installer/                  # install.ps1 / upgrade.ps1 / uninstall.ps1
├── tests/                      # C++20 自动化回归测试（对齐 dict.db 与 pinyin-v1.dbz）
├── THIRD_PARTY_NOTICES.md      # rime-ice GPL-3.0 声明
└── licenses/                   # GPL-3.0 许可文本
```

## 构建指南（Windows x64 / Visual Studio 2022）

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DFANGCUN_BUILD_TSF=ON -DFANGCUN_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

若需同时兼容 32 位宿主程序，可额外构建 `-A Win32` 产物并用 `SysWOW64\regsvr32.exe` 注册。

## 安装、升级与卸载（管理员 PowerShell）

- **安装**：`.\installer\install.ps1`（部署 `FangCunTSF.dll` 与词库至 `%ProgramFiles%\FangCunIME`，执行 `regsvr32 /s`，随后按 `Win+Space` 选择「方寸输入法」）
- **升级**：`.\installer\upgrade.ps1`（安全注销并替换 DLL，保留 `%LOCALAPPDATA%\FangCunIME` 词频与个人词典）
- **卸载**：`.\installer\uninstall.ps1`（注销 COM/TSF 并清理程序目录；加 `-PurgeUserData` 可同时清理本机学习数据）

## 键盘行为（MVP）

- **小鹤双拼（默认）**：`wojbtmhfkdxb` → `我今天很开心`
- **全拼**：`wojintianhenkaixin` → `我今天很开心`
- **空格**：提交首选（混输英文候选自动追加空格）；**数字 1–9**：在有效候选状态下选词
- **Backspace**：有编码时删编码，无编码时交给宿主
- **Esc**：取消当前编码，保留宿主正文；**Enter**：有编码时原文上屏，无编码时交给宿主
- **Ctrl / Alt / Win**：组合键全部透传给应用与系统
- **焦点/文档切换**：自动递增会话代次并取消后台查询，防止串字
