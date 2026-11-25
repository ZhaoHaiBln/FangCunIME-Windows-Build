# `.fcskin v1` 测试数据

这些 PNG 仅是验证解析器的合成占位像素，不是用户皮肤设计，也不会自动安装到应用。

跨平台用同一份 [`../test-vectors/index.json`](../test-vectors/index.json)：

```json
{
  "format": "fangcun.skin-test-vectors",
  "version": 1,
  "vectors": [
    {
      "name": "valid_stored",
      "file": "valid_stored.fcskin",
      "valid": true,
      "reason": "Standard STORE ZIP",
      "sha256": "…",
      "bytes": 2248
    }
  ]
}
```

`file` 相对索引所在目录；`valid` 是必须一致的接受/拒绝结果，不能因平台压缩库的默认行为而改变。`bytes` 与 `sha256` 用于确认各端测试了同一份原始 ZIP。当前共有 87 个向量：10 个应接受、77 个应安全拒绝。

在仓库根目录运行：

```sh
python3 skin-format/tests/generate_vectors.py
python3 -m unittest discover -s skin-format/tests -p 'test_*.py' -v
```

生成器固定 ZIP 元数据及内容排序，可逐字节复现所有向量，并同时生成 `schema-v1.json`。有效数据涵盖 STORE、DEFLATE、签名/无签名数据描述符、合法父目录和所有图片角色；恶意数据涵盖 JSON、ZIP 元数据、路径、Windows 保留设备名、PNG 编码与资源预算。压缩包合计约 155 KiB，像素预算用高度可压缩的静态 PNG 验证。

JSON Schema 是编辑器使用的结构检查。JSON 标准的 Schema `integer` 无法区别 `1` 与 `1.0` 的原始字面量，也无法发现重复字段或检查 ZIP、PNG 内容；发布前必须继续运行 `tools/fcskin.py validate`，各端导入器同样执行完整检查。

制作工具回归还检查：重复打包结果相同；成功时可原子更新明确指定的输出文件；失败时保留原稿和旧包；并行制作使用独立临时文件；符号链接不能把目录外的素材带进包中。
