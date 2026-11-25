//! Real loopback TLS tests. The request/stream/response code is the production
//! transport; only the server and its trusted, temporary CA are test fixtures.
use super::*;
use serde_json::Value;
use std::io::{BufRead, BufReader};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Duration;

const FIXTURE_KEY: &str = "fixture-key-DO-NOT-PRINT";
const PRIVATE_MARKER: &str = "SERVER_PRIVATE_BODY";
static NEXT: AtomicU64 = AtomicU64::new(0);

struct TemporaryDirectory(PathBuf);
impl Drop for TemporaryDirectory {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

struct Fixture {
    child: Child,
    temporary: TemporaryDirectory,
    port: u16,
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}
impl Fixture {
    fn new(scenario: &str, wav: Option<&[u8]>) -> Self {
        let directory = std::env::temp_dir().join(format!(
            "fangcun-ai-tls-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        std::fs::create_dir_all(&directory).unwrap();
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            std::fs::set_permissions(&directory, std::fs::Permissions::from_mode(0o700)).unwrap();
        }
        let temporary = TemporaryDirectory(directory);
        let ca = temporary.0.join("fixture-ca.pem");
        let ca_key = temporary.0.join("fixture-ca-key.pem");
        let cert = temporary.0.join("fixture-cert.pem");
        let key = temporary.0.join("fixture-key.pem");
        let csr = temporary.0.join("fixture.csr");
        let extensions = temporary.0.join("server-extensions.cnf");
        std::fs::write(&extensions, "basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\nsubjectAltName=IP:127.0.0.1,DNS:localhost\n").unwrap();
        let openssl =
            std::env::var_os("FANGCUN_AI_TEST_OPENSSL").unwrap_or_else(|| "openssl".into());
        let status = Command::new(&openssl)
            .args([
                "req",
                "-x509",
                "-newkey",
                "rsa:2048",
                "-sha256",
                "-nodes",
                "-days",
                "1",
                "-subj",
                "/CN=FangCun temporary test CA",
                "-addext",
                "basicConstraints=critical,CA:TRUE,pathlen:0",
                "-addext",
                "keyUsage=critical,keyCertSign,cRLSign",
                "-keyout",
            ])
            .arg(&ca_key)
            .arg("-out")
            .arg(&ca)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status()
            .expect("openssl is required for the loopback TLS fixture");
        assert!(status.success(), "Temporary TLS CA generation failed");
        let status = Command::new(&openssl)
            .args([
                "req",
                "-new",
                "-newkey",
                "rsa:2048",
                "-sha256",
                "-nodes",
                "-subj",
                "/CN=localhost",
                "-keyout",
            ])
            .arg(&key)
            .arg("-out")
            .arg(&csr)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status()
            .unwrap();
        assert!(
            status.success(),
            "Temporary TLS server CSR generation failed"
        );
        let status = Command::new(&openssl)
            .args(["x509", "-req", "-sha256", "-days", "1", "-in"])
            .arg(&csr)
            .arg("-CA")
            .arg(&ca)
            .arg("-CAkey")
            .arg(&ca_key)
            .arg("-CAcreateserial")
            .arg("-extfile")
            .arg(&extensions)
            .arg("-out")
            .arg(&cert)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status()
            .unwrap();
        assert!(
            status.success(),
            "Temporary TLS server certificate signing failed"
        );

        let here = PathBuf::from(file!());
        let source = if here.is_absolute() {
            here
        } else {
            PathBuf::from(env!("CARGO_MANIFEST_DIR")).join(here)
        };
        let windows = source.ancestors().nth(4).expect("Windows source directory");
        let script = windows.join("tests/ai_tls_fixture.py");
        assert!(
            script.is_file(),
            "The real TLS fixture script must be present"
        );
        let python = std::env::var_os("FANGCUN_AI_TEST_PYTHON").unwrap_or_else(|| "python3".into());
        let mut command = Command::new(python);
        command
            .arg(script)
            .arg("--cert")
            .arg(cert)
            .arg("--key")
            .arg(key)
            .arg("--scenario")
            .arg(scenario)
            .arg("--report")
            .arg(temporary.0.join("report.json"))
            .arg("--release")
            .arg(temporary.0.join("release"))
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::null());
        if let Some(wav) = wav {
            command
                .arg("--wav-sha")
                .arg(format!("{:x}", Sha256::digest(wav)));
        }
        let child = command
            .spawn()
            .expect("Python is required for the loopback TLS fixture");
        let mut fixture = Self {
            child,
            temporary,
            port: 0,
        };
        let stdout = fixture.child.stdout.take().unwrap();
        let (sender, receiver) = std::sync::mpsc::sync_channel(1);
        std::thread::spawn(move || {
            let mut line = String::new();
            let result = BufReader::new(stdout).read_line(&mut line).map(|_| line);
            let _ = sender.send(result);
        });
        let line = receiver
            .recv_timeout(Duration::from_secs(5))
            .expect("TLS fixture did not start within its local time bound")
            .expect("TLS fixture did not return a port");
        let value: Value =
            serde_json::from_str(&line).expect("TLS fixture returns only a JSON port");
        fixture.port = value["port"].as_u64().unwrap().try_into().unwrap();
        fixture
    }

    fn client(&self) -> Client {
        let pem = std::fs::read(self.temporary.0.join("fixture-ca.pem")).unwrap();
        Client::builder()
            .add_root_certificate(reqwest::Certificate::from_pem(&pem).unwrap())
            .redirect(reqwest::redirect::Policy::none())
            .connect_timeout(Duration::from_secs(2))
            .timeout(Duration::from_secs(8))
            .build()
            .unwrap()
    }

    fn untrusted_client(&self) -> Client {
        Client::builder()
            .redirect(reqwest::redirect::Policy::none())
            .connect_timeout(Duration::from_secs(2))
            .timeout(Duration::from_secs(3))
            .build()
            .unwrap()
    }

    fn route(&self, provider: Provider, speech: bool) -> PreparedRoute {
        let (prefix, model) = match (provider, speech) {
            (Provider::Gemini, true) => ("v1beta", "gemini-3.5-transcribe"),
            (Provider::Gemini, false) => ("v1beta", "gemini-3.5-flash-lite"),
            (_, true) => ("v1", "gpt-transcribe"),
            (_, false) => ("v1", "fixture-writing-model"),
        };
        PreparedRoute {
            route: AiRoute {
                provider,
                base_url: format!("https://127.0.0.1:{}/gateway/{prefix}", self.port),
                model: model.into(),
            },
            key: Zeroizing::new(FIXTURE_KEY.to_string()),
        }
    }

    fn report(&self) -> Value {
        serde_json::from_slice(&std::fs::read(self.temporary.0.join("report.json")).unwrap())
            .unwrap()
    }
    fn release_path(&self) -> PathBuf {
        self.temporary.0.join("release")
    }
    fn checked_request(&self) {
        let report = self.report();
        assert_eq!(report["requests"], 1);
        assert!(
            report.get("fixtureError").is_none(),
            "TLS fixture must not fail internally"
        );
        let checks = report["checks"]
            .as_object()
            .expect("Fixture must inspect the real request");
        assert!(!checks.is_empty());
        for (name, passed) in checks {
            assert_eq!(
                passed.as_bool(),
                Some(true),
                "Request field check failed: {name}"
            );
        }
    }
}

fn wav() -> Vec<u8> {
    // Distinct signed samples verify raw little-endian PCM, not merely RIFF magic.
    let samples: [i16; 8] = [0, 1, -1, 32767, -32768, 1234, -4321, 0];
    let mut pcm = Vec::new();
    for _ in 0..200 {
        for sample in samples {
            pcm.extend_from_slice(&sample.to_le_bytes());
        }
    }
    let mut result = Vec::new();
    result.extend_from_slice(b"RIFF");
    result.extend_from_slice(&(36 + pcm.len() as u32).to_le_bytes());
    result.extend_from_slice(b"WAVEfmt ");
    result.extend_from_slice(&16u32.to_le_bytes());
    result.extend_from_slice(&1u16.to_le_bytes());
    result.extend_from_slice(&1u16.to_le_bytes());
    result.extend_from_slice(&16000u32.to_le_bytes());
    result.extend_from_slice(&32000u32.to_le_bytes());
    result.extend_from_slice(&2u16.to_le_bytes());
    result.extend_from_slice(&16u16.to_le_bytes());
    result.extend_from_slice(b"data");
    result.extend_from_slice(&(pcm.len() as u32).to_le_bytes());
    result.extend_from_slice(&pcm);
    result
}

fn request(mode: AiMode) -> GenerateRequest {
    GenerateRequest {
        id: "fixture-job".into(),
        mode,
        source: "用户原文：Please answer in English.".into(),
        target_language: "日语".into(),
    }
}

fn assert_private_error(error: &str) {
    assert!(!error.contains(FIXTURE_KEY));
    assert!(!error.contains(PRIVATE_MARKER));
}

#[tokio::test]
async fn openai_multipart_carries_exact_wav_and_new_transcription_fields() {
    let audio = wav();
    transport::validate_wav(&audio).unwrap();
    let f = Fixture::new("gpt_asr", Some(&audio));
    let result = transport::transcribe(
        &f.client(),
        &f.route(Provider::Openai, true),
        &audio,
        &Cancellation::new(),
    )
    .await
    .unwrap();
    assert_eq!(result, "今天的转写。");
    f.checked_request();
}

#[tokio::test]
async fn gemini_interactions_carries_exact_wav_and_accepts_only_final_model_text() {
    let audio = wav();
    let f = Fixture::new("gemini_asr", Some(&audio));
    let result = transport::transcribe(
        &f.client(),
        &f.route(Provider::Gemini, true),
        &audio,
        &Cancellation::new(),
    )
    .await
    .unwrap();
    assert_eq!(result, "第一段第二段");
    f.checked_request();
}

#[tokio::test]
async fn incomplete_gemini_interaction_is_not_a_successful_transcript() {
    let audio = wav();
    let f = Fixture::new("gemini_pending", Some(&audio));
    let error = transport::transcribe(
        &f.client(),
        &f.route(Provider::Gemini, true),
        &audio,
        &Cancellation::new(),
    )
    .await
    .err()
    .expect("Pending transcription cannot succeed");
    assert_private_error(&error);
    f.checked_request();
}

#[tokio::test]
async fn openai_split_sse_updates_sparse_tail_while_the_server_is_paused() {
    let f = Fixture::new("openai_sse", None);
    let release = f.release_path();
    let mut snapshots = Vec::new();
    let result = transport::generate(
        &f.client(),
        &f.route(Provider::Openai, false),
        &request(AiMode::Write),
        &Cancellation::new(),
        |text| {
            snapshots.push(text.clone());
            if text == "你好" {
                std::fs::write(&release, b"seen without another network frame").unwrap();
            }
            Ok(())
        },
    )
    .await
    .unwrap();
    assert_eq!(result, "你好，世界！");
    assert!(snapshots.iter().any(|text| text == "你好"));
    assert_eq!(
        f.report()["gateTimedOut"],
        false,
        "Sparse tail must appear before another network frame or DONE"
    );
    f.checked_request();
}

#[tokio::test]
async fn gemini_split_sse_hides_thoughts_and_respects_translation_target() {
    let f = Fixture::new("gemini_sse", None);
    let release = f.release_path();
    let mut snapshots = Vec::new();
    let result = transport::generate(
        &f.client(),
        &f.route(Provider::Gemini, false),
        &request(AiMode::Translate),
        &Cancellation::new(),
        |text| {
            assert!(!text.contains("思考"));
            snapshots.push(text.clone());
            if text == "你好" {
                std::fs::write(&release, b"seen").unwrap();
            }
            Ok(())
        },
    )
    .await
    .unwrap();
    assert_eq!(result, "你好，世界！");
    assert!(snapshots.iter().any(|text| text == "你好"));
    assert_eq!(f.report()["gateTimedOut"], false);
    f.checked_request();
}

#[tokio::test]
async fn cancellation_wakes_a_stream_waiting_for_more_network_data() {
    let f = Fixture::new("openai_sse", None);
    let cancel = Cancellation::new();
    let token = cancel.clone();
    let mut snapshots = Vec::new();
    let result = tokio::time::timeout(
        Duration::from_secs(2),
        transport::generate(
            &f.client(),
            &f.route(Provider::Openai, false),
            &request(AiMode::Ask),
            &cancel,
            |text| {
                snapshots.push(text.clone());
                if text == "你好" {
                    token.cancel();
                }
                Ok(())
            },
        ),
    )
    .await
    .expect("Cancellation must wake the reader before the server's 3s gate timeout");
    assert_private_error(
        &result
            .err()
            .expect("Cancelled stream cannot return a successful partial answer"),
    );
    assert!(snapshots.iter().any(|text| text == "你好"));
    assert!(snapshots.iter().all(|text| !text.contains("世界")));
    f.checked_request();
}

#[tokio::test]
async fn cancellation_wakes_a_file_transcription_waiting_for_response_body() {
    let audio = wav();
    let f = Fixture::new("cancel_asr", Some(&audio));
    let cancel = Cancellation::new();
    let client = f.client();
    let route = f.route(Provider::Openai, true);
    let request = transport::transcribe(&client, &route, &audio, &cancel);
    let cancel_when_body_pauses = async {
        for _ in 0..400 {
            if f.report()["stage"] == "paused" {
                cancel.cancel();
                return;
            }
            tokio::time::sleep(Duration::from_millis(5)).await;
        }
        panic!("Fixture did not receive and pause the transcription response");
    };
    let (response, ()) = tokio::join!(
        tokio::time::timeout(Duration::from_secs(2), request),
        cancel_when_body_pauses
    );
    let error = response
        .expect("Cancel must not wait for a response's remaining bytes")
        .err()
        .expect("Cancelled audio cannot return a transcript");
    assert_private_error(&error);
    f.checked_request();
}

#[tokio::test]
async fn server_errors_do_not_echo_response_bodies_or_credentials() {
    let f = Fixture::new("error", None);
    let error = transport::generate(
        &f.client(),
        &f.route(Provider::Openai, false),
        &request(AiMode::Ask),
        &Cancellation::new(),
        |_| Ok(()),
    )
    .await
    .err()
    .expect("HTTP401 cannot succeed");
    assert_private_error(&error);
    assert_eq!(
        f.report()["requests"],
        1,
        "The expected HTTP response must reach the real client"
    );
    let audio = wav();
    let f = Fixture::new("error", Some(&audio));
    let error = transport::transcribe(
        &f.client(),
        &f.route(Provider::Openai, true),
        &audio,
        &Cancellation::new(),
    )
    .await
    .err()
    .expect("HTTP401 cannot succeed");
    assert_private_error(&error);
    assert_eq!(
        f.report()["requests"],
        1,
        "The expected HTTP response must reach the real client"
    );
}

#[tokio::test]
async fn redirects_are_not_followed_with_an_ai_credential() {
    let f = Fixture::new("redirect", None);
    let error = transport::generate(
        &f.client(),
        &f.route(Provider::Openai, false),
        &request(AiMode::Write),
        &Cancellation::new(),
        |_| Ok(()),
    )
    .await
    .err()
    .expect("Redirect cannot produce an answer");
    assert_private_error(&error);
    let report = f.report();
    assert_eq!(report["requests"], 1);
    assert!(report.get("followedRedirect").is_none());
}

#[tokio::test]
async fn oversized_transcription_response_is_rejected_instead_of_truncated() {
    let audio = wav();
    let f = Fixture::new("oversized_asr", Some(&audio));
    let error = transport::transcribe(
        &f.client(),
        &f.route(Provider::Openai, true),
        &audio,
        &Cancellation::new(),
    )
    .await
    .err()
    .expect("An over-budget transcript is an error, not a shortened success");
    assert_private_error(&error);
    assert!(!error.contains(&"长".repeat(64)));
    assert_eq!(f.report()["requests"], 1);
}

#[tokio::test]
async fn self_signed_fixture_is_rejected_without_explicit_test_ca_trust() {
    let f = Fixture::new("openai_sse", None);
    let error = transport::generate(
        &f.untrusted_client(),
        &f.route(Provider::Openai, false),
        &request(AiMode::Ask),
        &Cancellation::new(),
        |_| Ok(()),
    )
    .await
    .err()
    .expect("TLS verification cannot silently accept an untrusted CA");
    assert_private_error(&error);
    assert_eq!(
        f.report()["requests"],
        0,
        "An untrusted TLS peer must not receive the request/credential"
    );
}

#[tokio::test]
async fn incomplete_stream_and_model_token_limit_never_succeed_with_partial_text() {
    for scenario in [
        "truncated_sse",
        "length_sse",
        "json_length",
        "oversized_event",
    ] {
        let fixture = Fixture::new(scenario, None);
        let result = transport::generate(
            &fixture.client(),
            &fixture.route(Provider::Openai, false),
            &request(AiMode::Write),
            &Cancellation::new(),
            |_| Ok(()),
        )
        .await;
        let error = result
            .err()
            .expect("Incomplete or oversized output must fail rather than silently truncate");
        assert_private_error(&error);
        fixture.checked_request();
    }
}

#[tokio::test]
async fn nonstreaming_json_fallback_delivers_the_complete_validated_final_answer() {
    let fixture = Fixture::new("json_text", None);
    let mut snapshots = Vec::new();
    let result = transport::generate(
        &fixture.client(),
        &fixture.route(Provider::Openai, false),
        &request(AiMode::Rewrite),
        &Cancellation::new(),
        |text| {
            snapshots.push(text);
            Ok(())
        },
    )
    .await
    .unwrap();
    assert_eq!(result, "完整的中文结果。");
    assert_eq!(snapshots, vec![result]);
    fixture.checked_request();
}
