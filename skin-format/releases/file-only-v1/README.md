# 极客机械、草绿机械：外部皮肤文件

这两份 `.fcskin v1` 是从已交付 Android 1.14.21（源码 `3626f9d`）的原有配色与素材转换而来，未另画一套风格。包可以在安卓和 Windows 的皮肤管理中导入，导入后点「使用」才应用。它们是外部可删除文件，不再作为应用内置皮肤。每份包均包含 Light / Dark 和原来的四种流光配色；流光开关、字号、粗体、高度、透明度仍由输入法设置控制。

- `fangcun.geek-mechanical.fcskin`：极客机械，纯直角、实线框、右下硬阴影和 2dp 对角按压。
- `fangcun.grass-mechanical.fcskin`：草绿机械，原白色/草绿照片键帽及原拼音、候选背景，按压为 1.5dp 竖直位移。
- 同名 `-preview.png`：真实原材质的深浅对照板，不是真机截图或完整键盘截图。显示文字只在预览图里；包内键帽、底板和面板材质均不含文字、候选或图标。
- `export-report.json`：原图 SHA256、去边后的尺寸/九切片边距、包体积和摘要。

## 转换范围

草绿普通键和空格使用原 `ime_key_white`；功能键及发送使用原 `ime_key_grass`。原 Android `.9.png` 只去掉 1px 标记外框，完整保留内部图像和透明阴影，转成普通 RGBA PNG，并明确保存九切片边距。原资源来自 xxxhdpi，因此 `assetScale=4`。深色 PNG 精确沿原代码的 Lighting RGB 乘色系数转换，Alpha 不变。材质 PNG 不再含 Android NinePatch 控制框或编译块。

标题栏沿本轮统一的平整栏设计，使用包内 `toolbar` 配色，未把旧照片标题栏拼回去。拼音和候选条保留原角色照片。草绿底板的纵向渐变和选中候选的内缩、圆角、渐变/白边，按原程序参数转成静态 PNG。按键尺寸与排布、实时文字、手势及按压继续由客户端处理，素材不会固定或覆盖按键。

极客原样式是程序绘制，包内不伪造带永久阴影的键帽图片。新版客户端以 `baseSkin=brutal` 和本包配色绘制缺图键帽的边框、硬阴影与按压反馈；这样按下时阴影能正确消失。旧版 v1 客户端可识别格式，但若缺少这项基础绘制，极客可能显示为纯色键。

v1 的既有字段有两项明确限制：极客没有独立 `hardShadow` 色字段，新版统一用包内 `border` 画边框和硬阴影，因此深色硬影为 `#46574F`，原内置版为纯黑；草绿只有一个 `functionText`，取原键帽文字的奶白色，不能同时表达原应用表面的独立功能文字色。包不新增私有字段、不修改 v1 契约，不宣称两平台或不同屏幕逐像素相同。

## 可复现制作源

`sources/grass/` 保存四份原始 NinePatch 制作源，`sources/palettes-v1.json` 记录两套原始的 18 色、流光、字体及按压参数。制作源位于软件工程之外，不会被 Android 或 Windows 构建为内置资源，也不会夹带进 `.fcskin` 归档。

在仓库根目录运行（制作端需 Pillow 与 Noto Sans CJK，客户端无需安装；预览字体摘要也记录在报告中）：

```sh
python3 skin-format/tools/export_current_skins.py
python3 skin-format/tools/fcskin.py validate skin-format/releases/file-only-v1/fangcun.geek-mechanical.fcskin
python3 skin-format/tools/fcskin.py validate skin-format/releases/file-only-v1/fangcun.grass-mechanical.fcskin
```

只更新该发布目录的两份包、两张预览和报告。构建工具验证标准 ZIP、清单字段、已声明文件、图片编码、像素及体积预算，并校验浅色原图去边后像素完全一致、深色转换前后 Alpha 完全一致。原图及源代码指纹的审计快照在本轮 `fangcun-file-only-skins-2026-11-26/export/`。
