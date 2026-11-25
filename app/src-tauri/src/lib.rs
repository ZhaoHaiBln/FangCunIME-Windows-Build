mod ai;
mod ai_commands;
mod clipboard;
mod clipboard_history;
mod settings;
mod settings_io;
mod sync_manager;
mod sync_protocol;
mod sync_transport;
mod sync_windows;

use arboard::Clipboard;
use clipboard::{ClipboardItem, ClipboardState};
use settings::AppSettings;
use std::sync::{Arc, Mutex};
use sync_manager::{ShareDraft, ShareManager, ShareSnapshot};
use tauri::{
    menu::{Menu, MenuItem},
    tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent},
    AppHandle, Emitter, Manager, State, WindowEvent,
};

struct AppState {
    clipboard: Arc<Mutex<ClipboardState>>,
    share: Arc<ShareManager>,
}

#[tauri::command]
async fn clipboard_share_snapshot(state: State<'_, AppState>) -> Result<ShareSnapshot, String> {
    state.share.snapshot()
}
#[tauri::command]
async fn clipboard_share_save(
    app: AppHandle,
    state: State<'_, AppState>,
    draft: ShareDraft,
) -> Result<ShareSnapshot, String> {
    state.share.save(&app, draft)
}
#[tauri::command]
async fn clipboard_share_enabled(
    app: AppHandle,
    state: State<'_, AppState>,
    enabled: bool,
) -> Result<ShareSnapshot, String> {
    state.share.set_enabled(&app, enabled)
}
#[tauri::command]
async fn clipboard_share_auto_send(
    app: AppHandle,
    state: State<'_, AppState>,
    enabled: bool,
) -> Result<ShareSnapshot, String> {
    state.share.set_auto_send(&app, enabled)
}
#[tauri::command]
async fn clipboard_share_generate_key(
    app: AppHandle,
    state: State<'_, AppState>,
) -> Result<ShareSnapshot, String> {
    state.share.generate_key(&app)
}
#[tauri::command]
async fn clipboard_share_import(
    app: AppHandle,
    state: State<'_, AppState>,
    pairing: String,
) -> Result<ShareSnapshot, String> {
    state.share.import_pairing(&app, &pairing)
}
#[tauri::command]
async fn clipboard_share_export(state: State<'_, AppState>) -> Result<(), String> {
    let text = state.share.pairing_text()?;
    sync_windows::write_excluded(&text).map_err(str::to_string)?;
    if let Ok(mut clipboard) = state.clipboard.lock() {
        clipboard.last_seen_text = text;
    }
    Ok(())
}
#[tauri::command]
async fn clipboard_share_send_current(
    app: AppHandle,
    state: State<'_, AppState>,
    revision: String,
) -> Result<(), String> {
    let text = sync_windows::read_snapshot()
        .map_err(str::to_string)?
        .text
        .ok_or("当前剪贴板为空或标记为敏感，未发送")?;
    state.share.send_text(&app, text, &revision)
}
#[tauri::command]
async fn clipboard_share_send_item(
    app: AppHandle,
    state: State<'_, AppState>,
    id: String,
    revision: String,
) -> Result<(), String> {
    let text = {
        let clipboard = state.clipboard.lock().map_err(|_| "本机剪贴板状态不可用")?;
        clipboard
            .items
            .iter()
            .find(|item| item.id == id && item.source == "windows")
            .map(|item| item.text.clone())
            .ok_or("该本机条目不存在或不能再次转发")?
    };
    state.share.send_text(&app, text, &revision)
}
#[tauri::command]
async fn clipboard_share_copy(state: State<'_, AppState>, id: String) -> Result<(), String> {
    let text = state.share.shared_text(&id)?;
    sync_windows::write_excluded(&text).map_err(str::to_string)?;
    if let Ok(mut clipboard) = state.clipboard.lock() {
        clipboard.last_seen_text = text;
    }
    Ok(())
}
#[tauri::command]
async fn clipboard_share_remove(
    app: AppHandle,
    state: State<'_, AppState>,
    id: String,
) -> Result<ShareSnapshot, String> {
    state.share.remove_item(&app, &id)
}
#[tauri::command]
async fn clipboard_share_reset(
    app: AppHandle,
    state: State<'_, AppState>,
) -> Result<ShareSnapshot, String> {
    state.share.reset(&app)
}

#[tauri::command]
async fn load_settings() -> Result<AppSettings, String> {
    tauri::async_runtime::spawn_blocking(settings::try_load_from_disk)
        .await
        .map_err(|_| "读取设置未完成，请重试".to_string())?
}

fn publish_settings(app: &AppHandle, clipboard: &Arc<Mutex<ClipboardState>>, saved: &AppSettings) {
    // Release the clipboard mutex before dispatching any window operations.
    if let Ok(mut guard) = clipboard.lock() {
        guard.enabled = saved.clipboard_enabled;
    }
    apply_window_appearance(app, saved);
    let _ = app.emit("settings://changed", saved);
    let _ = app.emit("clipboard://enabled-changed", saved.clipboard_enabled);
}

#[tauri::command]
async fn save_settings(
    app: AppHandle,
    state: State<'_, AppState>,
    settings: AppSettings,
) -> Result<AppSettings, String> {
    let clipboard = Arc::clone(&state.clipboard);
    tauri::async_runtime::spawn_blocking(move || {
        settings::save_to_disk_and(settings, |saved| publish_settings(&app, &clipboard, saved))
    })
    .await
    .map_err(|_| "保存设置未完成，请重新读取后重试".to_string())?
}

#[tauri::command]
async fn patch_settings(
    app: AppHandle,
    state: State<'_, AppState>,
    patch: serde_json::Value,
) -> Result<AppSettings, String> {
    let clipboard = Arc::clone(&state.clipboard);
    tauri::async_runtime::spawn_blocking(move || {
        settings::patch_to_disk(patch, |saved| publish_settings(&app, &clipboard, saved))
    })
    .await
    .map_err(|_| "保存设置未完成，请重新读取后重试".to_string())?
}

#[tauri::command]
async fn reset_settings(app: AppHandle, state: State<'_, AppState>) -> Result<AppSettings, String> {
    let clipboard = Arc::clone(&state.clipboard);
    tauri::async_runtime::spawn_blocking(move || {
        settings::save_to_disk_and(AppSettings::default(), |saved| {
            publish_settings(&app, &clipboard, saved)
        })
    })
    .await
    .map_err(|_| "恢复设置未完成，请重试".to_string())?
}

fn cancel_ai_window(app: &AppHandle, label: &str) {
    if let Some(manager) = app.try_state::<Arc<ai::AiManager>>() {
        manager.cancel_window(label);
    }
    if let Some(window) = app.get_webview_window(label) {
        let _ = window.emit("ai://window-hidden", ());
    }
}

#[tauri::command]
fn hide_window(app: AppHandle) {
    cancel_ai_window(&app, "main");
    if let Some(win) = app.get_webview_window("main") {
        let _ = win.hide();
    }
}

#[tauri::command]
fn hide_clipboard_window(app: AppHandle) {
    cancel_ai_window(&app, "clipboard");
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
            position_statusbar_near_taskbar(&app);
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

async fn change_clipboard(
    app: AppHandle,
    clipboard: Arc<Mutex<ClipboardState>>,
    change: impl FnOnce(&mut ClipboardState) -> Result<(), String> + Send + 'static,
) -> Result<Vec<ClipboardItem>, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let mut guard = clipboard
            .lock()
            .map_err(|_| "剪贴板状态不可用，请重试".to_string())?;
        let previous = guard.items.clone();
        if let Err(message) = change(&mut guard) {
            guard.items = previous;
            return Err(message);
        }
        if guard.persist().is_err() {
            guard.items = previous;
            return Err("剪贴板历史保存失败，原记录已保留".to_string());
        }
        let ordered = guard.ordered_items();
        drop(guard);
        let _ = app.emit("clipboard://changed", &ordered);
        Ok(ordered)
    })
    .await
    .map_err(|_| "剪贴板操作未完成，请重试".to_string())?
}

#[tauri::command]
async fn toggle_pin_clipboard(
    app: AppHandle,
    state: State<'_, AppState>,
    id: String,
) -> Result<Vec<ClipboardItem>, String> {
    change_clipboard(app, Arc::clone(&state.clipboard), move |clipboard| {
        let item = clipboard
            .items
            .iter_mut()
            .find(|item| item.id == id)
            .ok_or_else(|| "未找到该剪贴板条目".to_string())?;
        item.pinned = !item.pinned;
        Ok(())
    })
    .await
}

#[tauri::command]
async fn delete_clipboard_item(
    app: AppHandle,
    state: State<'_, AppState>,
    id: String,
) -> Result<Vec<ClipboardItem>, String> {
    change_clipboard(app, Arc::clone(&state.clipboard), move |clipboard| {
        if !clipboard.items.iter().any(|item| item.id == id) {
            return Err("未找到该剪贴板条目".to_string());
        }
        clipboard.items.retain(|item| item.id != id);
        Ok(())
    })
    .await
}

#[tauri::command]
async fn clear_clipboard(
    app: AppHandle,
    state: State<'_, AppState>,
    keep_pinned: bool,
) -> Result<Vec<ClipboardItem>, String> {
    change_clipboard(app, Arc::clone(&state.clipboard), move |clipboard| {
        if keep_pinned {
            clipboard.items.retain(|item| item.pinned);
        } else {
            clipboard.items.clear();
        }
        Ok(())
    })
    .await
}

#[tauri::command]
fn get_clipboard_enabled(state: State<'_, AppState>) -> bool {
    state.clipboard.lock().unwrap().enabled
}

#[tauri::command]
fn clipboard_persistence_status(state: State<'_, AppState>) -> Option<String> {
    state
        .clipboard
        .lock()
        .ok()
        .and_then(|guard| guard.persistence_error().map(str::to_string))
}

#[tauri::command]
async fn set_clipboard_enabled(
    app: AppHandle,
    state: State<'_, AppState>,
    enabled: bool,
) -> Result<bool, String> {
    let clipboard = Arc::clone(&state.clipboard);
    tauri::async_runtime::spawn_blocking(move || {
        settings::patch_to_disk(
            serde_json::json!({ "clipboardEnabled": enabled }),
            |saved| publish_settings(&app, &clipboard, saved),
        )
        .map(|saved| saved.clipboard_enabled)
    })
    .await
    .map_err(|_| "监听设置未保存，请重试".to_string())?
}

#[tauri::command]
fn set_english_mode(app: AppHandle, english: bool) -> bool {
    settings::broadcast_english_to_tsf(english);
    let _ = app.emit("ime://english-changed", english);
    english
}

fn apply_window_appearance(app: &AppHandle, settings: &AppSettings) {
    #[cfg(target_os = "windows")]
    for window in app.webview_windows().values() {
        let theme = match settings.theme_mode.as_str() {
            "dark" => Some(tauri::Theme::Dark),
            "light" => Some(tauri::Theme::Light),
            _ => None,
        };
        let _ = window.set_theme(theme);
        if let Ok(hwnd) = window.hwnd() {
            apply_native_chrome(hwnd.0 as windows_sys::Win32::Foundation::HWND, settings);
            if window.label() == "statusbar" {
                use windows_sys::Win32::UI::WindowsAndMessaging::{
                    GetWindowLongPtrW, SetWindowLongPtrW, GWL_EXSTYLE, WS_EX_NOACTIVATE,
                };
                let handle = hwnd.0 as windows_sys::Win32::Foundation::HWND;
                unsafe {
                    let style = GetWindowLongPtrW(handle, GWL_EXSTYLE);
                    SetWindowLongPtrW(handle, GWL_EXSTYLE, style | WS_EX_NOACTIVATE as isize);
                }
            }
        }
    }
    #[cfg(not(target_os = "windows"))]
    let _ = (app, settings);
}

#[cfg(target_os = "windows")]
fn apply_native_chrome(hwnd: windows_sys::Win32::Foundation::HWND, settings: &AppSettings) {
    use windows_sys::Win32::Graphics::Dwm::DwmSetWindowAttribute;
    // These Windows 11 attributes safely return E_INVALIDARG on older Windows.
    // They affect only FangCun's own HWNDs; editor windows are never touched.
    const DWMWA_WINDOW_CORNER_PREFERENCE: u32 = 33;
    const DWMWCP_DONOTROUND: i32 = 1;
    let (background, text): (u32, u32) = match (settings.skin.as_str(), settings.dark_mode) {
        ("typewriter", false) => (0xEBDDCA, 0x272019),
        ("typewriter", true) => (0x302923, 0xF6E8D1),
        ("terminal", false) => (0xD7CDBA, 0x252722),
        ("terminal", true) => (0x34382F, 0xF0ECDB),
        ("brutal", false) => (0xEFEAE0, 0x121212),
        ("brutal", true) => (0x0F1412, 0xE6F4EC),
        (_, true) => (0x3C2D24, 0xF5E3CC),
        _ => (0xE8D6BA, 0x251B14),
    };
    let color_ref = |rgb: u32| ((rgb & 0xff) << 16) | (rgb & 0xff00) | ((rgb >> 16) & 0xff);
    let caption = color_ref(background);
    let foreground = color_ref(text);
    unsafe {
        let _ = DwmSetWindowAttribute(
            hwnd,
            DWMWA_WINDOW_CORNER_PREFERENCE,
            (&DWMWCP_DONOTROUND as *const i32).cast(),
            4,
        );
        let _ = DwmSetWindowAttribute(hwnd, 35, (&caption as *const u32).cast(), 4);
        let _ = DwmSetWindowAttribute(hwnd, 36, (&foreground as *const u32).cast(), 4);
    }
}

fn show_main_window(app: &AppHandle) {
    if let Some(win) = app.get_webview_window("main") {
        let _ = win.show();
        let _ = win.unminimize();
        let _ = win.set_focus();
    }
}

static LAST_CLIPBOARD_SHOW_MS: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);

fn now_epoch_ms() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

fn toggle_clipboard_window(app: &AppHandle) {
    if let Some(win) = app.get_webview_window("clipboard") {
        let visible = win.is_visible().unwrap_or(false);
        let since_show = now_epoch_ms()
            .saturating_sub(LAST_CLIPBOARD_SHOW_MS.load(std::sync::atomic::Ordering::Relaxed));
        if visible && since_show > 350 {
            let _ = win.hide();
        } else {
            LAST_CLIPBOARD_SHOW_MS.store(now_epoch_ms(), std::sync::atomic::Ordering::Relaxed);
            let _ = win.show();
            let _ = win.unminimize();
            let _ = win.set_focus();
            #[cfg(target_os = "windows")]
            if let Ok(hwnd) = win.hwnd() {
                unsafe {
                    windows_sys::Win32::UI::WindowsAndMessaging::SetForegroundWindow(
                        hwnd.0 as windows_sys::Win32::Foundation::HWND,
                    );
                }
            }
            let _ = app.emit("ui://clipboard-opened", ());
        }
    }
}

fn position_statusbar_near_taskbar(app: &AppHandle) {
    if let Some(win) = app.get_webview_window("statusbar") {
        if let Ok(Some(monitor)) = win.primary_monitor() {
            let work = monitor.work_area();
            let scale = monitor.scale_factor();
            let bar_w = (218.0 * scale).round() as i32;
            let bar_h = (32.0 * scale).round() as i32;
            let margin_x = (18.0 * scale).round() as i32;
            let margin_y = (12.0 * scale).round() as i32;
            let x = work.position.x + (work.size.width as i32) - bar_w - margin_x;
            let y = work.position.y + (work.size.height as i32) - bar_h - margin_y;
            let _ = win.set_position(tauri::Position::Physical(tauri::PhysicalPosition::new(
                x, y,
            )));
        }
    }
}

#[cfg(target_os = "windows")]
fn start_hotkey_listener(app: AppHandle) {
    std::thread::spawn(move || {
        use windows_sys::Win32::UI::Input::KeyboardAndMouse::{
            RegisterHotKey, UnregisterHotKey, MOD_ALT, MOD_NOREPEAT,
        };
        use windows_sys::Win32::UI::WindowsAndMessaging::{
            CreateWindowExW, DestroyWindow, DispatchMessageW, GetMessageW, RegisterWindowMessageW,
            TranslateMessage, HWND_MESSAGE, MSG, WM_HOTKEY,
        };

        const HOTKEY_CLIPBOARD_ID: i32 = 1;
        unsafe {
            let class_name = settings::to_wide_null("Static");
            let wnd_title = settings::to_wide_null("FangCunCompanionMsgWindow");
            let msg_hwnd = CreateWindowExW(
                0,
                class_name.as_ptr(),
                wnd_title.as_ptr(),
                0,
                0,
                0,
                0,
                0,
                HWND_MESSAGE,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                std::ptr::null(),
            );
            let notify_msg_name = settings::to_wide_null("FangCunIME_CompanionNotify");
            let notify_msg = RegisterWindowMessageW(notify_msg_name.as_ptr());

            let hotkey_ok = RegisterHotKey(
                std::ptr::null_mut(),
                HOTKEY_CLIPBOARD_ID,
                MOD_ALT | MOD_NOREPEAT,
                'V' as u32,
            ) != 0;

            let mut msg: MSG = std::mem::zeroed();
            while GetMessageW(&mut msg, std::ptr::null_mut(), 0, 0) > 0 {
                if msg.message == WM_HOTKEY && msg.wParam == HOTKEY_CLIPBOARD_ID as usize {
                    toggle_clipboard_window(&app);
                } else if notify_msg != 0 && msg.message == notify_msg {
                    match msg.wParam {
                        1 => {
                            let english = msg.lParam != 0;
                            let _ = app.emit("ime://english-changed", english);
                        }
                        2 => {
                            let clipboard = Arc::clone(&app.state::<AppState>().clipboard);
                            let app = app.clone();
                            tauri::async_runtime::spawn_blocking(move || {
                                let _ = settings::publish_current(|fresh| {
                                    publish_settings(&app, &clipboard, fresh)
                                });
                            });
                        }
                        4 => {
                            toggle_clipboard_window(&app);
                        }
                        _ => {}
                    }
                } else {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
            if hotkey_ok {
                UnregisterHotKey(std::ptr::null_mut(), HOTKEY_CLIPBOARD_ID);
            }
            if !msg_hwnd.is_null() {
                DestroyWindow(msg_hwnd);
            }
        }
    });
}

#[cfg(not(target_os = "windows"))]
fn start_hotkey_listener(_app: AppHandle) {}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    let clipboard_state = Arc::new(Mutex::new(ClipboardState::load()));
    let watcher_state = Arc::clone(&clipboard_state);
    let share = ShareManager::load();
    let share_watcher = Arc::clone(&share);
    let start_in_tray = std::env::args().any(|a| a == "--tray");
    let start_in_clipboard = std::env::args().any(|a| a == "--clipboard");

    tauri::Builder::default()
        .manage(Arc::new(ai::AiManager::new(settings::data_dir())))
        .manage(AppState {
            clipboard: clipboard_state,
            share,
        })
        .setup(move |app| {
            apply_window_appearance(app.handle(), &settings::load_from_disk());
            clipboard::start_watcher(
                app.handle().clone(),
                watcher_state,
                Arc::clone(&share_watcher),
            );
            share_watcher.resume(app.handle());
            start_hotkey_listener(app.handle().clone());

            if start_in_clipboard {
                toggle_clipboard_window(app.handle());
            } else if !start_in_tray {
                show_main_window(app.handle());
            }

            let open_item = MenuItem::with_id(app, "open", "打开方寸设置", true, None::<&str>)?;
            let clip_item =
                MenuItem::with_id(app, "clipboard", "打开剪贴板 (Alt+V)", true, None::<&str>)?;
            let status_item =
                MenuItem::with_id(app, "statusbar", "显示或隐藏状态栏", true, None::<&str>)?;
            let quit_item = MenuItem::with_id(app, "quit", "退出方寸伴侣", true, None::<&str>)?;
            let menu = Menu::with_items(app, &[&open_item, &clip_item, &status_item, &quit_item])?;

            let mut tray_builder = TrayIconBuilder::new()
                .menu(&menu)
                .tooltip("方寸输入法 · 设置与剪贴板 (Alt+V)")
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
            WindowEvent::ThemeChanged(_) => {
                let window = window.clone();
                tauri::async_runtime::spawn_blocking(move || {
                    let _ = settings::publish_current(|saved| {
                        #[cfg(target_os = "windows")]
                        if let Ok(hwnd) = window.hwnd() {
                            apply_native_chrome(
                                hwnd.0 as windows_sys::Win32::Foundation::HWND,
                                saved,
                            );
                        }
                        let _ = window.emit("settings://changed", saved);
                    });
                });
            }
            WindowEvent::CloseRequested { api, .. } => {
                cancel_ai_window(window.app_handle(), window.label());
                api.prevent_close();
                let _ = window.hide();
            }
            WindowEvent::Destroyed => {
                cancel_ai_window(window.app_handle(), window.label());
            }
            WindowEvent::Focused(false) if window.label() == "clipboard" => {
                let since_show = now_epoch_ms().saturating_sub(
                    LAST_CLIPBOARD_SHOW_MS.load(std::sync::atomic::Ordering::Relaxed),
                );
                if since_show > 450 {
                    let _ = window.hide();
                }
            }
            _ => {}
        })
        .invoke_handler(tauri::generate_handler![
            ai_commands::ai_get_config,
            ai_commands::ai_save_config,
            ai_commands::ai_generate,
            ai_commands::ai_transcribe,
            ai_commands::ai_cancel,
            ai_commands::ai_copy_result,
            load_settings,
            save_settings,
            patch_settings,
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
            clipboard_persistence_status,
            set_clipboard_enabled,
            set_english_mode,
            clipboard_share_snapshot,
            clipboard_share_save,
            clipboard_share_enabled,
            clipboard_share_auto_send,
            clipboard_share_generate_key,
            clipboard_share_import,
            clipboard_share_export,
            clipboard_share_send_current,
            clipboard_share_send_item,
            clipboard_share_copy,
            clipboard_share_remove,
            clipboard_share_reset,
        ])
        .run(tauri::generate_context!())
        .expect("error while running FangCunSettings");
}
