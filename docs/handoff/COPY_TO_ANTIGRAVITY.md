# 复制给 Antigravity

把下面代码块内的文字复制到 Antigravity，并打开整个 `FangCunIME-Windows` 文件夹。后面追加你这次具体想修改的界面、交互或功能。

```text
你现在接手“方寸输入法 Windows”现有工程，请按我接下来给出的具体修改要求开发。

工程与仓库：
- Windows 10/11 系统级输入法，C++20 + CMake + SQLite + zlib + Windows TSF COM DLL。
- Windows UI 是 Win32 / Direct2D / DirectWrite。
- 唯一 Windows 开发和 CI 仓库：https://github.com/ZhaoHaiBln/FangCunIME-Windows
- 使用 main 分支。不要在私有 cursor-apps-archive 触发 Windows 构建，不要修改 Android 工程。
- 工程和安装器当前版本 0.1.0。不要把它当成 MyClash、Go 项目或普通网页应用。

开始时先阅读：
1. ANTIGRAVITY.md
2. docs/handoff/PROJECT_MAP.md
3. docs/handoff/BUILD_AND_TEST.md
4. docs/handoff/KNOWN_ISSUES.md
5. docs/handoff/SOURCE_BASELINE.json

工作要求：
- 基于现有工程实现我的具体需求，先简短说明涉及的文件和验证方式，再执行。
- 保留源码模块结构；不要为了一个界面修改重写输入引擎、TSF 生命周期或更换技术栈。
- 保留异步查询取消、会话 generation、焦点切换和 composition 清理，防止串字。
- 保留 Ctrl/Alt/Win 组合键透传、候选选择、空格/数字/退格/Esc/Enter 的既有规则。
- 普通升级和卸载保留 %LOCALAPPDATA%\FangCunIME 下的学习、个人词典与设置。清除数据必须来自用户明确要求。
- assets 下三份词库和第三方许可属于完整工程，不要丢失。不要把用户 user.db、设置或构建缓存提交进仓库。
- 当前源码安装脚本仍有旧 Android 默认路径，按交接文档显式传 -AssetsDir .\assets；需要修这个问题时再有针对性修改。
- Windows 修改后执行 CMake 构建及 CTest；涉及 TSF、UI 或安装时补充相应 Windows 宿主验证。
- 说明哪些是自动化通过、哪些是实际操作通过，未执行的项目写清楚，不用模拟结果替代。
- 最终给出修改文件、行为变化、测试结果、剩余问题与公开仓库中的构建产物入口。

交接前源码基线：028be5cf70bac494f0005172bfe52c491d085c6c。
该基线 Windows CI：https://github.com/ZhaoHaiBln/FangCunIME-Windows/actions/runs/43800795464 ，已成功。
交接文档提交可能晚于这个源码基线；开始修改前核对实际 HEAD，保留他人已有改动。

本次我希望修改的具体内容：
（在这里补充需求）
```
