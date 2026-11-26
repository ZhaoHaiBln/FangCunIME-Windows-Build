# 内置皮肤编辑与统一标题栏

本轮从已交付Android1.14.20 / Windows0.6.6的 `fc15fc8` 建立 `codex/editable-builtin-skins`。用户要求现有内置皮肤全部可编辑，手机标题/工具栏按参考图统一。原位编辑保留四张皮肤卡，不生成重复副本；默认材质与动态键帽继续使用原绘制。

## 使用

在外观页选择某张内置皮肤的「编辑」。浅色和深色分别预览，配色与四种流光颜色分别保存；字体和按压下沉由该皮肤两种模式共用。可编辑常用/高级18种语义配色，但当前编辑范围不含替换背景或键帽图片。「保存并使用」明确保存并启用；取消或放弃草稿不写设置。皮肤卡「恢复原样」直接移除该张皮肤的编辑，编辑页「将草稿恢复原样」需保存才生效；恢复操作不改其他皮肤、深浅模式、字号、粗体、透明度、输入、AI或共享设置。颜色必须是 #RRGGBB，错误草稿不能保存。

内部稀疏编辑仅记录用户覆盖的字段；省略值沿原样。公开 `.fcskin v1` 归档、导入和缺图语义保持兼容，本地包优先于内置编辑。技术合同见仓库根 `skin-format/BUILTIN-EDITING.md`，素材制作仍以原 `SPEC-v1.md` 为准。本次没有把内置照片或程序绘制伪导出为完整可移植素材包。

## 验证范围

证据目录 `/workspace/ci-artifacts/fangcun-editable-skins-2026-11-26/` 保留初轮失败、修复、代码指纹与最终报告。框架/浏览器及CI验证不能代替用户机器安装、真实麦克风、厂商账号或跨应用手感验收。本轮交付Android1.14.21（46）/Windows0.6.7，保持既有安装身份与数据目录；实际包、升级、下载结果见下文记录。

## Windows

四张内置皮肤卡各有编辑和恢复操作，原木纹/打字机/CRT/机械分支保留，独立组件预览不套当前本地包。前端18项编辑回归（含真实Rust快照）、原30项皮肤、21项设置与41项AI，共110项通过；TypeScript/Vite生产构建通过。首轮实际失败修复了编辑按钮冒泡触发错误skin_select、Brutal不透明候选下条遮住编辑颜色；同时修复单色流光恢复误清其余颜色及打字机预览字色对比。原来只计皮肤/预览的选择器保留严格数量，仅排除新增操作按钮/编辑预览。

内部INI单独引用安全的builtin-styles目录，后台有界加载，缺省逐字段继承、每帧不读盘，本地包优先。实际DirectWrite回归覆盖已保存深色木框字体在首键立即生效、逐字及仅改颜色复用格式、恢复后不需另打字。实际profile读取、替换、清空、损坏、预算和本地隔离复用原Windows缓存测试。

本地实际Rust逻辑模块88项通过，最小数导出调整后相关7项复验通过；4份真实Rust导出INI由C++逐字段读取一致。最终原生CI [45752862609](https://github.com/ZhaoHaiBln/FangCunIME-Windows-Build/actions/runs/45752862609) 成功：MSVC、6项CTest、TypeScript/Vite、实际Tauri适配器和90项Rust测试通过。首轮CI的原生组件/6项CTest与前端已过，Rust依赖解析因应用版本替换误动ZIP依赖失败，已准确恢复依赖0.6.6后复验；应用仍为0.6.7，未更新第三方代码。

正式源码Windows部分与公开Build同名分支253个对应文件逐字一致，实际验证来源为 `ecb7008` / `cbfb8cf`。原发布工作流SHA256仍为 `a861d82d2948d71b6eba9dac67162ae74ee38b19506bc26a0d75436a15ba9877`；没有覆盖main或使用私有Actions额度。两端功能修改和Android最终补验全部通过后，由root触发既有公开发布工作流，未改发布流程。

## 实际 Windows 安装器

原发布工作流 [45755764227](https://github.com/ZhaoHaiBln/FangCunIME-Windows-Build/actions/runs/45755764227) 在公开Build的 `cbfb8cf` 成功，7分45秒；MSVC、6项CTest、Tauri与Inno全部通过。安装器0.6.7为47,890,680字节，SHA256 `54e87a04c48f0edfaef0cde94aef6a73216105e7455001a3ca8e6166bdf5fcb6`。只下载setup artifact15087483013，归档47,348,891字节，SHA256 `25e21099215eec9e2573a001fef15f3586be0eb793aac2b35519593d414b967f`，匹配GitHub官方digest；ZIP完整性、唯一EXE成员、PE与实际CI压缩文件清单（含七份许可）均通过。253个对应Windows/共享文件与正式源码 `90ef5d7` 一致，安装身份和数据目录沿原值，主分支未改。

实际安装器未在用户Windows执行，本机官方innoextract1.9仅支持Inno≤6.0.5，不能解新旧同样采用的loader revision2；已以CI真实打包日志和官方归档digest验证，未把静态检查冒称设备安装/WebView2/麦克风验收。完整报告及回执位于artifact目录的 `windows/verification.json`、`verified-installer.json`。

已按既有直链交付授权仅上传该正式EXE到原tmpfiles服务，完整回下载同字节数/SHA256，实际直链HTTP200及长度通过；源码、用户数据和密钥未上传。安装器与下载回执同上述windows证据目录。
