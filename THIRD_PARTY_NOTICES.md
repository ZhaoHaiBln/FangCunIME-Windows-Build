# 第三方数据与组件声明

- 项目：iDvel/rime-ice
- 使用内容：`cn_dicts` 字表与基础词库，`double_pinyin_flypy.schema.yaml` 的小鹤双拼 algebra 规则。
- 许可证：GNU General Public License v3.0（GPL-3.0）。
- 上游：https://github.com/iDvel/rime-ice

本工程未复制 Trime 或其他 GPL 输入法的代码实现；引擎与 Windows TSF 服务为独立实现。词库与规则沿数据授权路线使用。若公开发布衍生软件，需履行按 GPL-3.0 开源的义务。

## 本地皮肤包的归档与图像组件

Windows 伴侣使用以下 Rust 组件读取 ZIP、校验 CRC、解压 PNG 并检查图像。具体解析边界由方寸的 `.fcskin v1` 规范限定；没有复制其他输入法的皮肤实现。

| 组件 | 版本范围 | 上游 | 许可与随附文本 |
| --- | --- | --- | --- |
| zip | 0.6.6 | https://github.com/zip-rs/zip | MIT：[文本](licenses/skin-pack/zip-LICENSE) |
| png | 0.17.16 | https://github.com/image-rs/image-png | MIT / Apache-2.0：[MIT](licenses/skin-pack/png-LICENSE-MIT)、[Apache](licenses/skin-pack/png-LICENSE-APACHE) |
| crc32fast | 1.x | https://github.com/srijs/rust-crc32fast | MIT / Apache-2.0：[MIT](licenses/skin-pack/crc32fast-LICENSE-MIT)、[Apache](licenses/skin-pack/crc32fast-LICENSE-APACHE) |
| flate2 | 1.x | https://github.com/rust-lang/flate2-rs | MIT / Apache-2.0：[MIT](licenses/skin-pack/flate2-LICENSE-MIT)、[Apache](licenses/skin-pack/flate2-LICENSE-APACHE) |
