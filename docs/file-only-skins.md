# 仅从文件加载皮肤

本轮从已交付3626f9d建立codex/file-only-skins，用户要求删除全部内置皮肤，将极客与草绿交付为外部可导入文件。保留.fcskin v1的归档/校验/角色/基础几何兼容；不将皮肤文件重新随程序预装。

## 使用

外观/管理皮肤中只保留系统默认及真实导入的本地包。选择“导入皮肤”，选.fcskin，导入完成后点“使用”；导入不会自动启用。两份包位于仓库根skin-format/releases/file-only-v1，均有明暗模式、四种流光配色及原素材制作记录。图片不含固定按键文字、候选或图标。极客的程序边框/硬阴影及按压由新客户端基础几何绘制；格式差异见该目录README。

没有本地包时键盘保持中性默认，可正常输入；删除正在使用/最后一本地包，先原子回退系统默认，再清理文件。原有本地包、词库、学习、剪贴板、AI/共享、字号、明暗、透明度、流光开关等偏好不清除。旧内置ID及隐藏/编辑记录只为解码兼容保存，不再展示或恢复主题；退休编辑损坏不阻塞新的本地管理。

## 验证与交付

证据目录 /workspace/ci-artifacts/fangcun-file-only-skins-2026-11-26/。实际编译/回归、包体/签名升级与直链结果在完成后追加；静态/框架/CI测试不冒称用户机器安装或真实厂商服务验收。

## Windows 实现与回归

Rust库兼容旧ID/隐藏和退休编辑原值，选中无文件/删最后包为system:default，旧内置写命令拒绝；原生与伴侣只用中性默认或包内配色/材质。删除4内置卡/editor/旧CSS材质分支、3张运行时PNG和原生木框/滚轴/CRT/LED绘制；加载中或缺图也不回到旧照片。既有本地包异步缓存、字体首键/恢复、文档事务与数据目录保留。

本机实际Rust逻辑88项通过，两个真实交付.fcskin在实际SkinLibrary验证/导入/选用成功并导出snapshot/resolved.ini；TypeScript/Vite和Linux Chromium109项前端检查通过（设置23、皮肤30、AI43、退休兼容13，含真实Rust空库DTO），输入11文件/删除状态前后不变。公开Build无安装器验证 [45763260506](https://github.com/ZhaoHaiBln/FangCunIME-Windows-Build/actions/runs/45763260506) 一次成功：MSVC、6项CTest、实际Tauri适配器、Windows Rust91项全过（0失败/忽略），完整日志保留。正式Windows与共享内容262文件投影一致，4退休目标确认不存在；原发布工作流SHA a861d82d2948d71b6eba9dac67162ae74ee38b19506bc26a0d75436a15ba9877不变，只在公开Build独立分支验证/构建，main与私有额度未使用。

## 实际 Windows 直链升级包

原正式构建45766019310已成功生成0.6.8，但其官方Azure下载节点在当前环境CONNECT阶段403，未读到产物，未声称其ZIP/digest通过。为完成持续授权的原tmpfiles直链交付，在公开Build独立分支codex/file-only-direct-delivery增加严格限定分支/path的构建端直传工作流，从同样262个源文件重新生成新EXE，未读取受阻Azure资源、未修改TLS或代理。原build.yml SHA仍为a861d82d2948d71b6eba9dac67162ae74ee38b19506bc26a0d75436a15ba9877，两个main不动；新增一次性交付workflow SHA为8b16c1d8bce585ea05395b7a208e39b69461ffb8e877f2712e3a7fe0af1e04fa，contents:read、timeout35、仅上传精确版本setup EXE，不上传源码/密钥/用户数据，不包含Azure读写或旧产物下载步骤。

新构建 [45768113779](https://github.com/ZhaoHaiBln/FangCunIME-Windows-Build/actions/runs/45768113779)，源9a854cf8e6284967043c9d148c37f9c7180deeb8（只比d5a0c46增加交付workflow，功能/共享262文件一致），MSVC、6项CTest、Tauri、Inno及直传步骤全部成功。新实际安装器47,725,680字节，SHA256 `0f5b470fc28a0360236905ffcfecd512f477863ff6486540419b0a5ffcb727dd`，来自runner本地输出；与旧无法读取的构建不声称同字节。实际CI Get-FileHash/长度/文件名/服务回复标记及14项压缩清单（含七份许可）核对通过；从真实tmpfiles landing完整回下载同字节数/SHA，PE与HTTP200/HEAD长度均过。回执为windows/verified-installer.json及direct-verification.json，明确没有Azure ZIP/digest验收。保持既有安装器身份、TSF/伴侣标识及数据目录；安装后按提示重启。

验证为真实构建与实际下载包静态检查，本机innoextract1.9不支持当前Inno loader2，未解包/执行该安装器，也未冒称设备安装、WebView2、硬件麦克风或付费服务验收。正式源码仍为codex/file-only-skins的99a3c03，后续提交仅交付文档。

## 皮肤文件下载与导入

临时服务对.fcskin实际返回422“Invalid file extension.”，因此只用传输ZIP承装两份原文件；格式仍为.fcskin v1，没有修改归档字段。FangCun-Geek-Grass-Skins-v1.zip为698,936字节，SHA256 `f7bb7c91cc23e56384fd28e6b5778c34b32a2805757f9fb544ed60ad0efa4f20`，仅含根目录fangcun.geek-mechanical.fcskin与fangcun.grass-mechanical.fcskin；CRC和内部文件字节/SHA与真实两端导入过的冻结文件完全一致。原服务上传/完整回下载/HTTP200和长度检查均过。先安装新版，再解压该ZIP，分别选择里面的.fcskin导入并点“使用”；不把外层多文件ZIP直接导入。公开Build固定commit的两条raw皮肤链接另已实际完整下载核验，记录在skins/github-raw-skin-downloads.json，可作长期备份。
