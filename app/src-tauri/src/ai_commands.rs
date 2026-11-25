//! Tauri adapter: every job and progress event belongs to the invoking window.
use crate::ai::{
    AiDraft, AiManager, AiSnapshot, GenerateRequest, ProgressSink, TranscribeRequest,
    MAX_OUTPUT_BYTES,
};
use std::sync::Arc;
use tauri::{Emitter, State, WebviewWindow};

#[tauri::command]
pub async fn ai_get_config(manager: State<'_, Arc<AiManager>>) -> Result<AiSnapshot, String> {
    let manager = Arc::clone(manager.inner());
    tauri::async_runtime::spawn_blocking(move || manager.get_config())
        .await
        .map_err(|_| "读取 AI 设置未完成，请重试".to_string())?
}

#[tauri::command]
pub async fn ai_save_config(
    manager: State<'_, Arc<AiManager>>,
    draft: AiDraft,
    revision: String,
) -> Result<AiSnapshot, String> {
    let manager = Arc::clone(manager.inner());
    tauri::async_runtime::spawn_blocking(move || manager.save_config(draft, &revision))
        .await
        .map_err(|_| "保存 AI 设置未完成，请重试".to_string())?
}

#[tauri::command]
pub async fn ai_generate(
    window: WebviewWindow,
    manager: State<'_, Arc<AiManager>>,
    request: GenerateRequest,
) -> Result<String, String> {
    let owner = window.label().to_owned();
    let sink: ProgressSink =
        Arc::new(move |progress| window.emit("ai://progress", progress).is_ok());
    Arc::clone(manager.inner())
        .generate(owner, request, sink)
        .await
}

#[tauri::command]
pub async fn ai_transcribe(
    window: WebviewWindow,
    manager: State<'_, Arc<AiManager>>,
    request: TranscribeRequest,
) -> Result<String, String> {
    let owner = window.label().to_owned();
    let sink: ProgressSink =
        Arc::new(move |progress| window.emit("ai://progress", progress).is_ok());
    Arc::clone(manager.inner())
        .transcribe(owner, request, sink)
        .await
}

#[tauri::command]
pub fn ai_cancel(window: WebviewWindow, manager: State<'_, Arc<AiManager>>, id: String) {
    manager.cancel(window.label(), &id);
}

#[tauri::command]
pub async fn ai_copy_result(text: String) -> Result<(), String> {
    if text.trim().is_empty() || text.len() > MAX_OUTPUT_BYTES {
        return Err("没有可复制的结果，或内容过长".into());
    }
    // A deliberate Copy writes the real OS clipboard. Exclusion markers keep
    // generated private text out of automatic clipboard sharing and history.
    tauri::async_runtime::spawn_blocking(move || {
        crate::sync_windows::write_excluded(&text).map_err(|_| "复制失败，请稍后重试".to_string())
    })
    .await
    .map_err(|_| "复制未完成，请重试".to_string())?
}
