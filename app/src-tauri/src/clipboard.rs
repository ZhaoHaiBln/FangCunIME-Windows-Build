use crate::settings::data_dir;
use arboard::Clipboard;
use serde::{Deserialize, Serialize};
use std::fs;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::time::{Duration, SystemTime, UNIX_EPOCH};
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
        Self {
            items,
            enabled: true,
            last_seen_text,
        }
    }

    pub fn persist(&self) {
        let path = clipboard_path();
        if let Some(parent) = path.parent() {
            let _ = fs::create_dir_all(parent);
        }
        if let Ok(json) = serde_json::to_string_pretty(&self.items) {
            let _ = fs::write(&path, json);
        }
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

pub fn start_watcher(app: AppHandle, state: Arc<Mutex<ClipboardState>>) {
    std::thread::spawn(move || {
        // Seed initial clipboard text without emitting duplicates if already persisted.
        let mut clipboard = Clipboard::new().ok();
        if let Some(cb) = clipboard.as_mut() {
            if let Ok(initial) = cb.get_text() {
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
            let enabled = { state.lock().unwrap().enabled };
            if !enabled {
                continue;
            }
            if clipboard.is_none() {
                clipboard = Clipboard::new().ok();
            }
            let Some(cb) = clipboard.as_mut() else {
                continue;
            };
            if let Ok(text) = cb.get_text() {
                let mut guard = state.lock().unwrap();
                if guard.record_text(text, "windows") {
                    let ordered = guard.ordered_items();
                    drop(guard);
                    let _ = app.emit("clipboard://changed", ordered);
                }
            }
        }
    });
}
