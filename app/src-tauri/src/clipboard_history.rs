//! Local history is acknowledged only after one atomic replacement completes.
//! Callers hold ClipboardState's mutex, so earlier snapshots cannot finish later.
use serde::{de::DeserializeOwned, Serialize};
use std::fs::{self, File};
use std::io::Read;
use std::path::Path;

const MAX_HISTORY_BYTES: usize = 64 * 1024 * 1024;
pub const READ_ERROR: &str = "无法读取剪贴板历史，已保留原文件";
pub const WRITE_ERROR: &str = "剪贴板历史保存失败，原文件已保留";

pub fn load<T: DeserializeOwned>(path: &Path) -> Result<Vec<T>, &'static str> {
    let file = match File::open(path) {
        Ok(file) => file,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(Vec::new()),
        Err(_) => return Err(READ_ERROR),
    };
    let metadata = file.metadata().map_err(|_| READ_ERROR)?;
    if !metadata.is_file() || metadata.len() > MAX_HISTORY_BYTES as u64 {
        return Err(READ_ERROR);
    }
    let mut bytes = Vec::new();
    file.take(MAX_HISTORY_BYTES as u64 + 1).read_to_end(&mut bytes).map_err(|_| READ_ERROR)?;
    if bytes.len() > MAX_HISTORY_BYTES { return Err(READ_ERROR); }
    serde_json::from_slice(&bytes).map_err(|_| READ_ERROR)
}

pub fn save<T: Serialize>(path: &Path, items: &[T]) -> Result<(), &'static str> {
    save_limited(path, items, MAX_HISTORY_BYTES)
}

fn save_limited<T: Serialize>(path: &Path, items: &[T], maximum: usize) -> Result<(), &'static str> {
    let body = serde_json::to_string_pretty(items).map_err(|_| WRITE_ERROR)?;
    if body.len() > maximum { return Err("剪贴板历史过大，未覆盖原文件，请清理后重试"); }
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|_| WRITE_ERROR)?;
    }
    crate::settings_io::replace_limited(path, &body, maximum).map_err(|_| WRITE_ERROR)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde::{Deserialize, Serialize};
    use std::path::PathBuf;
    use std::sync::{Arc, Barrier, atomic::{AtomicU64, AtomicBool, AtomicUsize, Ordering}};

    static NEXT: AtomicU64 = AtomicU64::new(0);

    struct Fixture(PathBuf);
    impl Fixture {
        fn new() -> Self {
            let root = std::env::temp_dir().join(format!("fangcun-history-{}-{}", std::process::id(), NEXT.fetch_add(1, Ordering::Relaxed)));
            fs::create_dir_all(&root).unwrap();
            Self(root)
        }
        fn file(&self) -> PathBuf { self.0.join("clipboard.json") }
    }
    impl Drop for Fixture { fn drop(&mut self) { let _ = fs::remove_dir_all(&self.0); } }

    #[derive(Debug, PartialEq, Serialize, Deserialize)]
    struct Item { text: String, pinned: bool }
    fn item(text: &str, pinned: bool) -> Item { Item { text: text.into(), pinned } }

    #[test]
    fn latest_snapshot_is_durable_before_returning_and_preserves_unicode() {
        let f = Fixture::new();
        save(&f.file(), &[item("旧内容", false)]).unwrap();
        save(&f.file(), &[item("新的置顶内容🙂", true)]).unwrap();
        assert_eq!(load::<Item>(&f.file()).unwrap(), vec![item("新的置顶内容🙂", true)]);
        save::<Item>(&f.file(), &[]).unwrap();
        assert!(load::<Item>(&f.file()).unwrap().is_empty());
        assert_eq!(fs::read_dir(&f.0).unwrap().count(), 1, "No detached writer or temporary history remains");
    }

    #[test]
    fn readers_never_observe_truncated_json_during_replacement() {
        let f = Fixture::new();
        save(&f.file(), &[item("初始", false)]).unwrap();
        let file = f.file();
        let start = Arc::new(Barrier::new(2));
        let done = Arc::new(AtomicBool::new(false));
        let reads = Arc::new(AtomicUsize::new(0));
        let (first_read, ready) = std::sync::mpsc::sync_channel(1);
        let handle = {
            let (start, done, reads) = (start.clone(), done.clone(), reads.clone());
            std::thread::spawn(move || {
                start.wait();
                while !done.load(Ordering::Acquire) {
                    let snapshot = load::<Item>(&file).expect("Each observed file must be a complete snapshot");
                    assert_eq!(snapshot.len(), 1);
                    reads.fetch_add(1, Ordering::Relaxed);
                    let _ = first_read.try_send(());
                }
            })
        };
        start.wait();
        ready.recv_timeout(std::time::Duration::from_secs(2)).unwrap();
        for n in 0..32 { save(&f.file(), &[item(&format!("{n}:{}", "新".repeat(2048)), n % 2 == 0)]).unwrap(); }
        done.store(true, Ordering::Release);
        handle.join().unwrap();
        assert!(reads.load(Ordering::Relaxed) > 0);
        assert!(load::<Item>(&f.file()).unwrap()[0].text.starts_with("31:"));
    }

    #[test]
    fn rejected_snapshot_leaves_original_bytes_and_no_temporary_files() {
        let f = Fixture::new();
        save(&f.file(), &[item("保留原文", true)]).unwrap();
        let original = fs::read(f.file()).unwrap();
        assert!(save_limited(&f.file(), &[item(&"大".repeat(100), false)], 32).is_err());
        assert_eq!(fs::read(f.file()).unwrap(), original);
        assert_eq!(fs::read_dir(&f.0).unwrap().count(), 1);
        let occupied = f.0.join("occupied"); fs::create_dir(&occupied).unwrap();
        assert!(save(&occupied, &[item("不会覆盖目录", false)]).is_err());
        assert!(occupied.is_dir());
        assert_eq!(fs::read(f.file()).unwrap(), original);
    }

    #[test]
    fn malformed_or_unreadable_history_is_not_treated_as_an_empty_new_file() {
        let f = Fixture::new();
        assert!(load::<Item>(&f.file()).unwrap().is_empty());
        fs::write(f.file(), b"[{\"text\":").unwrap();
        let original = fs::read(f.file()).unwrap();
        assert_eq!(load::<Item>(&f.file()).unwrap_err(), READ_ERROR);
        assert_eq!(fs::read(f.file()).unwrap(), original);
        assert_eq!(load::<Item>(&f.0).unwrap_err(), READ_ERROR);
    }
}
