# Windows 0.4.0 直角皮肤核验

基于实际交付0.3.0的公开Build提交 `3919941817438b4de6dc338c7189531ffad20485`，本轮公开构建提交为 `57695d340492ed4b6cfe6067e774b954083960a6`。四套皮肤、旧ID迁移、原生候选/设置绘制及Tauri界面均与正式工程一致，73个源码/测试/资源文件逐字核对；58个原输入、共享协议、数据与工作流文件未变，安装身份、标识和数据目录不变。

[实际构建 44069558460](https://github.com/ZhaoHaiBln/FangCunIME-Windows-Build/actions/runs/44069558460) 全绿，包含MSVC C++、核心/按键测试、Rust/Tauri、Inno及两个artifact上传；仅执行一次现有公开手动工作流，未修改main、工作流或计费。本地另通过26项Rust测试（含旧ID真实INI往返）、TypeScript/Vite、原生API类型检查与整组KeyHandler测试。

9张Chromium预览通过真实页面点击确认四皮肤、两类候选预览、设置页和剪贴板主题同步，Tauri IPC采用最小可控替身；不把该预览称为真实Windows GUI。自身HWND设置DWM不圆角偏好，内部绘制和CSS均直角，宿主软件外观不受影响。

实际ZIP已完整回下载，官方digest与ZIP完整性检查通过。伴侣和TSF内容均为x64，伴侣实际FileVersion为0.4.0.0。Inno解包程序为32位且其FileVersion为0；安装版本按构建输入AppVersion0.4.0及产物名核对，不误称其stub携带0.4版本字段。两份词库二进制逐字不变，拼音JSON仅Windows checkout的CRLF差异。

| 产物 | 字节数 | SHA256 |
| --- | ---: | --- |
| `FangCunIME-0.4.0-win64-setup.exe` | 47,467,578 | `2fda214b880ea4d2eadb3e1f7142b2cb8bb969229922548aeabf399398e6e332` |
| setup ZIP | 46,925,507 | `3620ff47cbe9b189b991a6c73700dffa97778fcb461466c3b20f795dff38e716` |
| portable ZIP | 52,595,017 | `95a43208c45458a2084e2a05cb04a49af94a91ecc3b928dad74ab2b9a93125a8` |

[安装包](https://github.com/ZhaoHaiBln/FangCunIME-Windows-Build/actions/runs/44069558460/artifacts/14322486908)和[便携包](https://github.com/ZhaoHaiBln/FangCunIME-Windows-Build/actions/runs/44069558460/artifacts/14321818866)均需解压。临时快下链接有短有效期，应从真实服务刷新；不拼造签名URL。

本轮未运行实体Windows GUI、DPAPI往返或用户中转账号，不重复执行已通过构建。共享与输入行为维持0.3.0范围；直角皮肤及旧设置映射见[双端说明](../../docs/square-skins.md)。
