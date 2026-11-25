# 本地皮肤管理与 `.fcskin v1`

2026-11-25，工作分支 `codex/skin-pack-management`，基于稳定化源码 `82e7980`。用户要求安卓与 Windows 同时实现，并固定今后设计皮肤的交付格式。本轮只改源码、制作规范和测试；不构建或上传安装器，不修改当前版本字段、签名、main 或用户数据。

## 使用与制作

外观新增皮肤管理：内置皮肤隐藏后可恢复，至少留一个；本地 `.fcskin` 导入后明确点“使用”，可删除。删除当前包先保存回退选择，再清理文件。允许导入开关只影响新导入，已有包继续可用。导入、隐藏、选择和删除通过同一保存事务与 revision 检查，失败不假报成功，也不清除深浅模式、字号、粗体、透明度、流光、输入、AI 和共享配置。

权威制作说明为仓库根 `skin-format/README.md`、`SPEC-v1.md`、`contract-v1.json`；编辑器结构检查使用 `schema-v1.json`。固定交付预览图和 `.fcskin`，两端共用。标准 ZIP 中包含清单、预览、两种明暗配色与可选静态 PNG 材质，八类角色支持拉伸、包含、平铺和九切片。文字、图标、候选、按键和触控不画死在运行时素材；不导入整张键盘截图或执行脚本。最小模板只是中性占位，不是新的皮肤设计。

Python 3 标准库工具：

```sh
python3 skin-format/tools/fcskin.py init my-skin
python3 skin-format/tools/fcskin.py build my-skin my-skin.fcskin
python3 skin-format/tools/fcskin.py validate my-skin.fcskin
```

先完整检查清单、ZIP、PNG 和预算，才写入不可变 SHA256 目录。同包幂等，同 ID 不同内容需先删旧包。两端共用 87 个带字节数和 SHA256 的原始测试包：10 个有效、77 个无效，包括压缩描述符、重复字段、图像边界、路径和 Windows 保留名。制作工具 13 项测试通过。

## 验证边界

本地证据目录 `/workspace/ci-artifacts/fangcun-skin-packs-2026-11-25/` 保留源码指纹、最初失败及修复后的日志。离线界面或本地服务测试不代表手机手感、Windows WebView2、实际文件选择器、真实麦克风、付费 AI 或跨设备服务已验收。本轮不交付应用安装包，当前 Android 1.14.19（44）和 Windows 0.6.5 仍是原版本字段。

## Windows 实现

Rust 后端管理注册表、选择与原子保存，主窗口专属导入/变更命令，三个伴侣窗口可读取快照。原生文件选择器取消返回 null；异步受控 `fcskin` PNG 资源协议只提供已登记文件，CSP 仅增加图片来源。Tauri 代码与现有 AI、共享设置兼容。

TSF 与原生回退设置窗口读取有界 `resolved.ini`，后台 WIC 解码、拒绝路径重解析跳转，D2D 缓存绘制与设备丢失重建；无逐键或逐帧读盘。候选、编码、选中、字体和流光统一使用当前包；缺图回退本包颜色。真实 Rust 导出 INI 与 C++ 读取的 36 色、8 流光和图片参数逐项兼容。

本机完整实际逻辑模块 Rust 宿主测试 82/82 通过，仅框架事件/系统剪贴板为替身；界面 92 项（皮肤30、设置21、AI41）与生产 TypeScript/Vite 构建通过。真实 Rust 快照/PNG 原样供给前端的额外 12 项正控通过。测试模拟 IPC/资源递送，不冒称真实 WebView2 或 TSF 手感。

Windows 原生验证 [45596560774](https://github.com/ZhaoHaiBln/FangCunIME-Windows-Build/actions/runs/45596560774) 成功，实际源码为公开镜像 `7e0cc77`（正式源码 `5b7df1a` 的 Windows 与共享格式投影）。MSVC TSF、实际 Tauri 适配器与 TypeScript/Vite 均编译通过；5/5 CTest，包括真实 WIC/缓存，以及 85/85 Rust 测试通过，无失败或忽略。使用公开 Build 镜像同名分支，发布工作流 SHA256 `a861d82d2948d71b6eba9dac67162ae74ee38b19506bc26a0d75436a15ba9877` 保持不变，只编译和测试，无安装器。报告附件下载曾被文件主机拒绝，已完整取得任务日志并核对上述实际结果，没有为取报告重跑构建。
