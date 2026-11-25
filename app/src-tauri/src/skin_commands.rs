//! The only renderer-facing skin mutations. Passive resources use a separate,
//! declared-PNG-only custom protocol; package contents never become code.
use crate::settings::AppSettings;
use crate::skin_library::{SkinLibrary, SkinSnapshot};
use crate::AppState;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use tauri::{Emitter, Manager, State, WebviewWindow};

static PICKER_OPEN: AtomicBool = AtomicBool::new(false);
struct PickerGuard;
impl PickerGuard {
    fn begin() -> Result<Self, String> {
        PICKER_OPEN
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .map(|_| Self)
            .map_err(|_| "皮肤文件选择器已打开".into())
    }
}
impl Drop for PickerGuard {
    fn drop(&mut self) {
        PICKER_OPEN.store(false, Ordering::Release);
    }
}

fn require_main(window: &WebviewWindow) -> Result<(), String> {
    if window.label() != "main" {
        return Err("请在方寸主设置窗口管理皮肤".into());
    }
    Ok(())
}

async fn change(
    window: WebviewWindow,
    library: Arc<SkinLibrary>,
    clipboard: Arc<Mutex<crate::clipboard::ClipboardState>>,
    operation: impl FnOnce(&SkinLibrary, &dyn Fn(&AppSettings)) -> Result<SkinSnapshot, String>
        + Send
        + 'static,
) -> Result<SkinSnapshot, String> {
    require_main(&window)?;
    let app = window.app_handle().clone();
    tauri::async_runtime::spawn_blocking(move || {
        let publish = |saved: &AppSettings| crate::publish_settings(&app, &clipboard, saved);
        let snapshot = operation(&library, &publish)?;
        crate::settings::notify_tsf_settings_changed();
        let _ = app.emit("skin-library://changed", &snapshot.revision);
        Ok(snapshot)
    })
    .await
    .map_err(|_| "皮肤操作未完成，请重新读取后重试".to_string())?
}

#[tauri::command]
pub async fn skin_library_snapshot(
    app: tauri::AppHandle,
    library: State<'_, Arc<SkinLibrary>>,
) -> Result<SkinSnapshot, String> {
    let library = Arc::clone(library.inner());
    tauri::async_runtime::spawn_blocking(move || {
        let snapshot = library.snapshot()?;
        crate::refresh_skin_chrome(&app);
        Ok(snapshot)
    })
    .await
    .map_err(|_| "无法读取皮肤库，请重试".to_string())?
}
#[tauri::command]
pub async fn skin_set_import_enabled(
    window: WebviewWindow,
    library: State<'_, Arc<SkinLibrary>>,
    state: State<'_, AppState>,
    enabled: bool,
    revision: String,
) -> Result<SkinSnapshot, String> {
    change(
        window,
        Arc::clone(library.inner()),
        Arc::clone(&state.clipboard),
        move |library, publish| library.set_import_enabled(enabled, &revision, publish),
    )
    .await
}
#[tauri::command]
pub async fn skin_set_builtin_hidden(
    window: WebviewWindow,
    library: State<'_, Arc<SkinLibrary>>,
    state: State<'_, AppState>,
    id: String,
    hidden: bool,
    revision: String,
) -> Result<SkinSnapshot, String> {
    change(
        window,
        Arc::clone(library.inner()),
        Arc::clone(&state.clipboard),
        move |library, publish| library.set_builtin_hidden(&id, hidden, &revision, publish),
    )
    .await
}
#[tauri::command]
pub async fn skin_select(
    window: WebviewWindow,
    library: State<'_, Arc<SkinLibrary>>,
    state: State<'_, AppState>,
    id: String,
    revision: String,
) -> Result<SkinSnapshot, String> {
    change(
        window,
        Arc::clone(library.inner()),
        Arc::clone(&state.clipboard),
        move |library, publish| library.select(&id, &revision, publish),
    )
    .await
}
#[tauri::command]
pub async fn skin_delete(
    window: WebviewWindow,
    library: State<'_, Arc<SkinLibrary>>,
    state: State<'_, AppState>,
    id: String,
    revision: String,
) -> Result<SkinSnapshot, String> {
    change(
        window,
        Arc::clone(library.inner()),
        Arc::clone(&state.clipboard),
        move |library, publish| library.delete(&id, &revision, publish),
    )
    .await
}

#[tauri::command]
pub async fn skin_import_pick(
    window: WebviewWindow,
    library: State<'_, Arc<SkinLibrary>>,
    state: State<'_, AppState>,
    revision: String,
) -> Result<Option<SkinSnapshot>, String> {
    require_main(&window)?;
    let _picking = PickerGuard::begin()?;
    let check_library = Arc::clone(library.inner());
    let check_revision = revision.clone();
    tauri::async_runtime::spawn_blocking(move || check_library.import_allowed(&check_revision))
        .await
        .map_err(|_| "无法读取导入设置".to_string())??;
    let picked = pick_on_main(&window).await?;
    let Some(path) = picked else {
        return Ok(None);
    };
    change(
        window,
        Arc::clone(library.inner()),
        Arc::clone(&state.clipboard),
        move |library, publish| library.import_path(&path, &revision, publish),
    )
    .await
    .map(Some)
}

#[cfg(not(target_os = "windows"))]
async fn pick_on_main(_: &WebviewWindow) -> Result<Option<PathBuf>, String> {
    Err("本机文件选择目前仅支持 Windows".into())
}

#[cfg(target_os = "windows")]
async fn pick_on_main(window: &WebviewWindow) -> Result<Option<PathBuf>, String> {
    let owner = window.hwnd().map_err(|_| "主窗口不可用")?.0 as usize;
    let (send, receive) = tokio::sync::oneshot::channel();
    window
        .app_handle()
        .run_on_main_thread(move || {
            let _ = send.send(pick_file(owner));
        })
        .map_err(|_| "无法打开皮肤文件选择器")?;
    receive
        .await
        .map_err(|_| "皮肤文件选择未完成".to_string())?
}

#[cfg(target_os = "windows")]
fn pick_file(owner: usize) -> Result<Option<PathBuf>, String> {
    use std::os::windows::ffi::OsStringExt;
    use windows_sys::Win32::UI::Controls::Dialogs::{
        CommDlgExtendedError, GetOpenFileNameW, OFN_DONTADDTORECENT, OFN_EXPLORER,
        OFN_FILEMUSTEXIST, OFN_HIDEREADONLY, OFN_NOCHANGEDIR, OFN_PATHMUSTEXIST, OPENFILENAMEW,
    };
    let mut file = vec![0u16; 32_768];
    let filter: Vec<u16> = "方寸皮肤包 (*.fcskin)\0*.fcskin\0\0"
        .encode_utf16()
        .collect();
    let title: Vec<u16> = "导入方寸皮肤".encode_utf16().chain(Some(0)).collect();
    let extension: Vec<u16> = "fcskin".encode_utf16().chain(Some(0)).collect();
    let mut options: OPENFILENAMEW = unsafe { std::mem::zeroed() };
    options.lStructSize = std::mem::size_of::<OPENFILENAMEW>() as u32;
    options.hwndOwner = owner as _;
    options.lpstrFilter = filter.as_ptr();
    options.lpstrTitle = title.as_ptr();
    options.lpstrDefExt = extension.as_ptr();
    options.lpstrFile = file.as_mut_ptr();
    options.nMaxFile = file.len() as u32;
    options.Flags = OFN_EXPLORER
        | OFN_FILEMUSTEXIST
        | OFN_PATHMUSTEXIST
        | OFN_NOCHANGEDIR
        | OFN_HIDEREADONLY
        | OFN_DONTADDTORECENT;
    if unsafe { GetOpenFileNameW(&mut options) } == 0 {
        return if unsafe { CommDlgExtendedError() } == 0 {
            Ok(None)
        } else {
            Err("无法打开选中的皮肤包，请重试".into())
        };
    }
    let end = file
        .iter()
        .position(|value| *value == 0)
        .ok_or("选择的文件路径无效")?;
    if end == 0 {
        return Err("选择的文件路径无效".into());
    }
    Ok(Some(std::ffi::OsString::from_wide(&file[..end]).into()))
}
