//! Windows-only protection and consistent clipboard snapshots for optional sharing.
//! Secret/plaintext data never appears in errors; other platforms fail closed.

#[derive(Clone)]
pub struct ClipboardSnapshot {
    pub sequence: u32,
    /// None means an explicitly excluded/sensitive clipboard; do not read or backfill it.
    pub text: Option<String>,
}

pub fn is_pairing_material(text: &str) -> bool {
    text.contains("fangcun-clip-v1:")
}

#[cfg(target_os = "windows")]
mod platform {
    use super::{is_pairing_material, ClipboardSnapshot};
    use std::ptr;
    use windows_sys::Win32::Foundation::HWND;
    use windows_sys::Win32::Foundation::{GlobalFree, LocalFree};
    use windows_sys::Win32::Security::Cryptography::{
        CryptProtectData, CryptUnprotectData, CRYPTPROTECT_UI_FORBIDDEN, CRYPT_INTEGER_BLOB,
    };
    use windows_sys::Win32::System::DataExchange::{
        CloseClipboard, EmptyClipboard, GetClipboardData, GetClipboardSequenceNumber,
        IsClipboardFormatAvailable, OpenClipboard, RegisterClipboardFormatW, SetClipboardData,
    };
    use windows_sys::Win32::System::Memory::{
        GlobalAlloc, GlobalLock, GlobalSize, GlobalUnlock, GMEM_MOVEABLE,
    };
    use windows_sys::Win32::UI::WindowsAndMessaging::{
        CreateWindowExW, DestroyWindow, HWND_MESSAGE,
    };

    const CF_UNICODETEXT: u32 = 13;
    // Reading local history has its own generous safety bound. The sharing protocol separately
    // enforces 32768 UTF-8 bytes; a large local copy must not silently become a truncated share.
    const MAX_TEXT_BYTES: usize = 1_048_576;
    const ENTROPY: &[u8] = b"FangCunClipboardShare/DPAPI/v1";

    struct ClipboardGuard {
        owner: HWND,
    }
    impl ClipboardGuard {
        fn open() -> Result<Self, &'static str> {
            if unsafe { OpenClipboard(ptr::null_mut()) } == 0 {
                return Err("系统剪贴板正忙，请稍后重试");
            }
            Ok(Self {
                owner: ptr::null_mut(),
            })
        }
        fn open_for_write() -> Result<Self, &'static str> {
            // EmptyClipboard with a NULL owner would make SetClipboardData fail. A small
            // message-only owner lives on this worker thread until all formats are transferred.
            let class: Vec<u16> = "STATIC".encode_utf16().chain(std::iter::once(0)).collect();
            let owner = unsafe {
                CreateWindowExW(
                    0,
                    class.as_ptr(),
                    ptr::null(),
                    0,
                    0,
                    0,
                    0,
                    0,
                    HWND_MESSAGE,
                    ptr::null_mut(),
                    ptr::null_mut(),
                    ptr::null(),
                )
            };
            if owner.is_null() {
                return Err("无法创建安全剪贴板窗口");
            }
            if unsafe { OpenClipboard(owner) } == 0 {
                unsafe {
                    DestroyWindow(owner);
                }
                return Err("系统剪贴板正忙，请稍后重试");
            }
            Ok(Self { owner })
        }
    }
    impl Drop for ClipboardGuard {
        fn drop(&mut self) {
            unsafe {
                CloseClipboard();
                if !self.owner.is_null() {
                    DestroyWindow(self.owner);
                }
            }
        }
    }

    fn format(name: &str) -> u32 {
        let name: Vec<u16> = name.encode_utf16().chain(std::iter::once(0)).collect();
        unsafe { RegisterClipboardFormatW(name.as_ptr()) }
    }

    fn flag_is_disabled(name: &str) -> bool {
        let id = format(name);
        if id == 0 {
            return true;
        }
        if unsafe { IsClipboardFormatAvailable(id) } == 0 {
            return false;
        }
        let data = unsafe { GetClipboardData(id) };
        if data.is_null() || unsafe { GlobalSize(data) } < 4 {
            return true;
        }
        let raw = unsafe { GlobalLock(data) } as *const u32;
        if raw.is_null() {
            return true;
        }
        let disabled = unsafe { ptr::read_unaligned(raw) } == 0;
        unsafe {
            GlobalUnlock(data);
        }
        disabled
    }

    pub fn sequence() -> u32 {
        unsafe { GetClipboardSequenceNumber() }
    }

    pub fn read() -> Result<ClipboardSnapshot, &'static str> {
        let _guard = ClipboardGuard::open()?;
        let sequence = sequence();
        let exclude = format("ExcludeClipboardContentFromMonitorProcessing");
        if exclude == 0
            || unsafe { IsClipboardFormatAvailable(exclude) } != 0
            || flag_is_disabled("CanUploadToCloudClipboard")
            || flag_is_disabled("CanIncludeInClipboardHistory")
        {
            return Ok(ClipboardSnapshot {
                sequence,
                text: None,
            });
        }
        if unsafe { IsClipboardFormatAvailable(CF_UNICODETEXT) } == 0 {
            return Ok(ClipboardSnapshot {
                sequence,
                text: None,
            });
        }
        let data = unsafe { GetClipboardData(CF_UNICODETEXT) };
        if data.is_null() {
            return Err("无法读取系统剪贴板");
        }
        let bytes = unsafe { GlobalSize(data) };
        if bytes < 2 || bytes % 2 != 0 {
            return Err("剪贴板文本格式无效");
        }
        let raw = unsafe { GlobalLock(data) } as *const u16;
        if raw.is_null() {
            return Err("无法读取系统剪贴板");
        }
        // Never allocate from the untrusted global allocation size. An ASCII payload may use
        // at most MAX_TEXT_BYTES UTF-16 units; other valid local text fits that unit budget.
        let units = unsafe { std::slice::from_raw_parts(raw, (bytes / 2).min(MAX_TEXT_BYTES + 1)) };
        let end = units.iter().position(|c| *c == 0);
        let text = end.and_then(|n| String::from_utf16(&units[..n]).ok());
        unsafe {
            GlobalUnlock(data);
        }
        let Some(text) = text else {
            return Err("剪贴板文本过长或格式无效");
        };
        if text.len() > MAX_TEXT_BYTES {
            return Err("剪贴板文本超过本机读取上限");
        }
        if is_pairing_material(&text) {
            return Ok(ClipboardSnapshot {
                sequence,
                text: None,
            });
        }
        Ok(ClipboardSnapshot {
            sequence,
            text: Some(text),
        })
    }

    fn put(format: u32, bytes: &[u8]) -> Result<(), &'static str> {
        if format == 0 {
            return Err("无法标记敏感剪贴板");
        }
        let memory = unsafe { GlobalAlloc(GMEM_MOVEABLE, bytes.len()) };
        if memory.is_null() {
            return Err("无法写入系统剪贴板");
        }
        let raw = unsafe { GlobalLock(memory) } as *mut u8;
        if raw.is_null() {
            unsafe {
                GlobalFree(memory);
            }
            return Err("无法写入系统剪贴板");
        }
        unsafe {
            ptr::copy_nonoverlapping(bytes.as_ptr(), raw, bytes.len());
            GlobalUnlock(memory);
        }
        if unsafe { SetClipboardData(format, memory) }.is_null() {
            unsafe {
                GlobalFree(memory);
            }
            return Err("无法写入系统剪贴板");
        }
        Ok(()) // Windows owns the handle only after a successful SetClipboardData.
    }

    pub fn write_excluded(text: &str) -> Result<(), &'static str> {
        if text.encode_utf16().any(|c| c == 0) {
            return Err("文本包含无法复制的字符");
        }
        let utf16: Vec<u16> = text.encode_utf16().chain(std::iter::once(0)).collect();
        let bytes =
            unsafe { std::slice::from_raw_parts(utf16.as_ptr() as *const u8, utf16.len() * 2) };
        let _guard = ClipboardGuard::open_for_write()?;
        if unsafe { EmptyClipboard() } == 0 {
            return Err("无法写入系统剪贴板");
        }
        // Add exclusion flags before text. If any marker fails, do not expose the new text.
        put(format("ExcludeClipboardContentFromMonitorProcessing"), &[0])?;
        put(format("CanUploadToCloudClipboard"), &0u32.to_le_bytes())?;
        put(format("CanIncludeInClipboardHistory"), &0u32.to_le_bytes())?;
        if let Err(error) = put(CF_UNICODETEXT, bytes) {
            unsafe {
                EmptyClipboard();
            }
            return Err(error);
        }
        Ok(())
    }

    pub fn protect(bytes: &[u8]) -> Result<Vec<u8>, &'static str> {
        if bytes.len() > 64 * 1024 * 1024 {
            return Err("共享本机状态过大");
        }
        let input = CRYPT_INTEGER_BLOB {
            cbData: bytes.len() as u32,
            pbData: bytes.as_ptr() as *mut u8,
        };
        let entropy = CRYPT_INTEGER_BLOB {
            cbData: ENTROPY.len() as u32,
            pbData: ENTROPY.as_ptr() as *mut u8,
        };
        let mut output = CRYPT_INTEGER_BLOB {
            cbData: 0,
            pbData: ptr::null_mut(),
        };
        if unsafe {
            CryptProtectData(
                &input,
                ptr::null(),
                &entropy,
                ptr::null_mut(),
                ptr::null(),
                CRYPTPROTECT_UI_FORBIDDEN,
                &mut output,
            )
        } == 0
        {
            return Err("无法使用 Windows 保护共享密钥，请检查当前账户");
        }
        let result =
            unsafe { std::slice::from_raw_parts(output.pbData, output.cbData as usize) }.to_vec();
        unsafe {
            LocalFree(output.pbData as _);
        }
        Ok(result)
    }

    pub fn unprotect(bytes: &[u8]) -> Result<Vec<u8>, &'static str> {
        if bytes.is_empty() || bytes.len() > 64 * 1024 * 1024 + 65_536 {
            return Err("共享状态无效");
        }
        let input = CRYPT_INTEGER_BLOB {
            cbData: bytes.len() as u32,
            pbData: bytes.as_ptr() as *mut u8,
        };
        let entropy = CRYPT_INTEGER_BLOB {
            cbData: ENTROPY.len() as u32,
            pbData: ENTROPY.as_ptr() as *mut u8,
        };
        let mut output = CRYPT_INTEGER_BLOB {
            cbData: 0,
            pbData: ptr::null_mut(),
        };
        if unsafe {
            CryptUnprotectData(
                &input,
                ptr::null_mut(),
                &entropy,
                ptr::null_mut(),
                ptr::null(),
                CRYPTPROTECT_UI_FORBIDDEN,
                &mut output,
            )
        } == 0
        {
            return Err("无法读取共享密钥，请在当前 Windows 账户重新配置");
        }
        let result =
            unsafe { std::slice::from_raw_parts(output.pbData, output.cbData as usize) }.to_vec();
        unsafe {
            LocalFree(output.pbData as _);
        }
        Ok(result)
    }
}

#[cfg(target_os = "windows")]
pub use platform::{protect, read as read_snapshot, sequence, unprotect, write_excluded};

#[cfg(not(target_os = "windows"))]
pub fn sequence() -> u32 {
    0
}
#[cfg(not(target_os = "windows"))]
pub fn read_snapshot() -> Result<ClipboardSnapshot, &'static str> {
    Err("跨设备共享目前需要 Windows 10 或更新系统")
}
#[cfg(not(target_os = "windows"))]
pub fn write_excluded(_: &str) -> Result<(), &'static str> {
    Err("当前平台不能保护敏感剪贴板，操作已停止")
}
#[cfg(not(target_os = "windows"))]
pub fn protect(_: &[u8]) -> Result<Vec<u8>, &'static str> {
    Err("跨设备共享需要 Windows DPAPI，不会回退到明文保存")
}
#[cfg(not(target_os = "windows"))]
pub fn unprotect(_: &[u8]) -> Result<Vec<u8>, &'static str> {
    Err("当前平台无法读取 Windows 保护的共享状态")
}
