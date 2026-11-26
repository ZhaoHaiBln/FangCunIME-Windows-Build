# 方寸皮肤包 `.fcskin` v1

本规范同时用于 Android 和 Windows，版本 1 一经交付保持兼容。以后设计皮肤时交付预览图与可直接导入的 `.fcskin` 文件，用户不需要编辑代码。扩展名是 `.fcskin`，内容是标准 ZIP，支持 STORE / DEFLATE。

## 固定清单

根目录必须有无 BOM 的 UTF-8 `manifest.json`，不允许未知字段：

```json
{
  "format": "fangcun.skin",
  "schemaVersion": 1,
  "id": "fangcun.example",
  "name": "皮肤名称",
  "version": 1,
  "author": "作者",
  "license": "原创",
  "baseSkin": "mechanical",
  "assetScale": 3,
  "fontFamily": "sans",
  "pressOffsetDp": 1,
  "preview": "preview.png",
  "themes": {
    "light": { "colors": {}, "glow": [], "images": {} },
    "dark": { "colors": {}, "glow": [], "images": {} }
  }
}
```

`colors` / `glow` 在此仅省略示意，实际包必须完整，详见 `contract-v1.json` 和示例。`id` 为 3–64 字节的小写命名空间，匹配 `^[a-z][a-z0-9]*(?:\.[a-z0-9][a-z0-9_-]*)+$`；名称 1–40、作者 0–48、license 0–120 个 Unicode 字符，不允许控制字符或孤立代理码点，名称不能只有空白。`author` / `license` 可省略为空。`version` 是 1–2147483647 整数。`baseSkin` 只能是 mechanical、typewriter、mahjong、brutal；仅复用基础布局和回退，不修改按键、手势或词库。`assetScale` 为 1–4 整数，推荐 3；`fontFamily` 为 sans、serif、mono；`pressOffsetDp` 为有限的 0–2 数值。深浅模式均必须显式提供。

两种模式各自必须给出以下 18 个不透明 `#RRGGBB` 颜色，不允许别名、表达式或网址：

`background, surface, border, function, text, muted, accent, onAccent, pressed, toolbar, candidateBackground, candidateText, selection, onSelection, keyText, keyHint, functionText, codeText`。

`glow` 是恰好四个 `#RRGGBB` 颜色。是否打开流光、字号、粗体、透明度和高度继续由用户设置决定，包不能覆盖它们。默认建议使用黑体（sans），不将字母、候选、图标或按键文字画进素材。

## PNG 素材与绘制

只支持普通、静态、非交错、8-bit RGB / RGBA PNG。不要使用 Android `.9.png`、整张键盘截图、JPEG、WebP、SVG、字体文件、脚本或 CSS。每张图像最大 2048×2048，文件最大 4 MiB，预览最大 1 MiB。一个包所有不同 PNG 的像素总和最大 16777216（解码 RGBA 64 MiB）。

`images` 可以省略任意角色，但不能出现未知角色。省略角色用本包颜色绘制，不能漏出旧内置照片材质。固定八个角色：

| 角色 | 用途 |
| --- | --- |
| deck | 键盘底板 / 桌面窗口底板 |
| toolbar | 顶部工具条 |
| code | 拼音编码条 |
| candidates | 候选条背景 / Windows 候选窗口 |
| selection | 选中候选背景 |
| keyNormal | 普通键帽 |
| keyFunction | 功能键帽 / 桌面辅助按钮 |
| keyAction | 发送、回车和主要操作键帽 |

每个存在的图像角色是：

```json
{"file":"light/key_normal.png","scale":"nineSlice","insets":[18,18,18,18]}
```

`scale` 只能是 stretch、contain、tile、nineSlice。`insets` 必须是 `[左, 上, 右, 下]` 四个非负整数（PNG 像素）；非 nineSlice 必须全零。左右之和必须小于图片宽，上下之和小于图片高。绘制边缘尺寸是 `inset / assetScale` dp；太小的目标等比缩小边缘，任何素材不得画出组件边界。tile 按 `imagePx / assetScale` dp 的自然尺寸重复；contain 居中等比完整显示；stretch 拉伸到组件范围。九切片保留边缘，中心拉伸。

素材只能是静态材质。运行时叠加层顺序为底板、用户开启的流光、键帽、文字及操作反馈，保证流光继续可用、字不重复、键位不改变。图像校验和解码在后台完成；打字、绘制与动画每帧只访问已缓存数据。自定义 PNG 不再套内置照片染色。

## 归档边界

压缩包最大 24 MiB，展开数据最大 48 MiB，最多 64 条目，清单最大 64 KiB。v1 不支持 ZIP64、多卷或自解压包。只允许 `manifest.json`、声明的 preview 和 images PNG，以及它们的父目录条目；不接受额外文件、重复条目、重复 JSON 字段、加密、链接、绝对路径、`..`、反斜杠或外部网址。图片路径长度不超过 96，匹配 `^(?:[a-z0-9_-]+/)*[a-z0-9_-]+\.png$`。同一个 PNG 可用于多个角色，不重复计像素。

为保证跨平台可用，任何目录名和 PNG 文件名去除扩展名后，都不能是 Windows 保留名 `con`、`prn`、`aux`、`nul`、`com1`–`com9`、`lpt1`–`lpt9`。

所有数据校验完成后才安装；任何失败保留原选择和原文件。相同归档摘要重复导入为幂等成功；同 ID 不同内容需要先删除旧包，不能静默覆盖。导入不会自动应用，用户点选后才使用。

## 管理与内部接口（两端协作约定）

当前客户端按用户2026-11-26要求不再提供命名内置皮肤。无本地包时使用中性系统默认；删除当前或最后一本地包时，先在同一次设置事务中切到 `system:default`，再清理素材，保留明暗、输入、字体、流光与 AI 设置。旧隐藏/编辑记录只作兼容读取，不会恢复已移除的主题。导入开关默认开，关掉只禁止新导入，不禁用已安装包。

内部安装根为各自应用数据目录的 `skins/<归档SHA256>/`，文件仍用已校验的包内相对路径。安装不可变；目录内另有导入器生成、最多 16 KiB 的 `resolved.ini` 给 Windows 原生组件。注册表保存在现有设置的 `skin_library_v1`（单行 JSON），结构为 `{"version":1,"importEnabled":true,"hiddenBuiltins":[],"packs":[{"id":"fangcun.example","hash":"64位小写sha256"}]}`。本地选择另有 `custom_skin_id` / `custom_skin_hash`；原 `skin` 在本地包激活时保存兼容基础几何ID，系统默认保存各平台默认ID；基础ID不再代表可选内置皮肤。逻辑移除先提交注册表和回退选择，再清理不再引用的素材；清理失败可留作后续垃圾清理，不得重新出现在列表。

Windows 原生 resolved.ini 字段固定：`schema_version=1`、`id`、`hash`、`base_skin`（mechanical/typewriter/mahjong/brutal）、`asset_scale`、`font_family`、`press_offset_dp`；每个模式 `light_` / `dark_` 后接 18 个颜色名的 snake_case、`glow_0` 到 `glow_3`。图像字段例如 `light_image_key_normal=light/key_normal.png`、`light_scale_key_normal=nineSlice`、`light_insets_key_normal=18,18,18,18`；其余八个角色同理转 snake_case。颜色统一存 `#RRGGBB`。不存绝对路径。原生组件校验大小、相对路径及字段后，后台 WIC 解码并缓存；切换主题时更新，按键和绘制不读盘。

Android 公共数据 API 由 `data.skin` 提供 `SkinPackManifest`、`SkinPackTheme`、`SkinPackImage`、`SkinLibrarySnapshot`、`SkinPackRepository`；绘制层从当前本地 ID + hash 异步取得 manifest / PNG。保持现有 `SkinPreset`，映射 mechanical→MECHANICAL、typewriter→IVORY_TYPEWRITER、mahjong→JADE_MAHJONG、brutal→BRUTAL；Windows 的兼容基础枚举分别为 WoodType、Typewriter、Terminal、Brutal。

Windows 伴侣命令统一 `skin_library_snapshot`、`skin_set_import_enabled(enabled,revision)`、`skin_set_builtin_hidden(id,hidden,revision)`、`skin_select(id,revision)`、`skin_delete(id,revision)`、`skin_import_pick(revision)`。当前选择 ID 用 `system:default` / `local:<manifest.id>`；旧 `builtin:<平台原生ID>` 和内置隐藏/编辑写命令只保留安全拒绝，不提供恢复入口。文件选择取消返回 null。所有变更都返回最新 snapshot，并以库 revision 防止迟到操作覆盖新状态；失败不显示成功。图片仅通过受控资源接口返回，禁止任意本机路径访问。
