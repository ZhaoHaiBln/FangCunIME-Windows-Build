use serde::{Deserialize, Serialize};
use std::fs;
use std::path::PathBuf;

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
            skin: "warm_grid".to_string(),
            theme_mode: "system".to_string(),
            dark_mode: sys_dark,
            system_dark: sys_dark,
            first_candidate_size: 20,
            key_opacity: 0.82,
            typing_glow: true,
            key_letter_size: 18,
            key_letter_bold: false,
            clipboard_enabled: true,
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

fn normalize(mut s: AppSettings) -> AppSettings {
    s.system_dark = is_windows_apps_dark();
    if !matches!(s.scheme.as_str(), "flypy" | "pinyin" | "ninekey") {
        s.scheme = "flypy".to_string();
    }
    if !matches!(
        s.skin.as_str(),
        "native_light" | "warm_grid" | "mint" | "deep_blue"
    ) {
        s.skin = "warm_grid".to_string();
    }
    if !matches!(s.theme_mode.as_str(), "system" | "light" | "dark") {
        s.theme_mode = "system".to_string();
    }
    s.dark_mode = match s.theme_mode.as_str() {
        "dark" => true,
        "light" => false,
        _ => s.system_dark,
    };
    s.page_size = s.page_size.clamp(5, 9);
    s.first_candidate_size = s.first_candidate_size.clamp(16, 30);
    s.key_letter_size = s.key_letter_size.clamp(14, 30);
    s.key_opacity = s.key_opacity.clamp(0.40, 1.00);
    s
}

pub fn load_from_disk() -> AppSettings {
    let mut s = AppSettings::default();
    let path = settings_path();
    if let Ok(content) = fs::read_to_string(&path) {
        for raw_line in content.lines() {
            let line = raw_line.trim();
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
                "theme_mode" => s.theme_mode = val.to_string(),
                "dark" | "dark_mode" => {
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
                "clipboard_enabled" => {
                    s.clipboard_enabled = val == "1" || val.eq_ignore_ascii_case("true")
                }
                _ => {}
            }
        }
    }
    normalize(s)
}

pub fn save_to_disk(input: AppSettings) -> Result<AppSettings, String> {
    let s = normalize(input);
    let path = settings_path();
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|e| format!("create settings dir failed: {e}"))?;
    }
    let bool_int = |b: bool| if b { 1 } else { 0 };
    let body = format!(
        "scheme={}\nmixed_input={}\nchinese_punctuation={}\nshift_toggle_english={}\ncapslock_bypass={}\npage_size={}\nwheel_paging={}\nskin={}\ntheme_mode={}\ndark={}\ndark_mode={}\nfirst_candidate_size={}\nkey_opacity={:.2}\ntyping_glow={}\nkey_letter_size={}\nkey_letter_bold={}\nclipboard_enabled={}\n",
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
    );
    fs::write(&path, body).map_err(|e| format!("write settings.ini failed: {e}"))?;
    notify_tsf_settings_changed();
    Ok(s)
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
