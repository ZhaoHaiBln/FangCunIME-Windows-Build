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
    let mut file = match File::open(path) {
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

#[cfg(not(target_os = "windows"))]
fn replace_file(source: &Path, destination: &Path) -> std::io::Result<()> {
    fs::rename(source, destination)
}

#[cfg(target_os = "windows")]
fn replace_file(source: &Path, destination: &Path) -> std::io::Result<()> {
    use std::os::windows::ffi::OsStrExt;
    use windows_sys::Win32::Storage::FileSystem::{
        MoveFileExW, MOVEFILE_REPLACE_EXISTING, MOVEFILE_WRITE_THROUGH,
    };
    let source: Vec<_> = source.as_os_str().encode_wide().chain(Some(0)).collect();
    let destination: Vec<_> = destination
        .as_os_str()
        .encode_wide()
        .chain(Some(0))
        .collect();
    if unsafe {
        MoveFileExW(
            source.as_ptr(),
            destination.as_ptr(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH,
        )
    } == 0
    {
        Err(std::io::Error::last_os_error())
    } else {
        Ok(())
    }
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
}
