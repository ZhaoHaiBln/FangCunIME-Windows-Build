# 0.3.0 构建交付核验

本轮共享功能的编译源码为正式仓库提交 `103acce86d4729798d7b7da493bdbde81f6692cc`。Windows 基准为用户指定的 Build/main `0bb5a10c9a423ce812d92871d55cd89c296d9980`，公开 Build 分支对应提交 `3919941817438b4de6dc338c7189531ffad20485`。72个编译源文件逐字一致；公开投影仅调整接入说明链接。原37个 core/tsf/tests/assets 文件、Inno AppId 与现有工作流保持不变。

[公开构建 44051691650](https://github.com/ZhaoHaiBln/FangCunIME-Windows-Build/actions/runs/44051691650) 已通过 MSVC C++、核心/按键测试、完整 Rust/Tauri 和 Inno 打包；本地另有24项 Rust 逻辑/协议测试、TypeScript/Vite及Windows native FFI类型检查通过。没有修改 main、工作流、计费或历史仓库，没有使用私有 Actions 配额。

实际 setup/portable ZIP 已完整下载，与 GitHub 官方 artifact digest 相同，ZIP 完整性通过。桌面伴侣与 TSF DLL 均为 x64 PE；Inno 安装器的解包程序为32位 PE，安装内容为 x64。两份词库二进制与来源逐字相同，拼音 JSON 元数据只有 Windows checkout 的 CRLF 换行差异。

| 产物 | 字节数 | SHA256 |
| --- | ---: | --- |
| `FangCunIME-0.3.0-win64-setup.exe` | 47,452,527 | `8b9948149443f13cdcbcd8fef99457a797ad2e328dc0724fde3d9a0fb70ed0c6` |
| setup artifact ZIP | 46,910,708 | `c15576a655917f3264a8dfae0217619d5c851252f58a9ba1f55a269507560e16` |
| portable artifact ZIP | 52,591,061 | `68464a326c32649df15e4763c948b7d65eb2d3312dc08e4618189081520412f5` |

安装器见 [setup artifact](https://github.com/ZhaoHaiBln/FangCunIME-Windows-Build/actions/runs/44051691650/artifacts/14314192923)，便携包见 [portable artifact](https://github.com/ZhaoHaiBln/FangCunIME-Windows-Build/actions/runs/44051691650/artifacts/14314013494)。下载后先解压；artifact 有保留期限，临时直链另有短有效期。

这些结果证明源码和交付包的构建/静态完整性，不等于实体 Windows GUI、DPAPI往返或用户中转账号的跨网验收。待发仅内存队列、失败需手动重发；配对与限制见 [使用说明](clipboard-sharing.md)。Android 1.11.0的固定签名与19→20升级记录见 `android/AGENTS.md`，两端实际配对需分别填写自己的设备令牌。
