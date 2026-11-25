//! Local config/job regressions. The deterministic cipher exists only in tests;
//! production secrets always use Windows DPAPI and fail closed elsewhere.
use super::*;
use aes_gcm::{
    aead::{Aead, KeyInit, Payload},
    Aes256Gcm, Nonce,
};
use std::fs;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Barrier;

static NEXT: AtomicU64 = AtomicU64::new(0);
struct Directory(PathBuf);
impl Directory {
    fn new() -> Self {
        let path = std::env::temp_dir().join(format!(
            "fangcun-ai-config-test-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir(&path).unwrap();
        Self(path)
    }
    fn manager(&self) -> Arc<AiManager> {
        Arc::new(AiManager {
            path: self.0.join("ai-config.json"),
            cipher: Arc::new(TestCipher),
            client: None,
            jobs: Mutex::new(HashMap::new()),
        })
    }
}
impl Drop for Directory {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}
struct TestCipher;
impl SecretCipher for TestCipher {
    fn seal(&self, scope: &str, plain: &[u8]) -> Result<Vec<u8>, String> {
        Aes256Gcm::new_from_slice(&[42u8; 32])
            .unwrap()
            .encrypt(
                Nonce::from_slice(&[0u8; 12]),
                Payload {
                    msg: plain,
                    aad: scope.as_bytes(),
                },
            )
            .map_err(|_| "测试加密失败".into())
    }
    fn open(&self, scope: &str, ciphertext: &[u8]) -> Result<Zeroizing<Vec<u8>>, String> {
        Aes256Gcm::new_from_slice(&[42u8; 32])
            .unwrap()
            .decrypt(
                Nonce::from_slice(&[0u8; 12]),
                Payload {
                    msg: ciphertext,
                    aad: scope.as_bytes(),
                },
            )
            .map(Zeroizing::new)
            .map_err(|_| "测试凭证不匹配".into())
    }
}
fn draft(snapshot: &AiSnapshot) -> AiDraft {
    AiDraft {
        enabled: true,
        text: snapshot.text.clone(),
        speech: snapshot.speech.clone(),
        text_key: None,
        speech_key: None,
        clear_text_key: false,
        clear_speech_key: false,
    }
}
fn save_text(manager: &AiManager, key: &str) -> AiSnapshot {
    let snapshot = manager.get_config().unwrap();
    let mut next = draft(&snapshot);
    next.text_key = Some(key.into());
    manager.save_config(next, &snapshot.revision).unwrap()
}

#[test]
fn defaults_are_disabled_and_never_claim_credentials_or_create_a_config() {
    let directory = Directory::new();
    let manager = directory.manager();
    let snapshot = manager.get_config().unwrap();
    assert!(!snapshot.enabled);
    assert!(!snapshot.text_key_saved);
    assert!(!snapshot.speech_key_saved);
    assert_eq!(snapshot.text.model, "deepseek-flash");
    assert_eq!(snapshot.speech.model, "gpt-transcribe");
    assert!(!manager.path.exists());
    assert!(manager.prepare(Role::Text).is_err());
}

#[test]
fn persisted_key_is_ciphertext_and_never_returned_in_snapshot() {
    let directory = Directory::new();
    let manager = directory.manager();
    let snapshot = save_text(&manager, "test-unique-plain-secret-a82f");
    assert!(snapshot.enabled && snapshot.text_key_saved && !snapshot.speech_key_saved);
    let file = fs::read_to_string(&manager.path).unwrap();
    assert!(!file.contains("test-unique-plain-secret-a82f"));
    let json = serde_json::to_string(&snapshot).unwrap();
    assert!(!json.contains("test-unique-plain-secret-a82f"));
    assert!(!json.contains("encrypted"));
    assert_eq!(
        manager.prepare(Role::Text).unwrap().key.as_str(),
        "test-unique-plain-secret-a82f"
    );
    assert_eq!(
        directory
            .manager()
            .prepare(Role::Text)
            .unwrap()
            .key
            .as_str(),
        "test-unique-plain-secret-a82f"
    );
}

#[test]
fn credentials_bind_role_provider_and_canonical_origin_but_keep_valid_same_origin_paths() {
    let directory = Directory::new();
    let manager = directory.manager();
    let snapshot = save_text(&manager, "text-only-key");
    let mut change = draft(&snapshot);
    change.text.base_url = "https://api.deepseek.com:443/custom/v1".into();
    let snapshot = manager.save_config(change, &snapshot.revision).unwrap();
    assert!(snapshot.text_key_saved);
    assert_eq!(
        manager.prepare(Role::Text).unwrap().key.as_str(),
        "text-only-key"
    );
    let mut change = draft(&snapshot);
    change.text.base_url = "https://other.example/v1".into();
    let snapshot = manager.save_config(change, &snapshot.revision).unwrap();
    assert!(!snapshot.text_key_saved);
    assert!(manager.prepare(Role::Text).is_err());
    let mut change = draft(&snapshot);
    change.text.base_url = "https://api.deepseek.com/v1".into();
    change.text.provider = Provider::Compatible;
    let snapshot = manager.save_config(change, &snapshot.revision).unwrap();
    assert!(!snapshot.text_key_saved);
    assert!(manager.prepare(Role::Text).is_err());
    let mut change = draft(&snapshot);
    change.text.provider = Provider::Deepseek;
    let snapshot = manager.save_config(change, &snapshot.revision).unwrap();
    assert!(snapshot.text_key_saved);
    assert_eq!(
        manager.prepare(Role::Text).unwrap().key.as_str(),
        "text-only-key"
    );
    assert!(manager.prepare(Role::Speech).is_err());
}

#[test]
fn speech_and_text_keys_on_same_openai_origin_remain_separate() {
    let directory = Directory::new();
    let manager = directory.manager();
    let snapshot = manager.get_config().unwrap();
    let mut change = draft(&snapshot);
    change.text = AiRoute {
        provider: Provider::Openai,
        base_url: change.speech.base_url.clone(),
        model: "gpt-test-text".into(),
    };
    change.text_key = Some("text-scope-key".into());
    change.speech_key = Some("speech-scope-key".into());
    let snapshot = manager.save_config(change, &snapshot.revision).unwrap();
    assert!(snapshot.text_key_saved && snapshot.speech_key_saved);
    assert_eq!(
        manager.prepare(Role::Text).unwrap().key.as_str(),
        "text-scope-key"
    );
    assert_eq!(
        manager.prepare(Role::Speech).unwrap().key.as_str(),
        "speech-scope-key"
    );
    let mut change = draft(&snapshot);
    change.clear_text_key = true;
    let snapshot = manager.save_config(change, &snapshot.revision).unwrap();
    assert!(!snapshot.text_key_saved && snapshot.speech_key_saved);
    assert!(manager.prepare(Role::Text).is_err());
    assert_eq!(
        manager.prepare(Role::Speech).unwrap().key.as_str(),
        "speech-scope-key"
    );
}

#[test]
fn explicit_delete_targets_only_the_active_draft_scope() {
    let directory = Directory::new();
    let manager = directory.manager();
    let snapshot = save_text(&manager, "origin-a-key");
    let mut change = draft(&snapshot);
    change.text.base_url = "https://other.example/v1".into();
    change.text_key = Some("origin-b-key".into());
    let snapshot = manager.save_config(change, &snapshot.revision).unwrap();
    let mut change = draft(&snapshot);
    change.clear_text_key = true;
    let snapshot = manager.save_config(change, &snapshot.revision).unwrap();
    assert!(!snapshot.text_key_saved);
    let mut change = draft(&snapshot);
    change.text.base_url = Provider::Deepseek.default_base().into();
    let snapshot = manager.save_config(change, &snapshot.revision).unwrap();
    assert!(snapshot.text_key_saved);
    assert_eq!(
        manager.prepare(Role::Text).unwrap().key.as_str(),
        "origin-a-key"
    );
}

#[test]
fn corrupted_oversize_and_nonfile_configs_are_not_replaced_by_defaults() {
    let directory = Directory::new();
    let manager = directory.manager();
    let baseline = manager.get_config().unwrap();
    for bytes in [
        b"{broken-private-settings".to_vec(),
        vec![b'x'; MAX_CONFIG_BYTES + 1],
    ] {
        fs::write(&manager.path, &bytes).unwrap();
        assert!(manager.get_config().is_err());
        assert!(manager
            .save_config(draft(&baseline), &baseline.revision)
            .is_err());
        assert_eq!(fs::read(&manager.path).unwrap(), bytes);
    }
    fs::remove_file(&manager.path).unwrap();
    fs::create_dir(&manager.path).unwrap();
    assert!(manager.get_config().is_err());
    assert!(manager
        .save_config(draft(&baseline), &baseline.revision)
        .is_err());
    assert!(manager.path.is_dir());
}

#[test]
fn concurrent_stale_revision_writers_cannot_silently_overwrite_each_other() {
    let directory = Directory::new();
    let manager = directory.manager();
    let initial = manager.get_config().unwrap();
    let revision = initial.revision.clone();
    let barrier = Arc::new(Barrier::new(3));
    let threads: Vec<_> = ["first-key", "second-key"]
        .into_iter()
        .map(|key| {
            let manager = Arc::clone(&manager);
            let revision = revision.clone();
            let barrier = Arc::clone(&barrier);
            let mut next = draft(&initial);
            next.text_key = Some(key.into());
            std::thread::spawn(move || {
                barrier.wait();
                manager.save_config(next, &revision).is_ok()
            })
        })
        .collect();
    barrier.wait();
    let accepted = threads
        .into_iter()
        .map(|thread| thread.join().unwrap())
        .filter(|accepted| *accepted)
        .count();
    assert_eq!(accepted, 1);
    assert!(manager.get_config().unwrap().text_key_saved);
    let key = manager.prepare(Role::Text).unwrap();
    assert!(matches!(key.key.as_str(), "first-key" | "second-key"));
}

#[test]
fn failed_encryption_and_conflicting_delete_preserve_previous_file_and_running_job() {
    struct FailingCipher;
    impl SecretCipher for FailingCipher {
        fn seal(&self, _: &str, _: &[u8]) -> Result<Vec<u8>, String> {
            Err("加密未完成".into())
        }
        fn open(&self, _: &str, _: &[u8]) -> Result<Zeroizing<Vec<u8>>, String> {
            Err("读取未完成".into())
        }
    }
    let directory = Directory::new();
    let manager = directory.manager();
    let snapshot = save_text(&manager, "old-good-key");
    let before = fs::read(&manager.path).unwrap();
    let mut invalid = draft(&snapshot);
    invalid.text_key = Some("new-key".into());
    invalid.clear_text_key = true;
    let live = manager.begin("main", "retained").unwrap();
    assert!(manager.save_config(invalid, &snapshot.revision).is_err());
    assert!(manager.active(&live.job));
    let failing = AiManager {
        path: manager.path.clone(),
        cipher: Arc::new(FailingCipher),
        client: None,
        jobs: Mutex::new(HashMap::new()),
    };
    let mut next = draft(&snapshot);
    next.text_key = Some("new-key".into());
    assert!(failing.save_config(next, &snapshot.revision).is_err());
    assert_eq!(fs::read(&manager.path).unwrap(), before);
}

#[test]
fn even_a_relabelled_ciphertext_cannot_be_decrypted_for_another_scope() {
    let directory = Directory::new();
    let manager = directory.manager();
    save_text(&manager, "secret-only-original-origin");
    let (mut config, _) = load_file(&manager.path).unwrap();
    let mut stolen = config.keys.values().next().unwrap().clone();
    stolen.origin = "https://attacker.example".into();
    let scope = key_scope(stolen.role, stolen.provider, &stolen.origin);
    config.keys.insert(digest(scope.as_bytes()), stolen);
    config.text.base_url = "https://attacker.example/v1".into();
    fs::write(&manager.path, serde_json::to_vec(&config).unwrap()).unwrap();
    assert!(manager.get_config().unwrap().text_key_saved);
    assert!(manager.prepare(Role::Text).is_err());
}

#[test]
fn endpoint_and_model_validation_rejects_credential_urls_and_wrong_protocols() {
    for raw in [
        "http://example.com/v1",
        "wss://example.com",
        "https://user@example.com/v1",
        "https://example.com/v1?key=secret",
        "https://example.com/v1#key",
        "https://example.com/v1?",
    ] {
        assert!(secure_url(raw).is_err());
    }
    for model in [
        "models/",
        "models/models/gemini-test",
        "gemini/test",
        "gemini:test",
    ] {
        let route = AiRoute {
            provider: Provider::Gemini,
            base_url: Provider::Gemini.default_base().into(),
            model: model.into(),
        };
        assert!(validate_route(route, Role::Text).is_err());
    }
    let mut speech = ConfigFile::default().speech;
    speech.provider = Provider::Compatible;
    assert!(validate_route(speech, Role::Speech).is_err());
}

#[test]
fn windows_cipher_is_not_a_plaintext_fallback_on_other_platforms() {
    #[cfg(not(target_os = "windows"))]
    {
        assert!(WindowsCipher.seal("test", b"secret").is_err());
        assert!(WindowsCipher.open("test", b"secret").is_err());
    }
}

#[test]
fn new_jobs_old_leases_and_cancel_ids_are_isolated_per_window() {
    let directory = Directory::new();
    let manager = directory.manager();
    let old = manager.begin("main", "old").unwrap();
    let other = manager.begin("clipboard", "other").unwrap();
    let current = manager.begin("main", "new").unwrap();
    assert!(old.job.cancel.is_cancelled());
    assert!(!manager.active(&old.job));
    drop(old);
    assert!(manager.active(&current.job));
    manager.cancel("main", "old");
    assert!(manager.active(&current.job));
    manager.cancel("clipboard", "new");
    assert!(manager.active(&other.job));
    manager.cancel_window("main");
    assert!(!manager.active(&current.job));
    assert!(manager.active(&other.job));
    manager.cancel_all();
    assert!(!manager.active(&other.job));
}

#[test]
fn stale_jobs_never_emit_and_closed_sinks_cancel_before_any_later_event() {
    let directory = Directory::new();
    let manager = directory.manager();
    let old = manager.begin("main", "old").unwrap();
    let received = Arc::new(Mutex::new(Vec::new()));
    let output = Arc::clone(&received);
    let sink: ProgressSink = Arc::new(move |progress| {
        output.lock().unwrap().push(progress.id);
        true
    });
    let new = manager.begin("main", "new").unwrap();
    assert!(manager
        .emit(&old.job, "generating", "stale".into(), &sink)
        .is_err());
    manager
        .emit(&new.job, "generating", "current".into(), &sink)
        .unwrap();
    assert_eq!(*received.lock().unwrap(), vec!["new"]);
    let closed: ProgressSink = Arc::new(|_| false);
    assert!(manager
        .emit(&new.job, "generating", "tail".into(), &closed)
        .is_err());
    assert!(!manager.active(&new.job));
    assert!(manager
        .emit(&new.job, "generating", "late".into(), &sink)
        .is_err());
}

#[test]
fn successful_config_save_invalidates_every_inflight_job() {
    let directory = Directory::new();
    let manager = directory.manager();
    let first = manager.begin("main", "a").unwrap();
    let second = manager.begin("clipboard", "b").unwrap();
    let initial = manager.get_config().unwrap();
    let mut next = draft(&initial);
    next.enabled = false;
    manager.save_config(next, &initial.revision).unwrap();
    assert!(!manager.active(&first.job));
    assert!(!manager.active(&second.job));
}

#[tokio::test]
async fn cancellation_waiters_observe_cancel_even_if_they_subscribe_after_it() {
    let cancel = Cancellation::new();
    cancel.cancel();
    tokio::time::timeout(Duration::from_millis(50), cancel.cancelled())
        .await
        .unwrap();
}

#[test]
fn wav_validation_checks_every_audio_parameter_and_exact_sixty_second_boundary() {
    fn recording(samples: usize) -> Vec<u8> {
        let mut wav = vec![0u8; 44 + samples * 2];
        wav[..4].copy_from_slice(b"RIFF");
        let size = wav.len() as u32 - 8;
        wav[4..8].copy_from_slice(&size.to_le_bytes());
        wav[8..16].copy_from_slice(b"WAVEfmt ");
        wav[16..20].copy_from_slice(&16u32.to_le_bytes());
        wav[20..22].copy_from_slice(&1u16.to_le_bytes());
        wav[22..24].copy_from_slice(&1u16.to_le_bytes());
        wav[24..28].copy_from_slice(&16000u32.to_le_bytes());
        wav[28..32].copy_from_slice(&32000u32.to_le_bytes());
        wav[32..34].copy_from_slice(&2u16.to_le_bytes());
        wav[34..36].copy_from_slice(&16u16.to_le_bytes());
        wav[36..40].copy_from_slice(b"data");
        wav[40..44].copy_from_slice(&(samples as u32 * 2).to_le_bytes());
        wav
    }
    let short = recording(1);
    transport::validate_wav(&short).unwrap();
    transport::validate_wav(&recording(60 * 16000)).unwrap();
    assert!(transport::validate_wav(&recording(0)).is_err());
    assert!(transport::validate_wav(&recording(60 * 16000 + 1)).is_err());
    for offset in [0, 4, 8, 12, 16, 20, 22, 24, 28, 32, 34, 36, 40] {
        let mut invalid = short.clone();
        invalid[offset] ^= 1;
        assert!(
            transport::validate_wav(&invalid).is_err(),
            "WAV header field at {offset} must be checked"
        );
    }
    let mut trailer = short;
    trailer.extend_from_slice(b"private hidden trailer");
    assert!(transport::validate_wav(&trailer).is_err());
}

#[test]
fn ipc_audio_arrays_are_bounded_and_validate_numeric_bytes_during_deserialization() {
    let too_long = format!(
        "{{\"id\":\"audio\",\"wav\":[{}0]}}",
        "0,".repeat(MAX_WAV_BYTES)
    );
    assert!(serde_json::from_str::<TranscribeRequest>(&too_long).is_err());
    for bytes in ["[256]", "[-1]", "[1.5]", "[\"0\"]"] {
        assert!(serde_json::from_str::<TranscribeRequest>(&format!(
            "{{\"id\":\"audio\",\"wav\":{bytes}}}"
        ))
        .is_err());
    }
    assert_eq!(
        serde_json::from_str::<TranscribeRequest>("{\"id\":\"audio\",\"wav\":[0,127,255]}")
            .unwrap()
            .wav,
        vec![0, 127, 255]
    );
}

#[test]
fn text_requests_reject_blank_or_oversize_sources_and_missing_translation_target() {
    let mut request = GenerateRequest {
        id: "text".into(),
        mode: AiMode::Ask,
        source: "字".repeat(16_000),
        target_language: "English".into(),
    };
    transport::validate_request(&request).unwrap();
    request.source.push('字');
    assert!(transport::validate_request(&request).is_err());
    request.source = " \r\n ".into();
    assert!(transport::validate_request(&request).is_err());
    request.source = "翻译这个".into();
    request.mode = AiMode::Translate;
    request.target_language.clear();
    assert!(transport::validate_request(&request).is_err());
    request.target_language = "英语".into();
    transport::validate_request(&request).unwrap();
}
