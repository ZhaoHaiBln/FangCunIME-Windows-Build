use crate::settings::data_dir;
use crate::sync_manager::ShareManager;
use crate::sync_windows::{self, ClipboardSnapshot};
#[cfg(not(target_os = "windows"))]
use arboard::Clipboard;
use serde::{Deserialize, Serialize};
use std::fs;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
use tauri::{AppHandle, Emitter};

const MAX_UNPINNED_ITEMS: usize = 200;

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ClipboardItem {
    pub id: String,
    pub text: String,
    pub pinned: bool,
    pub created_at_ms: u64,
    pub source: String,
}

#[derive(Debug, Default)]
pub struct ClipboardState {
    pub items: Vec<ClipboardItem>,
    pub enabled: bool,
    pub last_seen_text: String,
}

impl ClipboardState {
    pub fn load() -> Self {
        let mut items = Vec::new();
        let path = clipboard_path();
        if let Ok(raw) = fs::read_to_string(&path) {
            if let Ok(parsed) = serde_json::from_str::<Vec<ClipboardItem>>(&raw) {
                items = parsed;
            }
        }
        let last_seen_text = items.first().map(|i| i.text.clone()).unwrap_or_default();
        let enabled = crate::settings::load_from_disk().clipboard_enabled;
        Self {
            items,
            enabled,
            last_seen_text,
        }
    }

    pub fn persist(&self) {
        let path = clipboard_path();
        let snapshot = self.items.clone();
        std::thread::spawn(move || {
            if let Some(parent) = path.parent() {
                let _ = fs::create_dir_all(parent);
            }
            if let Ok(json) = serde_json::to_string_pretty(&snapshot) {
                let _ = fs::write(&path, json);
            }
        });
    }

    pub fn ordered_items(&self) -> Vec<ClipboardItem> {
        let mut out = self.items.clone();
        out.sort_by(|a, b| {
            b.pinned
                .cmp(&a.pinned)
                .then_with(|| b.created_at_ms.cmp(&a.created_at_ms))
        });
        out
    }

    pub fn record_text(&mut self, text: String, source: &str) -> bool {
        if text.trim().is_empty() || text == self.last_seen_text {
            return false;
        }
        self.last_seen_text = text.clone();

        // Deduplicate existing unpinned or pinned item by moving it to top while preserving pin status.
        let mut was_pinned = false;
        if let Some(idx) = self.items.iter().position(|item| item.text == text) {
            was_pinned = self.items[idx].pinned;
            self.items.remove(idx);
        }

        let now = now_ms();
        self.items.insert(
            0,
            ClipboardItem {
                id: format!("cb-{now}-{}", self.items.len()),
                text,
                pinned: was_pinned,
                created_at_ms: now,
                source: source.to_string(),
            },
        );

        let mut unpinned_count = 0usize;
        self.items.retain(|item| {
            if item.pinned {
                true
            } else {
                unpinned_count += 1;
                unpinned_count <= MAX_UNPINNED_ITEMS
            }
        });

        self.persist();
        true
    }
}

fn clipboard_path() -> PathBuf {
    data_dir().join("clipboard.json")
}

fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

fn read_local_clipboard() -> Result<ClipboardSnapshot, &'static str> {
    #[cfg(target_os = "windows")]
    {
        sync_windows::read_snapshot()
    }
    #[cfg(not(target_os = "windows"))]
    {
        let text = Clipboard::new()
            .and_then(|mut cb| cb.get_text())
            .map_err(|_| "无法读取系统剪贴板")?;
        Ok(ClipboardSnapshot {
            sequence: 0,
            text: if sync_windows::is_pairing_material(&text) {
                None
            } else {
                Some(text)
            },
        })
    }
}

pub fn start_watcher(app: AppHandle, state: Arc<Mutex<ClipboardState>>, share: Arc<ShareManager>) {
    std::thread::spawn(move || {
        let mut last_sequence = sync_windows::sequence();
        // Startup seeding stays local. Loading history or starting the app never uploads it.
        let initially_enabled = state.lock().map(|guard| guard.enabled).unwrap_or(false);
        if let Ok(snapshot) = if initially_enabled {
            read_local_clipboard()
        } else {
            Err("监听已暂停")
        } {
            last_sequence = snapshot.sequence;
            if let Some(initial) = snapshot.text {
                let mut guard = state.lock().unwrap();
                if guard.enabled && !initial.trim().is_empty() && guard.items.is_empty() {
                    if guard.record_text(initial, "windows") {
                        let _ = app.emit("clipboard://changed", guard.ordered_items());
                    }
                } else if !initial.is_empty() {
                    guard.last_seen_text = initial;
                }
            }
        }
        loop {
            std::thread::sleep(Duration::from_millis(500));
            let observed_at = Instant::now();
            let enabled = { state.lock().unwrap().enabled };
            if !enabled {
                // Copies made while paused are not backfilled when monitoring resumes.
                last_sequence = sync_windows::sequence();
                continue;
            }
            #[cfg(target_os = "windows")]
            if sync_windows::sequence() == last_sequence {
                continue;
            }
            let Ok(snapshot) = read_local_clipboard() else {
                continue;
            };
            #[cfg(target_os = "windows")]
            if snapshot.sequence == last_sequence {
                continue;
            }
            last_sequence = snapshot.sequence;
            let Some(text) = snapshot.text else {
                continue;
            };
            let ordered = {
                let mut guard = state.lock().unwrap();
                if guard.record_text(text.clone(), "windows") {
                    Some(guard.ordered_items())
                } else {
                    None
                }
            };
            if let Some(ordered) = ordered {
                let _ = app.emit("clipboard://changed", ordered);
            }
            // A new OS copy is independent of list refresh/dedup/pin/delete. Session start time
            // plus the activation sequence rejects startup/enable races and old clipboard text.
            #[cfg(target_os = "windows")]
            share.local_copy(&app, text, snapshot.sequence, observed_at);
            #[cfg(not(target_os = "windows"))]
            let _ = (&share, observed_at, last_sequence);
        }
    });
}
