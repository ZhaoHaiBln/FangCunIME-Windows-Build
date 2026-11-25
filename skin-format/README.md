# 方寸本地皮肤 `.fcskin v1`

这是安卓和 Windows 共用的固定皮肤格式。以后设计皮肤，交付**效果预览图 + 可导入的 `.fcskin` 包**即可，不需要修改输入法代码。完整规则以 [SPEC-v1.md](SPEC-v1.md) 为准，v1 保持兼容。

皮肤由颜色和分层 PNG 材质组成，不是把一张键盘截图铺到屏幕上。按键、文字、候选、图标和操作继续由输入法绘制，素材里不要画死这些内容。必须分别提供 Light / Dark 两套配色；用户的字号、粗体、键盘高度、透明度和流光设置仍然有效。

## 包里有什么

`.fcskin` 的内容是标准 ZIP，根目录包含：

```text
manifest.json          # 名称、版本、基础样式、深浅配色、图片引用
preview.png            # 必需的预览图，名称可在清单里指定
light/key_normal.png   # 可选：浅色普通键帽材质
dark/key_normal.png    # 可选：深色普通键帽材质
```

图片只支持静态、非交错的 8-bit RGB / RGBA PNG。可按需提供底板、工具栏、拼音栏、候选栏、选中高亮、普通键帽、功能键帽和操作键帽；没有提供的角色使用本包配色。图片伸缩、九切片边界、大小限制和路径规则见[规范](SPEC-v1.md#png-素材与绘制)。不要加入整张截图、字体文件、脚本、CSS 或未声明文件。

- [contract-v1.json](contract-v1.json)：固定颜色名、图片角色和限制。
- [schema-v1.json](schema-v1.json)：供编辑器检查清单结构；发布前仍须使用制作工具完整校验。
- [examples/template](examples/template)：最小有效模板。里面只有中性占位预览和示例配色，不是一套新皮肤。
- [tools/fcskin.py](tools/fcskin.py)：使用 Python 3 标准库制作和校验，无需额外安装依赖。

## 制作和检查

在仓库根目录运行，`my-skin` 应当是一个不存在或为空的目录：

```sh
python3 skin-format/tools/fcskin.py init my-skin
```

替换占位预览，修改 `manifest.json`，并按清单加入自己的 PNG 材质，然后运行：

```sh
python3 skin-format/tools/fcskin.py build my-skin my-skin.fcskin
python3 skin-format/tools/fcskin.py validate my-skin.fcskin
```

`build` 只更新明确指定的输出包；验证失败会保留原稿和旧包。清单的 `id` 是皮肤身份，`version` 是正整数。已经安装的同 ID 不同内容，须先删除旧包再导入；相同包重复导入不会重复安装。

## 在输入法中使用

进入外观里的「管理皮肤」导入 `.fcskin`。导入成功后，点「使用」才会切换。

内置皮肤可以隐藏、恢复，至少保留一款可见内置皮肤。本地皮肤可以删除；删除当前皮肤时会切回可用的内置皮肤，并保留其他设置。「允许导入本地皮肤」开关只阻止新导入，关闭后已经安装的皮肤仍可使用。
