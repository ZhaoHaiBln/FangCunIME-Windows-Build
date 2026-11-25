//! Optional AI for explicit user requests. Ordinary input never calls this module.
#[path = "ai_secrets.rs"]
mod secrets;
#[path = "ai_transport.rs"]
mod transport;

use base64::{engine::general_purpose::STANDARD as B64, Engine};
use reqwest::{Client, Url};
use secrets::{SecretCipher, WindowsCipher};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, HashMap};
use std::fs::File;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, Weak};
use std::time::Duration;
use tokio::sync::watch;
use zeroize::Zeroizing;

pub(super) const MAX_CONFIG_BYTES: usize = 256 * 1024;
pub(super) const MAX_OUTPUT_BYTES: usize = 256 * 1024;
pub(super) const MAX_WAV_BYTES: usize = 44 + 60 * 16_000 * 2;
const MAX_KEY_BYTES: usize = 8192;
const MAX_KEY_RECORDS: usize = 32;

#[derive(Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Provider {
    Openai,
    Gemini,
    Deepseek,
    Volcengine,
    Siliconflow,
    Compatible,
}
impl Provider {
    pub(super) fn id(self) -> &'static str {
        match self {
            Self::Openai => "openai",
            Self::Gemini => "gemini",
            Self::Deepseek => "deepseek",
            Self::Volcengine => "volcengine",
            Self::Siliconflow => "siliconflow",
            Self::Compatible => "compatible",
        }
    }
    pub(super) fn default_base(self) -> &'static str {
        match self {
            Self::Openai | Self::Compatible => "https://api.openai.com/v1",
            Self::Gemini => "https://generativelanguage.googleapis.com/v1beta",
            Self::Deepseek => "https://api.deepseek.com",
            Self::Volcengine => "https://ark.cn-beijing.volces.com/api/v3",
            Self::Siliconflow => "https://api.siliconflow.cn/v1",
        }
    }
}

#[derive(Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct AiRoute {
    pub provider: Provider,
    pub base_url: String,
    pub model: String,
}

#[derive(Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
enum Role {
    Text,
    Speech,
}
impl Role {
    fn id(self) -> &'static str {
        if self == Self::Text {
            "text"
        } else {
            "speech"
        }
    }
}

#[derive(Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct StoredKey {
    role: Role,
    provider: Provider,
    origin: String,
    encrypted: String,
}

#[derive(Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct ConfigFile {
    version: u32,
    enabled: bool,
    text: AiRoute,
    speech: AiRoute,
    keys: BTreeMap<String, StoredKey>,
}
impl Default for ConfigFile {
    fn default() -> Self {
        Self {
            version: 1,
            enabled: false,
            text: AiRoute {
                provider: Provider::Deepseek,
                base_url: Provider::Deepseek.default_base().into(),
                model: "deepseek-flash".into(),
            },
            speech: AiRoute {
                provider: Provider::Openai,
                base_url: Provider::Openai.default_base().into(),
                model: "gpt-transcribe".into(),
            },
            keys: BTreeMap::new(),
        }
    }
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct AiSnapshot {
    pub enabled: bool,
    pub text: AiRoute,
    pub speech: AiRoute,
    pub text_key_saved: bool,
    pub speech_key_saved: bool,
    pub revision: String,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct AiDraft {
    pub enabled: bool,
    pub text: AiRoute,
    pub speech: AiRoute,
    #[serde(default)]
    pub text_key: Option<String>,
    #[serde(default)]
    pub speech_key: Option<String>,
    #[serde(default)]
    pub clear_text_key: bool,
    #[serde(default)]
    pub clear_speech_key: bool,
}

#[derive(Clone, Copy, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum AiMode {
    Write,
    Rewrite,
    Translate,
    Ask,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct GenerateRequest {
    pub id: String,
    pub mode: AiMode,
    pub source: String,
    #[serde(default)]
    pub target_language: String,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct TranscribeRequest {
    pub id: String,
    #[serde(deserialize_with = "deserialize_wav")]
    pub wav: Vec<u8>,
}

fn deserialize_wav<'de, D: serde::Deserializer<'de>>(deserializer: D) -> Result<Vec<u8>, D::Error> {
    struct BoundedAudio;
    impl<'de> serde::de::Visitor<'de> for BoundedAudio {
        type Value = Vec<u8>;
        fn expecting(&self, formatter: &mut std::fmt::Formatter) -> std::fmt::Result {
            formatter.write_str("最多 60 秒的 PCM16 WAV 字节数组")
        }
        fn visit_seq<A: serde::de::SeqAccess<'de>>(
            self,
            mut sequence: A,
        ) -> Result<Vec<u8>, A::Error> {
            if sequence
                .size_hint()
                .is_some_and(|size| size > MAX_WAV_BYTES)
            {
                return Err(serde::de::Error::custom("录音超过 60 秒，请分段录入"));
            }
            let mut bytes = Zeroizing::new(Vec::with_capacity(
                sequence.size_hint().unwrap_or(0).min(MAX_WAV_BYTES),
            ));
            while let Some(byte) = sequence.next_element::<u8>()? {
                if bytes.len() == MAX_WAV_BYTES {
                    return Err(serde::de::Error::custom("录音超过 60 秒，请分段录入"));
                }
                bytes.push(byte);
            }
            Ok(std::mem::take(&mut *bytes))
        }
    }
    deserializer.deserialize_seq(BoundedAudio)
}
#[derive(Clone, Serialize)]
pub struct AiProgress {
    pub id: String,
    pub phase: &'static str,
    pub text: String,
}
pub type ProgressSink = Arc<dyn Fn(AiProgress) -> bool + Send + Sync>;

#[derive(Clone)]
pub(super) struct Cancellation(watch::Sender<bool>);
impl Cancellation {
    fn new() -> Self {
        Self(watch::channel(false).0)
    }
    fn cancel(&self) {
        self.0.send_replace(true);
    }
    pub(super) fn is_cancelled(&self) -> bool {
        *self.0.borrow()
    }
    pub(super) async fn cancelled(&self) {
        let mut receiver = self.0.subscribe();
        loop {
            if *receiver.borrow_and_update() {
                return;
            }
            if receiver.changed().await.is_err() {
                return;
            }
        }
    }
}
struct Job {
    owner: String,
    id: String,
    cancel: Cancellation,
}
struct Lease {
    manager: Weak<AiManager>,
    job: Arc<Job>,
}
impl Drop for Lease {
    fn drop(&mut self) {
        if let Some(manager) = self.manager.upgrade() {
            if let Ok(mut jobs) = manager.jobs.lock() {
                if jobs
                    .get(&self.job.owner)
                    .is_some_and(|current| Arc::ptr_eq(current, &self.job))
                {
                    jobs.remove(&self.job.owner);
                }
            }
        }
    }
}

pub struct AiManager {
    path: PathBuf,
    cipher: Arc<dyn SecretCipher>,
    client: Option<Client>,
    jobs: Mutex<HashMap<String, Arc<Job>>>,
}

pub(super) struct PreparedRoute {
    pub route: AiRoute,
    pub key: Zeroizing<String>,
}

impl AiManager {
    pub fn new(data_dir: PathBuf) -> Self {
        Self {
            path: data_dir.join("ai-config.json"),
            cipher: Arc::new(WindowsCipher),
            client: Client::builder()
                .redirect(reqwest::redirect::Policy::none())
                .connect_timeout(Duration::from_secs(10))
                .timeout(Duration::from_secs(120))
                .build()
                .ok(),
            jobs: Mutex::new(HashMap::new()),
        }
    }

    pub fn get_config(&self) -> Result<AiSnapshot, String> {
        let _lock = crate::settings_io::lock(&self.path)?;
        let (stored, revision) = load_file(&self.path)?;
        snapshot(stored, revision)
    }

    pub fn save_config(&self, mut draft: AiDraft, revision: &str) -> Result<AiSnapshot, String> {
        // Own the IPC plaintext in zeroizing allocations before any fallible work.
        let text_key = draft.text_key.take().map(Zeroizing::new);
        let speech_key = draft.speech_key.take().map(Zeroizing::new);
        let text = validate_route(draft.text, Role::Text)?;
        let speech = validate_route(draft.speech, Role::Speech)?;
        let _lock = crate::settings_io::lock(&self.path)?;
        let (mut stored, current) = load_file(&self.path)?;
        if current != revision {
            return Err("AI 设置已被其他窗口修改，请重新读取后保存".into());
        }
        put_key(
            &mut stored,
            self.cipher.as_ref(),
            Role::Text,
            &text,
            text_key.as_deref().map(String::as_str),
            draft.clear_text_key,
        )?;
        put_key(
            &mut stored,
            self.cipher.as_ref(),
            Role::Speech,
            &speech,
            speech_key.as_deref().map(String::as_str),
            draft.clear_speech_key,
        )?;
        stored.enabled = draft.enabled;
        stored.text = text;
        stored.speech = speech;
        if stored.keys.len() > MAX_KEY_RECORDS {
            return Err("保存的 AI 地址过多，请删除旧地址的凭证后重试".into());
        }
        let body =
            serde_json::to_string_pretty(&stored).map_err(|_| "无法保存 AI 设置".to_string())?;
        crate::settings_io::replace_limited(&self.path, &body, MAX_CONFIG_BYTES)?;
        // Credentials/routes and disabling invalidate every old in-flight request.
        self.cancel_all();
        snapshot(stored, digest(body.as_bytes()))
    }

    fn prepare(&self, role: Role) -> Result<PreparedRoute, String> {
        let _lock = crate::settings_io::lock(&self.path)?;
        let (config, _) = load_file(&self.path)?;
        if !config.enabled {
            return Err("请先开启可选 AI 功能并保存设置".into());
        }
        let route = if role == Role::Text {
            config.text
        } else {
            config.speech
        };
        let origin = origin(&route)?;
        let scope = key_scope(role, route.provider, &origin);
        let record = config.keys.get(&digest(scope.as_bytes())).ok_or_else(|| {
            if role == Role::Text {
                "请先为当前文字服务地址保存 API Key"
            } else {
                "请先为当前语音服务地址保存 API Key"
            }
            .to_string()
        })?;
        let encrypted = B64
            .decode(&record.encrypted)
            .map_err(|_| "AI 凭证数据无效，请重新保存".to_string())?;
        let plain = self.cipher.open(&scope, &encrypted)?;
        let key = Zeroizing::new(
            String::from_utf8(plain.to_vec())
                .map_err(|_| "AI 凭证数据无效，请重新保存".to_string())?,
        );
        validate_key(&key)?;
        Ok(PreparedRoute { route, key })
    }

    fn begin(self: &Arc<Self>, owner: &str, id: &str) -> Result<Lease, String> {
        if owner.is_empty()
            || owner.len() > 128
            || id.is_empty()
            || id.len() > 96
            || !id
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || b"_-:.".contains(&byte))
        {
            return Err("AI 请求标识不正确，请重新操作".into());
        }
        let job = Arc::new(Job {
            owner: owner.into(),
            id: id.into(),
            cancel: Cancellation::new(),
        });
        let mut jobs = self
            .jobs
            .lock()
            .map_err(|_| "AI 状态暂不可用".to_string())?;
        if jobs.len() >= 8 && !jobs.contains_key(owner) {
            return Err("AI 窗口过多，请关闭多余窗口".into());
        }
        if let Some(previous) = jobs.insert(owner.into(), Arc::clone(&job)) {
            previous.cancel.cancel();
        }
        Ok(Lease {
            manager: Arc::downgrade(self),
            job,
        })
    }

    fn active(&self, job: &Arc<Job>) -> bool {
        !job.cancel.is_cancelled()
            && self
                .jobs
                .lock()
                .ok()
                .and_then(|jobs| {
                    jobs.get(&job.owner)
                        .map(|current| Arc::ptr_eq(current, job))
                })
                .unwrap_or(false)
    }

    fn emit(
        &self,
        job: &Arc<Job>,
        phase: &'static str,
        text: String,
        sink: &ProgressSink,
    ) -> Result<(), String> {
        let jobs = self
            .jobs
            .lock()
            .map_err(|_| "AI 状态暂不可用".to_string())?;
        if job.cancel.is_cancelled()
            || !jobs
                .get(&job.owner)
                .is_some_and(|current| Arc::ptr_eq(current, job))
        {
            return Err("已取消".into());
        }
        if !sink(AiProgress {
            id: job.id.clone(),
            phase,
            text,
        }) {
            job.cancel.cancel();
            return Err("AI 窗口已关闭，本次操作已取消".into());
        }
        Ok(())
    }

    pub fn cancel(&self, owner: &str, id: &str) {
        if let Ok(mut jobs) = self.jobs.lock() {
            if jobs.get(owner).is_some_and(|job| job.id == id) {
                if let Some(job) = jobs.remove(owner) {
                    job.cancel.cancel();
                }
            }
        }
    }
    pub fn cancel_window(&self, owner: &str) {
        if let Ok(mut jobs) = self.jobs.lock() {
            if let Some(job) = jobs.remove(owner) {
                job.cancel.cancel();
            }
        }
    }
    pub fn cancel_all(&self) {
        if let Ok(mut jobs) = self.jobs.lock() {
            for (_, job) in jobs.drain() {
                job.cancel.cancel();
            }
        }
    }

    pub async fn generate(
        self: Arc<Self>,
        owner: String,
        request: GenerateRequest,
        sink: ProgressSink,
    ) -> Result<String, String> {
        let lease = self.begin(&owner, &request.id)?;
        transport::validate_request(&request)?;
        let manager = Arc::clone(&self);
        let route = tokio::task::spawn_blocking(move || manager.prepare(Role::Text))
            .await
            .map_err(|_| "读取 AI 配置未完成，请重试".to_string())??;
        if !self.active(&lease.job) {
            return Err("已取消".into());
        }
        let client = self
            .client
            .as_ref()
            .ok_or("AI 网络组件未能启动，请重新打开方寸设置")?;
        let job = Arc::clone(&lease.job);
        let manager = Arc::clone(&self);
        let progress = |text| manager.emit(&job, "generating", text, &sink);
        let result = transport::generate(client, &route, &request, &job.cancel, progress).await?;
        if !self.active(&job) {
            return Err("已取消".into());
        }
        Ok(result)
    }

    pub async fn transcribe(
        self: Arc<Self>,
        owner: String,
        mut request: TranscribeRequest,
        sink: ProgressSink,
    ) -> Result<String, String> {
        let wav = Zeroizing::new(std::mem::take(&mut request.wav));
        let lease = self.begin(&owner, &request.id)?;
        transport::validate_wav(&wav)?;
        let manager = Arc::clone(&self);
        let route = tokio::task::spawn_blocking(move || manager.prepare(Role::Speech))
            .await
            .map_err(|_| "读取语音配置未完成，请重试".to_string())??;
        if !self.active(&lease.job) {
            return Err("已取消".into());
        }
        let client = self
            .client
            .as_ref()
            .ok_or("语音网络组件未能启动，请重新打开方寸设置")?;
        self.emit(&lease.job, "transcribing", String::new(), &sink)?;
        let result = transport::transcribe(client, &route, &wav, &lease.job.cancel).await?;
        self.emit(&lease.job, "transcribing", result.clone(), &sink)?;
        if !self.active(&lease.job) {
            return Err("已取消".into());
        }
        Ok(result)
    }
}

fn digest(bytes: &[u8]) -> String {
    format!("{:x}", Sha256::digest(bytes))
}
fn key_scope(role: Role, provider: Provider, origin: &str) -> String {
    format!("{}\0{}\0{origin}", role.id(), provider.id())
}
fn origin(route: &AiRoute) -> Result<String, String> {
    Ok(secure_url(&route.base_url)?.origin().ascii_serialization())
}

pub(super) fn secure_url(raw: &str) -> Result<Url, String> {
    if raw.len() > 2048 || raw.trim().is_empty() {
        return Err("请填写不含密钥的 HTTPS 服务地址".into());
    }
    let url = Url::parse(raw.trim()).map_err(|_| "AI 服务地址格式不正确".to_string())?;
    if url.scheme() != "https"
        || url.host_str().is_none()
        || !url.username().is_empty()
        || url.password().is_some()
        || url.query().is_some()
        || url.fragment().is_some()
    {
        return Err("AI 服务地址须使用 HTTPS，且不要包含账户、密钥、查询参数或片段".into());
    }
    Ok(url)
}

fn validate_route(mut route: AiRoute, role: Role) -> Result<AiRoute, String> {
    route.base_url = secure_url(&route.base_url)?
        .as_str()
        .trim_end_matches('/')
        .to_string();
    route.model = route.model.trim().to_string();
    if route.model.is_empty()
        || route.model.len() > 128
        || !route.model.bytes().all(|byte| (33..=126).contains(&byte))
    {
        return Err("请填写有效的 AI 模型名称".into());
    }
    if role == Role::Speech
        && !matches!(
            (route.provider, route.model.as_str()),
            (Provider::Openai, "gpt-transcribe") | (Provider::Gemini, "gemini-3.5-transcribe")
        )
    {
        return Err("Windows 语音目前支持 OpenAI 或 Gemini 录音转写，请选择对应文件模型".into());
    }
    if role == Role::Text
        && matches!(
            route.model.as_str(),
            "gpt-transcribe" | "gpt-live-transcribe" | "gemini-3.5-transcribe"
        )
    {
        return Err("文字 AI 请选择文字模型，录音转写模型仅用于语音".into());
    }
    if route.provider == Provider::Gemini {
        let model = route.model.strip_prefix("models/").unwrap_or(&route.model);
        if model.is_empty()
            || !model
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || b"._-".contains(&byte))
        {
            return Err("Gemini 模型名称不正确".into());
        }
    }
    Ok(route)
}

fn validate_key(key: &str) -> Result<(), String> {
    if key.is_empty()
        || key.len() > MAX_KEY_BYTES
        || !key.bytes().all(|byte| (33..=126).contains(&byte))
    {
        Err("API Key 格式不正确，请只填写密钥内容".into())
    } else {
        Ok(())
    }
}

fn put_key(
    config: &mut ConfigFile,
    cipher: &dyn SecretCipher,
    role: Role,
    route: &AiRoute,
    key: Option<&str>,
    clear: bool,
) -> Result<(), String> {
    if clear && key.is_some() {
        return Err("不能同时保存和删除同一 API Key".into());
    }
    let origin = origin(route)?;
    let scope = key_scope(role, route.provider, &origin);
    let id = digest(scope.as_bytes());
    if clear {
        config.keys.remove(&id);
    } else if let Some(raw) = key {
        let clean = raw.trim();
        validate_key(clean)?;
        config.keys.insert(
            id,
            StoredKey {
                role,
                provider: route.provider,
                origin,
                encrypted: B64.encode(cipher.seal(&scope, clean.as_bytes())?),
            },
        );
    }
    Ok(())
}

fn snapshot(config: ConfigFile, revision: String) -> Result<AiSnapshot, String> {
    let text_scope = key_scope(Role::Text, config.text.provider, &origin(&config.text)?);
    let speech_scope = key_scope(
        Role::Speech,
        config.speech.provider,
        &origin(&config.speech)?,
    );
    Ok(AiSnapshot {
        enabled: config.enabled,
        text_key_saved: config.keys.contains_key(&digest(text_scope.as_bytes())),
        speech_key_saved: config.keys.contains_key(&digest(speech_scope.as_bytes())),
        text: config.text,
        speech: config.speech,
        revision,
    })
}

fn load_file(path: &Path) -> Result<(ConfigFile, String), String> {
    let mut file = match File::open(path) {
        Ok(file) => file,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            return Ok((ConfigFile::default(), digest(b"FangCunAI/absent/v1")))
        }
        Err(_) => return Err("无法读取 AI 配置，已保留原文件，请检查权限".into()),
    };
    if !file
        .metadata()
        .map_err(|_| "无法读取 AI 配置文件信息")?
        .is_file()
    {
        return Err("AI 配置文件无效，已保留原文件".into());
    }
    let mut bytes = Vec::new();
    Read::by_ref(&mut file)
        .take(MAX_CONFIG_BYTES as u64 + 1)
        .read_to_end(&mut bytes)
        .map_err(|_| "读取 AI 配置失败，已保留原文件".to_string())?;
    if bytes.len() > MAX_CONFIG_BYTES {
        return Err("AI 配置文件过大，已保留原文件".into());
    }
    let mut stored: ConfigFile =
        serde_json::from_slice(&bytes).map_err(|_| "AI 配置文件损坏，已保留原文件".to_string())?;
    if stored.version != 1 || stored.keys.len() > MAX_KEY_RECORDS {
        return Err("AI 配置版本或凭证数量无效，已保留原文件".into());
    }
    stored.text = validate_route(stored.text, Role::Text)?;
    stored.speech = validate_route(stored.speech, Role::Speech)?;
    for (id, key) in &stored.keys {
        let parsed = secure_url(&key.origin)?;
        if parsed.origin().ascii_serialization() != key.origin
            || parsed.path() != "/"
            || *id != digest(key_scope(key.role, key.provider, &key.origin).as_bytes())
            || key.encrypted.len() > 48_000
            || B64
                .decode(&key.encrypted)
                .map_or(true, |bytes| bytes.is_empty() || bytes.len() > 32_768)
        {
            return Err("AI 加密凭证记录无效，已保留原文件".into());
        }
    }
    Ok((stored, digest(&bytes)))
}

#[cfg(test)]
#[path = "ai_tests.rs"]
mod tests;

#[cfg(test)]
#[path = "ai_protocol_tests.rs"]
mod protocol_tests;
