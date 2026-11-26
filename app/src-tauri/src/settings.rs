use crate::settings_io;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct AppSettings {
    pub scheme: String,
    pub mixed_input: bool,
    pub chinese_punctuation: bool,
    #[serde(default = "default_true")]
    pub shift_toggle_english: bool,
    #[serde(default = "default_true")]
    pub caps_lock_bypass: bool,
    #[serde(default = "default_page_size")]
    pub page_size: i32,
    #[serde(default = "default_true")]
    pub wheel_paging: bool,
    pub skin: String,
    #[serde(default)]
    pub custom_skin_id: String,
    #[serde(default)]
    pub custom_skin_hash: String,
    pub theme_mode: String,
    pub dark_mode: bool,
    pub system_dark: bool,
    pub first_candidate_size: i32,
    pub key_opacity: f64,
    pub typing_glow: bool,
    pub key_letter_size: i32,
    pub key_letter_bold: bool,
    #[serde(default = "default_true")]
    pub clipboard_enabled: bool,
    #[serde(default = "default_candidate_layout")]
    pub candidate_layout: String,
    #[serde(default)]
    pub full_width: bool,
    #[serde(default)]
    pub traditional: bool,
    #[serde(default)]
    pub revision: String,
}

fn default_candidate_layout() -> String {
    "double".to_string()
}

fn default_true() -> bool {
    true
}

fn default_page_size() -> i32 {
    7
}

impl Default for AppSettings {
    fn default() -> Self {
        let sys_dark = is_windows_apps_dark();
        Self {
            scheme: "flypy".to_string(),
            mixed_input: true,
            chinese_punctuation: true,
            shift_toggle_english: true,
            caps_lock_bypass: true,
            page_size: 7,
            wheel_paging: true,
            skin: "system_default".to_string(),
            custom_skin_id: String::new(),
            custom_skin_hash: String::new(),
            theme_mode: "system".to_string(),
            dark_mode: sys_dark,
            system_dark: sys_dark,
            first_candidate_size: 20,
            key_opacity: 0.82,
            typing_glow: true,
            key_letter_size: 18,
            key_letter_bold: false,
            clipboard_enabled: true,
            candidate_layout: default_candidate_layout(),
            full_width: false,
            traditional: false,
            revision: String::new(),
        }
    }
}

pub fn data_dir() -> PathBuf {
    #[cfg(target_os = "windows")]
    {
        if let Ok(local) = std::env::var("LOCALAPPDATA") {
            if !local.is_empty() {
                return PathBuf::from(local).join("FangCunIME");
            }
        }
    }
    if let Ok(home) = std::env::var("HOME") {
        if !home.is_empty() {
            return PathBuf::from(home).join(".fangcun_ime");
        }
    }
    PathBuf::from(".fangcun_ime")
}

fn settings_path() -> PathBuf {
    data_dir().join("settings.ini")
}

// Legacy base IDs remain readable for installed .fcskin packages.
// Without a local package the UI always uses the neutral system appearance.
// This changes one visual identifier; language, privacy, sharing and typography
// preferences continue to be read from the same existing settings file.
fn canonical_skin(id: &str) -> &'static str {
    match id {
        "system_default" => "system_default",
        "terminal" | "metro" | "square_aluminum" | "native_light" => "terminal",
        "brutal" | "square_amber" | "deep_blue" => "brutal",
        "typewriter" | "eink" | "square_jade" | "mint" => "typewriter",
        "wood_type" | "swiss" | "square_cream" | "warm_grid" => "wood_type",
        _ => "system_default",
    }
}

fn normalize(mut s: AppSettings) -> AppSettings {
    s.system_dark = is_windows_apps_dark();
    if !matches!(s.scheme.as_str(), "flypy" | "pinyin" | "ninekey") {
        s.scheme = "flypy".to_string();
    }
    s.skin = canonical_skin(&s.skin).to_string();
    if !crate::skin_pack::valid_id(&s.custom_skin_id)
        || s.custom_skin_hash.len() != 64
        || !s
            .custom_skin_hash
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
    {
        s.custom_skin_id.clear();
        s.custom_skin_hash.clear();
    }
    if s.custom_skin_id.is_empty() {
        s.skin = "system_default".into();
    }
    if !matches!(s.theme_mode.as_str(), "system" | "light" | "dark") {
        s.theme_mode = "system".to_string();
    }
    s.dark_mode = match s.theme_mode.as_str() {
        "dark" => true,
        "light" => false,
        _ => s.system_dark,
    };
    if !matches!(
        s.candidate_layout.as_str(),
        "double" | "single" | "vertical"
    ) {
        s.candidate_layout = default_candidate_layout();
    }
    s.page_size = s.page_size.clamp(5, 9);
    s.first_candidate_size = s.first_candidate_size.clamp(16, 30);
    s.key_letter_size = s.key_letter_size.clamp(14, 30);
    s.key_opacity = if s.key_opacity.is_finite() {
        s.key_opacity.clamp(0.40, 1.00)
    } else {
        0.82
    };
    s
}

pub fn load_from_disk() -> AppSettings {
    try_load_from_disk().unwrap_or_else(|_| {
        // Unknown persistence state must not enable background clipboard capture.
        AppSettings {
            clipboard_enabled: false,
            ..AppSettings::default()
        }
    })
}

pub fn try_load_from_disk() -> Result<AppSettings, String> {
    try_load_from_path(&settings_path())
}

fn revision(content: Option<&str>) -> String {
    match content {
        Some(value) => format!("{:x}", Sha256::digest(value.as_bytes())),
        None => "missing".to_string(),
    }
}

fn try_load_from_path(path: &Path) -> Result<AppSettings, String> {
    let content = settings_io::read(path)?;
    let mut settings = parse_content(content.as_deref().unwrap_or(""));
    settings.revision = revision(content.as_deref());
    Ok(settings)
}

#[cfg(test)]
fn load_from_path(path: &Path) -> AppSettings {
    try_load_from_path(path).unwrap()
}

pub(crate) fn parse_content(content: &str) -> AppSettings {
    let mut s = AppSettings::default();
    let mut has_theme_mode = false;
    let mut has_legacy_dark = false;
    {
        for raw_line in content.lines() {
            let line = raw_line.trim().trim_start_matches('\u{feff}');
            if line.is_empty() || line.starts_with('#') || line.starts_with(';') {
                continue;
            }
            let Some((k, v)) = line.split_once('=') else {
                continue;
            };
            let key = k.trim();
            let val = v.trim();
            match key {
                "scheme" => s.scheme = val.to_string(),
                "mixed_input" => s.mixed_input = val == "1" || val.eq_ignore_ascii_case("true"),
                "chinese_punctuation" => {
                    s.chinese_punctuation = val == "1" || val.eq_ignore_ascii_case("true")
                }
                "shift_toggle_english" => {
                    s.shift_toggle_english = val == "1" || val.eq_ignore_ascii_case("true")
                }
                "capslock_bypass" => {
                    s.caps_lock_bypass = val == "1" || val.eq_ignore_ascii_case("true")
                }
                "page_size" => {
                    if let Ok(n) = val.parse::<i32>() {
                        s.page_size = n;
                    }
                }
                "wheel_paging" => s.wheel_paging = val == "1" || val.eq_ignore_ascii_case("true"),
                "skin" => s.skin = val.to_string(),
                "custom_skin_id" => s.custom_skin_id = val.to_string(),
                "custom_skin_hash" => s.custom_skin_hash = val.to_string(),
                "theme_mode" => {
                    has_theme_mode = true;
                    s.theme_mode = val.to_string();
                }
                "dark" | "dark_mode" => {
                    has_legacy_dark = true;
                    s.dark_mode = val == "1" || val.eq_ignore_ascii_case("true")
                }
                "first_candidate_size" => {
                    if let Ok(n) = val.parse::<i32>() {
                        s.first_candidate_size = n;
                    }
                }
                "key_opacity" => {
                    if let Ok(f) = val.parse::<f64>() {
                        s.key_opacity = f;
                    }
                }
                "typing_glow" => s.typing_glow = val == "1" || val.eq_ignore_ascii_case("true"),
                "key_letter_size" => {
                    if let Ok(n) = val.parse::<i32>() {
                        s.key_letter_size = n;
                    }
                }
                "key_letter_bold" => {
                    s.key_letter_bold = val == "1" || val.eq_ignore_ascii_case("true")
                }
                "candidate_layout" => s.candidate_layout = val.to_string(),
                "full_width" => s.full_width = val == "1",
                "traditional" => s.traditional = val == "1",
                "clipboard_enabled" => {
                    s.clipboard_enabled = val == "1" || val.eq_ignore_ascii_case("true")
                }
                _ => {}
            }
        }
    }
    if !has_theme_mode && has_legacy_dark {
        s.theme_mode = if s.dark_mode { "dark" } else { "light" }.to_string();
    }
    normalize(s)
}

fn known_values(s: &AppSettings) -> BTreeMap<String, String> {
    let bool_int = |b: bool| if b { 1 } else { 0 };
    let body = format!(
        "scheme={}\nmixed_input={}\nchinese_punctuation={}\nshift_toggle_english={}\ncapslock_bypass={}\npage_size={}\nwheel_paging={}\nskin={}\ntheme_mode={}\ndark={}\ndark_mode={}\nfirst_candidate_size={}\nkey_opacity={:.2}\ntyping_glow={}\nkey_letter_size={}\nkey_letter_bold={}\nclipboard_enabled={}\ncandidate_layout={}\nfull_width={}\ntraditional={}\n",
        s.scheme,
        bool_int(s.mixed_input),
        bool_int(s.chinese_punctuation),
        bool_int(s.shift_toggle_english),
        bool_int(s.caps_lock_bypass),
        s.page_size,
        bool_int(s.wheel_paging),
        s.skin,
        s.theme_mode,
        bool_int(s.dark_mode),
        bool_int(s.dark_mode),
        s.first_candidate_size,
        s.key_opacity,
        bool_int(s.typing_glow),
        s.key_letter_size,
        bool_int(s.key_letter_bold),
        bool_int(s.clipboard_enabled),
        s.candidate_layout,
        bool_int(s.full_width),
        bool_int(s.traditional),
    );

    let mut values: BTreeMap<String, String> = body
        .lines()
        .filter_map(|line| line.split_once('='))
        .map(|(key, value)| (key.to_string(), value.to_string()))
        .collect();
    values.insert("custom_skin_id".into(), s.custom_skin_id.clone());
    values.insert("custom_skin_hash".into(), s.custom_skin_hash.clone());
    values
}

fn merge_document(content: &str, updates: &BTreeMap<String, String>) -> String {
    let ending = if content.contains("\r\n") {
        "\r\n"
    } else {
        "\n"
    };
    let mut output = String::new();
    let mut missing = updates.clone();
    for raw in content.split_inclusive('\n') {
        let line = raw.trim_end_matches('\n').trim_end_matches('\r');
        let trimmed = line.trim().trim_start_matches('\u{feff}');
        let update = if trimmed.starts_with(['#', ';']) {
            None
        } else {
            trimmed
                .split_once('=')
                .and_then(|(key, _)| updates.get(key.trim()).map(|value| (key.trim(), value)))
        };
        if let Some((key, value)) = update {
            output.push_str(key);
            output.push('=');
            output.push_str(value);
            output.push_str(ending);
            missing.remove(key);
        } else {
            output.push_str(raw);
        }
    }
    if !missing.is_empty() && !output.is_empty() && !output.ends_with('\n') {
        output.push_str(ending);
    }
    for (key, value) in missing {
        output.push_str(&key);
        output.push('=');
        output.push_str(&value);
        output.push_str(ending);
    }
    output
}

fn write_transaction(
    path: &Path,
    change: impl FnOnce(&AppSettings) -> Result<(AppSettings, Option<Vec<&'static str>>), String>,
    committed: impl FnOnce(&AppSettings),
) -> Result<AppSettings, String> {
    let _lock = settings_io::lock(path)?;
    let content = settings_io::read(path)?;
    let mut latest = parse_content(content.as_deref().unwrap_or(""));
    latest.revision = revision(content.as_deref());
    let (input, fields) = change(&latest)?;
    if !input.key_opacity.is_finite() {
        return Err("透明度必须是有效数值".to_string());
    }
    let mut saved = normalize(input);
    let mut values = known_values(&saved);
    if let Some(fields) = fields {
        values.retain(|key, _| {
            fields.contains(&key.as_str())
                || matches!(key.as_str(), "theme_mode" | "dark" | "dark_mode")
        });
    }
    let body = merge_document(content.as_deref().unwrap_or(""), &values);
    settings_io::replace(path, &body)?;
    saved.revision = revision(Some(&body));
    // Commit notifications/runtime changes stay inside the same cross-process write order.
    committed(&saved);
    Ok(saved)
}

pub fn save_to_disk(input: AppSettings) -> Result<AppSettings, String> {
    save_to_disk_and(input, |_| {})
}

pub fn reset_to_disk_and(committed: impl FnOnce(&AppSettings)) -> Result<AppSettings, String> {
    let path = settings_path();
    let saved = write_transaction(
        &path,
        |_| {
            let document = settings_io::read(&path)?.unwrap_or_default();
            let fallback = crate::skin_library::visible_fallback(&document)?;
            Ok((
                AppSettings {
                    skin: fallback.into(),
                    ..AppSettings::default()
                },
                None,
            ))
        },
        committed,
    )?;
    notify_tsf_settings_changed();
    Ok(saved)
}

pub fn save_to_disk_and(
    input: AppSettings,
    committed: impl FnOnce(&AppSettings),
) -> Result<AppSettings, String> {
    let saved = save_to_path_and(&settings_path(), input, committed)?;
    notify_tsf_settings_changed();
    Ok(saved)
}

fn save_to_path(path: &Path, input: AppSettings) -> Result<AppSettings, String> {
    save_to_path_and(path, input, |_| {})
}

fn save_to_path_and(
    path: &Path,
    input: AppSettings,
    committed: impl FnOnce(&AppSettings),
) -> Result<AppSettings, String> {
    write_transaction(
        path,
        |latest| {
            let mut input = input;
            if !input.revision.is_empty() && input.revision != latest.revision {
                return Err("设置已由其他窗口修改，请重新读取后重试".to_string());
            }
            // Only the skin-library API can apply a validated local package.
            // Legacy full saves preserve its selection while unrelated fields
            // change; selecting a different built-in clears the pair.
            if canonical_skin(&input.skin) == latest.skin {
                input.custom_skin_id = latest.custom_skin_id.clone();
                input.custom_skin_hash = latest.custom_skin_hash.clone();
            } else if input.skin == "system_default" {
                input.custom_skin_id.clear();
                input.custom_skin_hash.clear();
            } else {
                return Err("内置皮肤已移除，请导入 .fcskin 皮肤文件".into());
            }
            Ok((input, None))
        },
        committed,
    )
}

#[derive(Default, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct SettingsPatch {
    scheme: Option<String>,
    mixed_input: Option<bool>,
    chinese_punctuation: Option<bool>,
    shift_toggle_english: Option<bool>,
    caps_lock_bypass: Option<bool>,
    page_size: Option<i32>,
    wheel_paging: Option<bool>,
    skin: Option<String>,
    theme_mode: Option<String>,
    first_candidate_size: Option<i32>,
    key_opacity: Option<f64>,
    typing_glow: Option<bool>,
    key_letter_size: Option<i32>,
    key_letter_bold: Option<bool>,
    clipboard_enabled: Option<bool>,
    candidate_layout: Option<String>,
    full_width: Option<bool>,
    traditional: Option<bool>,
}

impl SettingsPatch {
    fn parse(value: serde_json::Value) -> Result<Self, String> {
        let map = value.as_object().ok_or("设置修改无效")?;
        if map.values().any(|value| {
            value.is_null()
                || value
                    .as_str()
                    .is_some_and(|text| text.chars().any(|c| c.is_control()))
        }) {
            return Err("设置值无效，未保存".to_string());
        }
        let patch: Self =
            serde_json::from_value(value).map_err(|_| "设置字段或类型无效，未保存".to_string())?;
        if patch
            .scheme
            .as_deref()
            .is_some_and(|v| !matches!(v, "flypy" | "pinyin" | "ninekey"))
            || patch
                .skin
                .as_deref()
                .is_some_and(|v| v != "system_default")
            || patch
                .theme_mode
                .as_deref()
                .is_some_and(|v| !matches!(v, "system" | "light" | "dark"))
            || patch
                .candidate_layout
                .as_deref()
                .is_some_and(|v| !matches!(v, "double" | "single" | "vertical"))
            || patch.page_size.is_some_and(|v| !(5..=9).contains(&v))
            || patch
                .first_candidate_size
                .is_some_and(|v| !(16..=30).contains(&v))
            || patch
                .key_letter_size
                .is_some_and(|v| !(14..=30).contains(&v))
            || patch
                .key_opacity
                .is_some_and(|v| !v.is_finite() || !(0.4..=1.0).contains(&v))
        {
            return Err("设置值超出支持范围，未保存".to_string());
        }
        Ok(patch)
    }

    fn apply(self, current: &AppSettings) -> (AppSettings, Option<Vec<&'static str>>) {
        let mut saved = current.clone();
        let mut changed = Vec::new();
        let selects_builtin = self.skin.is_some();
        macro_rules! field {
            ($name:ident, $key:literal) => {
                if let Some(value) = self.$name {
                    saved.$name = value;
                    changed.push($key);
                }
            };
        }
        field!(scheme, "scheme");
        field!(mixed_input, "mixed_input");
        field!(chinese_punctuation, "chinese_punctuation");
        field!(shift_toggle_english, "shift_toggle_english");
        field!(caps_lock_bypass, "capslock_bypass");
        field!(page_size, "page_size");
        field!(wheel_paging, "wheel_paging");
        field!(skin, "skin");
        field!(theme_mode, "theme_mode");
        field!(first_candidate_size, "first_candidate_size");
        field!(key_opacity, "key_opacity");
        field!(typing_glow, "typing_glow");
        field!(key_letter_size, "key_letter_size");
        field!(key_letter_bold, "key_letter_bold");
        field!(clipboard_enabled, "clipboard_enabled");
        field!(candidate_layout, "candidate_layout");
        field!(full_width, "full_width");
        field!(traditional, "traditional");
        if selects_builtin {
            saved.custom_skin_id.clear();
            saved.custom_skin_hash.clear();
            changed.extend(["custom_skin_id", "custom_skin_hash"]);
        }
        (saved, Some(changed))
    }
}

/// Private skin-library transaction. Callers cannot submit these fields through
/// the public generic settings patch. Other preferences and unknown lines stay
/// untouched, and publication follows the durable replacement under one lock.
pub(crate) fn skin_transaction(
    path: &Path,
    change: impl FnOnce(&str, &AppSettings) -> Result<BTreeMap<String, String>, String>,
    committed: impl FnOnce(&AppSettings),
) -> Result<AppSettings, String> {
    let _lock = settings_io::lock(path)?;
    let original = settings_io::read(path)?.unwrap_or_default();
    let current = parse_content(&original);
    let values = change(&original, &current)?;
    if values.keys().any(|key| {
        !matches!(
            key.as_str(),
            "skin_library_v1"
                | "skin"
                | "custom_skin_id"
                | "custom_skin_hash"
                | "builtin_skin_edits_v1"
                | "builtin_style_wood_type"
                | "builtin_style_typewriter"
                | "builtin_style_terminal"
                | "builtin_style_brutal"
        )
    }) {
        return Err("皮肤设置字段无效，未保存".into());
    }
    let body = merge_document(&original, &values);
    settings_io::replace(path, &body)?;
    let mut saved = parse_content(&body);
    saved.revision = revision(Some(&body));
    committed(&saved);
    Ok(saved)
}

pub fn publish_current(committed: impl FnOnce(&AppSettings)) -> Result<(), String> {
    let path = settings_path();
    let _lock = settings_io::lock(&path)?;
    let current = try_load_from_path(&path)?;
    committed(&current);
    Ok(())
}

pub fn patch_to_disk(
    patch: serde_json::Value,
    committed: impl FnOnce(&AppSettings),
) -> Result<AppSettings, String> {
    let saved = patch_to_path(&settings_path(), patch, committed)?;
    notify_tsf_settings_changed();
    Ok(saved)
}

fn patch_to_path(
    path: &Path,
    patch: serde_json::Value,
    committed: impl FnOnce(&AppSettings),
) -> Result<AppSettings, String> {
    let patch = SettingsPatch::parse(patch)?;
    write_transaction(path, |latest| Ok(patch.apply(latest)), committed)
}

#[cfg(target_os = "windows")]
pub fn to_wide_null(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(std::iter::once(0)).collect()
}

#[cfg(target_os = "windows")]
pub fn notify_tsf_settings_changed() {
    use windows_sys::Win32::UI::WindowsAndMessaging::{
        FindWindowExW, PostMessageW, RegisterWindowMessageW, HWND_MESSAGE,
    };
    unsafe {
        let msg_name = to_wide_null("FangCunIME_SettingsChanged");
        let msg = RegisterWindowMessageW(msg_name.as_ptr());
        if msg == 0 {
            return;
        }
        let class_name = to_wide_null("FangCunTsfMessageWindow");
        let mut hwnd = 0isize as windows_sys::Win32::Foundation::HWND;
        loop {
            hwnd = FindWindowExW(HWND_MESSAGE, hwnd, class_name.as_ptr(), std::ptr::null());
            if hwnd.is_null() {
                break;
            }
            PostMessageW(hwnd, msg, 0, 0);
        }
    }
}

#[cfg(not(target_os = "windows"))]
pub fn notify_tsf_settings_changed() {}

#[cfg(target_os = "windows")]
pub fn broadcast_english_to_tsf(english: bool) {
    use windows_sys::Win32::UI::WindowsAndMessaging::{
        FindWindowExW, PostMessageW, RegisterWindowMessageW, HWND_MESSAGE,
    };
    unsafe {
        let msg_name = to_wide_null("FangCunIME_SetEnglish");
        let msg = RegisterWindowMessageW(msg_name.as_ptr());
        if msg == 0 {
            return;
        }
        let class_name = to_wide_null("FangCunTsfMessageWindow");
        let mut hwnd = 0isize as windows_sys::Win32::Foundation::HWND;
        loop {
            hwnd = FindWindowExW(HWND_MESSAGE, hwnd, class_name.as_ptr(), std::ptr::null());
            if hwnd.is_null() {
                break;
            }
            PostMessageW(hwnd, msg, usize::from(english), 0);
        }
    }
}

#[cfg(not(target_os = "windows"))]
pub fn broadcast_english_to_tsf(_english: bool) {}

#[cfg(target_os = "windows")]
pub fn is_windows_apps_dark() -> bool {
    use windows_sys::Win32::System::Registry::{
        RegCloseKey, RegOpenKeyExW, RegQueryValueExW, HKEY, HKEY_CURRENT_USER, KEY_READ,
    };
    unsafe {
        let subkey = to_wide_null(r"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize");
        let value_name = to_wide_null("AppsUseLightTheme");
        let mut hkey: HKEY = std::ptr::null_mut();
        if RegOpenKeyExW(HKEY_CURRENT_USER, subkey.as_ptr(), 0, KEY_READ, &mut hkey) != 0 {
            return false;
        }
        let mut val: u32 = 1;
        let mut size: u32 = std::mem::size_of::<u32>() as u32;
        let rc = RegQueryValueExW(
            hkey,
            value_name.as_ptr(),
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            &mut val as *mut u32 as *mut u8,
            &mut size,
        );
        RegCloseKey(hkey);
        if rc != 0 {
            return false;
        }
        val == 0
    }
}

#[cfg(not(target_os = "windows"))]
pub fn is_windows_apps_dark() -> bool {
    false
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, Ordering};

    static NEXT_PATH: AtomicU64 = AtomicU64::new(0);

    struct IniFixture(PathBuf);
    impl IniFixture {
        fn new() -> Self {
            let nanos = std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos();
            Self(std::env::temp_dir().join(format!(
                "fangcun-square-settings-{}-{nanos}-{}.ini",
                std::process::id(),
                NEXT_PATH.fetch_add(1, Ordering::Relaxed)
            )))
        }
    }
    impl Drop for IniFixture {
        fn drop(&mut self) {
            let _ = fs::remove_file(&self.0);
            let mut lock = self.0.as_os_str().to_os_string();
            lock.push(".lock");
            let _ = fs::remove_file(PathBuf::from(lock));
        }
    }

    #[test]
    fn upgrading_legacy_skin_ini_preserves_typography_language_and_clipboard_preferences() {
        for legacy in [
            "native_light", "metro", "warm_grid", "swiss", "mint", "eink", "deep_blue",
        ] {
            let current = "system_default";
            let file = IniFixture::new();
            fs::write(&file.0, format!(
                "skin={legacy}\nscheme=ninekey\ntheme_mode=light\nmixed_input=0\nchinese_punctuation=0\n\
                 shift_toggle_english=0\ncapslock_bypass=0\npage_size=9\nwheel_paging=0\n\
                 first_candidate_size=28\nkey_opacity=0.47\ntyping_glow=0\nkey_letter_size=25\n\
                 key_letter_bold=1\nclipboard_enabled=0\n"
            )).unwrap();
            let loaded = load_from_path(&file.0);
            assert_eq!(loaded.skin, current);
            assert_eq!(loaded.theme_mode, "light");
            assert!(!loaded.dark_mode);
            assert_eq!(loaded.scheme, "ninekey");
            assert!(!loaded.mixed_input && !loaded.chinese_punctuation);
            assert!(!loaded.shift_toggle_english && !loaded.caps_lock_bypass);
            assert_eq!(loaded.page_size, 9);
            assert!(!loaded.wheel_paging && !loaded.clipboard_enabled);
            assert_eq!(loaded.first_candidate_size, 28);
            assert_eq!(loaded.key_letter_size, 25);
            assert!(loaded.key_letter_bold);
            assert!(!loaded.typing_glow);
            assert!((loaded.key_opacity - 0.47).abs() < f64::EPSILON);
            let mut before = serde_json::to_value(&loaded).unwrap();
            before.as_object_mut().unwrap().remove("revision");
            save_to_path(&file.0, loaded).unwrap();
            let saved = fs::read_to_string(&file.0).unwrap();
            assert!(saved.contains(&format!("skin={current}\n")));
            let mut after = serde_json::to_value(load_from_path(&file.0)).unwrap();
            after.as_object_mut().unwrap().remove("revision");
            assert_eq!(after, before);
        }
    }

    #[test]
    fn all_four_local_package_bases_round_trip_without_resetting_explicit_theme() {
        for skin in ["wood_type", "typewriter", "terminal", "brutal"] {
            let file = IniFixture::new();
            fs::write(&file.0, format!("skin={skin}\ncustom_skin_id=fangcun.sample\ncustom_skin_hash={}\ntheme_mode=dark\nfirst_candidate_size=23\nclipboard_enabled=0\n", "a".repeat(64))).unwrap();
            let selected = load_from_path(&file.0);
            let saved = save_to_path(&file.0, selected).unwrap();
            let loaded = load_from_path(&file.0);
            assert_eq!(loaded.skin, skin);
            assert_eq!(loaded.custom_skin_id, "fangcun.sample");
            assert_eq!(loaded.custom_skin_hash, "a".repeat(64));
            assert_eq!(loaded.theme_mode, "dark");
            assert!(loaded.dark_mode);
            assert_eq!(loaded.first_candidate_size, 23);
            assert!(!loaded.clipboard_enabled);
            assert_eq!(serde_json::to_value(loaded).unwrap(), serde_json::to_value(saved).unwrap());
        }
    }

    #[test]
    fn independent_patches_merge_under_the_same_real_file_lock_and_keep_unknown_lines() {
        let file = IniFixture::new();
        fs::write(&file.0, "# keep this comment\r\nfuture_option = 未知设置\r\nmixed_input=1\r\nclipboard_enabled=1\r\n").unwrap();
        let barrier = std::sync::Arc::new(std::sync::Barrier::new(3));
        let mut workers = Vec::new();
        for patch in [
            serde_json::json!({"skin":"system_default","themeMode":"dark"}),
            serde_json::json!({"clipboardEnabled":false,"pageSize":9}),
        ] {
            let path = file.0.clone();
            let barrier = barrier.clone();
            workers.push(std::thread::spawn(move || {
                barrier.wait();
                patch_to_path(&path, patch, |_| {}).unwrap()
            }));
        }
        barrier.wait();
        for worker in workers {
            worker.join().unwrap();
        }
        let saved = try_load_from_path(&file.0).unwrap();
        assert_eq!(saved.skin, "system_default");
        assert_eq!(saved.theme_mode, "dark");
        assert_eq!(saved.page_size, 9);
        assert!(!saved.clipboard_enabled);
        assert!(saved.mixed_input);
        let content = fs::read_to_string(&file.0).unwrap();
        assert!(content.starts_with("# keep this comment\r\nfuture_option = 未知设置\r\n"));
        assert_eq!(content.matches("future_option").count(), 1);
    }

    #[test]
    fn a_stale_full_snapshot_cannot_undo_a_newer_clipboard_patch() {
        let file = IniFixture::new();
        save_to_path(&file.0, AppSettings::default()).unwrap();
        let mut stale = try_load_from_path(&file.0).unwrap();
        patch_to_path(
            &file.0,
            serde_json::json!({"clipboardEnabled":false}),
            |_| {},
        )
        .unwrap();
        let committed = fs::read(&file.0).unwrap();
        stale.skin = "brutal".to_string();
        assert!(save_to_path(&file.0, stale).is_err());
        assert_eq!(fs::read(&file.0).unwrap(), committed);
        let updated = patch_to_path(&file.0, serde_json::json!({"skin":"system_default"}), |_| {}).unwrap();
        assert_eq!(updated.skin, "system_default");
        assert!(!updated.clipboard_enabled);
        assert_eq!(
            updated.revision,
            try_load_from_path(&file.0).unwrap().revision
        );
    }

    #[test]
    fn failed_reads_and_invalid_patches_preserve_existing_file_and_do_not_notify() {
        let file = IniFixture::new();
        fs::write(&file.0, b"\xff\xfeoriginal").unwrap();
        let original = fs::read(&file.0).unwrap();
        assert!(try_load_from_path(&file.0).is_err());
        let notified = std::cell::Cell::new(false);
        assert!(
            patch_to_path(&file.0, serde_json::json!({"mixedInput":false}), |_| {
                notified.set(true)
            })
            .is_err()
        );
        assert!(!notified.get());
        assert_eq!(fs::read(&file.0).unwrap(), original);
        fs::write(&file.0, "scheme=flypy\nfuture=preserved\n").unwrap();
        let original = fs::read(&file.0).unwrap();
        for invalid in [
            serde_json::json!({"themeMode":"dark\nclipboard_enabled=1"}),
            serde_json::json!({"keyOpacity":null}),
            serde_json::json!({"keyOpacity":0.2}),
            serde_json::json!({"pageSize":99}),
            serde_json::json!({"unknownSetting":true}),
            serde_json::json!({"systemDark":true}),
            serde_json::json!({"mixedInput":"false"}),
        ] {
            assert!(patch_to_path(&file.0, invalid, |_| notified.set(true)).is_err());
            assert!(!notified.get());
            assert_eq!(fs::read(&file.0).unwrap(), original);
        }
        let mut invalid = AppSettings::default();
        invalid.key_opacity = f64::NAN;
        assert!(save_to_path(&file.0, invalid).is_err());
        assert_eq!(fs::read(&file.0).unwrap(), original);
    }

    #[test]
    fn readers_never_observe_half_a_settings_transaction() {
        let file = IniFixture::new();
        patch_to_path(
            &file.0,
            serde_json::json!({"pageSize":5,"firstCandidateSize":16}),
            |_| {},
        )
        .unwrap();
        let path = file.0.clone();
        let finished = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
        let done = finished.clone();
        let writer = std::thread::spawn(move || {
            // A writer error must fail this test through join(), not leave its
            // reader spinning forever and hide the original Windows failure.
            struct FinishOnDrop(std::sync::Arc<std::sync::atomic::AtomicBool>);
            impl Drop for FinishOnDrop {
                fn drop(&mut self) {
                    self.0.store(true, Ordering::Release);
                }
            }
            let _finished = FinishOnDrop(done);
            for index in 0..30 {
                let (page, size) = if index % 2 == 0 { (9, 30) } else { (5, 16) };
                patch_to_path(
                    &path,
                    serde_json::json!({"pageSize":page,"firstCandidateSize":size}),
                    |_| {},
                )
                .unwrap();
            }
        });
        let mut reads = 0;
        while !finished.load(Ordering::Acquire) {
            let snapshot = try_load_from_path(&file.0).unwrap();
            assert!(matches!(
                (snapshot.page_size, snapshot.first_candidate_size),
                (5, 16) | (9, 30)
            ));
            reads += 1;
        }
        writer.join().unwrap();
        assert!(reads > 0);
    }

    #[test]
    fn busy_settings_lock_fails_in_bounded_time_without_replacing_preferences() {
        let file = IniFixture::new();
        save_to_path(&file.0, AppSettings::default()).unwrap();
        let before = fs::read(&file.0).unwrap();
        let lock = settings_io::lock(&file.0).unwrap();
        let path = file.0.clone();
        let start = std::time::Instant::now();
        let failed = std::thread::spawn(move || {
            patch_to_path(&path, serde_json::json!({"pageSize":9}), |_| {})
        })
        .join()
        .unwrap();
        assert!(failed.is_err());
        assert!(start.elapsed() < std::time::Duration::from_secs(2));
        assert_eq!(fs::read(&file.0).unwrap(), before);
        drop(lock);
        assert_eq!(
            patch_to_path(&file.0, serde_json::json!({"pageSize":9}), |_| {})
                .unwrap()
                .page_size,
            9
        );
    }

    #[test]
    fn unreadable_or_oversized_settings_are_never_treated_as_a_new_file() {
        let file = IniFixture::new();
        fs::write(&file.0, "x".repeat(64 * 1024 + 1)).unwrap();
        let original = fs::read(&file.0).unwrap();
        assert!(try_load_from_path(&file.0).is_err());
        assert!(patch_to_path(&file.0, serde_json::json!({"skin":"terminal"}), |_| {}).is_err());
        assert_eq!(fs::read(&file.0).unwrap(), original);
        let child = file.0.join("settings.ini");
        assert!(save_to_path(&child, AppSettings::default()).is_err());
        assert_eq!(fs::read(&file.0).unwrap(), original);
    }

    #[test]
    fn legacy_dark_preference_and_unknown_fields_survive_a_partial_edit() {
        let file = IniFixture::new();
        fs::write(&file.0, "dark=1\nfuture=keep\nclipboard_enabled=0\n").unwrap();
        let saved =
            patch_to_path(&file.0, serde_json::json!({"skin":"system_default"}), |_| {}).unwrap();
        assert_eq!(saved.theme_mode, "dark");
        assert!(saved.dark_mode);
        assert!(!saved.clipboard_enabled);
        assert!(fs::read_to_string(&file.0)
            .unwrap()
            .contains("future=keep\n"));
        assert_eq!(try_load_from_path(&file.0).unwrap().theme_mode, "dark");
    }

    #[test]
    fn first_partial_write_keeps_follow_system_instead_of_creating_a_legacy_light_override() {
        let file = IniFixture::new();
        let saved =
            patch_to_path(&file.0, serde_json::json!({"mixedInput":false}), |_| {}).unwrap();
        assert_eq!(saved.theme_mode, "system");
        let reloaded = try_load_from_path(&file.0).unwrap();
        assert_eq!(reloaded.theme_mode, "system");
        assert!(!reloaded.mixed_input);
        assert!(fs::read_to_string(&file.0)
            .unwrap()
            .contains("theme_mode=system\n"));
    }

    #[test]
    fn ordinary_edits_preserve_local_skin_and_explicit_system_selection_clears_it() {
        let file = IniFixture::new();
        let hash = "a".repeat(64);
        fs::write(&file.0, format!("skin=brutal\ncustom_skin_id=fangcun.sample\ncustom_skin_hash={hash}\ntheme_mode=dark\nfuture=keep\n")).unwrap();
        let saved = patch_to_path(
            &file.0,
            serde_json::json!({"mixedInput":false,"keyLetterSize":25}),
            |_| {},
        )
        .unwrap();
        assert_eq!(saved.custom_skin_id, "fangcun.sample");
        assert_eq!(saved.custom_skin_hash, hash);
        let saved = patch_to_path(&file.0, serde_json::json!({"skin":"system_default"}), |_| {}).unwrap();
        assert!(saved.custom_skin_id.is_empty() && saved.custom_skin_hash.is_empty());
        assert_eq!(saved.theme_mode, "dark");
        assert_eq!(saved.key_letter_size, 25);
        assert!(!saved.mixed_input);
        assert!(fs::read_to_string(&file.0)
            .unwrap()
            .contains("future=keep\n"));
    }

    #[test]
    fn retired_named_skin_patches_never_clear_local_selection_or_publish() {
        let file = IniFixture::new();
        let body = format!("skin=brutal\ncustom_skin_id=fangcun.sample\ncustom_skin_hash={}\nfuture=keep\n", "a".repeat(64));
        fs::write(&file.0, &body).unwrap();
        for id in ["wood_type", "typewriter", "terminal", "brutal"] {
            assert!(patch_to_path(&file.0, serde_json::json!({"skin":id}), |_| panic!("Retired patch cannot publish")).is_err());
            assert_eq!(fs::read_to_string(&file.0).unwrap(), body);
        }
    }

    #[test]
    fn public_generic_settings_cannot_forge_a_local_skin_selection() {
        let file = IniFixture::new();
        let hash = "a".repeat(64);
        fs::write(
            &file.0,
            format!("skin=brutal\ncustom_skin_id=fangcun.sample\ncustom_skin_hash={hash}\n"),
        )
        .unwrap();
        let before = fs::read(&file.0).unwrap();
        assert!(patch_to_path(
            &file.0,
            serde_json::json!({"customSkinId":"fangcun.forged","customSkinHash":"b".repeat(64)}),
            |_| {}
        )
        .is_err());
        assert_eq!(fs::read(&file.0).unwrap(), before);
        let mut forged = try_load_from_path(&file.0).unwrap();
        forged.custom_skin_id = "fangcun.forged".into();
        forged.custom_skin_hash = "b".repeat(64);
        forged.mixed_input = false;
        let saved = save_to_path(&file.0, forged).unwrap();
        assert_eq!(saved.custom_skin_id, "fangcun.sample");
        assert_eq!(saved.custom_skin_hash, hash);
        assert!(!saved.mixed_input);
    }
}
