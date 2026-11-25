//! Account-bound AI secrets. There is no non-Windows plaintext fallback.
use zeroize::Zeroizing;

pub(super) trait SecretCipher: Send + Sync {
    fn seal(&self, scope: &str, plain: &[u8]) -> Result<Vec<u8>, String>;
    fn open(&self, scope: &str, encrypted: &[u8]) -> Result<Zeroizing<Vec<u8>>, String>;
}

pub(super) struct WindowsCipher;

#[cfg(target_os = "windows")]
fn crypt(scope: &str, bytes: &[u8], decrypt: bool) -> Result<Vec<u8>, String> {
    use std::ptr;
    use windows_sys::Win32::Foundation::LocalFree;
    use windows_sys::Win32::Security::Cryptography::{
        CryptProtectData, CryptUnprotectData, CRYPTPROTECT_UI_FORBIDDEN, CRYPT_INTEGER_BLOB,
    };
    if bytes.is_empty() || bytes.len() > 32_768 {
        return Err("AI 凭证格式不正确".into());
    }
    let entropy = format!("FangCunAI/DPAPI/v1/{scope}");
    let input = CRYPT_INTEGER_BLOB {
        cbData: bytes.len() as u32,
        pbData: bytes.as_ptr() as *mut u8,
    };
    let salt = CRYPT_INTEGER_BLOB {
        cbData: entropy.len() as u32,
        pbData: entropy.as_ptr() as *mut u8,
    };
    let mut output = CRYPT_INTEGER_BLOB {
        cbData: 0,
        pbData: ptr::null_mut(),
    };
    let ok = unsafe {
        if decrypt {
            CryptUnprotectData(
                &input,
                ptr::null_mut(),
                &salt,
                ptr::null_mut(),
                ptr::null(),
                CRYPTPROTECT_UI_FORBIDDEN,
                &mut output,
            )
        } else {
            CryptProtectData(
                &input,
                ptr::null(),
                &salt,
                ptr::null_mut(),
                ptr::null(),
                CRYPTPROTECT_UI_FORBIDDEN,
                &mut output,
            )
        }
    };
    if ok == 0 || output.pbData.is_null() {
        return Err(if decrypt {
            "无法读取 AI 凭证，请在当前 Windows 账户重新保存"
        } else {
            "无法加密 AI 凭证，请检查当前 Windows 账户"
        }
        .into());
    }
    let size = output.cbData as usize;
    let result = if size > 32_768 {
        Err("AI 凭证数据异常，未使用".into())
    } else {
        Ok(unsafe { std::slice::from_raw_parts(output.pbData, size) }.to_vec())
    };
    unsafe {
        std::ptr::write_bytes(output.pbData, 0, size);
        LocalFree(output.pbData as _);
    }
    result
}

impl SecretCipher for WindowsCipher {
    fn seal(&self, scope: &str, plain: &[u8]) -> Result<Vec<u8>, String> {
        #[cfg(target_os = "windows")]
        {
            crypt(scope, plain, false)
        }
        #[cfg(not(target_os = "windows"))]
        {
            let _ = (scope, plain);
            Err("AI 凭证需要 Windows 账户保护，不会明文保存".into())
        }
    }
    fn open(&self, scope: &str, encrypted: &[u8]) -> Result<Zeroizing<Vec<u8>>, String> {
        #[cfg(target_os = "windows")]
        {
            crypt(scope, encrypted, true).map(Zeroizing::new)
        }
        #[cfg(not(target_os = "windows"))]
        {
            let _ = (scope, encrypted);
            Err("当前平台无法读取 Windows 保护的 AI 凭证".into())
        }
    }
}
