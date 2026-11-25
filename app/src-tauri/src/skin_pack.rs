//! Fixed, portable .fcskin v1 data. No paths or executables supplied by a pack
//! are trusted. ZIP inflation and PNG decoding use their standard codecs.
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};
use std::io::{Cursor, Read};

pub const MAX_ARCHIVE_BYTES: usize = 24 * 1024 * 1024;
pub const MAX_EXPANDED_BYTES: usize = 48 * 1024 * 1024;
pub const MAX_MANIFEST_BYTES: usize = 64 * 1024;
pub const MAX_IMAGE_BYTES: usize = 4 * 1024 * 1024;
pub const MAX_PREVIEW_BYTES: usize = 1024 * 1024;
pub const MAX_PIXELS: u64 = 16_777_216;
pub const COLORS: [&str; 18] = [
    "background",
    "surface",
    "border",
    "function",
    "text",
    "muted",
    "accent",
    "onAccent",
    "pressed",
    "toolbar",
    "candidateBackground",
    "candidateText",
    "selection",
    "onSelection",
    "keyText",
    "keyHint",
    "functionText",
    "codeText",
];
pub const IMAGE_ROLES: [&str; 8] = [
    "deck",
    "toolbar",
    "code",
    "candidates",
    "selection",
    "keyNormal",
    "keyFunction",
    "keyAction",
];
const INVALID: &str = "皮肤包格式不正确，请使用方寸 .fcskin v1 皮肤包";

#[derive(Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct SkinPackManifest {
    pub format: String,
    pub schema_version: u32,
    pub id: String,
    pub name: String,
    pub version: u32,
    #[serde(default)]
    pub author: String,
    #[serde(default)]
    pub license: String,
    pub base_skin: String,
    pub asset_scale: u32,
    pub font_family: String,
    pub press_offset_dp: f64,
    pub preview: String,
    pub themes: SkinPackThemes,
}

#[derive(Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SkinPackThemes {
    pub light: SkinPackTheme,
    pub dark: SkinPackTheme,
}

#[derive(Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SkinPackTheme {
    #[serde(deserialize_with = "unique_map")]
    pub colors: BTreeMap<String, String>,
    pub glow: Vec<String>,
    #[serde(deserialize_with = "unique_map")]
    pub images: BTreeMap<String, SkinPackImage>,
}

#[derive(Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SkinPackImage {
    pub file: String,
    pub scale: String,
    pub insets: [u32; 4],
}

pub struct ValidatedPack {
    pub manifest: SkinPackManifest,
    pub hash: String,
    pub files: BTreeMap<String, Vec<u8>>,
}

fn unique_map<'de, D, T>(deserializer: D) -> Result<BTreeMap<String, T>, D::Error>
where
    D: serde::Deserializer<'de>,
    T: Deserialize<'de>,
{
    struct Visitor<T>(std::marker::PhantomData<T>);
    impl<'de, T: Deserialize<'de>> serde::de::Visitor<'de> for Visitor<T> {
        type Value = BTreeMap<String, T>;
        fn expecting(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
            f.write_str("an object with unique fields")
        }
        fn visit_map<A: serde::de::MapAccess<'de>>(
            self,
            mut map: A,
        ) -> Result<Self::Value, A::Error> {
            let mut result = BTreeMap::new();
            while let Some((key, value)) = map.next_entry::<String, T>()? {
                if result.insert(key, value).is_some() {
                    return Err(serde::de::Error::custom("duplicate field"));
                }
            }
            Ok(result)
        }
    }
    deserializer.deserialize_map(Visitor(std::marker::PhantomData))
}

pub fn valid_id(id: &str) -> bool {
    if !(3..=64).contains(&id.len()) {
        return false;
    }
    let parts: Vec<_> = id.split('.').collect();
    parts.len() >= 2
        && parts.iter().enumerate().all(|(index, part)| {
            !part.is_empty()
                && part.bytes().enumerate().all(|(offset, c)| {
                    if index == 0 {
                        c.is_ascii_lowercase() || (offset > 0 && c.is_ascii_digit())
                    } else {
                        c.is_ascii_lowercase()
                            || c.is_ascii_digit()
                            || (offset > 0 && matches!(c, b'_' | b'-'))
                    }
                })
        })
}

fn valid_component(part: &str) -> bool {
    let reserved = matches!(part, "con" | "prn" | "aux" | "nul")
        || ((part.starts_with("com") || part.starts_with("lpt"))
            && part.len() == 4
            && matches!(part.as_bytes()[3], b'1'..=b'9'));
    !part.is_empty()
        && !reserved
        && part
            .bytes()
            .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || matches!(c, b'_' | b'-'))
}

pub fn valid_image_path(path: &str) -> bool {
    path.len() <= 96
        && path
            .strip_suffix(".png")
            .is_some_and(|stem| stem.split('/').all(valid_component))
}

fn valid_directory(path: &str) -> bool {
    path.len() <= 96
        && path
            .strip_suffix('/')
            .is_some_and(|stem| stem.split('/').all(valid_component))
}

fn color(value: &str) -> bool {
    value.len() == 7
        && value.starts_with('#')
        && value.as_bytes()[1..].iter().all(u8::is_ascii_hexdigit)
}

fn label(value: &str, minimum: usize, maximum: usize) -> bool {
    (minimum..=maximum).contains(&value.chars().count())
        && !value.chars().any(char::is_control)
        && (minimum == 0 || !value.trim().is_empty())
}

fn le16(data: &[u8], at: usize) -> Option<u16> {
    Some(u16::from_le_bytes(data.get(at..at + 2)?.try_into().ok()?))
}
fn le32(data: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_le_bytes(data.get(at..at + 4)?.try_into().ok()?))
}

fn zip_extra(data: &[u8], start: usize, size: usize) -> Option<()> {
    let end = start.checked_add(size).filter(|&n| n <= data.len())?;
    let mut at = start;
    while at < end {
        if at + 4 > end {
            return None;
        }
        let kind = le16(data, at)?;
        let length = le16(data, at + 2)? as usize;
        if matches!(kind, 1 | 0x9901) || at + 4 + length > end {
            return None;
        }
        at += 4 + length;
    }
    Some(())
}

fn zip_metadata(data: &[u8]) -> Option<()> {
    if data.len() < 22 || !data.starts_with(b"PK\x03\x04") {
        return None;
    }
    // Locate the real EOCD, including an optional bounded ZIP comment. Reject
    // ZIP64 and multi-volume metadata before the codec allocates its directory.
    let start = data.len().saturating_sub(65_557);
    let end = (start..=data.len() - 22).rev().find(|&at| {
        data.get(at..at + 4) == Some(b"PK\x05\x06")
            && le16(data, at + 20).is_some_and(|n| at + 22 + n as usize == data.len())
    });
    let end = end?;
    if le16(data, end + 4)? != 0
        || le16(data, end + 6)? != 0
        || le16(data, end + 8)? != le16(data, end + 10)?
    {
        return None;
    }
    let count = le16(data, end + 10)? as usize;
    if count == 0 || count > 64 {
        return None;
    }
    let size = le32(data, end + 12)? as usize;
    let central = le32(data, end + 16)? as usize;
    if central.checked_add(size)? != end
        || (end >= 20 && data.get(end - 20..end - 16) == Some(b"PK\x06\x07"))
    {
        return None;
    }
    let mut at = central;
    let mut regions = Vec::new();
    let mut names = BTreeSet::new();
    let mut expanded = 0u64;
    for _ in 0..count {
        if at + 46 > end || data.get(at..at + 4) != Some(b"PK\x01\x02") {
            return None;
        }
        let flags = le16(data, at + 8)?;
        let method = le16(data, at + 10)?;
        let crc = le32(data, at + 16)?;
        let compressed = le32(data, at + 20)? as usize;
        let inflated = le32(data, at + 24)? as usize;
        let name_len = le16(data, at + 28)? as usize;
        let extra_len = le16(data, at + 30)? as usize;
        let comment_len = le16(data, at + 32)? as usize;
        let attributes = le32(data, at + 38)?;
        let local = le32(data, at + 42)? as usize;
        let finish = at.checked_add(46 + name_len + extra_len + comment_len)?;
        if finish > end
            || !(1..=97).contains(&name_len)
            || !matches!(method, 0 | 8)
            || flags & 0xf7f1 != 0
            || le16(data, at + 34)? != 0
            || attributes & 0x400 != 0
        {
            return None;
        }
        let raw_name = data.get(at + 46..at + 46 + name_len)?;
        let name = std::str::from_utf8(raw_name).ok()?;
        let directory = name.ends_with('/');
        if !names.insert(name)
            || !(name == "manifest.json" || valid_image_path(name) || valid_directory(name))
        {
            return None;
        }
        let mode = (attributes >> 16) & 0xf000;
        if mode != 0 && mode != if directory { 0x4000 } else { 0x8000 } {
            return None;
        }
        if inflated > MAX_IMAGE_BYTES
            || compressed > MAX_ARCHIVE_BYTES
            || (directory && inflated != 0)
        {
            return None;
        }
        expanded += inflated as u64;
        if expanded > MAX_EXPANDED_BYTES as u64 {
            return None;
        }
        zip_extra(data, at + 46 + name_len, extra_len)?;
        if local + 30 > central
            || data.get(local..local + 4) != Some(b"PK\x03\x04")
            || le16(data, local + 6)? != flags
            || le16(data, local + 8)? != method
        {
            return None;
        }
        let local_name = le16(data, local + 26)? as usize;
        let local_extra = le16(data, local + 28)? as usize;
        let body_start = local.checked_add(30 + local_name + local_extra)?;
        let mut region_end = body_start.checked_add(compressed)?;
        if local_name != name_len
            || region_end > central
            || data.get(local + 30..local + 30 + local_name)? != raw_name
        {
            return None;
        }
        zip_extra(data, local + 30 + local_name, local_extra)?;
        if flags & 8 == 0 {
            if le32(data, local + 14)? != crc
                || le32(data, local + 18)? as usize != compressed
                || le32(data, local + 22)? as usize != inflated
            {
                return None;
            }
        } else {
            if le32(data, region_end)? == 0x08074b50 {
                region_end += 4;
            }
            if region_end + 12 > central
                || le32(data, region_end)? != crc
                || le32(data, region_end + 4)? as usize != compressed
                || le32(data, region_end + 8)? as usize != inflated
            {
                return None;
            }
            region_end += 12;
        }
        regions.push((local, region_end));
        at = finish;
    }
    if at != end {
        return None;
    }
    regions.sort_unstable();
    let mut next = 0;
    for (start, finish) in regions {
        if start != next || finish < start {
            return None;
        }
        next = finish;
    }
    (next == central).then_some(())
}

pub fn validate_archive(bytes: &[u8]) -> Result<ValidatedPack, String> {
    if bytes.len() > MAX_ARCHIVE_BYTES || zip_metadata(bytes).is_none() {
        return Err(INVALID.into());
    }
    let mut zip = zip::ZipArchive::new(Cursor::new(bytes)).map_err(|_| INVALID)?;
    if zip.len() > 64 {
        return Err(INVALID.into());
    }
    let mut files = BTreeMap::new();
    let mut names = BTreeSet::new();
    let mut directories = Vec::new();
    let mut total = 0usize;
    for index in 0..zip.len() {
        let mut item = zip.by_index(index).map_err(|_| INVALID)?;
        let name = item.name().to_owned();
        if !names.insert(name.clone())
            || !matches!(
                item.compression(),
                zip::CompressionMethod::Stored | zip::CompressionMethod::Deflated
            )
        {
            return Err(INVALID.into());
        }
        let mode = item.unix_mode().unwrap_or(0) & 0o170000;
        if item.is_dir() {
            if !matches!(mode, 0 | 0o040000) || !valid_directory(&name) || item.size() != 0 {
                return Err(INVALID.into());
            }
            directories.push(name);
            continue;
        }
        if !matches!(mode, 0 | 0o100000) || !(name == "manifest.json" || valid_image_path(&name)) {
            return Err(INVALID.into());
        }
        let limit = if name == "manifest.json" {
            MAX_MANIFEST_BYTES
        } else {
            MAX_IMAGE_BYTES
        };
        if item.size() > limit as u64 {
            return Err("皮肤素材过大，未导入".into());
        }
        let mut body = Vec::new();
        Read::by_ref(&mut item)
            .take(limit as u64 + 1)
            .read_to_end(&mut body)
            .map_err(|_| INVALID)?;
        if body.len() > limit || body.len() as u64 != item.size() {
            return Err(INVALID.into());
        }
        total = total.checked_add(body.len()).ok_or(INVALID)?;
        if total > MAX_EXPANDED_BYTES {
            return Err("皮肤包展开后过大，未导入".into());
        }
        files.insert(name, body);
    }
    if directories
        .iter()
        .any(|dir| !files.keys().any(|file| file.starts_with(dir)))
    {
        return Err(INVALID.into());
    }
    let manifest = validate_files(&files)?;
    Ok(ValidatedPack {
        manifest,
        hash: format!("{:x}", Sha256::digest(bytes)),
        files,
    })
}

pub fn validate_files(files: &BTreeMap<String, Vec<u8>>) -> Result<SkinPackManifest, String> {
    if files.is_empty()
        || files.len() > 64
        || files
            .values()
            .try_fold(0usize, |n, b| n.checked_add(b.len()))
            .is_none_or(|n| n > MAX_EXPANDED_BYTES)
    {
        return Err(INVALID.into());
    }
    let json = files
        .get("manifest.json")
        .filter(|b| b.len() <= MAX_MANIFEST_BYTES)
        .ok_or(INVALID)?;
    let manifest: SkinPackManifest = serde_json::from_slice(json).map_err(|_| INVALID)?;
    if manifest.format != "fangcun.skin"
        || manifest.schema_version != 1
        || !valid_id(&manifest.id)
        || !label(&manifest.name, 1, 40)
        || !label(&manifest.author, 0, 48)
        || !label(&manifest.license, 0, 120)
        || manifest.version == 0
        || manifest.version > i32::MAX as u32
        || !["mechanical", "typewriter", "mahjong", "brutal"].contains(&manifest.base_skin.as_str())
        || !(1..=4).contains(&manifest.asset_scale)
        || !["sans", "serif", "mono"].contains(&manifest.font_family.as_str())
        || !manifest.press_offset_dp.is_finite()
        || !(0.0..=2.0).contains(&manifest.press_offset_dp)
        || !valid_image_path(&manifest.preview)
    {
        return Err(INVALID.into());
    }
    let mut required = BTreeSet::from(["manifest.json".to_string(), manifest.preview.clone()]);
    for theme in [&manifest.themes.light, &manifest.themes.dark] {
        if theme.colors.len() != COLORS.len()
            || COLORS
                .iter()
                .any(|key| !theme.colors.get(*key).is_some_and(|v| color(v)))
            || theme.glow.len() != 4
            || theme.glow.iter().any(|v| !color(v))
            || theme
                .images
                .keys()
                .any(|key| !IMAGE_ROLES.contains(&key.as_str()))
        {
            return Err(INVALID.into());
        }
        for image in theme.images.values() {
            if !valid_image_path(&image.file)
                || !["stretch", "contain", "tile", "nineSlice"].contains(&image.scale.as_str())
                || (image.scale != "nineSlice" && image.insets != [0; 4])
            {
                return Err(INVALID.into());
            }
            required.insert(image.file.clone());
        }
    }
    if files.keys().any(|name| !required.contains(name))
        || required.iter().any(|name| !files.contains_key(name))
    {
        return Err(INVALID.into());
    }
    let mut dimensions = BTreeMap::new();
    let mut pixels = 0u64;
    for (name, data) in files
        .iter()
        .filter(|(name, _)| name.as_str() != "manifest.json")
    {
        if data.len() > MAX_IMAGE_BYTES
            || (name == &manifest.preview && data.len() > MAX_PREVIEW_BYTES)
        {
            return Err("皮肤图片过大，未导入".into());
        }
        let (width, height) = png_dimensions(data)?;
        pixels = pixels
            .checked_add(width as u64 * height as u64)
            .ok_or(INVALID)?;
        if pixels > MAX_PIXELS {
            return Err("皮肤图片总像素过大，未导入".into());
        }
        dimensions.insert(name.as_str(), (width, height));
    }
    for theme in [&manifest.themes.light, &manifest.themes.dark] {
        for image in theme.images.values() {
            let (width, height) = dimensions[image.file.as_str()];
            if image.insets[0] as u64 + image.insets[2] as u64 >= width as u64
                || image.insets[1] as u64 + image.insets[3] as u64 >= height as u64
            {
                return Err("皮肤九切片边距不正确，未导入".into());
            }
        }
    }
    Ok(manifest)
}

fn png_dimensions(data: &[u8]) -> Result<(u32, u32), String> {
    if data.len() < 45
        || !data.starts_with(b"\x89PNG\r\n\x1a\n")
        || data.get(8..16) != Some(b"\0\0\0\rIHDR")
    {
        return Err("皮肤图片不是有效的普通 PNG".into());
    }
    let number = |at: usize| u32::from_be_bytes(data[at..at + 4].try_into().unwrap());
    let (width, height) = (number(16), number(20));
    if width == 0
        || height == 0
        || width > 2048
        || height > 2048
        || data[24] != 8
        || !matches!(data[25], 2 | 6)
        || data[26..29] != [0, 0, 0]
    {
        return Err("皮肤 PNG 尺寸或编码不符合 v1 规范".into());
    }
    let mut offset = 8usize;
    let mut ended = false;
    let mut compressed = Vec::new();
    let mut seen_data = false;
    let mut data_ended = false;
    while offset < data.len() {
        if offset + 12 > data.len() {
            return Err(INVALID.into());
        }
        let length = number(offset) as usize;
        let end = offset
            .checked_add(12)
            .and_then(|n| n.checked_add(length))
            .filter(|&n| n <= data.len())
            .ok_or(INVALID)?;
        let kind = &data[offset + 4..offset + 8];
        if matches!(kind, b"acTL" | b"fcTL" | b"fdAT") {
            return Err("暂不支持动画皮肤图片".into());
        }
        if crc32fast::hash(&data[offset + 4..end - 4]) != number(end - 4) {
            return Err("皮肤 PNG 校验失败，未导入".into());
        }
        if kind == b"IHDR" && (offset != 8 || length != 13) {
            return Err(INVALID.into());
        }
        if kind == b"PLTE" && (seen_data || !(3..=768).contains(&length) || length % 3 != 0) {
            return Err(INVALID.into());
        }
        if !matches!(kind, b"IHDR" | b"PLTE" | b"IDAT" | b"IEND") && !kind[0].is_ascii_lowercase() {
            return Err(INVALID.into());
        }
        if kind == b"IDAT" {
            if data_ended {
                return Err(INVALID.into());
            }
            seen_data = true;
            compressed.extend_from_slice(&data[offset + 8..end - 4]);
        }
        if seen_data && kind != b"IDAT" {
            data_ended = true;
        }
        if kind == b"IEND" {
            if length != 0 || end != data.len() {
                return Err(INVALID.into());
            }
            ended = true;
        }
        offset = end;
    }
    if !ended {
        return Err(INVALID.into());
    }
    // next_frame() may finish a valid visible frame without consuming surplus
    // inflated scanlines. Validate the entire bounded zlib stream as well, so
    // all three v1 readers agree on truncated/extra image data.
    {
        let stride = width as usize * if data[25] == 2 { 3 } else { 4 } + 1;
        let expected = stride * height as usize;
        let mut decoder = flate2::bufread::ZlibDecoder::new(compressed.as_slice());
        let mut rows = Vec::new();
        Read::by_ref(&mut decoder)
            .take(expected as u64 + 1)
            .read_to_end(&mut rows)
            .map_err(|_| "PNG 图像数据解码失败")?;
        if rows.len() != expected
            || decoder.total_in() != compressed.len() as u64
            || (0..height as usize).any(|row| rows[row * stride] > 4)
        {
            return Err("PNG 图像数据长度不正确".into());
        }
    }
    let mut decoder = png::Decoder::new(Cursor::new(data));
    decoder.set_limits(png::Limits {
        bytes: 64 * 1024 * 1024,
    });
    decoder.set_ignore_text_chunk(true);
    let mut reader = decoder.read_info().map_err(|_| "无法解码皮肤 PNG")?;
    let expected = (width as usize)
        .checked_mul(height as usize)
        .and_then(|n| n.checked_mul(4))
        .ok_or(INVALID)?;
    if reader.output_buffer_size() > expected {
        return Err(INVALID.into());
    }
    let mut pixels = vec![0; reader.output_buffer_size()];
    let frame = reader
        .next_frame(&mut pixels)
        .map_err(|_| "无法解码皮肤 PNG")?;
    if frame.width != width || frame.height != height {
        return Err(INVALID.into());
    }
    Ok((width, height))
}

fn snake(value: &str) -> String {
    let mut result = String::new();
    for c in value.chars() {
        if c.is_ascii_uppercase() {
            result.push('_');
            result.push(c.to_ascii_lowercase());
        } else {
            result.push(c);
        }
    }
    result
}

pub fn resolved_ini(pack: &ValidatedPack) -> Result<String, String> {
    let m = &pack.manifest;
    let mut result = format!("schema_version=1\nid={}\nhash={}\nbase_skin={}\nasset_scale={}\nfont_family={}\npress_offset_dp={}\n", m.id, pack.hash, m.base_skin, m.asset_scale, m.font_family, m.press_offset_dp);
    for (mode, theme) in [("light", &m.themes.light), ("dark", &m.themes.dark)] {
        for (key, value) in &theme.colors {
            result.push_str(&format!("{mode}_{}={value}\n", snake(key)));
        }
        for (index, value) in theme.glow.iter().enumerate() {
            result.push_str(&format!("{mode}_glow_{index}={value}\n"));
        }
        for (role, image) in &theme.images {
            let role = snake(role);
            result.push_str(&format!("{mode}_image_{role}={}\n{mode}_scale_{role}={}\n{mode}_insets_{role}={},{},{},{}\n", image.file, image.scale, image.insets[0], image.insets[1], image.insets[2], image.insets[3]));
        }
    }
    if result.len() > 16 * 1024 {
        return Err(INVALID.into());
    }
    Ok(result)
}

#[cfg(test)]
#[path = "skin_pack_tests.rs"]
mod tests;
