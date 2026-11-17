//! Optional clipboard-sharing lifecycle. Network tasks never run in the TSF DLL or hold a UI lock.
use crate::settings::data_dir;
use crate::sync_protocol::{
    self, Endpoint, Frame, Pairing, Payload, ReceiveOutcome, ReceiveState, SharedKey,
};
use crate::sync_transport::{
    CancelToken, NtfyClient, NtfyError, NtfyMessage, MAX_POLL_BYTES, MAX_POLL_MESSAGES,
};
use crate::sync_windows;
use fs2::FileExt;
use serde::{Deserialize, Serialize};
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
use tauri::{AppHandle, Emitter};
use tokio::sync::mpsc;
use zeroize::Zeroize;

// A legal 200-item inbox can expand sixfold when JSON escapes control characters. Seen-ID and
// pending-owner maps are independently bounded by the protocol; this is a local encrypted-file
// ceiling, not a larger HTTP/event/recovery budget.
const MAX_STORE_BYTES: usize = 64 * 1024 * 1024;
const MAX_PROTECTED_BYTES: usize = MAX_STORE_BYTES + 65_536;
const MAX_SHARED_ITEMS: usize = 200;

#[derive(Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SharedItem {
    pub id: String,
    pub sender_id: String,
    pub sender_name: String,
    pub created_at: i64,
    pub received_at: i64,
    pub expires_at: i64,
    pub text: String,
}

#[derive(Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct Stored {
    v: u32,
    server: String,
    topic: String,
    sender_id: String,
    sender_name: String,
    enabled: bool,
    auto_send: bool,
    key: String,
    token: String,
    receive: ReceiveState,
    shared: Vec<SharedItem>,
    gap: bool,
}
impl Stored {
    fn fresh() -> Result<Self, String> {
        Ok(Self {
            v: 1,
            server: sync_protocol::DEFAULT_SERVER.to_string(),
            topic: sync_protocol::DEFAULT_TOPIC.to_string(),
            sender_id: sync_protocol::new_id().map_err(safe_protocol)?,
            sender_name: "我的电脑".to_string(),
            enabled: false,
            auto_send: false,
            key: String::new(),
            token: String::new(),
            receive: ReceiveState::default(),
            shared: Vec::new(),
            gap: false,
        })
    }
    fn validate(&mut self, now: i64) -> Result<(), String> {
        if self.v != 1 || self.shared.len() > MAX_SHARED_ITEMS {
            return Err("共享状态版本或大小无效".into());
        }
        let endpoint = Endpoint::new(&self.server, &self.topic).map_err(safe_protocol)?;
        self.server = endpoint.server;
        self.topic = endpoint.topic;
        Payload::new(&self.sender_id, &self.sender_name, "校验", now).map_err(safe_protocol)?;
        validate_token(&self.token, false)?;
        if !self.key.is_empty() {
            SharedKey::parse(&self.key).map_err(safe_protocol)?;
        }
        self.receive.validate(now).map_err(safe_protocol)?;
        self.receive.prune(now);
        for item in &self.shared {
            if item.id.len() > 80
                || item.text.is_empty()
                || item.text.len() > sync_protocol::MAX_TEXT_BYTES
                || item.sender_name.is_empty()
                || item.sender_name.chars().count() > 64
                || item.sender_name.len() > 256
            {
                return Err("共享历史格式无效".into());
            }
        }
        self.shared.retain(|item| item.expires_at > now);
        Ok(())
    }
    fn ready(&self) -> Result<SharedKey, String> {
        validate_token(&self.token, true)?;
        SharedKey::parse(&self.key).map_err(safe_protocol)
    }
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ShareSnapshot {
    pub revision: String,
    pub view_revision: String,
    pub server: String,
    pub topic: String,
    pub sender_name: String,
    pub enabled: bool,
    pub auto_send: bool,
    pub token_saved: bool,
    pub key_saved: bool,
    pub key_id: String,
    pub status: String,
    pub error: Option<String>,
    pub gap: bool,
    pub needs_reset: bool,
    pub items: Vec<SharedItem>,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ShareDraft {
    pub server: String,
    pub topic: String,
    pub sender_name: String,
    /// Empty/None preserves the saved secret. Changing server never forwards an old token.
    pub token: Option<String>,
    pub key: Option<String>,
}

struct Outbound {
    frames: Vec<Frame>,
}
struct Session {
    generation: u64,
    cancel: CancelToken,
    send: mpsc::Sender<Outbound>,
    started_at: Instant,
    baseline_sequence: u32,
}
struct Inner {
    stored: Stored,
    generation: u64,
    session: Option<Session>,
    status: String,
    error: Option<String>,
    load_failed: bool,
    view_revision: u64,
}

pub struct ShareManager {
    path: PathBuf,
    // Keep an exclusive per-user file lock for the process lifetime. Other companion instances
    // may still serve local UI, but cannot race the durable cursor or secret store.
    owner: Option<File>,
    inner: Mutex<Inner>,
}

impl ShareManager {
    pub fn load() -> Arc<Self> {
        let directory = data_dir();
        let path = directory.join("clipboard-share-v1.bin");
        let owner = fs::create_dir_all(&directory)
            .ok()
            .and_then(|_| {
                OpenOptions::new()
                    .read(true)
                    .write(true)
                    .create(true)
                    .truncate(false)
                    .open(directory.join("clipboard-share-v1.lock"))
                    .ok()
            })
            .and_then(|file| file.try_lock_exclusive().ok().map(|_| file));
        let mut error = None;
        let mut load_failed = false;
        let stored = match read_store(&path) {
            Ok(Some(stored)) => stored,
            Ok(None) => Stored::fresh().unwrap_or_else(|message| {
                error = Some(message);
                load_failed = true;
                // No random/crypto fallback: this value cannot pass ready()/validate().
                Stored {
                    v: 1,
                    server: sync_protocol::DEFAULT_SERVER.into(),
                    topic: sync_protocol::DEFAULT_TOPIC.into(),
                    sender_id: String::new(),
                    sender_name: "我的电脑".into(),
                    enabled: false,
                    auto_send: false,
                    key: String::new(),
                    token: String::new(),
                    receive: ReceiveState::default(),
                    shared: Vec::new(),
                    gap: false,
                }
            }),
            Err(message) => {
                error = Some(message);
                load_failed = true;
                Stored::fresh().unwrap_or_else(|_| Stored {
                    v: 1,
                    server: sync_protocol::DEFAULT_SERVER.into(),
                    topic: sync_protocol::DEFAULT_TOPIC.into(),
                    sender_id: String::new(),
                    sender_name: "我的电脑".into(),
                    enabled: false,
                    auto_send: false,
                    key: String::new(),
                    token: String::new(),
                    receive: ReceiveState::default(),
                    shared: Vec::new(),
                    gap: false,
                })
            }
        };
        if owner.is_none() {
            error = Some("另一个方寸窗口正在管理共享，或共享目录不可写".into());
        }
        let mut stored = stored;
        if load_failed || owner.is_none() {
            stored.enabled = false;
        }
        Arc::new(Self {
            path,
            owner,
            inner: Mutex::new(Inner {
                stored,
                generation: 0,
                session: None,
                status: "共享已关闭".into(),
                error,
                load_failed,
                view_revision: 0,
            }),
        })
    }

    pub fn resume(self: &Arc<Self>, app: &AppHandle) {
        let enabled = self
            .inner
            .lock()
            .map(|g| g.stored.enabled && !g.load_failed)
            .unwrap_or(false);
        if enabled {
            let _ = self.set_enabled(app, true);
        }
    }

    fn writable(&self, inner: &Inner) -> Result<(), String> {
        if self.owner.is_none() {
            return Err("共享由另一个方寸窗口管理，或目录不可写".into());
        }
        if inner.load_failed {
            return Err("本机共享资料无法读取，请先重置共享配置".into());
        }
        Ok(())
    }

    pub fn snapshot(&self) -> Result<ShareSnapshot, String> {
        let mut guard = self.inner.lock().map_err(|_| "共享状态不可用")?;
        guard.view_revision = guard.view_revision.wrapping_add(1);
        Ok(snapshot(&guard))
    }

    fn emit(&self, app: &AppHandle) {
        if let Ok(value) = self.snapshot() {
            let _ = app.emit("clipboard-share://changed", value);
        }
    }

    pub fn save(
        self: &Arc<Self>,
        app: &AppHandle,
        draft: ShareDraft,
    ) -> Result<ShareSnapshot, String> {
        let mut guard = self.inner.lock().map_err(|_| "共享状态不可用")?;
        self.writable(&guard)?;
        let next = apply_draft(&guard.stored, draft)?;
        stop(&mut guard);
        if let Err(message) = write_store(&self.path, &next) {
            guard.stored.enabled = false;
            guard.error = Some(message.clone());
            guard.status = "共享已停止，配置未能保存".into();
            return Err(message);
        }
        guard.stored = next;
        guard.error = None;
        guard.status = "已保存，开启共享后连接".into();
        drop(guard);
        self.emit(app);
        self.snapshot()
    }

    pub fn import_pairing(
        self: &Arc<Self>,
        app: &AppHandle,
        value: &str,
    ) -> Result<ShareSnapshot, String> {
        let pairing = Pairing::parse(value.trim()).map_err(safe_protocol)?;
        let current = self.snapshot()?;
        self.save(
            app,
            ShareDraft {
                server: pairing.endpoint.server,
                topic: pairing.endpoint.topic,
                sender_name: current.sender_name,
                token: None,
                key: Some(pairing.key.export()),
            },
        )
    }

    pub fn generate_key(self: &Arc<Self>, app: &AppHandle) -> Result<ShareSnapshot, String> {
        let key = SharedKey::generate().map_err(safe_protocol)?;
        let current = self.snapshot()?;
        self.save(
            app,
            ShareDraft {
                server: current.server,
                topic: current.topic,
                sender_name: current.sender_name,
                token: None,
                key: Some(key.export()),
            },
        )
    }

    pub fn pairing_text(&self) -> Result<String, String> {
        let guard = self.inner.lock().map_err(|_| "共享状态不可用")?;
        self.writable(&guard)?;
        Pairing {
            endpoint: Endpoint::new(&guard.stored.server, &guard.stored.topic)
                .map_err(safe_protocol)?,
            key: SharedKey::parse(&guard.stored.key).map_err(safe_protocol)?,
        }
        .export()
        .map_err(safe_protocol)
    }

    pub fn set_enabled(
        self: &Arc<Self>,
        app: &AppHandle,
        enabled: bool,
    ) -> Result<ShareSnapshot, String> {
        let mut guard = self.inner.lock().map_err(|_| "共享状态不可用")?;
        self.writable(&guard)?;
        stop(&mut guard);
        let mut next = guard.stored.clone();
        next.enabled = enabled;
        let prepared = if enabled {
            let key = next.ready()?;
            let client = NtfyClient::new(&next.server, &next.topic, &next.token)
                .map_err(|e| e.safe_message().to_string())?;
            Some((key, client))
        } else {
            None
        };
        if let Err(message) = write_store(&self.path, &next) {
            guard.stored.enabled = false;
            guard.status = "共享已停止，开关未能保存".into();
            guard.error = Some(message.clone());
            return Err(message);
        }
        guard.stored = next;
        guard.error = None;
        guard.status = if enabled {
            "正在连接共享服务…"
        } else {
            "共享已关闭"
        }
        .into();
        if let Some((key, client)) = prepared {
            let generation = guard.generation;
            let cancel = CancelToken::new();
            let (sender, receiver) = mpsc::channel(16);
            guard.session = Some(Session {
                generation,
                cancel: cancel.clone(),
                send: sender,
                started_at: Instant::now(),
                baseline_sequence: sync_windows::sequence(),
            });
            let receiver_owner = Arc::clone(self);
            let sender_owner = Arc::clone(self);
            let recv_app = app.clone();
            let send_app = app.clone();
            let recv_client = client.clone();
            let recv_key = key.clone();
            let recv_cancel = cancel.clone();
            tauri::async_runtime::spawn(async move {
                receiver_owner
                    .receive_loop(recv_app, generation, recv_key, recv_client, recv_cancel)
                    .await;
            });
            tauri::async_runtime::spawn(async move {
                sender_owner
                    .send_loop(send_app, generation, client, cancel, receiver)
                    .await;
            });
        }
        drop(guard);
        self.emit(app);
        self.snapshot()
    }

    pub fn set_auto_send(
        self: &Arc<Self>,
        app: &AppHandle,
        enabled: bool,
    ) -> Result<ShareSnapshot, String> {
        let was_enabled;
        {
            let mut guard = self.inner.lock().map_err(|_| "共享状态不可用")?;
            self.writable(&guard)?;
            let mut next = guard.stored.clone();
            next.auto_send = enabled;
            was_enabled = next.enabled;
            stop(&mut guard);
            if let Err(message) = write_store(&self.path, &next) {
                guard.stored.enabled = false;
                guard.error = Some(message.clone());
                return Err(message);
            }
            guard.stored = next;
        }
        if was_enabled {
            self.set_enabled(app, true)
        } else {
            self.emit(app);
            self.snapshot()
        }
    }

    pub fn reset(self: &Arc<Self>, app: &AppHandle) -> Result<ShareSnapshot, String> {
        if self.owner.is_none() {
            return Err("共享由另一个方寸窗口管理".into());
        }
        let mut guard = self.inner.lock().map_err(|_| "共享状态不可用")?;
        stop(&mut guard);
        guard.stored.enabled = false;
        let next = Stored::fresh()?;
        if self.path.exists() {
            fs::copy(&self.path, self.path.with_extension("previous.bin"))
                .map_err(|_| "旧共享资料未能保留，已停止重置")?;
        }
        write_store(&self.path, &next)?;
        guard.stored = next;
        guard.load_failed = false;
        guard.error = None;
        guard.status = "共享配置已重置，本机剪贴板历史保留".into();
        drop(guard);
        self.emit(app);
        self.snapshot()
    }

    pub fn remove_item(&self, app: &AppHandle, id: &str) -> Result<ShareSnapshot, String> {
        let mut guard = self.inner.lock().map_err(|_| "共享状态不可用")?;
        self.writable(&guard)?;
        let mut next = guard.stored.clone();
        next.shared.retain(|item| item.id != id);
        write_store(&self.path, &next)?;
        guard.stored = next;
        drop(guard);
        self.emit(app);
        self.snapshot()
    }

    pub fn shared_text(&self, id: &str) -> Result<String, String> {
        let guard = self.inner.lock().map_err(|_| "共享状态不可用")?;
        guard
            .stored
            .shared
            .iter()
            .find(|item| item.id == id && item.expires_at > now_ms())
            .map(|item| item.text.clone())
            .ok_or_else(|| "共享条目不存在或已过期".into())
    }

    /// Called only for a newly observed OS copy, never from list refresh or startup seeding.
    pub fn local_copy(
        self: &Arc<Self>,
        app: &AppHandle,
        text: String,
        sequence: u32,
        observed_at: Instant,
    ) {
        let generation = self.inner.lock().ok().and_then(|guard| {
            guard
                .session
                .as_ref()
                .filter(|s| {
                    guard.stored.enabled
                        && guard.stored.auto_send
                        && !s.cancel.is_cancelled()
                        && copy_is_new(s.started_at, s.baseline_sequence, observed_at, sequence)
                })
                .map(|s| s.generation)
        });
        if let Some(generation) = generation {
            if let Err(message) = self.queue_text(app, text, generation) {
                self.status(app, generation, "本次复制未自动发送", Some(&message));
            }
        }
    }

    pub fn send_text(&self, app: &AppHandle, text: String, revision: &str) -> Result<(), String> {
        let expected = revision
            .parse::<u64>()
            .map_err(|_| "共享配置已变化，请刷新后重试")?;
        self.queue_text(app, text, expected)
    }

    fn queue_text(
        &self,
        app: &AppHandle,
        text: String,
        expected_generation: u64,
    ) -> Result<(), String> {
        let (generation, sender, frames) = self.prepare_outbound(text, expected_generation)?;
        sender
            .try_send(Outbound { frames })
            .map_err(|_| "发送队列已满或共享已停止，请稍后重试".to_string())?;
        self.status(app, generation, "已加入发送队列", None);
        Ok(())
    }

    fn prepare_outbound(
        &self,
        text: String,
        expected_generation: u64,
    ) -> Result<(u64, mpsc::Sender<Outbound>, Vec<Frame>), String> {
        if sync_windows::is_pairing_material(&text) {
            return Err("配对资料包含共享密钥，不能当普通文字发送".into());
        }
        let (generation, sender, key, payload) = {
            let guard = self.inner.lock().map_err(|_| "共享状态不可用")?;
            self.writable(&guard)?;
            if guard.generation != expected_generation {
                return Err("共享配置已变化，请重新发送".into());
            }
            let session = guard
                .session
                .as_ref()
                .filter(|s| !s.cancel.is_cancelled() && guard.stored.enabled)
                .ok_or("请先开启共享")?;
            let key = guard.stored.ready()?;
            let payload = Payload::new(
                &guard.stored.sender_id,
                &guard.stored.sender_name,
                &text,
                now_ms(),
            )
            .map_err(safe_protocol)?;
            (session.generation, session.send.clone(), key, payload)
        };
        let frames =
            sync_protocol::encrypt_payload(&payload, &key, now_ms()).map_err(safe_protocol)?;
        if !self.current(generation) {
            return Err("共享配置已变化，请重新发送".into());
        }
        Ok((generation, sender, frames))
    }

    fn current(&self, generation: u64) -> bool {
        self.inner
            .lock()
            .map(|guard| {
                guard.generation == generation
                    && guard.stored.enabled
                    && guard
                        .session
                        .as_ref()
                        .is_some_and(|s| !s.cancel.is_cancelled())
            })
            .unwrap_or(false)
    }

    fn status(&self, app: &AppHandle, generation: u64, status: &str, error: Option<&str>) {
        if let Ok(mut guard) = self.inner.lock() {
            if guard.generation != generation || !guard.stored.enabled {
                return;
            }
            guard.status = status.to_string();
            guard.error = error.map(str::to_string);
        }
        self.emit(app);
    }

    fn fatal(&self, app: &AppHandle, generation: u64, message: &str, gap: bool) {
        if let Ok(mut guard) = self.inner.lock() {
            if guard.generation != generation {
                return;
            }
            stop(&mut guard);
            guard.stored.enabled = false;
            guard.stored.gap |= gap;
            guard.status = "共享已暂停".into();
            guard.error = Some(message.to_string());
            let _ = write_store(&self.path, &guard.stored);
        }
        self.emit(app);
    }

    fn cursor(&self, generation: u64) -> Option<String> {
        self.inner
            .lock()
            .ok()
            .filter(|guard| guard.generation == generation)
            .and_then(|guard| guard.stored.receive.cursor.clone())
    }

    fn accept(
        &self,
        app: &AppHandle,
        generation: u64,
        key: &SharedKey,
        event: NtfyMessage,
        signal_recovery: bool,
    ) -> Result<(), NtfyError> {
        let mut guard = self.inner.lock().map_err(|_| NtfyError::CallbackFailed)?;
        if guard.generation != generation || !guard.stored.enabled {
            return Err(NtfyError::Cancelled);
        }
        let mut next = guard.stored.clone();
        let now = now_ms();
        let outcome = next
            .receive
            .process(&event.id, &event.message, key, &next.sender_id, now);
        next.shared.retain(|item| item.expires_at > now);
        let delivered = if let ReceiveOutcome::Delivered(message) = outcome {
            let id = format!("{}:{}", message.kid, message.mid);
            next.shared.retain(|item| item.id != id);
            next.shared.insert(
                0,
                SharedItem {
                    id,
                    sender_id: message.payload.sender_id,
                    sender_name: message.payload.sender_name,
                    created_at: message.payload.created_at,
                    expires_at: message.payload.expires_at,
                    text: message.payload.text,
                    received_at: now,
                },
            );
            next.shared.truncate(MAX_SHARED_ITEMS);
            true
        } else {
            false
        };
        // Durably save pending parts + seen IDs + cursor + delivered history in one DPAPI-protected
        // atomic replacement. Never show a message or advance live state after a failed write.
        write_store(&self.path, &next).map_err(|_| NtfyError::CallbackFailed)?;
        let pending_recovery = next.receive.pending_recovery();
        guard.stored = next;
        if delivered {
            guard.status = "已收到共享内容".into();
            guard.error = None;
        }
        drop(guard);
        if delivered {
            self.emit(app);
        }
        if signal_recovery && pending_recovery {
            return Err(NtfyError::CursorExpired);
        }
        Ok(())
    }

    fn recovery_marker(&self, generation: u64) -> Option<u64> {
        self.inner
            .lock()
            .ok()
            .filter(|guard| guard.generation == generation)
            .and_then(|guard| guard.stored.receive.recovery_marker())
    }

    fn acknowledge_recovery(&self, generation: u64, marker: u64) -> Result<bool, NtfyError> {
        let mut guard = self.inner.lock().map_err(|_| NtfyError::CallbackFailed)?;
        if guard.generation != generation || !guard.stored.enabled {
            return Err(NtfyError::Cancelled);
        }
        let mut next = guard.stored.clone();
        next.receive.acknowledge_recovery(marker);
        let complete = !next.receive.pending_recovery();
        write_store(&self.path, &next).map_err(|_| NtfyError::CallbackFailed)?;
        guard.stored = next;
        Ok(complete)
    }

    async fn receive_loop(
        self: Arc<Self>,
        app: AppHandle,
        generation: u64,
        key: SharedKey,
        client: NtfyClient,
        cancel: CancelToken,
    ) {
        let mut failures = 0u32;
        let mut recovered_once = false;
        // Capture before any network handshake. With an empty durable cursor, a message copied
        // between the finite poll and SSE establishment must still be replayed from this boundary.
        // This anchor is only a request parameter; it is never persisted as an ntfy event cursor.
        let startup_since = (now_ms().max(0) / 1000).to_string();
        while self.current(generation) && !cancel.is_cancelled() {
            let mut remaining_messages = MAX_POLL_MESSAGES;
            let mut remaining_bytes = MAX_POLL_BYTES;
            let cursor = self.cursor(generation);
            let mut recovery_marker = self.recovery_marker(generation);
            let mut recovery_day = recovery_marker.is_some();
            if recovery_day && recovered_once {
                self.fatal(
                    &app,
                    generation,
                    "同步缺口：仍有未恢复分片，请稍后重新连接",
                    true,
                );
                return;
            }
            if recovery_day {
                recovered_once = true;
            }
            let polled = 'polling: loop {
                let batch = client
                    .poll_with_budget(
                        cursor.as_deref(),
                        recovery_day,
                        &cancel,
                        remaining_messages,
                        remaining_bytes,
                    )
                    .await;
                match batch {
                    Ok(batch) => {
                        remaining_messages =
                            remaining_messages.saturating_sub(batch.messages.len());
                        remaining_bytes = remaining_bytes.saturating_sub(batch.raw_bytes);
                        for message in batch.messages {
                            if let Err(error) = self.accept(&app, generation, &key, message, false)
                            {
                                break 'polling Err(error);
                            }
                        }
                        let current_marker = self.recovery_marker(generation);
                        if batch.truncated || (!recovery_day && current_marker.is_some()) {
                            if recovered_once || remaining_messages == 0 || remaining_bytes == 0 {
                                break Err(NtfyError::Truncated);
                            }
                            recovered_once = true;
                            recovery_day = true;
                            recovery_marker = current_marker;
                            continue;
                        }
                        if recovery_day {
                            if let Some(marker) = recovery_marker {
                                match self.acknowledge_recovery(generation, marker) {
                                    Ok(true) => {}
                                    Ok(false) => break Err(NtfyError::Truncated),
                                    Err(error) => break Err(error),
                                }
                            } else if current_marker.is_some() {
                                break Err(NtfyError::Truncated);
                            }
                        }
                        break Ok(());
                    }
                    Err(NtfyError::CursorExpired | NtfyError::Truncated)
                        if !recovered_once && !recovery_day =>
                    {
                        recovered_once = true;
                        recovery_day = true;
                        recovery_marker = self.recovery_marker(generation);
                    }
                    Err(error) => break Err(error),
                }
            };
            let outcome = match polled {
                Ok(()) => {
                    self.status(&app, generation, "共享已连接，等待内容", None);
                    failures = 0;
                    let cursor = self.cursor(generation);
                    let owner = Arc::clone(&self);
                    let callback_app = app.clone();
                    let callback_key = key.clone();
                    client
                        .stream(
                            cursor.as_deref().or(Some(startup_since.as_str())),
                            &cancel,
                            move |event| {
                                owner.accept(&callback_app, generation, &callback_key, event, true)
                            },
                        )
                        .await
                }
                Err(error) => Err(error),
            };
            let error = outcome.err().unwrap_or(NtfyError::Network);
            match error {
                NtfyError::Cancelled => return,
                NtfyError::Auth | NtfyError::Configuration | NtfyError::CallbackFailed => {
                    self.fatal(&app, generation, error.safe_message(), false);
                    return;
                }
                NtfyError::Truncated => {
                    self.fatal(
                        &app,
                        generation,
                        "同步缺口：缓存超过恢复上限，已暂停共享，请检查后重新连接",
                        true,
                    );
                    return;
                }
                NtfyError::CursorExpired if recovered_once => {
                    self.fatal(
                        &app,
                        generation,
                        "同步缺口：服务缓存无法恢复，已暂停共享",
                        true,
                    );
                    return;
                }
                NtfyError::CursorExpired => continue,
                _ => {
                    failures = failures.saturating_add(1);
                    self.status(
                        &app,
                        generation,
                        "连接中断，稍后重连",
                        Some(error.safe_message()),
                    );
                    let delay = error.retry_after().unwrap_or_else(|| backoff(failures));
                    if sleep_or_cancel(delay, &cancel).await {
                        return;
                    }
                }
            }
        }
    }

    async fn send_loop(
        self: Arc<Self>,
        app: AppHandle,
        generation: u64,
        client: NtfyClient,
        cancel: CancelToken,
        mut receiver: mpsc::Receiver<Outbound>,
    ) {
        loop {
            let outgoing = tokio::select! {
                _ = cancel.cancelled() => return,
                item = receiver.recv() => match item { Some(value) => value, None => return },
            };
            let mut delivered = false;
            for attempt in 0..3u32 {
                if !self.current(generation) || cancel.is_cancelled() {
                    return;
                }
                let mut result = Ok(());
                for frame in &outgoing.frames {
                    if !self.current(generation) {
                        return;
                    }
                    if let Err(error) = client.publish(frame, &cancel).await {
                        result = Err(error);
                        break;
                    }
                }
                match result {
                    Ok(()) => {
                        delivered = true;
                        break;
                    }
                    Err(NtfyError::Cancelled) => return,
                    Err(error @ (NtfyError::Auth | NtfyError::Configuration)) => {
                        self.fatal(&app, generation, error.safe_message(), false);
                        return;
                    }
                    Err(error) => {
                        self.status(&app, generation, "正在重试发送", Some(error.safe_message()));
                        if attempt < 2
                            && sleep_or_cancel(
                                error.retry_after().unwrap_or_else(|| backoff(attempt + 1)),
                                &cancel,
                            )
                            .await
                        {
                            return;
                        }
                    }
                }
            }
            self.status(
                &app,
                generation,
                if delivered {
                    "已发送到共享服务"
                } else {
                    "发送未完成，请手动重试"
                },
                if delivered {
                    None
                } else {
                    Some("部分分片可能尚未送达，接收端不会显示未完成内容")
                },
            );
        }
    }
}

fn apply_draft(current: &Stored, draft: ShareDraft) -> Result<Stored, String> {
    let endpoint = Endpoint::new(draft.server.trim(), draft.topic.trim()).map_err(safe_protocol)?;
    let name = draft.sender_name.trim();
    if name.is_empty()
        || name.chars().count() > 64
        || name.len() > 256
        || name.chars().any(char::is_control)
    {
        return Err("设备名称须为1至64个字符".into());
    }
    let mut next = current.clone();
    let old_scope = (next.server.clone(), next.topic.clone(), next.key.clone());
    let server_changed = next.server != endpoint.server;
    next.server = endpoint.server;
    next.topic = endpoint.topic;
    next.sender_name = name.to_string();
    if let Some(token) = draft.token.filter(|value| !value.trim().is_empty()) {
        validate_token(token.trim(), true)?;
        next.token = token.trim().to_string();
    } else if server_changed {
        next.token.clear();
    }
    if let Some(key) = draft.key.filter(|value| !value.trim().is_empty()) {
        next.key = SharedKey::parse(key.trim())
            .map_err(safe_protocol)?
            .export();
    }
    if old_scope != (next.server.clone(), next.topic.clone(), next.key.clone()) {
        next.receive = ReceiveState::default();
        next.gap = false;
    }
    next.enabled = false;
    Ok(next)
}

fn stop(inner: &mut Inner) {
    inner.generation = inner.generation.wrapping_add(1);
    if let Some(session) = inner.session.take() {
        session.cancel.cancel();
    }
}

fn snapshot(inner: &Inner) -> ShareSnapshot {
    ShareSnapshot {
        revision: inner.generation.to_string(),
        view_revision: inner.view_revision.to_string(),
        server: inner.stored.server.clone(),
        topic: inner.stored.topic.clone(),
        sender_name: inner.stored.sender_name.clone(),
        enabled: inner.stored.enabled && inner.session.is_some(),
        auto_send: inner.stored.auto_send,
        token_saved: !inner.stored.token.is_empty(),
        key_saved: !inner.stored.key.is_empty(),
        key_id: SharedKey::parse(&inner.stored.key)
            .map(|key| key.kid())
            .unwrap_or_default(),
        status: inner.status.clone(),
        error: inner.error.clone(),
        gap: inner.stored.gap,
        needs_reset: inner.load_failed,
        items: inner
            .stored
            .shared
            .iter()
            .filter(|item| item.expires_at > now_ms())
            .cloned()
            .collect(),
    }
}

fn validate_token(value: &str, required: bool) -> Result<(), String> {
    if (required && value.is_empty())
        || value.len() > 8192
        || !value.bytes().all(|c| (33..=126).contains(&c))
    {
        return Err("请填写有效的本机中转服务 Token".into());
    }
    Ok(())
}
fn copy_is_new(started: Instant, baseline: u32, observed: Instant, sequence: u32) -> bool {
    observed >= started && sequence != baseline
}
fn safe_protocol(error: sync_protocol::ProtocolError) -> String {
    error.0.to_string()
}
fn now_ms() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|v| v.as_millis().min(i64::MAX as u128) as i64)
        .unwrap_or(0)
}
fn backoff(failures: u32) -> Duration {
    let seconds = 2u64.saturating_pow(failures.min(6)).min(60);
    Duration::from_millis(seconds * 1000 + u64::from(rand::random::<u16>()) % 1000)
}
async fn sleep_or_cancel(duration: Duration, cancel: &CancelToken) -> bool {
    tokio::select! { _ = cancel.cancelled() => true, _ = tokio::time::sleep(duration) => false }
}
fn read_store(path: &Path) -> Result<Option<Stored>, String> {
    let file = match File::open(path) {
        Ok(file) => file,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(_) => return Err("无法读取本机共享配置".into()),
    };
    let mut bytes = Vec::new();
    file.take(MAX_PROTECTED_BYTES as u64 + 1)
        .read_to_end(&mut bytes)
        .map_err(|_| "无法读取本机共享配置")?;
    if bytes.len() > MAX_PROTECTED_BYTES {
        return Err("本机共享配置超过大小限制".into());
    }
    let mut plain = sync_windows::unprotect(&bytes).map_err(str::to_string)?;
    let parsed = serde_json::from_slice::<Stored>(&plain);
    plain.zeroize();
    let mut stored = parsed.map_err(|_| "本机共享配置格式无效")?;
    stored.validate(now_ms())?;
    Ok(Some(stored))
}
fn write_store(path: &Path, stored: &Stored) -> Result<(), String> {
    let mut candidate = stored.clone();
    candidate.validate(now_ms())?;
    let mut plain = serde_json::to_vec(&candidate).map_err(|_| "无法编码本机共享状态")?;
    if plain.len() > MAX_STORE_BYTES {
        plain.zeroize();
        return Err("本机共享状态超过大小限制".into());
    }
    let protected = sync_windows::protect(&plain).map_err(str::to_string);
    plain.zeroize();
    let bytes = protected?;
    if bytes.len() > MAX_PROTECTED_BYTES {
        return Err("本机共享保护数据超过大小限制".into());
    }
    let parent = path.parent().ok_or("共享目录无效")?;
    fs::create_dir_all(parent).map_err(|_| "无法创建共享目录")?;
    let tmp = path.with_extension(format!(
        "tmp-{}",
        sync_protocol::new_id().map_err(safe_protocol)?
    ));
    let result = (|| {
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&tmp)
            .map_err(|_| "无法保存本机共享配置")?;
        file.write_all(&bytes).map_err(|_| "无法保存本机共享配置")?;
        file.sync_all().map_err(|_| "无法可靠保存本机共享配置")?;
        drop(file);
        fs::rename(&tmp, path).map_err(|_| "无法替换本机共享配置")?;
        Ok::<(), String>(())
    })();
    if result.is_err() {
        let _ = fs::remove_file(tmp);
    }
    result
}

#[cfg(test)]
mod tests {
    use super::*;

    fn draft(server: &str) -> ShareDraft {
        ShareDraft {
            server: server.into(),
            topic: "fangcun-clipboard".into(),
            sender_name: "我的电脑".into(),
            token: None,
            key: None,
        }
    }

    #[test]
    fn canonical_same_origin_keeps_token_but_new_origin_never_reuses_it() {
        let mut original = Stored::fresh().unwrap();
        original.token = "device-private-token".into();
        original.enabled = true;
        let same = apply_draft(&original, draft("https://CLIP.KIVOHUB.SITE:443/")).unwrap();
        assert_eq!(same.token, original.token);
        assert!(!same.enabled);
        let changed = apply_draft(&original, draft("https://another.example")).unwrap();
        assert!(changed.token.is_empty());
        assert!(!changed.enabled);
        let mut replacement = draft("https://another.example");
        replacement.token = Some("new-origin-token".into());
        assert_eq!(
            apply_draft(&original, replacement).unwrap().token,
            "new-origin-token"
        );
    }

    #[test]
    fn importing_a_different_pairing_resets_receive_scope_and_preserves_local_shared_history() {
        let mut original = Stored::fresh().unwrap();
        original.token = "old-origin-token".into();
        original.receive.cursor = Some("old-event-id".into());
        original.enabled = true;
        original.shared.push(SharedItem {
            id: "saved-item".into(),
            sender_id: sync_protocol::new_id().unwrap(),
            sender_name: "手机".into(),
            created_at: now_ms(),
            received_at: now_ms(),
            expires_at: now_ms() + 86_400_000,
            text: "原来的共享历史".into(),
        });
        let pairing = Pairing {
            endpoint: Endpoint::new("https://new.example", "private-topic").unwrap(),
            key: SharedKey::generate().unwrap(),
        };
        let decoded = Pairing::parse(&pairing.export().unwrap()).unwrap();
        let mut input = draft(&decoded.endpoint.server);
        input.topic = decoded.endpoint.topic;
        input.key = Some(decoded.key.export());
        let next = apply_draft(&original, input).unwrap();
        assert!(next.token.is_empty());
        assert!(!next.enabled);
        assert!(next.receive.cursor.is_none());
        assert_eq!(next.shared.len(), 1);
        assert_eq!(next.shared[0].text, "原来的共享历史");
        assert_eq!(next.key, pairing.key.export());
    }

    #[test]
    fn auto_send_excludes_pre_activation_and_current_baseline_even_when_sequence_wraps() {
        let start = Instant::now();
        assert!(!copy_is_new(
            start,
            10,
            start - Duration::from_millis(1),
            11
        ));
        assert!(!copy_is_new(
            start,
            10,
            start + Duration::from_millis(1),
            10
        ));
        assert!(copy_is_new(start, 10, start + Duration::from_millis(1), 11));
        assert!(copy_is_new(
            start,
            u32::MAX,
            start + Duration::from_millis(1),
            0
        ));
    }

    #[test]
    fn stale_auto_or_manual_actions_cannot_queue_against_a_new_destination() {
        let path = std::env::temp_dir().join(format!(
            "fangcun-share-generation-{}",
            sync_protocol::new_id().unwrap()
        ));
        let file = OpenOptions::new()
            .create_new(true)
            .read(true)
            .write(true)
            .open(&path)
            .unwrap();
        let mut stored = Stored::fresh().unwrap();
        stored.enabled = true;
        stored.token = "new-token".into();
        stored.key = SharedKey::generate().unwrap().export();
        let (send, mut receiver) = mpsc::channel(16);
        let manager = ShareManager {
            path: path.clone(),
            owner: Some(file),
            inner: Mutex::new(Inner {
                stored,
                generation: 8,
                session: Some(Session {
                    generation: 8,
                    cancel: CancelToken::new(),
                    send,
                    started_at: Instant::now(),
                    baseline_sequence: 1,
                }),
                status: String::new(),
                error: None,
                load_failed: false,
                view_revision: 0,
            }),
        };
        let error = manager
            .prepare_outbound("旧会话复制".into(), 7)
            .err()
            .expect("stale action must fail");
        assert_eq!(error, "共享配置已变化，请重新发送");
        assert!(receiver.try_recv().is_err());
        drop(manager);
        fs::remove_file(path).unwrap();
    }
}
