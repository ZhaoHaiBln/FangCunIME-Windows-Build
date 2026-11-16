# 已知问题与后续验收

以下以交接前 `028be5cf70bac494f0005172bfe52c491d085c6c` 为准。本轮整理文件不修复这些代码问题，避免把未验证的行为更改混进交接。

## 已确认需要留意

| 项目 | 证据与影响 | 当前做法或后续修改方向 |
| --- | --- | --- |
| 源码安装脚本的默认词库路径仍指向 Android | `installer/install.ps1`、`upgrade.ps1` 的 `$AssetsDir`；缺资源时部分复制会跳过 | 从源码安装必须显式传 `-AssetsDir .\assets`；后续可统一为独立 Windows 仓库路径并检查资源是否齐全 |
| 源码目录 BAT 的 fallback 使用旧工程路径 | `installer/安装-方寸输入法.bat` 等 | 源码开发使用文档中的显式 PowerShell 命令；CI 解压包 BAT 会识别根目录 DLL/词库 |
| 安装器链接仍指向旧归档仓库 | `installer/FangCunIME.iss` 的 `MyAppURL` | 这是安装器元数据，不是开发入口；后续改为公开 Windows 仓库链接 |
| 当前 CI 只构建 x64 | workflow 的 `arch: x64`、Ninja；ISS 的 x64 架构要求 | 不宣称兼容所有 32 位宿主；需要时单独实现和验证 x86 注册/部署 |
| 升级时可能遇到被加载的 DLL | `upgrade.ps1` 注销后移动旧 DLL，失败会抛错 | 关闭宿主、切换输入法后升级；后续可完善占用提示及失败恢复 |
| 个人词典当前读取最近 500 条 | `core/src/UserStore.cpp` 的 `loadPersonalWordsLocked` 查询 | 这是现有实现边界；若用户要求大个人词典，先评估查词和加载策略 |

## 修改后需要在 Windows 观察的行为

1. `Win+Space` 能选择方寸；中英切换、输入法重新激活正常。
2. 小鹤、全拼、九宫格与混输保持；空格、1–9、退格、Esc、Enter 符合既有规则。
3. Ctrl/Alt/Win 快捷键透传；没有编码时不截获宿主正常按键。
4. 快速输入、切焦点、切文档、取消 composition 后，不出现旧查询结果串入新会话。
5. 候选窗位置、点击、DPI、深浅色及设置保存/重新读取正常。
6. 至少在记事本、浏览器输入框和常用编辑器中输入；不同宿主分别记录结果。
7. 升级保留 `user.db`、`settings.ini`；默认卸载保留个人数据，注册和程序文件状态正确。

## 交接中已核对的事实

- 主仓库当前 Windows 构建在公开 `FangCunIME-Windows/main`，不再沿用私有 `cursor-apps-archive` 的 Windows 构建。
- 当前基线最近成功 CI 为 [43800795464](https://github.com/ZhaoHaiBln/FangCunIME-Windows/actions/runs/43800795464)。用户之前提供的 [43795856219](https://github.com/ZhaoHaiBln/FangCunIME-Windows/actions/runs/43795856219) 对应更早提交，不能混作同次结果。
- 三份基础词库已在源码包中；用户个人数据不属于交接资源。
- 自动化测试与打包成功不等于所有系统输入法交互都已经实机通过；不要在报告中补写没有执行的结果。
