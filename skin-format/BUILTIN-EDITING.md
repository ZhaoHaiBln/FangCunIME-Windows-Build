# 内置皮肤原位编辑（历史兼容记录）

2026-11-26后续用户明确要求删除全部内置皮肤，当前客户端已改为中性系统默认＋外部 `.fcskin v1`。以下保存格式仅用于兼容旧数据，不再提供编辑/恢复/应用入口，旧记录不触发材质加载；新皮肤从文件导入。文件制作与使用见 `releases/file-only-v1/README.md`，归档规范仍为 `SPEC-v1.md`。

内置皮肤的编辑直接叠加在原绘制上，保留照片材质、圆键/胶囊、CRT 和各皮肤的动态尺寸处理。默认或恢复后的皮肤使用原值，不产生重复的本地皮肤副本。

公开 `.fcskin v1` 清单、角色、导入和缺图回退规则保持不变。本文描述应用内部编辑设置；它不作为可导入的素材包格式发布。完整内置素材提取与可移植导出没有混入本次设置保存流程。

## 内部设置

单独的 `builtin_skin_edits_v1` 保存以下单行 JSON：

```json
{"version":1,"styles":{"brutal":{"fontFamily":"sans","pressOffsetDp":1.5,"themes":{"light":{"colors":{"toolbar":"#F6F3EC","accent":"#B8F500"},"glow":["#D9F58B","#8FDCC5","#E5B7CC","#EAD695"]}}}}}
```

四个键使用各平台现有内置 ID；Android 为 mechanical、ivory_typewriter、jade_mahjong、brutal，Windows 为 wood_type、typewriter、terminal、brutal。只允许这四个条目。根 version 为整数1，UTF-8 数据最大32KiB，拒绝重复、未知或非法字段。style 各字段可省略；省略值继承该内置皮肤原值：

- fontFamily 为 sans、serif、mono，省略保留原字体；仍保留用户字号和粗体。
- pressOffsetDp 为有限的0–2数值，省略保留原按压。
- themes 只接受 light、dark，各自 colors 是 `.fcskin v1` 18个语义色的任意子集，每个为不透明 #RRGGBB；glow 存在时必须完整4色。两种模式分别编辑。

此设置原文参与皮肤 revision。保存和恢复沿现有事务与并发检查：取消不写入，失败不显示成功；保存并使用同时完成选内置/解除隐藏/清本地包选择，其他设置保留。恢复只清该条目。正在使用本地包时，内置编辑不覆盖本地包。

## Windows 原生派生文件

每个内置 ID 的设置引用 `builtin_style_<id>=<sha256>`，指向应用私有 `builtin-styles/<id>/<sha256>/resolved.ini`。摘要来自 id、NUL 与规范化 style JSON。先写不可变小文件，再在同一次设置事务提交编辑 JSON 和引用；不使用本地包 skins 目录或 custom_skin_id。

派生 INI 最大16KiB，必须有 schema_version=1、builtin、hash；可选 font_family、press_offset_dp；每种模式 light_/dark_ 后接颜色 snake_case 或 glow_0–3（全部四个或全部无）。不接受图片字段、外部地址、重复或未知键。原生后台读取并校验预期 ID/hash 和目录边界，缺省项保留当前内置值；绘制和打字不读盘。异步迟到结果不得覆盖新皮肤或恢复后的状态。
