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
fn save_settings(app: AppHandle, settings: AppSettings) -> Result<AppSettings, String> {
    let saved = settings::save_to_disk(settings)?;
    let _ = app.emit("settings://changed", &saved);
    Ok(saved)
}

#[tauri::command]
fn reset_settings(app: AppHandle) -> Result<AppSettings, String> {
    let saved = settings::save_to_disk(AppSettings::default())?;
    let _ = app.emit("settings://changed", &saved);
    Ok(saved)
}

#[tauri::command]
fn hide_window(app: AppHandle) {
    if let Some(win) = app.get_webview_window("main") {
        let _ = win.hide();
    }
}

#[tauri::command]
fn hide_clipboard_window(app: AppHandle) {
    if let Some(win) = app.get_webview_window("clipboard") {
        let _ = win.hide();
    }
}

#[tauri::command]
fn open_settings_tab(app: AppHandle, tab: String) {
    if let Some(cb) = app.get_webview_window("clipboard") {
        let _ = cb.hide();
    }
    show_main_window(&app);
    let _ = app.emit("ui://switch-tab", tab);
}

#[tauri::command]
fn toggle_statusbar(app: AppHandle) {
    if let Some(win) = app.get_webview_window("statusbar") {
        if win.is_visible().unwrap_or(false) {
            let _ = win.hide();
        } else {
            let _ = win.show();
        }
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

fn toggle_clipboard_window(app: &AppHandle) {
    if let Some(win) = app.get_webview_window("clipboard") {
        let visible = win.is_visible().unwrap_or(false);
        let focused = win.is_focused().unwrap_or(false);
        if visible && focused {
            let _ = win.hide();
        } else {
            let _ = win.show();
            let _ = win.unminimize();
            let _ = win.set_focus();
            let _ = app.emit("ui://clipboard-opened", ());
        }
    }
}

fn position_statusbar_near_taskbar(app: &AppHandle) {
    if let Some(win) = app.get_webview_window("statusbar") {
        if let Ok(Some(monitor)) = win.primary_monitor() {
            let work = monitor.work_area();
            let scale = monitor.scale_factor();
            let bar_w = (224.0 * scale).round() as i32;
            let bar_h = (44.0 * scale).round() as i32;
            let margin_x = (18.0 * scale).round() as i32;
            let margin_y = (12.0 * scale).round() as i32;
            let x = work.position.x + (work.size.width as i32) - bar_w - margin_x;
            let y = work.position.y + (work.size.height as i32) - bar_h - margin_y;
            let _ = win.set_position(tauri::Position::Physical(tauri::PhysicalPosition::new(x, y)));
        }
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
                    toggle_clipboard_window(&app);
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
    let start_in_tray = std::env::args().any(|a| a == "--tray");

    tauri::Builder::default()
        .manage(AppState {
            clipboard: clipboard_state,
        })
        .setup(move |app| {
            clipboard::start_watcher(app.handle().clone(), watcher_state);
            start_hotkey_listener(app.handle().clone());

            if start_in_tray {
                if let Some(win) = app.get_webview_window("main") {
                    let _ = win.hide();
                }
            }
            position_statusbar_near_taskbar(app.handle());

            let open_item =
                MenuItem::with_id(app, "open", "打开方寸设置", true, None::<&str>)?;
            let clip_item =
                MenuItem::with_id(app, "clipboard", "打开剪贴板 (Alt+V)", true, None::<&str>)?;
            let bar_item =
                MenuItem::with_id(app, "statusbar", "显示/隐藏浮动状态栏", true, None::<&str>)?;
            let quit_item = MenuItem::with_id(app, "quit", "退出方寸伴侣", true, None::<&str>)?;
            let menu = Menu::with_items(app, &[&open_item, &clip_item, &bar_item, &quit_item])?;

            let mut tray_builder = TrayIconBuilder::new()
                .menu(&menu)
                .tooltip("方寸输入法 · 状态栏与剪贴板 (Alt+V)")
                .on_menu_event(|app, event| match event.id.as_ref() {
                    "open" => show_main_window(app),
                    "clipboard" => toggle_clipboard_window(app),
                    "statusbar" => toggle_statusbar(app.clone()),
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
        .on_window_event(|window, event| match event {
            WindowEvent::CloseRequested { api, .. } => {
                api.prevent_close();
                let _ = window.hide();
            }
            WindowEvent::Focused(false) if window.label() == "clipboard" => {
                let _ = window.hide();
            }
            _ => {}
        })
        .invoke_handler(tauri::generate_handler![
            load_settings,
            save_settings,
            reset_settings,
            hide_window,
            hide_clipboard_window,
            open_settings_tab,
            toggle_statusbar,
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
