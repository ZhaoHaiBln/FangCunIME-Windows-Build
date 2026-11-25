//! Shared settings-file transactions. Readers see an entire old or new file;
//! writers in the companion and TSF coordinate on the same sidecar byte lock.
use fs2::FileExt;
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{Duration, Instant};

const MAX_SETTINGS_BYTES: u64 = 64 * 1024;
static NEXT_TEMP: AtomicU64 = AtomicU64::new(0);

pub struct SettingsLock(File);
impl Drop for SettingsLock {
    fn drop(&mut self) {
        let _ = FileExt::unlock(&self.0);
    }
}

pub fn lock(path: &Path) -> Result<SettingsLock, String> {
    let parent = path.parent().ok_or("设置目录不可用")?;
    fs::create_dir_all(parent).map_err(|_| "无法创建设置目录".to_string())?;
    let mut name = path.as_os_str().to_os_string();
    name.push(".lock");
    let file = OpenOptions::new()
        .create(true)
        .truncate(false)
        .read(true)
        .write(true)
        .open(PathBuf::from(name))
        .map_err(|_| "无法锁定设置文件".to_string())?;
    let start = Instant::now();
    loop {
        match FileExt::try_lock_exclusive(&file) {
            Ok(()) => return Ok(SettingsLock(file)),
            Err(error)
                if (error.kind() == std::io::ErrorKind::WouldBlock
                    || matches!(error.raw_os_error(), Some(32 | 33)))
                    && start.elapsed() < Duration::from_millis(250) =>
            {
                std::thread::sleep(Duration::from_millis(5));
            }
            Err(_) => return Err("设置正在由其他窗口保存，请重试".to_string()),
        }
    }
}

pub fn read(path: &Path) -> Result<Option<String>, String> {
    let mut file = match open_read(path) {
        Ok(file) => file,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(_) => return Err("无法读取设置，请检查文件权限后重试".to_string()),
    };
    let metadata = file
        .metadata()
        .map_err(|_| "无法读取设置文件信息".to_string())?;
    if !metadata.is_file() || metadata.len() > MAX_SETTINGS_BYTES {
        return Err("设置文件无效，已保留原文件".to_string());
    }
    let mut bytes = Vec::new();
    Read::by_ref(&mut file)
        .take(MAX_SETTINGS_BYTES + 1)
        .read_to_end(&mut bytes)
        .map_err(|_| "读取设置失败，已保留原文件".to_string())?;
    if bytes.len() as u64 > MAX_SETTINGS_BYTES {
        return Err("设置文件过大，已保留原文件".to_string());
    }
    let text =
        String::from_utf8(bytes).map_err(|_| "设置文件编码无效，已保留原文件".to_string())?;
    if text.contains('\0') {
        return Err("设置文件包含无效字符，已保留原文件".to_string());
    }
    Ok(Some(text))
}

/// Open an immutable file snapshot. A transient Windows sharing restriction is
/// an I/O error, never permission to treat an existing settings/history file as
/// an empty new installation. These Rust callers run outside the TSF key path.
pub fn open_read(path: &Path) -> std::io::Result<File> {
    retry_sharing("read", || File::open(path))
}

#[cfg(not(target_os = "windows"))]
fn retry_sharing<T>(
    _: &str,
    mut operation: impl FnMut() -> std::io::Result<T>,
) -> std::io::Result<T> {
    operation()
}

#[cfg(target_os = "windows")]
fn retry_sharing<T>(
    label: &str,
    mut operation: impl FnMut() -> std::io::Result<T>,
) -> std::io::Result<T> {
    let started = Instant::now();
    let budget = Duration::from_millis(250);
    let mut delay = Duration::from_millis(1);
    loop {
        match operation() {
            Ok(value) => return Ok(value),
            Err(error) => {
                // ACCESS_DENIED also covers legacy rename/delete-pending
                // contention. Missing files and permanent format errors are
                // deliberately not converted into retries or empty snapshots.
                let transient = matches!(error.raw_os_error(), Some(5 | 32 | 33 | 303));
                if !transient || started.elapsed() >= budget {
                    #[cfg(test)]
                    eprintln!(
                        "Atomic file {label} failed; OS code: {:?}",
                        error.raw_os_error()
                    );
                    #[cfg(not(test))]
                    let _ = label;
                    return Err(error);
                }
                std::thread::sleep(delay.min(budget.saturating_sub(started.elapsed())));
                delay = (delay * 2).min(Duration::from_millis(10));
            }
        }
    }
}

struct Temporary(PathBuf);
impl Drop for Temporary {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.0);
    }
}

pub fn replace(path: &Path, content: &str) -> Result<(), String> {
    replace_limited(path, content, MAX_SETTINGS_BYTES as usize)
}

pub fn replace_limited(path: &Path, content: &str, max_bytes: usize) -> Result<(), String> {
    if content.len() > max_bytes {
        return Err("文件过大，未保存".to_string());
    }
    let mut name = path.as_os_str().to_os_string();
    name.push(format!(
        ".tmp.{}.{}",
        std::process::id(),
        NEXT_TEMP.fetch_add(1, Ordering::Relaxed)
    ));
    let temporary = Temporary(PathBuf::from(name));
    let mut file = OpenOptions::new()
        .create_new(true)
        .write(true)
        .open(&temporary.0)
        .map_err(|_| "无法创建设置临时文件，原设置未更改".to_string())?;
    file.write_all(content.as_bytes())
        .and_then(|_| file.sync_all())
        .map_err(|_| "设置写入失败，原设置未更改".to_string())?;
    drop(file);
    replace_file(&temporary.0, path).map_err(|_| "无法替换设置文件，原设置未更改".to_string())?;
    Ok(())
}

fn replace_file(source: &Path, destination: &Path) -> std::io::Result<()> {
    // Rust >=1.85 uses FileRenameInfoEx/POSIX replacement when Windows' legacy
    // MoveFileEx cannot replace an open destination. Existing reader handles
    // keep their old snapshot while new opens see the complete new file.
    // Calling MoveFileEx directly bypassed that essential fallback.
    // The temporary file's contents have already been flushed before here.
    retry_sharing("replace", || fs::rename(source, destination))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn directory() -> PathBuf {
        let path = std::env::temp_dir().join(format!(
            "fangcun-atomic-write-{}-{}",
            std::process::id(),
            NEXT_TEMP.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir(&path).unwrap();
        path
    }

    #[test]
    fn replacement_failure_preserves_destination_and_cleans_its_temporary_file() {
        let root = directory();
        let target = root.join("settings.ini");
        fs::create_dir(&target).unwrap();
        fs::write(target.join("unchanged"), "original").unwrap();
        assert!(replace(&target, "scheme=pinyin\n").is_err());
        assert_eq!(
            fs::read_to_string(target.join("unchanged")).unwrap(),
            "original"
        );
        assert_eq!(fs::read_dir(&root).unwrap().count(), 1);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn size_rejection_never_truncates_previous_contents_or_creates_a_temp() {
        let root = directory();
        let target = root.join("settings.ini");
        fs::write(&target, "keep this original").unwrap();
        assert!(replace_limited(&target, "replacement exceeds explicit limit", 5).is_err());
        assert_eq!(fs::read_to_string(&target).unwrap(), "keep this original");
        assert_eq!(fs::read_dir(&root).unwrap().count(), 1);
        replace_limited(&target, "short", 5).unwrap();
        assert_eq!(fs::read_to_string(&target).unwrap(), "short");
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn held_reader_keeps_old_snapshot_while_new_opens_see_complete_replacement() {
        let root = directory();
        let target = root.join("settings.ini");
        fs::write(&target, "old complete snapshot").unwrap();
        let mut old_reader = open_read(&target).unwrap();
        replace(&target, "new complete snapshot").unwrap();
        let mut old = String::new();
        old_reader.read_to_string(&mut old).unwrap();
        assert_eq!(old, "old complete snapshot");
        assert_eq!(
            fs::read_to_string(&target).unwrap(),
            "new complete snapshot"
        );
        drop(old_reader);
        assert_eq!(fs::read_dir(&root).unwrap().count(), 1);
        fs::remove_dir_all(root).unwrap();
    }

    #[cfg(target_os = "windows")]
    #[test]
    fn transient_non_delete_sharing_reader_is_retried_without_losing_either_snapshot() {
        use std::os::windows::fs::OpenOptionsExt;
        let root = directory();
        let target = root.join("settings.ini");
        fs::write(&target, "old").unwrap();
        let held = OpenOptions::new()
            .read(true)
            .share_mode(1 | 2)
            .open(&target)
            .unwrap();
        let release = std::thread::spawn(move || {
            std::thread::sleep(Duration::from_millis(40));
            drop(held);
        });
        replace(&target, "new").unwrap();
        release.join().unwrap();
        assert_eq!(fs::read_to_string(&target).unwrap(), "new");
        assert_eq!(fs::read_dir(&root).unwrap().count(), 1);
        fs::remove_dir_all(root).unwrap();
    }

    #[cfg(target_os = "windows")]
    #[test]
    fn persistent_non_delete_sharing_failure_is_bounded_and_preserves_original_bytes() {
        use std::os::windows::fs::OpenOptionsExt;
        let root = directory();
        let target = root.join("settings.ini");
        fs::write(&target, "original remains").unwrap();
        let held = OpenOptions::new()
            .read(true)
            .share_mode(1 | 2)
            .open(&target)
            .unwrap();
        let started = Instant::now();
        assert!(replace(&target, "must not replace").is_err());
        assert!(started.elapsed() < Duration::from_secs(2));
        assert_eq!(fs::read_to_string(&target).unwrap(), "original remains");
        assert_eq!(fs::read_dir(&root).unwrap().count(), 1);
        drop(held);
        fs::remove_dir_all(root).unwrap();
    }

    #[cfg(target_os = "windows")]
    #[test]
    fn readers_retry_short_sharing_conflicts_but_never_invent_empty_contents() {
        use std::os::windows::fs::OpenOptionsExt;
        let root = directory();
        let target = root.join("settings.ini");
        fs::write(&target, "complete original").unwrap();
        let held = OpenOptions::new()
            .read(true)
            .share_mode(0)
            .open(&target)
            .unwrap();
        let release = std::thread::spawn(move || {
            std::thread::sleep(Duration::from_millis(40));
            drop(held);
        });
        assert_eq!(read(&target).unwrap().as_deref(), Some("complete original"));
        release.join().unwrap();
        let held = OpenOptions::new()
            .read(true)
            .share_mode(0)
            .open(&target)
            .unwrap();
        let started = Instant::now();
        assert!(
            read(&target).is_err(),
            "An inaccessible existing file is not a fresh installation"
        );
        assert!(started.elapsed() < Duration::from_secs(2));
        drop(held);
        assert_eq!(fs::read_to_string(&target).unwrap(), "complete original");
        assert!(read(&root.join("genuinely-missing.ini")).unwrap().is_none());
        fs::remove_dir_all(root).unwrap();
    }
}
