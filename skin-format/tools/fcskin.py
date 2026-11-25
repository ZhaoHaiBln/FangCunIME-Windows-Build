#!/usr/bin/env python3
"""Create and validate the fixed .fcskin v1 format (Python standard library)."""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import re
import stat
import struct
import sys
import tempfile
import zipfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONTRACT = json.loads((ROOT / "contract-v1.json").read_text(encoding="utf-8"))
LIMITS = CONTRACT["limits"]
COLOR = re.compile(r"#[0-9a-fA-F]{6}\Z")
IMAGE_PATH = re.compile(r"(?:[a-z0-9_-]+/)*[a-z0-9_-]+\.png\Z")
IDENTIFIER = re.compile(r"[a-z][a-z0-9]*(?:\.[a-z0-9][a-z0-9_-]*)+\Z")


class Invalid(ValueError):
    pass


def check(condition, reason="皮肤包不符合 .fcskin v1 规范"):
    if not condition:
        raise Invalid(reason)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        check(key not in result, "JSON 中存在重复字段")
        result[key] = value
    return result


def exact_keys(value, required, optional=()):
    check(isinstance(value, dict))
    check(set(required) <= set(value) <= set(required) | set(optional))


def integer(value, minimum, maximum):
    return type(value) is int and minimum <= value <= maximum


def color(value):
    return isinstance(value, str) and bool(COLOR.fullmatch(value))


def image_path(value):
    return isinstance(value, str) and len(value) <= LIMITS["pathLength"] and bool(IMAGE_PATH.fullmatch(value)) and all(portable_component(p) for p in value[:-4].split("/"))


def portable_component(value):
    return value not in {"con", "prn", "aux", "nul"} and not re.fullmatch(r"(?:com|lpt)[1-9]", value)


def label(value, minimum, maximum):
    return isinstance(value, str) and minimum <= len(value) <= maximum and not any(ord(c) < 32 or 127 <= ord(c) <= 159 or 0xD800 <= ord(c) <= 0xDFFF for c in value) and (minimum == 0 or bool(value.strip()))


def manifest_bytes(data):
    check(len(data) <= LIMITS["manifestBytes"])
    try:
        value = json.loads(data.decode("utf-8"), object_pairs_hook=unique_object,
                           parse_constant=lambda _: (_ for _ in ()).throw(Invalid("JSON 数值无效")))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise Invalid("清单必须是有效 UTF-8 JSON") from error
    exact_keys(value, ("format", "schemaVersion", "id", "name", "version", "baseSkin", "assetScale", "fontFamily", "pressOffsetDp", "preview", "themes"), ("author", "license"))
    check(value["format"] == "fangcun.skin" and type(value["schemaVersion"]) is int and value["schemaVersion"] == 1)
    check(isinstance(value["id"], str) and 3 <= len(value["id"]) <= 64 and IDENTIFIER.fullmatch(value["id"]))
    check(label(value["name"], 1, 40) and label(value.get("author", ""), 0, 48) and label(value.get("license", ""), 0, 120))
    check(integer(value["version"], 1, 2147483647))
    check(value["baseSkin"] in CONTRACT["baseSkins"] and value["fontFamily"] in CONTRACT["fontFamilies"])
    check(integer(value["assetScale"], 1, 4))
    check(type(value["pressOffsetDp"]) in (int, float) and 0 <= value["pressOffsetDp"] <= 2)
    check(image_path(value["preview"]))
    exact_keys(value["themes"], CONTRACT["themes"])
    for theme in value["themes"].values():
        exact_keys(theme, ("colors", "glow", "images"))
        exact_keys(theme["colors"], CONTRACT["colors"])
        check(all(color(c) for c in theme["colors"].values()))
        check(isinstance(theme["glow"], list) and len(theme["glow"]) == 4 and all(color(c) for c in theme["glow"]))
        check(isinstance(theme["images"], dict) and set(theme["images"]) <= set(CONTRACT["imageRoles"]))
        for image in theme["images"].values():
            exact_keys(image, ("file", "scale", "insets"))
            check(image_path(image["file"]) and image["scale"] in CONTRACT["scales"])
            check(isinstance(image["insets"], list) and len(image["insets"]) == 4 and all(integer(n, 0, 2147483647) for n in image["insets"]))
            check(image["scale"] == "nineSlice" or image["insets"] == [0, 0, 0, 0])
    return value


def referenced_files(manifest):
    return {"manifest.json", manifest["preview"]} | {image["file"] for theme in manifest["themes"].values() for image in theme["images"].values()}


def png_dimensions(data):
    check(len(data) >= 45 and data[:16] == b"\x89PNG\r\n\x1a\n\0\0\0\rIHDR", "不是普通 PNG 图片")
    width, height, depth, kind, compression, filtering, interlace = struct.unpack(">IIBBBBB", data[16:29])
    check(1 <= width <= 2048 and 1 <= height <= 2048 and depth == 8 and kind in (2, 6) and (compression, filtering, interlace) == (0, 0, 0), "PNG 编码或尺寸不符合 v1")
    offset, idat, seen_header, seen_end, data_ended = 8, [], False, False, False
    while offset < len(data):
        check(offset + 12 <= len(data))
        length = struct.unpack(">I", data[offset:offset + 4])[0]
        end = offset + 12 + length
        check(end <= len(data))
        chunk, payload = data[offset + 4:offset + 8], data[offset + 8:end - 4]
        check(zlib.crc32(data[offset + 4:end - 4]) == struct.unpack(">I", data[end - 4:end])[0], "PNG 校验失败")
        check(chunk not in (b"acTL", b"fcTL", b"fdAT"), "不支持动画 PNG")
        if chunk == b"IHDR":
            check(not seen_header and offset == 8 and length == 13)
            seen_header = True
        elif chunk == b"IDAT":
            check(not data_ended, "PNG 图像数据不连续")
            idat.append(payload)
        elif chunk == b"IEND":
            check(length == 0 and end == len(data))
            seen_end = True
        elif chunk == b"PLTE":
            check(not idat and 3 <= length <= 768 and length % 3 == 0, "PNG 调色信息不正确")
        else:
            check(chunk and 97 <= chunk[0] <= 122, "不支持的 PNG 关键块")
        if idat and chunk != b"IDAT":
            data_ended = True
        offset = end
    check(seen_end and idat)
    expected = (width * (3 if kind == 2 else 4) + 1) * height
    decoder = zlib.decompressobj()
    try:
        rows = decoder.decompress(b"".join(idat), expected + 1)
    except zlib.error as error:
        raise Invalid("PNG 图像数据解码失败") from error
    check(len(rows) == expected and decoder.eof and not decoder.unconsumed_tail and not decoder.unused_data, "PNG 图像数据长度不正确")
    stride = expected // height
    check(all(rows[n * stride] <= 4 for n in range(height)), "PNG 行过滤数据不正确")
    return width, height


def validate_files(files):
    check(0 < len(files) <= LIMITS["entries"] and sum(map(len, files.values())) <= LIMITS["expandedBytes"])
    check("manifest.json" in files)
    manifest = manifest_bytes(files["manifest.json"])
    check(set(files) == referenced_files(manifest), "皮肤包含缺失或未声明文件")
    dimensions, pixels = {}, 0
    for path, data in files.items():
        if path == "manifest.json":
            continue
        check(len(data) <= LIMITS["imageBytes"] and (path != manifest["preview"] or len(data) <= LIMITS["previewBytes"]))
        size = png_dimensions(data)
        pixels += size[0] * size[1]
        check(pixels <= LIMITS["totalPixels"], "图片总像素过大")
        dimensions[path] = size
    for theme in manifest["themes"].values():
        for image in theme["images"].values():
            left, top, right, bottom = image["insets"]
            width, height = dimensions[image["file"]]
            check(left + right < width and top + bottom < height, "九切片边距不正确")
    return manifest


def standard_zip(data):
    check(len(data) >= 22 and data[:4] == b"PK\x03\x04")
    found = None
    for offset in range(len(data) - 22, max(-1, len(data) - 65558), -1):
        if data[offset:offset + 4] == b"PK\x05\x06" and offset + 22 + struct.unpack_from("<H", data, offset + 20)[0] == len(data):
            found = offset
            break
    check(found is not None, "ZIP 结束记录不正确")
    disk, central_disk, entries_disk, entries, size, start = struct.unpack_from("<HHHHII", data, found + 4)
    check(disk == central_disk == 0 and entries_disk == entries and 0 < entries <= LIMITS["entries"] and start + size == found)
    check(data[max(0, found - 20):max(0, found - 16)] != b"PK\x06\x07", "v1 不支持 ZIP64")
    # Match Android's seekable standard ZIP reader: validate metadata only here,
    # leaving all decompression and CRC reads to zipfile below. Every local entry
    # must be represented, with no aliases, gaps, overlaps or renamed headers.
    def extra(at, length):
        end = at + length
        check(end <= len(data))
        while at < end:
            check(at + 4 <= end)
            kind, count = struct.unpack_from("<HH", data, at)
            check(kind not in (1, 0x9901) and at + 4 + count <= end)
            at += 4 + count
    offset, regions, names, expanded = start, [], set(), 0
    for _ in range(entries):
        check(offset + 46 <= found and data[offset:offset + 4] == b"PK\x01\x02")
        flags, method = struct.unpack_from("<HH", data, offset + 8)
        crc, compressed, inflated = struct.unpack_from("<III", data, offset + 16)
        name_len, extra_len, comment_len, disk_start = struct.unpack_from("<HHHH", data, offset + 28)
        attributes, local = struct.unpack_from("<II", data, offset + 38)
        finish = offset + 46 + name_len + extra_len + comment_len
        check(finish <= found and 1 <= name_len <= 97 and method in (0, 8) and not flags & 0xF7F1 and disk_start == 0 and not attributes & 0x400)
        raw_name = data[offset + 46:offset + 46 + name_len]
        try:
            name = raw_name.decode("ascii")
        except UnicodeError as error:
            raise Invalid("包内路径必须为小写 ASCII") from error
        directory = name.endswith("/")
        valid_dir = directory and len(name) <= 96 and bool(re.fullmatch(r"(?:[a-z0-9_-]+/)+", name)) and all(portable_component(p) for p in name[:-1].split("/"))
        check(name not in names and (name == "manifest.json" or image_path(name) or valid_dir))
        names.add(name)
        check((attributes >> 16) & 0xF000 in (0, 0x4000 if directory else 0x8000))
        check(inflated <= LIMITS["imageBytes"] and compressed <= LIMITS["archiveBytes"] and (not directory or inflated == 0))
        expanded += inflated
        check(expanded <= LIMITS["expandedBytes"])
        extra(offset + 46 + name_len, extra_len)
        check(local + 30 <= start and data[local:local + 4] == b"PK\x03\x04")
        check(struct.unpack_from("<HH", data, local + 6) == (flags, method))
        local_name_len, local_extra_len = struct.unpack_from("<HH", data, local + 26)
        body_start = local + 30 + local_name_len + local_extra_len
        region_end = body_start + compressed
        check(local_name_len == name_len and region_end <= start and data[local + 30:local + 30 + local_name_len] == raw_name)
        extra(local + 30 + local_name_len, local_extra_len)
        if flags & 8:
            if data[region_end:region_end + 4] == b"PK\x07\x08":
                region_end += 4
            check(region_end + 12 <= start and struct.unpack_from("<III", data, region_end) == (crc, compressed, inflated))
            region_end += 12
        else:
            check(struct.unpack_from("<III", data, local + 14) == (crc, compressed, inflated))
        regions.append((local, region_end))
        offset = finish
    check(offset == found)
    next_offset = 0
    for begin, end in sorted(regions):
        check(begin == next_offset and end >= begin)
        next_offset = end
    check(next_offset == start)


def validate_archive(data):
    check(len(data) <= LIMITS["archiveBytes"], "皮肤包超过 24 MiB")
    standard_zip(data)
    files, names, directories, expanded = {}, set(), [], 0
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            check(len(archive.infolist()) <= LIMITS["entries"])
            for entry in archive.infolist():
                path = entry.filename
                check(path == entry.orig_filename and path not in names and not entry.flag_bits & 0x41)
                names.add(path)
                check(entry.compress_type in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED))
                mode = stat.S_IFMT(entry.external_attr >> 16)
                if entry.is_dir():
                    check(mode in (0, stat.S_IFDIR) and entry.file_size == 0 and len(path) <= 96 and re.fullmatch(r"(?:[a-z0-9_-]+/)+", path) and all(portable_component(p) for p in path[:-1].split("/")))
                    directories.append(path)
                    continue
                check(mode in (0, stat.S_IFREG) and (path == "manifest.json" or image_path(path)))
                maximum = LIMITS["manifestBytes"] if path == "manifest.json" else LIMITS["imageBytes"]
                check(entry.file_size <= maximum)
                with archive.open(entry) as stream:
                    body = stream.read(maximum + 1)
                check(len(body) == entry.file_size and len(body) <= maximum)
                expanded += len(body)
                check(expanded <= LIMITS["expandedBytes"])
                files[path] = body
    except (zipfile.BadZipFile, NotImplementedError, RuntimeError, EOFError, zlib.error) as error:
        raise Invalid("ZIP 数据或校验不正确") from error
    check(all(any(path.startswith(directory) for path in files) for directory in directories))
    manifest = validate_files(files)
    return manifest, hashlib.sha256(data).hexdigest()


def archive_bytes(files, compression=zipfile.ZIP_DEFLATED):
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w", compression=compression) as archive:
        for name in sorted(files):
            info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            info.create_system = 3
            info.external_attr = (stat.S_IFREG | 0o644) << 16
            info.compress_type = compression
            archive.writestr(info, files[name])
    return stream.getvalue()


def placeholder_png(width=8, height=8, rgba=(240, 240, 240, 255)):
    """Neutral test/template data, not a finished visual design."""
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    rows = (b"\0" + bytes(rgba) * width) * height
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")


def template_manifest():
    light_values = ["#EEEDE9", "#FFFFFF", "#8A9389", "#51743B", "#172017", "#596356", "#51743B", "#FFFFFF", "#D9E4D1", "#F4F3EF", "#F4F3EF", "#172017", "#51743B", "#FFFFFF", "#172017", "#596356", "#FFFFFF", "#172017"]
    dark_values = ["#172019", "#29382B", "#8BA383", "#406239", "#F0F4EC", "#B8C8AE", "#A6D591", "#152710", "#3B5037", "#213024", "#213024", "#F0F4EC", "#A6D591", "#152710", "#F0F4EC", "#B8C8AE", "#FFFFFF", "#F0F4EC"]
    theme = lambda values: {"colors": dict(zip(CONTRACT["colors"], values)), "glow": ["#8ABE78", "#A2C9D2", "#D8B9CE", "#D7C59A"], "images": {}}
    return {"format": "fangcun.skin", "schemaVersion": 1, "id": "fangcun.template", "name": "皮肤制作模板", "version": 1, "author": "方寸", "license": "模板素材仅为占位", "baseSkin": "mechanical", "assetScale": 3, "fontFamily": "sans", "pressOffsetDp": 1, "preview": "preview.png", "themes": {"light": theme(light_values), "dark": theme(dark_values)}}


def create_template(folder):
    check(not folder.exists() or not any(folder.iterdir()), "模板目录必须为空，防止覆盖已有作品")
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "manifest.json").write_text(json.dumps(template_manifest(), ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (folder / "preview.png").write_bytes(placeholder_png(32, 32))


def build(folder, output):
    check(output.suffix.lower() == ".fcskin", "输出文件扩展名必须是 .fcskin")
    root = folder.resolve(strict=True)
    manifest_path = root / "manifest.json"
    check(not manifest_path.is_symlink(), "清单不能是符号链接")
    with manifest_path.open("rb") as stream:
        raw = stream.read(LIMITS["manifestBytes"] + 1)
    manifest = manifest_bytes(raw)
    files = {"manifest.json": raw}
    for relative in referenced_files(manifest) - {"manifest.json"}:
        candidate = root / relative
        check(candidate.resolve(strict=True).is_relative_to(root), "素材路径超出皮肤目录")
        check(not any(parent.is_symlink() for parent in [candidate, *candidate.parents] if parent.is_relative_to(root)), "素材不能是符号链接")
        with candidate.open("rb") as stream:
            files[relative] = stream.read(LIMITS["imageBytes"] + 1)
    validate_files(files)
    data = archive_bytes(files)
    validate_archive(data)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=output.parent, prefix=output.name + ".tmp.", delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(data)
        temporary.replace(output)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return manifest, hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description="方寸 .fcskin v1 皮肤制作和检查工具")
    commands = parser.add_subparsers(dest="command", required=True)
    init = commands.add_parser("init"); init.add_argument("folder", type=Path)
    pack = commands.add_parser("build"); pack.add_argument("folder", type=Path); pack.add_argument("output", type=Path)
    verify = commands.add_parser("validate"); verify.add_argument("file", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "init":
            create_template(args.folder)
            print("已创建模板；请按规范替换占位预览并加入分层 PNG 素材。")
            return
        if args.command == "build":
            manifest, digest = build(args.folder, args.output)
        else:
            with args.file.open("rb") as stream:
                manifest, digest = validate_archive(stream.read(LIMITS["archiveBytes"] + 1))
        print(json.dumps({"valid": True, "id": manifest["id"], "name": manifest["name"], "version": manifest["version"], "sha256": digest}, ensure_ascii=False))
    except (Invalid, OSError, ValueError, KeyError, TypeError) as error:
        print("皮肤包检查失败：" + str(error), file=sys.stderr)
        raise SystemExit(1)


if __name__ == "__main__":
    main()
