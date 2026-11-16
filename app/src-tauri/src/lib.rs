mod clipboard;
mod settings;

use arboard::Clipboard;
use clipboard::{ClipboardItem, ClipboardState};
use settings::AppSettings;
use std::sync::{Arc, Mutex};
use tauri::{
    menu::{Menu, MenuItem},
    tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent},
    AppHandle, Emitter, Manager, State, WindowEvent,
};

struct AppState {
    clipboard: Arc<Mutex<ClipboardState>>,
}

#[tauri::command]
fn load_settings() -> AppSettings {
    settings::load_from_disk()
}

#[tauri::command]
fn save_settings(settings: AppSettings) -> Result<AppSettings, String> {
    settings::save_to_disk(settings)
}

#[tauri::command]
fn reset_settings() -> Result<AppSettings, String> {
    settings::save_to_disk(AppSettings::default())
}

#[tauri::command]
fn hide_window(app: AppHandle) {
    if let Some(win) = app.get_webview_window("main") {
        let _ = win.hide();
    }
}

#[tauri::command]
fn list_clipboard(state: State<'_, AppState>) -> Vec<ClipboardItem> {
    state.clipboard.lock().unwrap().ordered_items()
}

#[tauri::command]
fn copy_clipboard_item(
    app: AppHandle,
    state: State<'_, AppState>,
    id: String,
) -> Result<Vec<ClipboardItem>, String> {
    let text = {
        let guard = state.clipboard.lock().unwrap();
        guard
            .items
            .iter()
            .find(|item| item.id == id)
            .map(|item| item.text.clone())
            .ok_or_else(|| "未找到该剪贴板条目".to_string())?
    };

    let mut cb = Clipboard::new().map_err(|e| format!("打开系统剪贴板失败: {e}"))?;
    cb.set_text(text.clone())
        .map_err(|e| format!("写入剪贴板失败: {e}"))?;

    let ordered = {
        let mut guard = state.clipboard.lock().unwrap();
        guard.last_seen_text = text;
        guard.ordered_items()
    };
    let _ = app.emit("clipboard://changed", &ordered);
    Ok(ordered)
}

#[tauri::command]
fn toggle_pin_clipboard(
    app: AppHandle,
    state: State<'_, AppState>,
    id: String,
) -> Vec<ClipboardItem> {
    let ordered = {
        let mut guard = state.clipboard.lock().unwrap();
        if let Some(item) = guard.items.iter_mut().find(|i| i.id == id) {
            item.pinned = !item.pinned;
        }
        guard.persist();
        guard.ordered_items()
    };
    let _ = app.emit("clipboard://changed", &ordered);
    ordered
}

#[tauri::command]
fn delete_clipboard_item(
    app: AppHandle,
    state: State<'_, AppState>,
    id: String,
) -> Vec<ClipboardItem> {
    let ordered = {
        let mut guard = state.clipboard.lock().unwrap();
        guard.items.retain(|i| i.id != id);
        guard.persist();
        guard.ordered_items()
    };
    let _ = app.emit("clipboard://changed", &ordered);
    ordered
}

#[tauri::command]
fn clear_clipboard(
    app: AppHandle,
    state: State<'_, AppState>,
    keep_pinned: bool,
) -> Vec<ClipboardItem> {
    let ordered = {
        let mut guard = state.clipboard.lock().unwrap();
        if keep_pinned {
            guard.items.retain(|i| i.pinned);
        } else {
            guard.items.clear();
        }
        guard.persist();
        guard.ordered_items()
    };
    let _ = app.emit("clipboard://changed", &ordered);
    ordered
}

#[tauri::command]
fn get_clipboard_enabled(state: State<'_, AppState>) -> bool {
    state.clipboard.lock().unwrap().enabled
}

#[tauri::command]
fn set_clipboard_enabled(state: State<'_, AppState>, enabled: bool) -> bool {
    let mut guard = state.clipboard.lock().unwrap();
    guard.enabled = enabled;
    guard.enabled
}

fn show_main_window(app: &AppHandle) {
    if let Some(win) = app.get_webview_window("main") {
        let _ = win.show();
        let _ = win.unminimize();
        let _ = win.set_focus();
    }
}

#[cfg(target_os = "windows")]
fn start_hotkey_listener(app: AppHandle) {
    std::thread::spawn(move || {
        use windows_sys::Win32::UI::Input::KeyboardAndMouse::{
            RegisterHotKey, UnregisterHotKey, MOD_ALT, MOD_NOREPEAT,
        };
        use windows_sys::Win32::UI::WindowsAndMessaging::{GetMessageW, MSG, WM_HOTKEY};

        const HOTKEY_CLIPBOARD_ID: i32 = 1;
        unsafe {
            if RegisterHotKey(
                std::ptr::null_mut(),
                HOTKEY_CLIPBOARD_ID,
                MOD_ALT | MOD_NOREPEAT,
                'V' as u32,
            ) == 0
            {
                return;
            }
            let mut msg: MSG = std::mem::zeroed();
            while GetMessageW(&mut msg, std::ptr::null_mut(), 0, 0) > 0 {
                if msg.message == WM_HOTKEY && msg.wParam == HOTKEY_CLIPBOARD_ID as usize {
                    if let Some(win) = app.get_webview_window("main") {
                        let visible = win.is_visible().unwrap_or(false);
                        let focused = win.is_focused().unwrap_or(false);
                        if visible && focused {
                            let _ = win.hide();
                        } else {
                            show_main_window(&app);
                            let _ = app.emit("ui://open-clipboard", ());
                        }
                    }
                }
            }
            UnregisterHotKey(std::ptr::null_mut(), HOTKEY_CLIPBOARD_ID);
        }
    });
}

#[cfg(not(target_os = "windows"))]
fn start_hotkey_listener(_app: AppHandle) {}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    let clipboard_state = Arc::new(Mutex::new(ClipboardState::load()));
    let watcher_state = Arc::clone(&clipboard_state);

    tauri::Builder::default()
        .manage(AppState {
            clipboard: clipboard_state,
        })
        .setup(move |app| {
            clipboard::start_watcher(app.handle().clone(), watcher_state);
            start_hotkey_listener(app.handle().clone());

            let open_item =
                MenuItem::with_id(app, "open", "打开方寸设置 & 剪贴板", true, None::<&str>)?;
            let quit_item = MenuItem::with_id(app, "quit", "退出方寸伴侣", true, None::<&str>)?;
            let menu = Menu::with_items(app, &[&open_item, &quit_item])?;

            let mut tray_builder = TrayIconBuilder::new()
                .menu(&menu)
                .tooltip("方寸输入法 · 设置与剪贴板")
                .on_menu_event(|app, event| match event.id.as_ref() {
                    "open" => show_main_window(app),
                    "quit" => app.exit(0),
                    _ => {}
                })
                .on_tray_icon_event(|tray, event| {
                    if let TrayIconEvent::Click {
                        button: MouseButton::Left,
                        button_state: MouseButtonState::Up,
                        ..
                    } = event
                    {
                        show_main_window(tray.app_handle());
                    }
                });

            if let Some(icon) = app.default_window_icon().cloned() {
                tray_builder = tray_builder.icon(icon);
            }
            let _ = tray_builder.build(app)?;
            Ok(())
        })
        .on_window_event(|window, event| {
            if let WindowEvent::CloseRequested { api, .. } = event {
                api.prevent_close();
                let _ = window.hide();
            }
        })
        .invoke_handler(tauri::generate_handler![
            load_settings,
            save_settings,
            reset_settings,
            hide_window,
            list_clipboard,
            copy_clipboard_item,
            toggle_pin_clipboard,
            delete_clipboard_item,
            clear_clipboard,
            get_clipboard_enabled,
            set_clipboard_enabled,
        ])
        .run(tauri::generate_context!())
        .expect("error while running FangCunSettings");
}
