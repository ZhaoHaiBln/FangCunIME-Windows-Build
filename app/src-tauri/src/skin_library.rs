//! Passive, immutable local skin packages. The existing INI transaction is the
//! authority; cached metadata never replaces a failed settings read.
use crate::builtin_skin_edits::{self, BuiltinEdits, BuiltinStyle};
use crate::settings::{self, AppSettings};
use crate::settings_io;
use crate::skin_pack::{self, SkinPackManifest, ValidatedPack};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

const SETTINGS_FILE: &str = "settings.ini";
const REGISTRY_KEY: &str = "skin_library_v1";
const CONFLICT: &str = "皮肤列表已更新，请重新读取后重试";
const INVALID: &str = "皮肤库数据无效，已保留原文件";
const FILE_ERROR: &str = "皮肤文件不可用，原选择和文件已保留";
const BUILTINS: [(&str, &str); 4] = [
    ("wood_type", "木框活字"),
    ("typewriter", "经典打字机"),
    ("terminal", "绿屏终端"),
    ("brutal", "极客机械"),
];

#[derive(Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct PackRef {
    id: String,
    hash: String,
}

#[derive(Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct Registry {
    version: u32,
    import_enabled: bool,
    hidden_builtins: Vec<String>,
    packs: Vec<PackRef>,
    #[serde(skip)]
    builtin_edits: BuiltinEdits,
}
impl Default for Registry {
    fn default() -> Self {
        Self {
            version: 1,
            import_enabled: true,
            hidden_builtins: vec![],
            packs: vec![],
            builtin_edits: BuiltinEdits::default(),
        }
    }
}
impl Registry {
    fn from_document(document: &str) -> Result<Self, String> {
        let mut value = None;
        for line in document.lines() {
            let line = line.trim().trim_start_matches('\u{feff}');
            if line.starts_with(['#', ';']) {
                continue;
            }
            if let Some((key, raw)) = line.split_once('=') {
                if key.trim() == REGISTRY_KEY {
                    value = Some(raw.trim());
                }
            }
        }
        let mut registry: Self = match value {
            None => Self::default(),
            Some(raw) => serde_json::from_str(raw).map_err(|_| INVALID.to_string())?,
        };
        let hidden: BTreeSet<_> = registry.hidden_builtins.iter().collect();
        let ids: BTreeSet<_> = registry.packs.iter().map(|pack| &pack.id).collect();
        let hashes: BTreeSet<_> = registry.packs.iter().map(|pack| &pack.hash).collect();
        if registry.version != 1
            || hidden.len() != registry.hidden_builtins.len()
            || hidden.len() >= BUILTINS.len()
            || hidden.iter().any(|id| !is_builtin(id))
            || ids.len() != registry.packs.len()
            || hashes.len() != registry.packs.len()
            || registry
                .packs
                .iter()
                .any(|pack| !skin_pack::valid_id(&pack.id) || !valid_hash(&pack.hash))
        {
            return Err(INVALID.into());
        }
        registry.builtin_edits = BuiltinEdits::from_document(document)?;
        Ok(registry)
    }
    fn fallback(&self) -> &'static str {
        BUILTINS
            .iter()
            .find(|(id, _)| !self.hidden_builtins.iter().any(|hidden| hidden == id))
            .unwrap()
            .0
    }
    fn encode(&self) -> Result<String, String> {
        serde_json::to_string(self).map_err(|_| INVALID.into())
    }
}

fn valid_hash(hash: &str) -> bool {
    hash.len() == 64
        && hash
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}
fn is_builtin(id: &str) -> bool {
    BUILTINS.iter().any(|(candidate, _)| *candidate == id)
}
pub(crate) fn visible_fallback(document: &str) -> Result<&'static str, String> {
    Ok(Registry::from_document(document)?.fallback())
}
fn native_base(common: &str) -> &'static str {
    match common {
        "typewriter" => "typewriter",
        "mahjong" => "terminal",
        "brutal" => "brutal",
        _ => "wood_type",
    }
}
fn revision(registry: &Registry, settings: &AppSettings) -> Result<String, String> {
    let source = serde_json::to_vec(&(
        registry,
        &settings.skin,
        &settings.custom_skin_id,
        &settings.custom_skin_hash,
        &registry.builtin_edits.raw,
    ))
    .map_err(|_| INVALID)?;
    Ok(format!("{:x}", Sha256::digest(source)))
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct BuiltinView {
    pub id: String,
    pub label: String,
    pub hidden: bool,
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PackView {
    pub id: String,
    pub hash: String,
    pub name: String,
    pub version: u32,
    pub author: String,
    pub base_skin: String,
    pub preview_url: String,
    pub unavailable: bool,
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ActiveSkin {
    pub id: String,
    pub hash: String,
    pub manifest: SkinPackManifest,
    pub image_urls: BTreeMap<String, BTreeMap<String, String>>,
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SkinSnapshot {
    pub revision: String,
    pub import_enabled: bool,
    pub hidden_builtins: Vec<String>,
    pub builtins: Vec<BuiltinView>,
    pub packs: Vec<PackView>,
    pub active_id: String,
    pub active: Option<ActiveSkin>,
    pub warning: Option<String>,
    pub builtin_styles: BTreeMap<String, BuiltinStyle>,
}

struct InstalledPack {
    manifest: SkinPackManifest,
    allowed: BTreeSet<String>,
}
pub struct SkinLibrary {
    root: PathBuf,
    cache: Mutex<BTreeMap<String, Arc<InstalledPack>>>,
    builtin_cache: Mutex<BTreeMap<String, BuiltinStyle>>,
}

impl SkinLibrary {
    pub fn new(root: PathBuf) -> Self {
        Self {
            root,
            cache: Mutex::new(BTreeMap::new()),
            builtin_cache: Mutex::new(BTreeMap::new()),
        }
    }
    fn settings_path(&self) -> PathBuf {
        self.root.join(SETTINGS_FILE)
    }
    fn document(&self) -> Result<(Registry, AppSettings), String> {
        let document = settings_io::read(&self.settings_path())?.unwrap_or_default();
        Ok((
            Registry::from_document(&document)?,
            settings::parse_content(&document),
        ))
    }
    pub fn snapshot(&self) -> Result<SkinSnapshot, String> {
        let (registry, settings) = self.document()?;
        self.snapshot_for(registry, settings)
    }
    fn snapshot_for(
        &self,
        registry: Registry,
        settings: AppSettings,
    ) -> Result<SkinSnapshot, String> {
        *self.builtin_cache.lock().map_err(|_| FILE_ERROR)? = registry.builtin_edits.styles.clone();
        let mut packs = vec![];
        let mut active = None;
        let mut warning = None;
        for entry in &registry.packs {
            match self.installed(entry) {
                Ok(pack) => {
                    packs.push(PackView {
                        id: entry.id.clone(),
                        hash: entry.hash.clone(),
                        name: pack.manifest.name.clone(),
                        version: pack.manifest.version,
                        author: pack.manifest.author.clone(),
                        base_skin: pack.manifest.base_skin.clone(),
                        preview_url: resource_url(&entry.hash, &pack.manifest.preview),
                        unavailable: false,
                    });
                    if settings.custom_skin_id == entry.id
                        && settings.custom_skin_hash == entry.hash
                    {
                        let mut image_urls = BTreeMap::new();
                        for (mode, theme) in [
                            ("light", &pack.manifest.themes.light),
                            ("dark", &pack.manifest.themes.dark),
                        ] {
                            image_urls.insert(
                                mode.into(),
                                theme
                                    .images
                                    .iter()
                                    .map(|(role, image)| {
                                        (role.clone(), resource_url(&entry.hash, &image.file))
                                    })
                                    .collect(),
                            );
                        }
                        active = Some(ActiveSkin {
                            id: entry.id.clone(),
                            hash: entry.hash.clone(),
                            manifest: pack.manifest.clone(),
                            image_urls,
                        });
                    }
                }
                Err(_) => {
                    warning = Some("部分本地皮肤文件不可用，可以删除后重新导入".into());
                    packs.push(PackView {
                        id: entry.id.clone(),
                        hash: entry.hash.clone(),
                        name: entry.id.clone(),
                        version: 0,
                        author: String::new(),
                        base_skin: "mechanical".into(),
                        preview_url: String::new(),
                        unavailable: true,
                    });
                }
            }
        }
        let builtin =
            if is_builtin(&settings.skin) && !registry.hidden_builtins.contains(&settings.skin) {
                settings.skin.as_str()
            } else {
                registry.fallback()
            };
        let active_id = active
            .as_ref()
            .map(|pack| format!("local:{}", pack.id))
            .unwrap_or_else(|| format!("builtin:{builtin}"));
        Ok(SkinSnapshot {
            revision: revision(&registry, &settings)?,
            import_enabled: registry.import_enabled,
            builtins: BUILTINS
                .iter()
                .map(|(id, label)| BuiltinView {
                    id: (*id).into(),
                    label: (*label).into(),
                    hidden: registry.hidden_builtins.iter().any(|hidden| hidden == id),
                })
                .collect(),
            hidden_builtins: registry.hidden_builtins,
            packs,
            active_id,
            active,
            warning,
            builtin_styles: registry.builtin_edits.styles,
        })
    }

    fn installed(&self, entry: &PackRef) -> Result<Arc<InstalledPack>, String> {
        if let Some(pack) = self
            .cache
            .lock()
            .map_err(|_| FILE_ERROR)?
            .get(&entry.hash)
            .cloned()
        {
            return if pack.manifest.id == entry.id {
                Ok(pack)
            } else {
                Err(INVALID.into())
            };
        }
        let directory = checked_pack_root(&self.root, &entry.hash)?;
        let files = read_installed_files(&directory)?;
        let manifest = skin_pack::validate_files(&files)?;
        if manifest.id != entry.id {
            return Err(INVALID.into());
        }
        let expected = skin_pack::resolved_ini(&ValidatedPack {
            manifest: manifest.clone(),
            hash: entry.hash.clone(),
            files: BTreeMap::new(),
        })?;
        let actual = read_limited(&regular_child(&directory, "resolved.ini")?, 16 * 1024)?;
        if actual != expected.as_bytes() {
            return Err(FILE_ERROR.into());
        }
        let allowed = files
            .keys()
            .filter(|path| path.as_str() != "manifest.json")
            .cloned()
            .collect();
        let pack = Arc::new(InstalledPack { manifest, allowed });
        self.cache
            .lock()
            .map_err(|_| FILE_ERROR)?
            .insert(entry.hash.clone(), pack.clone());
        Ok(pack)
    }

    fn mutate(
        &self,
        expected: &str,
        change: impl FnOnce(&mut Registry, &mut AppSettings) -> Result<(), String>,
        committed: impl FnOnce(&AppSettings),
    ) -> Result<SkinSnapshot, String> {
        let mut result = None;
        let saved = settings::skin_transaction(
            &self.settings_path(),
            |document, current| {
                let mut registry = Registry::from_document(document)?;
                if revision(&registry, current)? != expected {
                    return Err(CONFLICT.into());
                }
                let old_styles = registry.builtin_edits.raw.clone();
                let mut settings = current.clone();
                change(&mut registry, &mut settings)?;
                let mut values = BTreeMap::new();
                if registry.builtin_edits.raw != old_styles {
                    // Derivatives are immutable and installed before the INI
                    // reference becomes visible to TSF. A failed durable write
                    // leaves at most an unreferenced, harmless small directory.
                    values.insert(
                        builtin_skin_edits::KEY.into(),
                        registry.builtin_edits.encode()?,
                    );
                    for id in builtin_skin_edits::IDS {
                        let hash = if let Some(style) = registry.builtin_edits.styles.get(id) {
                            install_builtin_style(&self.root, id, style)?
                        } else {
                            String::new()
                        };
                        values.insert(format!("builtin_style_{id}"), hash);
                    }
                }
                values.insert(REGISTRY_KEY.into(), registry.encode()?);
                values.insert("skin".into(), settings.skin.clone());
                values.insert("custom_skin_id".into(), settings.custom_skin_id.clone());
                values.insert("custom_skin_hash".into(), settings.custom_skin_hash.clone());
                result = Some(registry);
                Ok(values)
            },
            committed,
        )?;
        self.snapshot_for(result.ok_or(INVALID)?, saved)
    }

    pub fn save_builtin_style(
        &self,
        id: &str,
        style: BuiltinStyle,
        expected: &str,
        activate: bool,
        committed: impl FnOnce(&AppSettings),
    ) -> Result<SkinSnapshot, String> {
        if !is_builtin(id) {
            return Err("内置皮肤不存在".into());
        }
        style.validate()?;
        self.mutate(
            expected,
            |registry, settings| {
                registry.builtin_edits.styles.insert(id.into(), style);
                registry.builtin_edits.raw = Some(registry.builtin_edits.encode()?);
                if activate {
                    registry.hidden_builtins.retain(|hidden| hidden != id);
                    select_builtin(settings, id);
                }
                Ok(())
            },
            committed,
        )
    }

    pub fn reset_builtin_style(
        &self,
        id: &str,
        expected: &str,
        committed: impl FnOnce(&AppSettings),
    ) -> Result<SkinSnapshot, String> {
        if !is_builtin(id) {
            return Err("内置皮肤不存在".into());
        }
        self.mutate(
            expected,
            |registry, _| {
                registry.builtin_edits.styles.remove(id);
                registry.builtin_edits.raw = Some(registry.builtin_edits.encode()?);
                Ok(())
            },
            committed,
        )
    }

    pub fn set_import_enabled(
        &self,
        enabled: bool,
        expected: &str,
        committed: impl FnOnce(&AppSettings),
    ) -> Result<SkinSnapshot, String> {
        self.mutate(
            expected,
            |registry, _| {
                registry.import_enabled = enabled;
                Ok(())
            },
            committed,
        )
    }
    pub fn set_builtin_hidden(
        &self,
        id: &str,
        hidden: bool,
        expected: &str,
        committed: impl FnOnce(&AppSettings),
    ) -> Result<SkinSnapshot, String> {
        if !is_builtin(id) {
            return Err("内置皮肤不存在".into());
        }
        self.mutate(
            expected,
            |registry, settings| {
                registry.hidden_builtins.retain(|item| item != id);
                if hidden {
                    if registry.hidden_builtins.len() == BUILTINS.len() - 1 {
                        return Err("至少保留一款可见的内置皮肤".into());
                    }
                    registry.hidden_builtins.push(id.into());
                    registry.hidden_builtins.sort();
                    if settings.custom_skin_id.is_empty() && settings.skin == id {
                        select_builtin(settings, registry.fallback());
                    }
                }
                Ok(())
            },
            committed,
        )
    }
    pub fn select(
        &self,
        id: &str,
        expected: &str,
        committed: impl FnOnce(&AppSettings),
    ) -> Result<SkinSnapshot, String> {
        let (registry, current) = self.document()?;
        if revision(&registry, &current)? != expected {
            return Err(CONFLICT.into());
        }
        let local = if let Some(id) = id.strip_prefix("local:") {
            let entry = registry
                .packs
                .iter()
                .find(|entry| entry.id == id)
                .ok_or("本地皮肤不存在")?;
            Some((entry.clone(), self.installed(entry)?))
        } else {
            None
        };
        self.mutate(
            expected,
            |registry, settings| {
                if let Some((entry, pack)) = local {
                    if !registry
                        .packs
                        .iter()
                        .any(|current| current.id == entry.id && current.hash == entry.hash)
                    {
                        return Err(CONFLICT.into());
                    }
                    settings.skin = native_base(&pack.manifest.base_skin).into();
                    settings.custom_skin_id = entry.id;
                    settings.custom_skin_hash = entry.hash;
                } else {
                    let builtin = id.strip_prefix("builtin:").ok_or("皮肤选择无效")?;
                    if !is_builtin(builtin)
                        || registry
                            .hidden_builtins
                            .iter()
                            .any(|hidden| hidden == builtin)
                    {
                        return Err("该内置皮肤已隐藏或不存在".into());
                    }
                    select_builtin(settings, builtin);
                }
                Ok(())
            },
            committed,
        )
    }
    pub fn delete(
        &self,
        id: &str,
        expected: &str,
        committed: impl FnOnce(&AppSettings),
    ) -> Result<SkinSnapshot, String> {
        let id = id
            .strip_prefix("local:")
            .ok_or("只能移除本地皮肤；内置皮肤请使用隐藏")?;
        let mut removed = None;
        let snapshot = self.mutate(
            expected,
            |registry, settings| {
                let entry = registry
                    .packs
                    .iter()
                    .find(|entry| entry.id == id)
                    .cloned()
                    .ok_or("本地皮肤不存在")?;
                registry.packs.retain(|entry| entry.id != id);
                if settings.custom_skin_id == id {
                    select_builtin(settings, registry.fallback());
                }
                removed = Some(entry.hash);
                Ok(())
            },
            committed,
        )?;
        if let Some(hash) = removed {
            self.cleanup(&hash);
        }
        Ok(snapshot)
    }
    pub fn import_allowed(&self, expected: &str) -> Result<(), String> {
        let (registry, settings) = self.document()?;
        if revision(&registry, &settings)? != expected {
            return Err(CONFLICT.into());
        }
        if !registry.import_enabled {
            return Err("皮肤导入已关闭，已安装的皮肤仍可使用".into());
        }
        Ok(())
    }
    pub fn import_path(
        &self,
        path: &Path,
        expected: &str,
        committed: impl FnOnce(&AppSettings),
    ) -> Result<SkinSnapshot, String> {
        self.import_allowed(expected)?;
        if !path
            .extension()
            .is_some_and(|extension| extension.eq_ignore_ascii_case("fcskin"))
        {
            return Err("请选择 .fcskin 皮肤包".into());
        }
        let file = File::open(path).map_err(|_| "无法读取选中的皮肤包")?;
        if !file.metadata().map_err(|_| FILE_ERROR)?.is_file() {
            return Err("请选择皮肤包文件".into());
        }
        let mut bytes = Vec::new();
        file.take(24 * 1024 * 1024 + 1)
            .read_to_end(&mut bytes)
            .map_err(|_| "无法读取选中的皮肤包")?;
        self.import_bytes(&bytes, expected, committed)
    }
    pub fn import_bytes(
        &self,
        bytes: &[u8],
        expected: &str,
        committed: impl FnOnce(&AppSettings),
    ) -> Result<SkinSnapshot, String> {
        self.import_allowed(expected)?;
        let pack = skin_pack::validate_archive(bytes)?;
        let stage = Stage::create(&self.root, &pack)?;
        let snapshot = self.mutate(
            expected,
            |registry, _| {
                if !registry.import_enabled {
                    return Err("皮肤导入已关闭，未安装".into());
                }
                if let Some(entry) = registry
                    .packs
                    .iter()
                    .find(|entry| entry.id == pack.manifest.id || entry.hash == pack.hash)
                {
                    if entry.id == pack.manifest.id && entry.hash == pack.hash {
                        return Ok(());
                    }
                    return Err("同名皮肤已存在，请先删除旧包再导入".into());
                }
                stage.install(&pack)?;
                registry.packs.push(PackRef {
                    id: pack.manifest.id.clone(),
                    hash: pack.hash.clone(),
                });
                Ok(())
            },
            committed,
        )?;
        Ok(snapshot)
    }

    fn cleanup(&self, hash: &str) {
        if let Ok(_lock) = settings_io::lock(&self.settings_path()) {
            if let Ok((registry, _)) = self.document() {
                if !registry.packs.iter().any(|entry| entry.hash == hash) {
                    if let Ok(mut cache) = self.cache.lock() {
                        cache.remove(hash);
                    }
                    if let Ok(path) = checked_pack_root(&self.root, hash) {
                        let _ = fs::remove_dir_all(path);
                    }
                }
            }
        }
    }

    /// Called on a blocking worker by the asynchronous custom-protocol handler.
    pub fn image(&self, uri_path: &str) -> Result<Vec<u8>, String> {
        let (hash, relative) = uri_path
            .strip_prefix('/')
            .and_then(|path| path.split_once('/'))
            .ok_or(FILE_ERROR)?;
        if !valid_hash(hash) || !skin_pack::valid_image_path(relative) {
            return Err(FILE_ERROR.into());
        }
        let (registry, _) = self.document()?;
        let entry = registry
            .packs
            .iter()
            .find(|entry| entry.hash == hash)
            .ok_or(FILE_ERROR)?;
        let pack = self.installed(entry)?;
        if !pack.allowed.contains(relative) {
            return Err(FILE_ERROR.into());
        }
        let root = checked_pack_root(&self.root, hash)?;
        let path = regular_child(&root, relative)?;
        let maximum = if relative == pack.manifest.preview {
            1024 * 1024
        } else {
            4 * 1024 * 1024
        };
        let bytes = read_limited(&path, maximum)?;
        if !basic_png_header(&bytes) {
            return Err(FILE_ERROR.into());
        }
        Ok(bytes)
    }
    pub fn chrome_colors(&self, settings: &AppSettings) -> Option<(u32, u32)> {
        if settings.custom_skin_id.is_empty() {
            let cache = self.builtin_cache.lock().ok()?;
            let style = cache.get(&settings.skin)?;
            let tone = style
                .themes
                .get(if settings.dark_mode { "dark" } else { "light" })?;
            let color = |name: &str| {
                tone.colors
                    .get(name)
                    .and_then(|v| u32::from_str_radix(v.strip_prefix('#')?, 16).ok())
            };
            let defaults = builtin_skin_edits::caption_defaults(settings);
            return Some((
                color("toolbar")
                    .or_else(|| color("background"))
                    .unwrap_or(defaults.0),
                color("text").unwrap_or(defaults.1),
            ));
        }
        let cache = self.cache.lock().ok()?;
        let pack = cache.get(&settings.custom_skin_hash)?;
        if pack.manifest.id != settings.custom_skin_id {
            return None;
        }
        let theme = if settings.dark_mode {
            &pack.manifest.themes.dark
        } else {
            &pack.manifest.themes.light
        };
        let color =
            |key: &str| u32::from_str_radix(theme.colors.get(key)?.strip_prefix('#')?, 16).ok();
        Some((color("background")?, color("text")?))
    }
}

fn select_builtin(settings: &mut AppSettings, id: &str) {
    settings.skin = id.into();
    settings.custom_skin_id.clear();
    settings.custom_skin_hash.clear();
}
fn resource_url(hash: &str, relative: &str) -> String {
    format!("http://fcskin.localhost/{hash}/{relative}")
}
fn is_link(metadata: &fs::Metadata) -> bool {
    if metadata.file_type().is_symlink() {
        return true;
    }
    #[cfg(windows)]
    {
        use std::os::windows::fs::MetadataExt;
        if metadata.file_attributes() & 0x400 != 0 {
            return true;
        }
    }
    false
}
fn checked_pack_root(root: &Path, hash: &str) -> Result<PathBuf, String> {
    if !valid_hash(hash) {
        return Err(FILE_ERROR.into());
    }
    let skins = root.join("skins");
    let metadata = fs::symlink_metadata(&skins).map_err(|_| FILE_ERROR)?;
    if !metadata.is_dir() || is_link(&metadata) {
        return Err(FILE_ERROR.into());
    }
    let pack = skins.join(hash);
    let metadata = fs::symlink_metadata(&pack).map_err(|_| FILE_ERROR)?;
    if !metadata.is_dir() || is_link(&metadata) {
        return Err(FILE_ERROR.into());
    }
    Ok(pack)
}
fn regular_child(root: &Path, relative: &str) -> Result<PathBuf, String> {
    if !skin_pack::valid_image_path(relative)
        && relative != "manifest.json"
        && relative != "resolved.ini"
    {
        return Err(FILE_ERROR.into());
    }
    let root_metadata = fs::symlink_metadata(root).map_err(|_| FILE_ERROR)?;
    if !root_metadata.is_dir() || is_link(&root_metadata) {
        return Err(FILE_ERROR.into());
    }
    let mut path = root.to_path_buf();
    let mut parts = relative.split('/').peekable();
    while let Some(part) = parts.next() {
        path.push(part);
        let metadata = fs::symlink_metadata(&path).map_err(|_| FILE_ERROR)?;
        if is_link(&metadata)
            || (parts.peek().is_some() && !metadata.is_dir())
            || (parts.peek().is_none() && !metadata.is_file())
        {
            return Err(FILE_ERROR.into());
        }
    }
    Ok(path)
}
fn read_limited(path: &Path, maximum: usize) -> Result<Vec<u8>, String> {
    let file = settings_io::open_read(path).map_err(|_| FILE_ERROR)?;
    let metadata = file.metadata().map_err(|_| FILE_ERROR)?;
    if !metadata.is_file() || metadata.len() > maximum as u64 {
        return Err(FILE_ERROR.into());
    }
    let mut bytes = Vec::new();
    file.take(maximum as u64 + 1)
        .read_to_end(&mut bytes)
        .map_err(|_| FILE_ERROR)?;
    if bytes.len() > maximum {
        return Err(FILE_ERROR.into());
    }
    Ok(bytes)
}
fn read_installed_files(root: &Path) -> Result<BTreeMap<String, Vec<u8>>, String> {
    fn walk(
        root: &Path,
        current: &Path,
        files: &mut BTreeMap<String, Vec<u8>>,
        count: &mut usize,
        total: &mut usize,
        depth: usize,
    ) -> Result<(), String> {
        if depth > 48 {
            return Err(FILE_ERROR.into());
        }
        let metadata = fs::symlink_metadata(current).map_err(|_| FILE_ERROR)?;
        if !metadata.is_dir() || is_link(&metadata) {
            return Err(FILE_ERROR.into());
        }
        for item in fs::read_dir(current).map_err(|_| FILE_ERROR)? {
            let path = item.map_err(|_| FILE_ERROR)?.path();
            *count += 1;
            // ZIP entries need not include explicit parent-directory entries.
            // 64 declared files with <=96-byte paths can legitimately create
            // more than 64 filesystem nodes after extraction.
            if *count > 4096 {
                return Err(FILE_ERROR.into());
            }
            let metadata = fs::symlink_metadata(&path).map_err(|_| FILE_ERROR)?;
            if is_link(&metadata) {
                return Err(FILE_ERROR.into());
            }
            if metadata.is_dir() {
                walk(root, &path, files, count, total, depth + 1)?;
                continue;
            }
            let relative = path
                .strip_prefix(root)
                .map_err(|_| FILE_ERROR)?
                .to_str()
                .ok_or(FILE_ERROR)?
                .replace('\\', "/");
            if relative == "resolved.ini" {
                continue;
            }
            regular_child(root, &relative)?;
            let maximum = if relative == "manifest.json" {
                64 * 1024
            } else {
                4 * 1024 * 1024
            };
            let bytes = read_limited(&path, maximum)?;
            *total += bytes.len();
            if *total > 48 * 1024 * 1024 {
                return Err(FILE_ERROR.into());
            }
            files.insert(relative, bytes);
        }
        Ok(())
    }
    let mut files = BTreeMap::new();
    walk(root, root, &mut files, &mut 0, &mut 0, 0)?;
    Ok(files)
}
fn install_builtin_style(root: &Path, id: &str, style: &BuiltinStyle) -> Result<String, String> {
    let hash = style.hash(id)?;
    let body = style.resolved_ini(id)?;
    let directory = root.join("builtin-styles").join(id);
    for path in [root.join("builtin-styles"), directory.clone()] {
        fs::create_dir_all(&path).map_err(|_| FILE_ERROR)?;
        let metadata = fs::symlink_metadata(&path).map_err(|_| FILE_ERROR)?;
        if !metadata.is_dir() || is_link(&metadata) {
            return Err(FILE_ERROR.into());
        }
    }
    let target = directory.join(&hash);
    if target.exists() {
        let metadata = fs::symlink_metadata(&target).map_err(|_| FILE_ERROR)?;
        if !metadata.is_dir() || is_link(&metadata) {
            return Err(FILE_ERROR.into());
        }
        if read_limited(&regular_child(&target, "resolved.ini")?, 16384)? != body.as_bytes() {
            return Err(FILE_ERROR.into());
        }
        return Ok(hash);
    }
    let staging = directory.join(format!(".stage-{:032x}", rand::random::<u128>()));
    fs::create_dir(&staging).map_err(|_| FILE_ERROR)?;
    struct RemoveStage(PathBuf);
    impl Drop for RemoveStage {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }
    let _stage = RemoveStage(staging.clone());
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(staging.join("resolved.ini"))
        .map_err(|_| FILE_ERROR)?;
    file.write_all(body.as_bytes())
        .and_then(|_| file.sync_all())
        .map_err(|_| FILE_ERROR)?;
    drop(file);
    fs::rename(&staging, &target).map_err(|_| FILE_ERROR)?;
    Ok(hash)
}

fn basic_png_header(bytes: &[u8]) -> bool {
    if bytes.len() < 33 || &bytes[..8] != b"\x89PNG\r\n\x1a\n" || &bytes[12..16] != b"IHDR" {
        return false;
    }
    let width = u32::from_be_bytes(bytes[16..20].try_into().unwrap());
    let height = u32::from_be_bytes(bytes[20..24].try_into().unwrap());
    width > 0
        && height > 0
        && width <= 2048
        && height <= 2048
        && bytes[24] == 8
        && matches!(bytes[25], 2 | 6)
        && bytes[26..29] == [0, 0, 0]
}

struct Stage {
    path: PathBuf,
    destination: PathBuf,
}
impl Drop for Stage {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.path);
    }
}
impl Stage {
    fn create(root: &Path, pack: &ValidatedPack) -> Result<Self, String> {
        let skins = root.join("skins");
        fs::create_dir_all(&skins).map_err(|_| "无法创建皮肤目录")?;
        if is_link(&fs::symlink_metadata(&skins).map_err(|_| FILE_ERROR)?) {
            return Err(FILE_ERROR.into());
        }
        let path = (0..32)
            .find_map(|_| {
                let path = skins.join(format!(".stage-{:032x}", rand::random::<u128>()));
                fs::create_dir(&path).ok().map(|_| path)
            })
            .ok_or("无法创建皮肤暂存目录")?;
        let stage = Self {
            destination: skins.join(&pack.hash),
            path,
        };
        for (relative, bytes) in &pack.files {
            let path = stage.path.join(relative);
            if let Some(parent) = path.parent() {
                fs::create_dir_all(parent).map_err(|_| FILE_ERROR)?;
            }
            let mut file = OpenOptions::new()
                .create_new(true)
                .write(true)
                .open(path)
                .map_err(|_| FILE_ERROR)?;
            file.write_all(bytes)
                .and_then(|_| file.sync_all())
                .map_err(|_| FILE_ERROR)?;
        }
        let resolved = skin_pack::resolved_ini(pack)?;
        let mut file = OpenOptions::new()
            .create_new(true)
            .write(true)
            .open(stage.path.join("resolved.ini"))
            .map_err(|_| FILE_ERROR)?;
        file.write_all(resolved.as_bytes())
            .and_then(|_| file.sync_all())
            .map_err(|_| FILE_ERROR)?;
        Ok(stage)
    }
    fn install(&self, pack: &ValidatedPack) -> Result<(), String> {
        if self.destination.exists() {
            let existing = read_installed_files(&self.destination)?;
            if existing != pack.files {
                return Err("同一皮肤摘要目录的文件不一致，未覆盖".into());
            }
            let resolved = read_limited(
                &regular_child(&self.destination, "resolved.ini")?,
                16 * 1024,
            )?;
            if resolved != skin_pack::resolved_ini(pack)?.as_bytes() {
                return Err(FILE_ERROR.into());
            }
            return Ok(());
        }
        fs::rename(&self.path, &self.destination)
            .map_err(|_| "无法安装皮肤文件，未修改原选择".into())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    struct Fixture(PathBuf);
    impl Fixture {
        fn new() -> Self {
            let root = std::env::temp_dir().join(format!(
                "fangcun-skin-library-{:032x}",
                rand::random::<u128>()
            ));
            fs::create_dir(&root).unwrap();
            Self(root)
        }
        fn library(&self) -> SkinLibrary {
            SkinLibrary::new(self.0.clone())
        }
        fn settings(&self) -> PathBuf {
            self.0.join(SETTINGS_FILE)
        }
    }
    impl Drop for Fixture {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    fn archive(id: &str, color: &str, version: u32) -> Vec<u8> {
        let mut image = vec![];
        {
            let mut encoder = png::Encoder::new(&mut image, 8, 8);
            encoder.set_color(png::ColorType::Rgba);
            encoder.set_depth(png::BitDepth::Eight);
            encoder
                .write_header()
                .unwrap()
                .write_image_data(&[220; 8 * 8 * 4])
                .unwrap();
        }
        let keys = [
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
        let colors: BTreeMap<_, _> = keys.iter().map(|key| (*key, color)).collect();
        let theme = serde_json::json!({"colors":colors,"glow":["#112233","#445566","#778899","#AABBCC"],"images":{"deck":{"file":"preview.png","scale":"tile","insets":[0,0,0,0]}}});
        let manifest = serde_json::json!({"format":"fangcun.skin","schemaVersion":1,"id":id,"name":"本地示例皮肤","version":version,"author":"示例作者","license":"原创","baseSkin":"brutal","assetScale":3,"fontFamily":"sans","pressOffsetDp":1,"preview":"preview.png","themes":{"light":theme,"dark":theme}});
        let mut zip = zip::ZipWriter::new(Cursor::new(vec![]));
        let options =
            zip::write::FileOptions::default().compression_method(zip::CompressionMethod::Deflated);
        zip.start_file("manifest.json", options).unwrap();
        zip.write_all(&serde_json::to_vec(&manifest).unwrap())
            .unwrap();
        zip.start_file("preview.png", options).unwrap();
        zip.write_all(&image).unwrap();
        zip.finish().unwrap().into_inner()
    }

    #[test]
    fn builtin_edits_are_in_place_and_activation_reset_preserve_every_other_preference() {
        let f = Fixture::new();
        fs::write(
            f.settings(),
            "theme_mode=dark\nscheme=ninekey\ntyping_glow=0\nfuture=保留\n",
        )
        .unwrap();
        let library = f.library();
        let initial = library.snapshot().unwrap();
        let imported = library
            .import_bytes(
                &archive("fangcun.sample", "#123456", 1),
                &initial.revision,
                |_| {},
            )
            .unwrap();
        let selected = library
            .select("local:fangcun.sample", &imported.revision, |_| {})
            .unwrap();
        let first:BuiltinStyle=serde_json::from_str(r##"{"fontFamily":"mono","themes":{"light":{"colors":{"candidateBackground":"#203040"}}}}"##).unwrap();
        let edited = library
            .save_builtin_style(
                "wood_type",
                first.clone(),
                &selected.revision,
                false,
                |_| {},
            )
            .unwrap();
        assert_eq!(edited.active_id, selected.active_id);
        assert_eq!(edited.packs.len(), 1);
        assert_eq!(edited.builtins.len(), 4);
        assert_eq!(edited.builtin_styles["wood_type"], first);
        let mut chrome_settings = AppSettings::default();
        chrome_settings.skin = "wood_type".into();
        chrome_settings.dark_mode = false;
        // No toolbar/background edits must retain the ordinary native caption.
        assert_eq!(
            library.chrome_colors(&chrome_settings),
            Some(builtin_skin_edits::caption_defaults(&chrome_settings))
        );
        let hash = first.hash("wood_type").unwrap();
        let ini = fs::read_to_string(
            f.0.join("builtin-styles/wood_type")
                .join(&hash)
                .join("resolved.ini"),
        )
        .unwrap();
        assert!(ini.contains("light_candidate_background=#203040\n") && !ini.contains("dark_"));
        let hidden = library
            .set_builtin_hidden("terminal", true, &edited.revision, |_| {})
            .unwrap();
        let other: BuiltinStyle = serde_json::from_str(
            r##"{"pressOffsetDp":1.25,"themes":{"dark":{"colors":{"accent":"#ABCDEF"}}}}"##,
        )
        .unwrap();
        let active = library
            .save_builtin_style("terminal", other.clone(), &hidden.revision, true, |_| {})
            .unwrap();
        assert_eq!(active.active_id, "builtin:terminal");
        assert!(!active.hidden_builtins.iter().any(|id| id == "terminal"));
        assert_eq!(active.packs.len(), 1);
        let restored = library
            .reset_builtin_style("wood_type", &active.revision, |_| {})
            .unwrap();
        assert_eq!(restored.active_id, "builtin:terminal");
        assert!(!restored.builtin_styles.contains_key("wood_type"));
        assert_eq!(restored.builtin_styles["terminal"], other);
        let body = fs::read_to_string(f.settings()).unwrap();
        for preserved in [
            "theme_mode=dark\n",
            "scheme=ninekey\n",
            "typing_glow=0\n",
            "future=保留\n",
        ] {
            assert!(body.contains(preserved));
        }
        assert!(body.contains("builtin_style_wood_type=\n"));
        assert!(body.contains(&format!(
            "builtin_style_terminal={}\n",
            other.hash("terminal").unwrap()
        )));
        assert_eq!(
            f.library().snapshot().unwrap().builtin_styles,
            restored.builtin_styles
        );
    }

    #[test]
    fn builtin_caption_uses_only_active_mode_edits_and_reset_clears_cached_overrides() {
        let f = Fixture::new();
        let library = f.library();
        let initial = library.snapshot().unwrap();
        let style:BuiltinStyle=serde_json::from_str(r##"{"themes":{"light":{"colors":{"toolbar":"#112233","text":"#445566"}},"dark":{"colors":{"background":"#778899"}}}}"##).unwrap();
        let saved = library
            .save_builtin_style("brutal", style, &initial.revision, true, |_| {})
            .unwrap();
        let mut settings = AppSettings::default();
        settings.skin = "brutal".into();
        settings.dark_mode = false;
        assert_eq!(library.chrome_colors(&settings), Some((0x112233, 0x445566)));
        settings.dark_mode = true;
        assert_eq!(
            library.chrome_colors(&settings),
            Some((0x778899, builtin_skin_edits::caption_defaults(&settings).1))
        );
        settings.custom_skin_id = "fangcun.local".into();
        settings.custom_skin_hash = "a".repeat(64);
        assert_eq!(
            library.chrome_colors(&settings),
            None,
            "Local package never inherits builtin edits"
        );
        let _ = library
            .reset_builtin_style("brutal", &saved.revision, |_| {})
            .unwrap();
        settings.custom_skin_id.clear();
        settings.custom_skin_hash.clear();
        assert_eq!(library.chrome_colors(&settings), None);
    }

    #[test]
    fn builtin_style_cas_includes_raw_json_and_failed_saves_never_publish_or_overwrite() {
        let f = Fixture::new();
        let library = f.library();
        let initial = library.snapshot().unwrap();
        let first: BuiltinStyle = serde_json::from_str(r#"{"fontFamily":"sans"}"#).unwrap();
        let saved = library
            .save_builtin_style("brutal", first, &initial.revision, false, |_| {})
            .unwrap();
        let body = fs::read_to_string(f.settings())
            .unwrap()
            .replace("\"styles\":{", "\"styles\": {");
        fs::write(f.settings(), &body).unwrap();
        assert_eq!(
            library
                .reset_builtin_style("brutal", &saved.revision, |_| panic!("No stale publish"))
                .err()
                .as_deref(),
            Some(CONFLICT)
        );
        let fresh = library.snapshot().unwrap();
        let lock = settings_io::lock(&f.settings()).unwrap();
        assert!(library
            .save_builtin_style(
                "brutal",
                BuiltinStyle::default(),
                &fresh.revision,
                true,
                |_| panic!("No failed publish")
            )
            .is_err());
        assert_eq!(fs::read_to_string(f.settings()).unwrap(), body);
        drop(lock);
        fs::write(f.settings(), "builtin_skin_edits_v1={broken\nfuture=keep\n").unwrap();
        let original = fs::read(f.settings()).unwrap();
        assert!(library.snapshot().is_err());
        assert!(library
            .reset_builtin_style("brutal", &fresh.revision, |_| panic!(
                "No invalid-state publish"
            ))
            .is_err());
        assert_eq!(fs::read(f.settings()).unwrap(), original);
    }

    #[cfg(unix)]
    #[test]
    fn builtin_derivative_link_is_rejected_without_rewriting_settings_or_external_file() {
        let f = Fixture::new();
        let library = f.library();
        let initial = library.snapshot().unwrap();
        let style = BuiltinStyle::default();
        let hash = style.hash("brutal").unwrap();
        let external = f.0.join("external");
        fs::create_dir(&external).unwrap();
        fs::write(external.join("resolved.ini"), "untouched").unwrap();
        fs::create_dir_all(f.0.join("builtin-styles/brutal")).unwrap();
        std::os::unix::fs::symlink(&external, f.0.join("builtin-styles/brutal").join(hash))
            .unwrap();
        assert!(library
            .save_builtin_style("brutal", style, &initial.revision, false, |_| panic!(
                "No link publish"
            ))
            .is_err());
        assert!(!f.settings().exists());
        assert_eq!(
            fs::read_to_string(external.join("resolved.ini")).unwrap(),
            "untouched"
        );
    }

    #[test]
    fn import_is_passive_duplicate_is_idempotent_and_selection_survives_restart() {
        let f = Fixture::new();
        fs::write(
            f.settings(),
            "# keep\nfuture=保留\ntheme_mode=dark\nkey_letter_size=25\ntyping_glow=0\n",
        )
        .unwrap();
        let library = f.library();
        let initial = library.snapshot().unwrap();
        let bytes = archive("fangcun.sample", "#123456", 1);
        let imported = library
            .import_bytes(&bytes, &initial.revision, |_| {})
            .unwrap();
        assert_eq!(imported.active_id, "builtin:wood_type");
        assert_eq!(imported.packs.len(), 1);
        assert_eq!(
            library
                .import_bytes(&bytes, &imported.revision, |_| {})
                .unwrap()
                .revision,
            imported.revision
        );
        let selected = library
            .select("local:fangcun.sample", &imported.revision, |_| {})
            .unwrap();
        assert_eq!(selected.active_id, "local:fangcun.sample");
        assert_eq!(
            f.library().snapshot().unwrap().active_id,
            selected.active_id
        );
        let body = fs::read_to_string(f.settings()).unwrap();
        assert!(
            body.contains("future=保留\n")
                && body.contains("theme_mode=dark\n")
                && body.contains("key_letter_size=25\n")
                && body.contains("typing_glow=0\n")
        );
        let settings = settings::parse_content(&body);
        assert_eq!(settings.skin, "brutal");
        assert_eq!(settings.custom_skin_id, "fangcun.sample");
        assert_eq!(
            library
                .image(&format!("/{}/preview.png", selected.packs[0].hash))
                .unwrap()[..8],
            *b"\x89PNG\r\n\x1a\n"
        );
    }

    #[test]
    fn stale_operations_and_same_id_different_content_never_replace_prior_state() {
        let f = Fixture::new();
        let library = f.library();
        let initial = library.snapshot().unwrap();
        let first = library
            .import_bytes(
                &archive("fangcun.sample", "#123456", 1),
                &initial.revision,
                |_| {},
            )
            .unwrap();
        let before = fs::read(f.settings()).unwrap();
        assert!(library
            .select("local:fangcun.sample", &initial.revision, |_| {})
            .is_err());
        assert!(library
            .import_bytes(
                &archive("fangcun.sample", "#654321", 2),
                &first.revision,
                |_| {}
            )
            .is_err());
        assert_eq!(fs::read(f.settings()).unwrap(), before);
        assert_eq!(
            fs::read_dir(f.0.join("skins")).unwrap().count(),
            1,
            "Rejected imports leave no staging directories"
        );
    }

    #[test]
    fn hiding_and_deleting_active_items_commit_a_visible_fallback_without_resetting_preferences() {
        let f = Fixture::new();
        let library = f.library();
        fs::write(
            f.settings(),
            "theme_mode=dark\nscheme=ninekey\nkey_opacity=0.73\n",
        )
        .unwrap();
        let mut state = library.snapshot().unwrap();
        state = library
            .set_builtin_hidden("wood_type", true, &state.revision, |_| {})
            .unwrap();
        assert_eq!(state.active_id, "builtin:typewriter");
        state = library
            .set_builtin_hidden("terminal", true, &state.revision, |_| {})
            .unwrap();
        state = library
            .set_builtin_hidden("brutal", true, &state.revision, |_| {})
            .unwrap();
        let before = fs::read(f.settings()).unwrap();
        assert!(library
            .set_builtin_hidden("typewriter", true, &state.revision, |_| {})
            .is_err());
        assert_eq!(fs::read(f.settings()).unwrap(), before);
        state = library
            .import_bytes(
                &archive("fangcun.sample", "#112233", 1),
                &state.revision,
                |_| {},
            )
            .unwrap();
        state = library
            .select("local:fangcun.sample", &state.revision, |_| {})
            .unwrap();
        let old_hash = state.packs[0].hash.clone();
        state = library
            .delete("local:fangcun.sample", &state.revision, |_| {})
            .unwrap();
        assert_eq!(state.active_id, "builtin:typewriter");
        assert!(state.packs.is_empty());
        let current = settings::parse_content(&fs::read_to_string(f.settings()).unwrap());
        assert_eq!(current.theme_mode, "dark");
        assert_eq!(current.scheme, "ninekey");
        assert_eq!(current.key_opacity, 0.73);
        assert!(current.custom_skin_id.is_empty() && current.custom_skin_hash.is_empty());
        assert!(!f.0.join("skins").join(old_hash).exists());
        assert_eq!(
            library
                .set_builtin_hidden("wood_type", false, &state.revision, |_| {})
                .unwrap()
                .hidden_builtins
                .len(),
            2
        );
    }

    #[test]
    fn import_toggle_does_not_disable_installed_skins_and_failed_save_never_notifies() {
        let f = Fixture::new();
        let library = f.library();
        let state = library.snapshot().unwrap();
        let state = library
            .import_bytes(
                &archive("fangcun.sample", "#112233", 1),
                &state.revision,
                |_| {},
            )
            .unwrap();
        let state = library
            .set_import_enabled(false, &state.revision, |_| {})
            .unwrap();
        assert!(library
            .import_bytes(
                &archive("fangcun.other", "#112233", 1),
                &state.revision,
                |_| {}
            )
            .is_err());
        let state = library
            .select("local:fangcun.sample", &state.revision, |_| {})
            .unwrap();
        assert!(state.active.is_some());
        let original = fs::read(f.settings()).unwrap();
        let lock = settings_io::lock(&f.settings()).unwrap();
        let notified = std::cell::Cell::new(false);
        assert!(library
            .set_import_enabled(true, &state.revision, |_| notified.set(true))
            .is_err());
        assert!(!notified.get());
        assert_eq!(fs::read(f.settings()).unwrap(), original);
        drop(lock);
    }

    #[test]
    fn resource_routes_only_serve_registered_declared_png_and_reject_links() {
        let f = Fixture::new();
        let library = f.library();
        let state = library.snapshot().unwrap();
        let state = library
            .import_bytes(
                &archive("fangcun.sample", "#112233", 1),
                &state.revision,
                |_| {},
            )
            .unwrap();
        let hash = state.packs[0].hash.clone();
        for path in [
            format!("/{hash}/manifest.json"),
            format!("/{hash}/resolved.ini"),
            format!("/{hash}/../settings.ini"),
            format!("/{hash}/missing.png"),
            format!("/{}/preview.png", "0".repeat(64)),
            format!("/{hash}/%2e%2e/private.png"),
        ] {
            assert!(library.image(&path).is_err());
        }
        #[cfg(unix)]
        {
            let external = f.0.join("external.png");
            fs::write(
                &external,
                library.image(&format!("/{hash}/preview.png")).unwrap(),
            )
            .unwrap();
            let installed = f.0.join("skins").join(hash).join("preview.png");
            fs::remove_file(&installed).unwrap();
            std::os::unix::fs::symlink(&external, &installed).unwrap();
            assert!(library
                .image(&format!("/{}/preview.png", state.packs[0].hash))
                .is_err());
        }
    }

    #[test]
    fn invalid_registry_cannot_be_overwritten_or_treated_as_an_empty_library() {
        let f = Fixture::new();
        fs::write(f.settings(), "skin_library_v1={broken\nfuture=keep\n").unwrap();
        let before = fs::read(f.settings()).unwrap();
        let library = f.library();
        assert!(library.snapshot().is_err());
        assert!(library
            .set_import_enabled(true, "anything", |_| panic!(
                "No publish after invalid read"
            ))
            .is_err());
        assert_eq!(fs::read(f.settings()).unwrap(), before);
    }
}
