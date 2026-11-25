#!/usr/bin/env python3
"""Deterministic, shared .fcskin v1 parser fixtures; no user artwork is generated.

Normal archives use the SDK writer. The tiny wire writer intentionally exposes
ZIP record fields needed to test readers before they extract an untrusted file.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import importlib.util
import json
import stat
import struct
import zipfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SDK_SPEC = importlib.util.spec_from_file_location("fcskin_sdk_vectors", ROOT / "tools/fcskin.py")
sdk = importlib.util.module_from_spec(SDK_SPEC)
SDK_SPEC.loader.exec_module(sdk)
CONTRACT = sdk.CONTRACT


def json_bytes(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def base_files(manifest=None, png=None):
    return {"manifest.json": json_bytes(manifest or sdk.template_manifest()),
            "preview.png": png if png is not None else sdk.placeholder_png()}


def wire_archive(entries, *, method=8, descriptor=False, signed_descriptor=True,
                 flags=0, local_extra=b"", central_extra=b"", comment=b""):
    """Write standard non-ZIP64 records, retaining deliberate duplicate names."""
    output, central = bytearray(), bytearray()
    for name, body, attributes in entries:
        encoded = name.encode("utf-8")
        actual_flags = flags | (8 if descriptor else 0)
        compressor = zlib.compressobj(6, zlib.DEFLATED, -15)
        compressed = compressor.compress(body) + compressor.flush() if method == 8 else body
        crc = zlib.crc32(body)
        local = len(output)
        output += struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, actual_flags, method, 0, 33,
                              0 if descriptor else crc, 0 if descriptor else len(compressed),
                              0 if descriptor else len(body), len(encoded), len(local_extra))
        output += encoded + local_extra + compressed
        if descriptor:
            if signed_descriptor:
                output += b"PK\x07\x08"
            output += struct.pack("<III", crc, len(compressed), len(body))
        central += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 0x0314, 20, actual_flags,
                               method, 0, 33, crc, len(compressed), len(body), len(encoded),
                               len(central_extra), 0, 0, 0, attributes, local)
        central += encoded + central_extra
    start = len(output)
    output += central
    output += struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, len(entries), len(entries), len(central), start, len(comment))
    output += comment
    return bytes(output)


def entries_for(files):
    return [(name, files[name], (stat.S_IFREG | 0o644) << 16) for name in sorted(files)]


def central_offsets(data):
    end = data.rfind(b"PK\x05\x06")
    start = struct.unpack_from("<I", data, end + 16)[0]
    count = struct.unpack_from("<H", data, end + 10)[0]
    result, at = [], start
    for _ in range(count):
        result.append(at)
        name, extra, comment = struct.unpack_from("<HHH", data, at + 28)
        at += 46 + name + extra + comment
    return result


def patch(data, offset, value):
    result = bytearray(data)
    result[offset:offset + len(value)] = value
    return bytes(result)


def chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))


def png(*, width=8, height=8, depth=8, kind=6, interlace=0, rows=None, compressed=None,
        before=(), after=()):
    raw = rows if rows is not None else (b"\0" + bytes((240, 240, 240, 255)) * width) * height
    compressed = zlib.compress(raw) if compressed is None else compressed
    header = struct.pack(">IIBBBBB", width, height, depth, kind, 0, 0, interlace)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + b"".join(before) + chunk(b"IDAT", compressed) + b"".join(after) + chunk(b"IEND", b"")


def with_image(manifest=None, *, role="keyNormal", scale="nineSlice", insets=(1, 1, 1, 1)):
    value = copy.deepcopy(manifest or sdk.template_manifest())
    value["themes"]["light"]["images"][role] = {"file": "preview.png", "scale": scale, "insets": list(insets)}
    return value


def make_vectors():
    vectors = []
    def add(name, data, valid, reason):
        vectors.append((name, data, valid, reason))
    def changed(name, change, reason):
        value = copy.deepcopy(sdk.template_manifest())
        change(value)
        add(name, sdk.archive_bytes(base_files(value)), False, reason)

    files = base_files()
    stored = sdk.archive_bytes(files, zipfile.ZIP_STORED)
    normal = sdk.archive_bytes(files)
    add("valid_stored", stored, True, "Standard STORE ZIP")
    add("valid_deflated", normal, True, "Standard DEFLATE ZIP")
    add("valid_deflated_descriptor", wire_archive(entries_for(files), descriptor=True), True, "Streaming DEFLATE with signed data descriptors")
    add("valid_stored_descriptor", wire_archive(entries_for(files), method=0, descriptor=True), True, "Seekable readers support streaming STORE with descriptors")
    add("valid_unsigned_descriptor", wire_archive(entries_for(files), descriptor=True, signed_descriptor=False), True, "Descriptor signature is optional in standard ZIP")
    add("valid_archive_comment", wire_archive(entries_for(files), comment=b"fcskin fixture"), True, "Standard ZIP archive comment")
    add("valid_deflate_options", wire_archive(entries_for(files), flags=2), True, "DEFLATE option flags are not encryption")
    shared = sdk.template_manifest()
    for mode in CONTRACT["themes"]:
        for role in CONTRACT["imageRoles"]:
            shared["themes"][mode]["images"][role] = {"file": "preview.png", "scale": "nineSlice", "insets": [1, 1, 1, 1]}
    add("valid_shared_image", sdk.archive_bytes(base_files(shared)), True, "One image can supply all roles and both modes without duplicate pixel charges")

    complete = sdk.template_manifest()
    complete["id"], complete["name"] = "fangcun.complete", "完整样例🙂"
    complete_files = base_files(complete)
    for mode in CONTRACT["themes"]:
        path = f"{mode}/material.png"
        complete_files[path] = sdk.placeholder_png()
        for index, role in enumerate(CONTRACT["imageRoles"]):
            scale = CONTRACT["scales"][index % 4]
            complete["themes"][mode]["images"][role] = {"file": path, "scale": scale,
                "insets": [1, 1, 1, 1] if scale == "nineSlice" else [0, 0, 0, 0]}
    complete_files["manifest.json"] = json_bytes(complete)
    directory_entries = [(f"{mode}/", b"", ((stat.S_IFDIR | 0o755) << 16) | 0x10) for mode in CONTRACT["themes"]]
    add("valid_directories_and_all_roles", wire_archive(directory_entries + entries_for(complete_files)), True, "Declared parent directories and every scale/role")

    def deep_directories(a_count, b_count):
        manifest = sdk.template_manifest()
        content = base_files(manifest)
        parents = []
        for number, (prefix, count) in enumerate((("a", a_count), ("b", b_count))):
            for depth in range(1, count + 1):
                parents.append(((prefix + "/") * depth, b"", ((stat.S_IFDIR | 0o755) << 16) | 0x10))
            path = (prefix + "/") * count + "material.png"
            manifest["themes"]["light"]["images"][CONTRACT["imageRoles"][number]] = {"file": path, "scale": "stretch", "insets": [0, 0, 0, 0]}
            content[path] = sdk.placeholder_png()
        content["manifest.json"] = json_bytes(manifest)
        return wire_archive(parents + entries_for(content))
    add("valid_64_entries", deep_directories(30, 30), True, "The 64-entry limit includes legitimate parent directories")
    add("zip_entry_limit", deep_directories(30, 31), False, "65 entries exceed the v1 maximum")

    changed("manifest_unknown_field", lambda v: v.update(layout="replacement"), "Unknown manifest fields are forbidden")
    changed("manifest_missing_dark", lambda v: v["themes"].pop("dark"), "Both Light and Dark are required")
    changed("manifest_extra_mode", lambda v: v["themes"].update(system=copy.deepcopy(v["themes"]["light"])), "There is no pack-defined system mode")
    changed("manifest_missing_color", lambda v: v["themes"]["light"]["colors"].pop("keyHint"), "All 18 color roles are required")
    changed("manifest_unknown_color", lambda v: v["themes"]["light"]["colors"].update(unknown="#112233"), "Unknown color roles are forbidden")
    changed("manifest_color_url", lambda v: v["themes"]["light"]["colors"].update(accent="https://example.invalid/a"), "Colors cannot contain URLs")
    changed("manifest_boolean_schema", lambda v: v.update(schemaVersion=True), "A boolean is not an integer schema token")
    changed("manifest_float_schema", lambda v: v.update(schemaVersion=1.0), "Integer fields require integer JSON tokens")
    changed("manifest_boolean_version", lambda v: v.update(version=True), "A boolean is not an integer version")
    changed("manifest_float_version", lambda v: v.update(version=1.0), "Integral floats are not integer JSON tokens")
    changed("manifest_boolean_scale", lambda v: v.update(assetScale=True), "Asset scale cannot be boolean")
    changed("manifest_float_scale", lambda v: v.update(assetScale=3.0), "Asset scale requires an integer token")
    changed("manifest_boolean_press", lambda v: v.update(pressOffsetDp=True), "Press offset cannot be boolean")
    changed("manifest_nan_press", lambda v: v.update(pressOffsetDp=float("nan")), "NaN is not a finite JSON number")
    changed("manifest_blank_name", lambda v: v.update(name=" \u00a0 "), "Names cannot be whitespace only")
    changed("manifest_control_name", lambda v: v.update(name="skin\nname"), "Display strings cannot contain control characters")
    changed("manifest_preview_url", lambda v: v.update(preview="https://example.invalid/skin.png"), "External images are forbidden")
    changed("manifest_preview_traversal", lambda v: v.update(preview="../preview.png"), "Image paths cannot traverse parents")
    changed("manifest_preview_absolute", lambda v: v.update(preview="/preview.png"), "Image paths must be relative")
    changed("manifest_preview_wrong_type", lambda v: v.update(preview="preview.webp"), "Only PNG assets are supported")
    changed("manifest_android_nine_patch", lambda v: v.update(preview="key.9.png"), "Android source nine-patch files are not ordinary pack PNGs")
    reserved_preview = sdk.template_manifest()
    reserved_preview["preview"] = "con.png"
    add("path_reserved_preview", sdk.archive_bytes({"manifest.json": json_bytes(reserved_preview),
        "con.png": sdk.placeholder_png()}), False, "Windows device names are not portable PNG asset names")
    reserved_role = with_image()
    reserved_role["themes"]["light"]["images"]["keyNormal"]["file"] = "aux.png"
    add("path_reserved_role", sdk.archive_bytes({**base_files(reserved_role),
        "aux.png": sdk.placeholder_png()}), False, "Reserved device names are forbidden for image roles too")
    reserved_parent = sdk.template_manifest()
    reserved_parent["preview"] = "con/key.png"
    add("path_reserved_parent", sdk.archive_bytes({"manifest.json": json_bytes(reserved_parent),
        "con/key.png": sdk.placeholder_png()}), False, "Every image path component must work on Android and Windows")
    raw = files["manifest.json"]
    duplicate = raw.replace(b'"schemaVersion":1', b'"schemaVersion":1,"schemaVersion":1', 1)
    add("manifest_duplicate_key", sdk.archive_bytes({**files, "manifest.json": duplicate}), False, "Duplicate JSON keys are rejected even when values match")
    duplicate = raw.replace(b'"background":"#EEEDE9"', b'"background":"#EEEDE9","background":"#EEEDE9"', 1)
    add("manifest_nested_duplicate_key", sdk.archive_bytes({**files, "manifest.json": duplicate}), False, "Nested duplicate JSON keys are rejected")
    add("manifest_utf8_bom", sdk.archive_bytes({**files, "manifest.json": b"\xef\xbb\xbf" + raw}), False, "v1 manifest is UTF-8 without a BOM")
    add("manifest_invalid_utf8", sdk.archive_bytes({**files, "manifest.json": raw + b"\xff"}), False, "Invalid UTF-8 cannot be replaced silently")
    invalid_surrogate = raw.replace('"name":"皮肤制作模板"'.encode("utf-8"), b'"name":"\\ud800"', 1)
    add("manifest_unicode_surrogate", sdk.archive_bytes({**files, "manifest.json": invalid_surrogate}), False, "Unpaired Unicode surrogates are not valid display text")
    add("manifest_unknown_role", sdk.archive_bytes(base_files(with_image(role="keySpace"))), False, "Image role names are fixed")
    add("manifest_boolean_insets", sdk.archive_bytes(base_files(with_image(insets=(True, 0, 0, 0)))), False, "Insets cannot contain booleans")
    add("manifest_float_insets", sdk.archive_bytes(base_files(with_image(insets=(1.0, 0, 0, 0)))), False, "Insets require integer tokens")
    add("manifest_non_slice_insets", sdk.archive_bytes(base_files(with_image(scale="stretch"))), False, "Only nineSlice accepts nonzero insets")
    add("image_slice_out_of_bounds", sdk.archive_bytes(base_files(with_image(insets=(4, 0, 4, 0)))), False, "Opposing borders must leave a nonempty image center")
    add("zip_undeclared_png", sdk.archive_bytes({**files, "unused.png": sdk.placeholder_png()}), False, "Unreferenced image files are forbidden")
    add("zip_extra_file", sdk.archive_bytes({**files, "readme.txt": b"not declared"}), False, "Only manifest and declared PNGs are allowed")
    for name, path in (("zip_traversal", "../escape.png"), ("zip_absolute", "/escape.png"), ("zip_backslash", "..\\escape.png")):
        add(name, wire_archive(entries_for(files) + [(path, sdk.placeholder_png(), (stat.S_IFREG | 0o644) << 16)]), False, "ZIP paths cannot escape their private skin directory")
    symlink = [(name, body, (stat.S_IFLNK | 0o777) << 16 if name == "preview.png" else attributes) for name, body, attributes in entries_for(files)]
    add("zip_symlink", wire_archive(symlink), False, "Unix links are forbidden")
    reparse = [(name, body, attributes | 0x400 if name == "preview.png" else attributes) for name, body, attributes in entries_for(files)]
    add("zip_reparse", wire_archive(reparse), False, "Windows reparse attributes are forbidden")
    add("zip_orphan_directory", wire_archive(entries_for(files) + [("unused/", b"", ((stat.S_IFDIR | 0o755) << 16) | 0x10)]), False, "Directory entries must be parents of declared regular files")
    add("zip_duplicate_file", wire_archive(entries_for(files) + [entries_for(files)[-1]]), False, "Duplicate ZIP entries are forbidden")
    positions = central_offsets(normal)
    first_local = struct.unpack_from("<I", normal, positions[0] + 42)[0]
    add("zip_local_name_mismatch", patch(normal, first_local + 30, b"x"), False, "Local and central path bytes must match")
    add("zip_local_method_mismatch", patch(normal, first_local + 8, struct.pack("<H", 0)), False, "Local and central compression methods must match")
    add("zip_local_flags_mismatch", patch(normal, first_local + 6, struct.pack("<H", 0x800)), False, "Local and central flags must match")
    add("zip_encrypted_flag", wire_archive(entries_for(files), flags=1), False, "Encrypted archives are unsupported")
    add("zip_unsupported_flag", wire_archive(entries_for(files), flags=16), False, "Unsupported ZIP processing flags are rejected")
    add("zip_unsupported_compression", sdk.archive_bytes(files, zipfile.ZIP_BZIP2), False, "Only STORE and DEFLATE are supported")
    zip64_extra = struct.pack("<HHQQ", 1, 16, 1, 1)
    add("zip64_extra", wire_archive(entries_for(files), local_extra=zip64_extra, central_extra=zip64_extra), False, "ZIP64 extra fields are not part of v1")
    end = normal.rfind(b"PK\x05\x06")
    add("zip_multivolume", patch(normal, end + 4, struct.pack("<H", 1)), False, "Multi-volume EOCD is forbidden")
    add("zip_entry_other_disk", patch(normal, positions[0] + 34, struct.pack("<H", 1)), False, "Every central entry belongs to disk zero")
    add("zip_overlapping_records", patch(normal, positions[1] + 42, struct.pack("<I", first_local)), False, "Local entries cannot alias or overlap")
    add("zip_malformed_extra", wire_archive(entries_for(files), local_extra=b"\x77\x77\x05\0x"), False, "Extra field lengths must remain inside their metadata record")
    add("zip_truncated", normal[:-9], False, "Truncated EOCD is rejected")
    add("zip_self_extracting_prefix", b"SFX" + normal, False, "Self-extracting prefixes are not v1 ZIPs")
    add("zip_trailing_bytes", normal + b"trailing", False, "EOCD must cover the end of the archive")
    data = bytearray(stored)
    local = struct.unpack_from("<I", stored, central_offsets(stored)[1] + 42)[0]
    name_len, extra_len = struct.unpack_from("<HH", stored, local + 26)
    data[local + 30 + name_len + extra_len] ^= 1
    add("zip_crc_failure", bytes(data), False, "CRC is checked while reading an entry")

    image = sdk.placeholder_png()
    bad = bytearray(image); bad[29] ^= 1
    for name, data, reason in [
        ("png_crc_failure", bytes(bad), "PNG chunk CRCs are checked"),
        ("png_truncated", image[:-6], "PNG IEND must be complete"),
        ("png_invalid_zlib", png(compressed=b"\x78\x9cINVALID"), "IDAT must contain a complete zlib stream"),
        ("png_inflated_extra", png(rows=(b"\0" + bytes((240,240,240,255))*8)*8+b"\0"), "Extra decoded scanline data is rejected"),
        ("png_inflated_short", png(rows=b"short"), "Decoded pixel data must match the declared dimensions"),
        ("png_invalid_filter", png(rows=(b"\5"+bytes((240,240,240,255))*8)*8), "PNG row filters must be 0 through 4"),
        ("png_sixteen_bit", png(depth=16), "Only eight-bit PNG is supported"),
        ("png_indexed", png(kind=3), "Only RGB and RGBA PNG is supported"),
        ("png_interlaced", png(interlace=1), "Interlaced PNG is unsupported"),
        ("png_animated", png(before=(chunk(b"acTL", struct.pack(">II",1,0)),)), "APNG control chunks are forbidden"),
        ("png_unknown_critical_chunk", png(before=(chunk(b"ABCD", b""),)), "Unknown critical PNG chunks are rejected"),
        ("png_trailing_bytes", image+b"extra", "The image must end at IEND"),
        ("png_dimension_limit", png(width=2049, height=1), "Dimensions are capped before pixel decoding"),
    ]:
        add(name, sdk.archive_bytes(base_files(png=data)), False, reason)
    header, rest = image[8:33], image[33:]
    add("png_duplicate_header", sdk.archive_bytes(base_files(png=image[:33] + header + rest)), False, "IHDR must appear exactly once at the start")
    for name, padding in (("png_preview_file_limit", CONTRACT["limits"]["previewBytes"]), ("png_asset_file_limit", CONTRACT["limits"]["imageBytes"])):
        padded = png(before=(chunk(b"tEXt", b"fixture\0" + b"x" * padding),))
        if name == "png_preview_file_limit":
            content = base_files(png=padded)
        else:
            value = with_image()
            value["themes"]["light"]["images"]["keyNormal"]["file"] = "material.png"
            content = {**base_files(value), "material.png": padded}
        add(name, sdk.archive_bytes(content), False, "Each image and preview has its own byte budget")
    large = sdk.template_manifest()
    large_files = base_files(large)
    for index in range(5):
        path = f"pixels_{index}.png"
        large_files[path] = sdk.placeholder_png(2048,2048,(30+index,40,50,255))
        large["themes"]["light"]["images"][CONTRACT["imageRoles"][index]] = {"file":path,"scale":"stretch","insets":[0,0,0,0]}
    large_files["manifest.json"] = json_bytes(large)
    add("png_total_pixel_budget", sdk.archive_bytes(large_files), False, "Five compressible 2048-square images exceed the aggregate decoded budget")

    expanded = sdk.template_manifest()
    expanded_files = base_files(expanded)
    padded = png(before=(chunk(b"tEXt", b"fixture\0" + b"x" * 3_200_000),))
    for mode in CONTRACT["themes"]:
        for role in CONTRACT["imageRoles"]:
            path = f"{mode}/{role.lower()}.png"
            expanded_files[path] = padded
            expanded["themes"][mode]["images"][role] = {"file":path,"scale":"stretch","insets":[0,0,0,0]}
    expanded_files["manifest.json"] = json_bytes(expanded)
    add("zip_expanded_budget", sdk.archive_bytes(expanded_files), False, "Individually small compressed entries cannot exceed the total expanded byte budget")
    return vectors


def schema_document():
    non_control = r"^[^\u0000-\u001f\u007f-\u009f\ud800-\udfff]*$"
    color = {"type":"string","pattern":r"^#[0-9a-fA-F]{6}$","minLength":7,"maxLength":7}
    path = {"type":"string","minLength":5,"maxLength":96,
            "$comment":"Each component also excludes Windows device names so the same pack remains portable.",
            "allOf":[{"pattern":r"^(?:[a-z0-9_-]+/)*[a-z0-9_-]+\.png$"},
                     {"pattern":r"^(?!(?:[a-z0-9_-]+/)*(?:con|prn|aux|nul|com[1-9]|lpt[1-9])(?:/|\.png$))"},
                     {"pattern":non_control}]}
    image = {"type":"object","additionalProperties":False,"required":["file","scale","insets"],
             "properties":{"file":{"$ref":"#/$defs/imagePath"},"scale":{"enum":CONTRACT["scales"]},
                           "insets":{"type":"array","minItems":4,"maxItems":4,"items":{"type":"integer","minimum":0,"maximum":2147483647}}},
             "if":{"properties":{"scale":{"const":"nineSlice"}}},
             "else":{"properties":{"insets":{"const":[0,0,0,0]}}}}
    colors = {"type":"object","additionalProperties":False,"required":CONTRACT["colors"],
              "properties":{name:{"$ref":"#/$defs/color"} for name in CONTRACT["colors"]}}
    theme = {"type":"object","additionalProperties":False,"required":["colors","glow","images"],
             "properties":{"colors":{"$ref":"#/$defs/colors"},
                           "glow":{"type":"array","minItems":4,"maxItems":4,"items":{"$ref":"#/$defs/color"}},
                           "images":{"type":"object","additionalProperties":False,"properties":{role:{"$ref":"#/$defs/image"} for role in CONTRACT["imageRoles"]}}}}
    return {"$schema":"https://json-schema.org/draft/2020-12/schema","$id":"urn:fangcun:skin:schema:v1",
            "title":"方寸 .fcskin v1 manifest","type":"object","additionalProperties":False,
            "$comment":"This structural schema cannot detect duplicate JSON keys, lexical integer-vs-float tokens, PNG dimensions or ZIP metadata. Run tools/fcskin.py validate for complete wire validation.",
            "required":["format","schemaVersion","id","name","version","baseSkin","assetScale","fontFamily","pressOffsetDp","preview","themes"],
            "properties":{"format":{"const":"fangcun.skin"},"schemaVersion":{"type":"integer","const":1},
                "id":{"type":"string","minLength":3,"maxLength":64,"allOf":[{"pattern":r"^[a-z][a-z0-9]*(?:\.[a-z0-9][a-z0-9_-]*)+$"},{"pattern":non_control}]},
                "name":{"type":"string","minLength":1,"maxLength":40,"allOf":[{"pattern":non_control},{"pattern":r"\S"}]},
                "author":{"type":"string","maxLength":48,"pattern":non_control},
                "license":{"type":"string","maxLength":120,"pattern":non_control},
                "version":{"type":"integer","minimum":1,"maximum":2147483647},
                "baseSkin":{"enum":CONTRACT["baseSkins"]},"assetScale":{"type":"integer","minimum":1,"maximum":4},
                "fontFamily":{"enum":CONTRACT["fontFamilies"]},"pressOffsetDp":{"type":"number","minimum":0,"maximum":2},
                "preview":{"$ref":"#/$defs/imagePath"},
                "themes":{"type":"object","additionalProperties":False,"required":CONTRACT["themes"],"properties":{mode:{"$ref":"#/$defs/theme"} for mode in CONTRACT["themes"]}}},
            "$defs":{"color":color,"imagePath":path,"image":image,"colors":colors,"theme":theme}}


def write_vectors(folder):
    folder.mkdir(parents=True,exist_ok=True)
    index = {"format":"fangcun.skin-test-vectors","version":1,"vectors":[]}
    for name,data,valid,reason in make_vectors():
        filename = name + ".fcskin"
        (folder/filename).write_bytes(data)
        index["vectors"].append({"name":name,"file":filename,"valid":valid,"reason":reason,
                                 "sha256":hashlib.sha256(data).hexdigest(),"bytes":len(data)})
    (folder/"index.json").write_text(json.dumps(index,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    return index


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output",type=Path,default=ROOT/"test-vectors")
    args=parser.parse_args()
    index=write_vectors(args.output)
    (ROOT/"schema-v1.json").write_text(json.dumps(schema_document(),ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    print(json.dumps({"vectors":len(index["vectors"]),"valid":sum(v["valid"] for v in index["vectors"]),
                      "archiveBytes":sum(v["bytes"] for v in index["vectors"])},ensure_ascii=False))


if __name__=="__main__":
    main()
