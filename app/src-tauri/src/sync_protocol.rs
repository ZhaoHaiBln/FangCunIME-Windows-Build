//! FangCun Clipboard Share v1. Payloads and secrets deliberately do not implement Debug.
use aes_gcm::{
    aead::{Aead, KeyInit, Payload as AeadPayload},
    Aes256Gcm, Nonce,
};
use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
use rand::{rngs::OsRng, RngCore};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};
use zeroize::Zeroize;

pub const DEFAULT_SERVER: &str = "https://clip.kivohub.site";
pub const DEFAULT_TOPIC: &str = "fangcun-clipboard";
pub const MAX_FRAME_BYTES: usize = 3800;
pub const MAX_TEXT_BYTES: usize = 32768;
pub const MAX_PAYLOAD_BYTES: usize = 65536;
pub const MAX_CIPHER_BYTES: usize = MAX_PAYLOAD_BYTES + 16;
pub const CHUNK_BYTES: usize = 2400;
pub const MAX_PARTS: u32 = 28;
pub const DAY_MS: i64 = 86_400_000;
const GROUP_TTL_MS: i64 = 600_000;
const MAX_GROUPS: usize = 16;
const MAX_PENDING_BYTES: usize = 1_048_576;
const MAX_SEEN: usize = 50_000;
const PAIR_PREFIX: &str = "fangcun-clip-v1:";

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ProtocolError(pub &'static str);
impl std::fmt::Display for ProtocolError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.0)
    }
}
impl std::error::Error for ProtocolError {}
pub type Result<T> = std::result::Result<T, ProtocolError>;

#[derive(Clone)]
pub struct SharedKey([u8; 32]);
impl Drop for SharedKey {
    fn drop(&mut self) {
        self.0.zeroize();
    }
}
impl SharedKey {
    pub fn parse(value: &str) -> Result<Self> {
        let mut bytes = decode(value, 32)?;
        if bytes.len() != 32 {
            return Err(ProtocolError("共享密钥必须为32字节"));
        }
        let mut key = [0; 32];
        key.copy_from_slice(&bytes);
        bytes.zeroize();
        Ok(Self(key))
    }
    pub fn generate() -> Result<Self> {
        let mut key = [0; 32];
        random(&mut key)?;
        Ok(Self(key))
    }
    pub fn export(&self) -> String {
        URL_SAFE_NO_PAD.encode(self.0)
    }
    pub fn kid(&self) -> String {
        hex(&Sha256::digest(self.0)[..16])
    }
}

#[derive(Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct Endpoint {
    pub server: String,
    pub topic: String,
}
impl Endpoint {
    pub fn new(server: &str, topic: &str) -> Result<Self> {
        if server.len() > 2048
            || server.chars().any(char::is_control)
            || server.trim() != server
            || server
                .split_once("://")
                .is_some_and(|(_, a)| a.split('/').next().unwrap_or("").contains('@'))
            || topic.is_empty()
            || topic.len() > 64
            || !topic
                .bytes()
                .all(|c| c.is_ascii_alphanumeric() || c == b'_' || c == b'-')
        {
            return Err(ProtocolError("服务地址或话题格式不正确"));
        }
        let url = reqwest::Url::parse(server).map_err(|_| ProtocolError("服务地址格式不正确"))?;
        if url.scheme() != "https"
            || url.host_str().is_none()
            || !url.username().is_empty()
            || url.password().is_some()
            || url.query().is_some()
            || url.fragment().is_some()
            || url.path() != "/"
        {
            return Err(ProtocolError("服务地址必须是无路径和凭证的HTTPS站点"));
        }
        Ok(Self {
            server: url.as_str().trim_end_matches('/').to_string(),
            topic: topic.to_string(),
        })
    }
    pub fn validate(&self) -> Result<Self> {
        Self::new(&self.server, &self.topic)
    }
}

#[derive(Clone)]
pub struct Pairing {
    pub endpoint: Endpoint,
    pub key: SharedKey,
}
#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct PairingWire {
    v: u32,
    server: String,
    topic: String,
    key: String,
}
impl Pairing {
    pub fn parse(encoded: &str) -> Result<Self> {
        if encoded.len() > 8192 {
            return Err(ProtocolError("配对资料过长"));
        }
        let value = encoded
            .strip_prefix(PAIR_PREFIX)
            .ok_or(ProtocolError("不是方寸v1配对资料"))?;
        let raw = decode(value, 6144)?;
        let wire: PairingWire =
            serde_json::from_slice(&raw).map_err(|_| ProtocolError("配对资料格式不正确"))?;
        if wire.v != 1 {
            return Err(ProtocolError("不支持该配对版本"));
        }
        Ok(Self {
            endpoint: Endpoint::new(&wire.server, &wire.topic)?,
            key: SharedKey::parse(&wire.key)?,
        })
    }
    pub fn export(&self) -> Result<String> {
        let endpoint = self.endpoint.validate()?;
        let wire = PairingWire {
            v: 1,
            server: endpoint.server,
            topic: endpoint.topic,
            key: self.key.export(),
        };
        let raw = serde_json::to_vec(&wire).map_err(|_| ProtocolError("无法生成配对资料"))?;
        Ok(format!("{PAIR_PREFIX}{}", URL_SAFE_NO_PAD.encode(raw)))
    }
}

#[derive(Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct Payload {
    pub v: u32,
    pub sender_id: String,
    pub sender_name: String,
    pub created_at: i64,
    pub expires_at: i64,
    pub text: String,
}
impl Payload {
    pub fn new(sender_id: &str, sender_name: &str, text: &str, now: i64) -> Result<Self> {
        let value = Self {
            v: 1,
            sender_id: sender_id.to_string(),
            sender_name: sender_name.to_string(),
            created_at: now,
            expires_at: now
                .checked_add(DAY_MS)
                .ok_or(ProtocolError("时间超出范围"))?,
            text: text.to_string(),
        };
        value.validate(now)?;
        Ok(value)
    }
    pub fn validate(&self, now: i64) -> Result<()> {
        let lifetime = self
            .expires_at
            .checked_sub(self.created_at)
            .ok_or(ProtocolError("消息时间不正确"))?;
        if self.v != 1 || !is_id(&self.sender_id) {
            return Err(ProtocolError("消息版本或设备标识不正确"));
        }
        if self.created_at < 0
            || lifetime <= 0
            || lifetime > DAY_MS
            || self.expires_at <= now
            || self.created_at > now.saturating_add(300_000)
        {
            return Err(ProtocolError("共享内容已过期或设备时间不正确"));
        }
        let name_chars = self.sender_name.chars().count();
        if name_chars == 0 || name_chars > 64 || self.sender_name.len() > 256 {
            return Err(ProtocolError("设备名称须为1至64个字符"));
        }
        if self.text.is_empty() || self.text.len() > MAX_TEXT_BYTES {
            return Err(ProtocolError("共享文字须为1至32768字节"));
        }
        Ok(())
    }
}

#[derive(Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct Frame {
    pub v: u32,
    pub mid: String,
    pub kid: String,
    pub i: u32,
    pub n: u32,
    pub iv: String,
    pub data: String,
}
impl Frame {
    pub fn parse(text: &str) -> Result<Self> {
        if text.len() > MAX_FRAME_BYTES {
            return Err(ProtocolError("共享分片超出大小限制"));
        }
        let frame: Self =
            serde_json::from_str(text).map_err(|_| ProtocolError("共享分片格式不正确"))?;
        frame.validate()?;
        Ok(frame)
    }
    pub fn to_json(&self) -> Result<String> {
        self.validate()?;
        let result = serde_json::to_string(self).map_err(|_| ProtocolError("无法编码共享分片"))?;
        if result.len() > MAX_FRAME_BYTES {
            return Err(ProtocolError("共享分片超出大小限制"));
        }
        Ok(result)
    }
    fn validate(&self) -> Result<(Vec<u8>, Vec<u8>)> {
        if self.v != 1
            || !is_id(&self.mid)
            || !is_id(&self.kid)
            || self.n == 0
            || self.n > MAX_PARTS
            || self.i >= self.n
        {
            return Err(ProtocolError("共享分片标识或序号不正确"));
        }
        let iv = decode(&self.iv, 12)?;
        let data = decode(&self.data, CHUNK_BYTES)?;
        if iv.len() != 12 || data.is_empty() || (self.i + 1 < self.n && data.len() != CHUNK_BYTES) {
            return Err(ProtocolError("共享分片长度不正确"));
        }
        Ok((iv, data))
    }
}

pub fn new_id() -> Result<String> {
    let mut bytes = [0; 16];
    random(&mut bytes)?;
    Ok(hex(&bytes))
}
pub fn encrypt_payload(payload: &Payload, key: &SharedKey, now: i64) -> Result<Vec<Frame>> {
    payload.validate(now)?;
    let bytes = serde_json::to_vec(payload).map_err(|_| ProtocolError("无法编码共享内容"))?;
    let mut iv = [0; 12];
    random(&mut iv)?;
    encrypt_bytes(&bytes, key, &new_id()?, iv, now)
}
fn encrypt_bytes(
    bytes: &[u8],
    key: &SharedKey,
    mid: &str,
    iv: [u8; 12],
    now: i64,
) -> Result<Vec<Frame>> {
    if bytes.len() > MAX_PAYLOAD_BYTES || !is_id(mid) {
        return Err(ProtocolError("共享内容超出限制"));
    }
    let parsed: Payload =
        serde_json::from_slice(bytes).map_err(|_| ProtocolError("共享内容格式不正确"))?;
    parsed.validate(now)?;
    let n = (bytes.len() + 16).div_ceil(CHUNK_BYTES) as u32;
    if n == 0 || n > MAX_PARTS {
        return Err(ProtocolError("共享分片数超出限制"));
    }
    let kid = key.kid();
    let aad = aad(mid, &kid, n);
    let cipher = Aes256Gcm::new_from_slice(&key.0).map_err(|_| ProtocolError("共享密钥不正确"))?;
    let ciphertext = cipher
        .encrypt(
            Nonce::from_slice(&iv),
            AeadPayload {
                msg: bytes,
                aad: aad.as_bytes(),
            },
        )
        .map_err(|_| ProtocolError("共享内容加密失败"))?;
    ciphertext
        .chunks(CHUNK_BYTES)
        .enumerate()
        .map(|(i, chunk)| {
            let frame = Frame {
                v: 1,
                mid: mid.to_string(),
                kid: kid.clone(),
                i: i as u32,
                n,
                iv: URL_SAFE_NO_PAD.encode(iv),
                data: URL_SAFE_NO_PAD.encode(chunk),
            };
            frame.to_json()?;
            Ok(frame)
        })
        .collect()
}

#[derive(Clone, Serialize, Deserialize)]
struct PendingGroup {
    kid: String,
    mid: String,
    iv: Vec<u8>,
    n: u32,
    started_at: i64,
    chunks: Vec<Option<Vec<u8>>>,
}
#[derive(Clone, Default, Serialize, Deserialize)]
pub struct ReceiveState {
    #[serde(default)]
    pub cursor: Option<String>,
    #[serde(default)]
    pending: Vec<PendingGroup>,
    #[serde(default)]
    completed: BTreeMap<String, i64>,
    #[serde(default)]
    event_ids: BTreeMap<String, i64>,
    /// Only unfinished groups retain ownership. Expiry/eviction must permit these event IDs to replay.
    #[serde(default)]
    event_groups: BTreeMap<String, String>,
    #[serde(default)]
    recovery_generation: u64,
    #[serde(default)]
    needs_recovery: bool,
}
#[derive(Clone, Serialize, Deserialize)]
pub struct ReceivedMessage {
    pub mid: String,
    pub kid: String,
    pub payload: Payload,
}
pub enum ReceiveOutcome {
    Partial,
    Duplicate,
    Ignored,
    Delivered(ReceivedMessage),
    Rejected(&'static str),
}
impl ReceiveState {
    /// Mutates a candidate snapshot. Persist it atomically before replacing live state or displaying Delivered.
    /// Invalid authenticated/frame data is also acknowledged locally, preventing poison-event retry loops.
    pub fn process(
        &mut self,
        event_id: &str,
        frame_json: &str,
        key: &SharedKey,
        local_sender_id: &str,
        now: i64,
    ) -> ReceiveOutcome {
        self.prune(now);
        if !valid_event_id(event_id) {
            return ReceiveOutcome::Rejected("服务消息标识不正确");
        }
        if self.event_ids.contains_key(event_id) {
            return ReceiveOutcome::Duplicate;
        }
        let result = self.process_frame(frame_json, key, local_sender_id, now);
        if matches!(
            &result,
            Ok(ReceiveOutcome::Partial | ReceiveOutcome::Duplicate)
        ) {
            if let Ok(frame) = Frame::parse(frame_json) {
                if self
                    .pending
                    .iter()
                    .any(|group| group.kid == frame.kid && group.mid == frame.mid)
                {
                    self.event_groups
                        .insert(event_id.to_string(), format!("{}:{}", frame.kid, frame.mid));
                }
            }
        }
        self.event_ids.insert(event_id.to_string(), now);
        trim_seen(&mut self.event_ids);
        let ids = &self.event_ids;
        self.event_groups.retain(|id, _| ids.contains_key(id));
        self.cursor = Some(event_id.to_string());
        match result {
            Ok(value) => value,
            Err(error) => ReceiveOutcome::Rejected(error.0),
        }
    }
    fn process_frame(
        &mut self,
        text: &str,
        key: &SharedKey,
        local_sender_id: &str,
        now: i64,
    ) -> Result<ReceiveOutcome> {
        let frame = Frame::parse(text)?;
        if frame.kid != key.kid() {
            return Ok(ReceiveOutcome::Ignored);
        }
        let identity = format!("{}:{}", frame.kid, frame.mid);
        if self.completed.contains_key(&identity) {
            return Ok(ReceiveOutcome::Duplicate);
        }
        let (iv, data) = frame.validate()?;
        let existing = self
            .pending
            .iter()
            .position(|g| g.kid == frame.kid && g.mid == frame.mid);
        if let Some(idx) = existing {
            let group = &self.pending[idx];
            if group.iv != iv
                || group.n != frame.n
                || group.chunks[frame.i as usize]
                    .as_ref()
                    .is_some_and(|old| old != &data)
            {
                self.reject_group(&identity, &frame.kid, &frame.mid, now);
                return Err(ProtocolError("共享分片冲突，已拒绝整条内容"));
            }
            if group.chunks[frame.i as usize].is_some() {
                return Ok(ReceiveOutcome::Duplicate);
            }
        }
        // Capacity pressure discards the oldest incomplete cache, not the new legitimate group.
        // Its event ownership is forgotten and a durable recovery marker requests one bounded replay.
        while self.cached_bytes() + data.len() > MAX_PENDING_BYTES
            || (existing.is_none() && self.pending.len() >= MAX_GROUPS)
        {
            let oldest = self
                .pending
                .iter()
                .enumerate()
                .filter(|(_, group)| group.kid != frame.kid || group.mid != frame.mid)
                .min_by_key(|(_, group)| group.started_at)
                .map(|(index, _)| index);
            if let Some(index) = oldest {
                self.evict_group(index);
            } else {
                return Err(ProtocolError("待接收共享内容超出缓存限制"));
            }
        }
        // Eviction may have shifted indices; locate the current group again.
        let idx = self
            .pending
            .iter()
            .position(|group| group.kid == frame.kid && group.mid == frame.mid)
            .unwrap_or_else(|| {
                self.pending.push(PendingGroup {
                    kid: frame.kid.clone(),
                    mid: frame.mid.clone(),
                    iv,
                    n: frame.n,
                    started_at: now,
                    chunks: vec![None; frame.n as usize],
                });
                self.pending.len() - 1
            });
        self.pending[idx].chunks[frame.i as usize] = Some(data);
        let group_size: usize = self.pending[idx]
            .chunks
            .iter()
            .filter_map(|c| c.as_ref())
            .map(Vec::len)
            .sum();
        if group_size > MAX_CIPHER_BYTES {
            self.reject_group(&identity, &frame.kid, &frame.mid, now);
            return Err(ProtocolError("共享密文超出大小限制"));
        }
        if self.pending[idx].chunks.iter().any(Option::is_none) {
            return Ok(ReceiveOutcome::Partial);
        }
        let group = self.pending.remove(idx);
        self.event_groups.retain(|_, owner| owner != &identity);
        self.completed.insert(identity, now);
        trim_seen(&mut self.completed);
        let ciphertext: Vec<u8> = group.chunks.into_iter().flatten().flatten().collect();
        if ciphertext.len() < 16 || ciphertext.len().div_ceil(CHUNK_BYTES) != frame.n as usize {
            return Err(ProtocolError("共享密文长度不正确"));
        }
        let cipher =
            Aes256Gcm::new_from_slice(&key.0).map_err(|_| ProtocolError("共享密钥不正确"))?;
        let plaintext = cipher
            .decrypt(
                Nonce::from_slice(&group.iv),
                AeadPayload {
                    msg: &ciphertext,
                    aad: aad(&frame.mid, &frame.kid, frame.n).as_bytes(),
                },
            )
            .map_err(|_| ProtocolError("共享内容验证失败，请检查配对密钥"))?;
        if plaintext.len() > MAX_PAYLOAD_BYTES {
            return Err(ProtocolError("共享内容超出大小限制"));
        }
        let payload: Payload =
            serde_json::from_slice(&plaintext).map_err(|_| ProtocolError("共享内容格式不正确"))?;
        payload.validate(now)?;
        if payload.sender_id == local_sender_id {
            return Ok(ReceiveOutcome::Ignored);
        }
        Ok(ReceiveOutcome::Delivered(ReceivedMessage {
            mid: frame.mid,
            kid: frame.kid,
            payload,
        }))
    }
    fn reject_group(&mut self, identity: &str, kid: &str, mid: &str, now: i64) {
        self.pending.retain(|g| g.kid != kid || g.mid != mid);
        self.event_groups.retain(|_, owner| owner != identity);
        self.completed.insert(identity.to_string(), now);
        trim_seen(&mut self.completed);
    }
    pub fn validate(&self, now: i64) -> Result<()> {
        if self.pending.len() > MAX_GROUPS
            || self.completed.len() > MAX_SEEN
            || self.event_ids.len() > MAX_SEEN
            || self.event_groups.len() > MAX_SEEN
            || self.cursor.as_deref().is_some_and(|id| !valid_event_id(id))
        {
            return Err(ProtocolError("共享恢复状态超出限制"));
        }
        let mut total = 0usize;
        let mut identities = BTreeSet::new();
        for group in &self.pending {
            if !identities.insert((&group.kid, &group.mid)) {
                return Err(ProtocolError("共享恢复分组重复"));
            }
            if group.started_at < 0
                || !is_id(&group.mid)
                || !is_id(&group.kid)
                || group.iv.len() != 12
                || group.n == 0
                || group.n > MAX_PARTS
                || group.chunks.len() != group.n as usize
                || group.started_at > now.saturating_add(300_000)
            {
                return Err(ProtocolError("共享恢复分片状态不正确"));
            }
            let mut group_total = 0;
            for (i, data) in group.chunks.iter().enumerate() {
                if let Some(data) = data {
                    if data.is_empty()
                        || data.len() > CHUNK_BYTES
                        || (i + 1 < group.n as usize && data.len() != CHUNK_BYTES)
                    {
                        return Err(ProtocolError("共享恢复分片长度不正确"));
                    }
                    group_total += data.len();
                }
            }
            if group_total > MAX_CIPHER_BYTES {
                return Err(ProtocolError("共享恢复密文过长"));
            }
            total += group_total;
        }
        if total > MAX_PENDING_BYTES {
            return Err(ProtocolError("共享恢复缓存超出限制"));
        }
        if self
            .event_ids
            .iter()
            .any(|(id, at)| !valid_event_id(id) || *at < 0 || *at > now.saturating_add(300_000))
            || self
                .completed
                .iter()
                .any(|(id, at)| !valid_identity(id) || *at < 0 || *at > now.saturating_add(300_000))
        {
            return Err(ProtocolError("共享去重状态不正确"));
        }
        if self.event_groups.iter().any(|(id, owner)| {
            !self.event_ids.contains_key(id)
                || !valid_identity(owner)
                || !self
                    .pending
                    .iter()
                    .any(|group| format!("{}:{}", group.kid, group.mid) == *owner)
        }) {
            return Err(ProtocolError("共享恢复事件归属不正确"));
        }
        Ok(())
    }
    fn cached_bytes(&self) -> usize {
        self.pending
            .iter()
            .flat_map(|group| &group.chunks)
            .filter_map(|part| part.as_ref())
            .map(Vec::len)
            .sum()
    }
    fn evict_group(&mut self, index: usize) {
        let group = self.pending.remove(index);
        let identity = format!("{}:{}", group.kid, group.mid);
        let ids: Vec<String> = self
            .event_groups
            .iter()
            .filter(|(_, owner)| *owner == &identity)
            .map(|(id, _)| id.clone())
            .collect();
        if ids.is_empty() {
            // Legacy snapshots did not record ownership. Keep completed dedup, but permit the
            // bounded replay to reconstruct incomplete work rather than silently losing it.
            self.event_ids.clear();
            self.event_groups.clear();
        } else {
            for id in ids {
                self.event_groups.remove(&id);
                self.event_ids.remove(&id);
            }
        }
        self.needs_recovery = true;
        self.recovery_generation = self.recovery_generation.wrapping_add(1);
    }
    pub fn recovery_marker(&self) -> Option<u64> {
        self.needs_recovery.then_some(self.recovery_generation)
    }
    pub fn acknowledge_recovery(&mut self, marker: u64) {
        if self.recovery_generation == marker {
            self.needs_recovery = false;
        }
    }
    pub fn pending_recovery(&self) -> bool {
        self.needs_recovery
    }
    pub fn prune(&mut self, now: i64) {
        let mut index = 0;
        while index < self.pending.len() {
            let group = &self.pending[index];
            if now.saturating_sub(group.started_at) >= GROUP_TTL_MS
                || group.started_at > now.saturating_add(300_000)
            {
                self.evict_group(index);
            } else {
                index += 1;
            }
        }
        self.completed
            .retain(|_, at| now.saturating_sub(*at) < DAY_MS && *at <= now.saturating_add(300_000));
        self.event_ids
            .retain(|_, at| now.saturating_sub(*at) < DAY_MS && *at <= now.saturating_add(300_000));
        trim_seen(&mut self.completed);
        trim_seen(&mut self.event_ids);
        let ids = &self.event_ids;
        self.event_groups.retain(|id, _| ids.contains_key(id));
    }
    pub fn pending_count(&self) -> usize {
        self.pending.len()
    }
}

pub fn valid_event_id(id: &str) -> bool {
    !id.is_empty()
        && id.len() <= 128
        && id != "all"
        && id != "24h"
        && id
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-')
}
fn valid_identity(id: &str) -> bool {
    id.split_once(':')
        .is_some_and(|(kid, mid)| is_id(kid) && is_id(mid))
}
fn trim_seen(set: &mut BTreeMap<String, i64>) {
    while set.len() > MAX_SEEN {
        if let Some(key) = set
            .iter()
            .min_by_key(|(_, at)| *at)
            .map(|(id, _)| id.clone())
        {
            set.remove(&key);
        } else {
            break;
        }
    }
}
fn is_id(value: &str) -> bool {
    value.len() == 32
        && value
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}
fn decode(value: &str, max: usize) -> Result<Vec<u8>> {
    if value.is_empty() || value.len() > max.div_ceil(3) * 4 || value.contains('=') {
        return Err(ProtocolError("Base64URL编码长度不正确"));
    }
    let bytes = URL_SAFE_NO_PAD
        .decode(value)
        .map_err(|_| ProtocolError("Base64URL编码不正确"))?;
    if bytes.len() > max || URL_SAFE_NO_PAD.encode(&bytes) != value {
        return Err(ProtocolError("Base64URL编码不规范"));
    }
    Ok(bytes)
}
fn random(bytes: &mut [u8]) -> Result<()> {
    OsRng
        .try_fill_bytes(bytes)
        .map_err(|_| ProtocolError("系统无法生成安全随机数"))
}
fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}
fn aad(mid: &str, kid: &str, n: u32) -> String {
    format!("FangCunClipboard/v1\n{mid}\n{kid}\n{n}")
}

#[cfg(test)]
mod tests {
    use super::*;
    use base64::engine::general_purpose::STANDARD;
    fn fixture() -> serde_json::Value {
        serde_json::from_str(include_str!("../tests/fixtures/clipboard-v1-vectors.json")).unwrap()
    }
    fn key() -> SharedKey {
        SharedKey::parse("AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8").unwrap()
    }
    fn sender() -> &'static str {
        "00112233445566778899aabbccddeeff"
    }
    fn now() -> i64 {
        1_794_900_001_000
    }
    fn payload() -> Payload {
        Payload::new(sender(), "我的电脑", "  第一行\n方寸🌱 👋  ", now()).unwrap()
    }

    #[test]
    fn public_single_and_multipart_vectors_match_exact_aead_and_survive_restart_between_fragments()
    {
        for vector in fixture()["vectors"].as_array().unwrap() {
            let key = SharedKey::parse(vector["keyBase64url"].as_str().unwrap()).unwrap();
            assert_eq!(key.kid(), vector["kid"].as_str().unwrap());
            let bytes = STANDARD
                .decode(vector["plaintextUtf8Base64"].as_str().unwrap())
                .unwrap();
            let mid = vector["mid"].as_str().unwrap();
            let now = vector["testNowMs"].as_i64().unwrap();
            let iv = hex_decode(vector["ivHex"].as_str().unwrap());
            let mut nonce = [0; 12];
            nonce.copy_from_slice(&iv);
            let encrypted = encrypt_bytes(&bytes, &key, mid, nonce, now).unwrap();
            let expected: Vec<Frame> = serde_json::from_value(vector["frames"].clone()).unwrap();
            assert_eq!(encrypted.len(), expected.len());
            for (actual, expected) in encrypted.iter().zip(&expected) {
                assert!(actual == expected);
                assert!(actual.to_json().unwrap().len() <= MAX_FRAME_BYTES);
            }
            let all: Vec<u8> = encrypted
                .iter()
                .flat_map(|f| decode(&f.data, CHUNK_BYTES).unwrap())
                .collect();
            assert_eq!(
                URL_SAFE_NO_PAD.encode(all),
                vector["ciphertextAndTagBase64url"].as_str().unwrap()
            );
            let mut receiver = ReceiveState::default();
            let mut delivered = None;
            for (number, frame) in encrypted.iter().rev().enumerate() {
                let id = format!("ntfy-{number}");
                let outcome = receiver.process(
                    &id,
                    &frame.to_json().unwrap(),
                    &key,
                    "ffeeddccbbaa99887766554433221100",
                    now,
                );
                if let ReceiveOutcome::Delivered(value) = outcome {
                    delivered = Some(value);
                }
                assert!(matches!(
                    receiver.process(
                        &id,
                        &frame.to_json().unwrap(),
                        &key,
                        "ffeeddccbbaa99887766554433221100",
                        now
                    ),
                    ReceiveOutcome::Duplicate
                ));
                let durable = serde_json::to_vec(&receiver).unwrap();
                receiver = serde_json::from_slice(&durable).unwrap();
                receiver.validate(now).unwrap();
                assert_eq!(receiver.cursor.as_deref(), Some(id.as_str()));
            }
            let received = delivered.expect("A complete vector must be delivered");
            assert_eq!(
                received.payload.text,
                vector["expectedPayload"]["text"].as_str().unwrap()
            );
            assert_eq!(received.payload.sender_name, "我的电脑");
            assert_eq!(receiver.pending_count(), 0);
            assert!(matches!(
                receiver.process(
                    "replayed-id",
                    &encrypted[0].to_json().unwrap(),
                    &key,
                    "ffeeddccbbaa99887766554433221100",
                    now
                ),
                ReceiveOutcome::Duplicate
            ));
        }
    }
    #[test]
    fn new_messages_use_fresh_ids_and_nonces_and_preserve_all_text_bytes() {
        let first = encrypt_payload(&payload(), &key(), now()).unwrap();
        let second = encrypt_payload(&payload(), &key(), now()).unwrap();
        assert_ne!(first[0].mid, second[0].mid);
        assert_ne!(first[0].iv, second[0].iv);
        let mut receiver = ReceiveState::default();
        if let ReceiveOutcome::Delivered(message) = receiver.process(
            "event-id",
            &first[0].to_json().unwrap(),
            &key(),
            "abcdefabcdefabcdefabcdefabcdefab",
            now(),
        ) {
            assert_eq!(message.payload.text, payload().text);
        } else {
            panic!("Expected authenticated complete payload");
        }
    }
    #[test]
    fn strict_integer_and_canonical_base64_fields_reject_strings_bools_padding_and_oversize() {
        let original = encrypt_payload(&payload(), &key(), now())
            .unwrap()
            .remove(0);
        for field in ["v", "i", "n"] {
            for bad in [
                serde_json::json!("1"),
                serde_json::json!(true),
                serde_json::json!(1.0),
                serde_json::json!(-1),
            ] {
                let mut json = serde_json::to_value(&original).unwrap();
                json[field] = bad;
                assert!(Frame::parse(&json.to_string()).is_err());
            }
        }
        let mut bad = original.clone();
        bad.iv.push('=');
        assert!(bad.to_json().is_err());
        bad = original.clone();
        bad.n = 29;
        assert!(bad.to_json().is_err());
        bad = original.clone();
        bad.i = bad.n;
        assert!(bad.to_json().is_err());
        assert!(Frame::parse(&"x".repeat(MAX_FRAME_BYTES + 1)).is_err());
        assert!(SharedKey::parse("123456").is_err());
    }
    #[test]
    fn bad_tag_or_aad_conflicts_and_plaintext_never_deliver_partial_content() {
        let mut bad = encrypt_payload(&payload(), &key(), now())
            .unwrap()
            .remove(0);
        let mut bytes = decode(&bad.data, CHUNK_BYTES).unwrap();
        bytes[0] ^= 1;
        bad.data = URL_SAFE_NO_PAD.encode(bytes);
        let mut receiver = ReceiveState::default();
        assert!(matches!(
            receiver.process("bad-tag", &bad.to_json().unwrap(), &key(), sender(), now()),
            ReceiveOutcome::Rejected(_)
        ));
        assert_eq!(receiver.pending_count(), 0);
        assert!(matches!(
            receiver.process(
                "plain-text",
                "{\"text\":\"should not appear\"}",
                &key(),
                sender(),
                now()
            ),
            ReceiveOutcome::Rejected(_)
        ));
        let large = Payload::new(sender(), "电脑", &"方寸🌱".repeat(700), now()).unwrap();
        let frames = encrypt_payload(&large, &key(), now()).unwrap();
        assert!(frames.len() > 1);
        assert!(matches!(
            receiver.process(
                "part-one",
                &frames[0].to_json().unwrap(),
                &key(),
                "abcdefabcdefabcdefabcdefabcdefab",
                now()
            ),
            ReceiveOutcome::Partial
        ));
        let mut conflicting = frames[0].clone();
        let mut data = decode(&conflicting.data, CHUNK_BYTES).unwrap();
        data[0] ^= 1;
        conflicting.data = URL_SAFE_NO_PAD.encode(data);
        assert!(matches!(
            receiver.process(
                "conflict",
                &conflicting.to_json().unwrap(),
                &key(),
                sender(),
                now()
            ),
            ReceiveOutcome::Rejected(_)
        ));
        assert_eq!(receiver.pending_count(), 0);
        assert!(matches!(
            receiver.process(
                "later-part",
                &frames[1].to_json().unwrap(),
                &key(),
                sender(),
                now()
            ),
            ReceiveOutcome::Duplicate
        ));
        let mut aad_bad = frames[0].clone();
        aad_bad.mid = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa".to_string();
        let one = encrypt_payload(&payload(), &key(), now())
            .unwrap()
            .remove(0);
        let mut changed = one.clone();
        changed.mid = aad_bad.mid;
        assert!(matches!(
            receiver.process(
                "bad-aad",
                &changed.to_json().unwrap(),
                &key(),
                sender(),
                now()
            ),
            ReceiveOutcome::Rejected(_)
        ));
    }
    #[test]
    fn expiration_future_clock_and_sender_or_text_byte_limits_are_checked() {
        let value = payload();
        assert!(value.validate(value.expires_at).is_err());
        let mut bad = value.clone();
        bad.created_at = now() + 300_001;
        bad.expires_at = bad.created_at + DAY_MS;
        assert!(bad.validate(now()).is_err());
        bad = value.clone();
        bad.expires_at = bad.created_at + DAY_MS + 1;
        assert!(bad.validate(now()).is_err());
        bad = value.clone();
        bad.sender_name = "🌱".repeat(65);
        assert!(bad.validate(now()).is_err());
        bad = value.clone();
        bad.text = "x".repeat(MAX_TEXT_BYTES + 1);
        assert!(bad.validate(now()).is_err());
        let spaces = Payload::new(sender(), "电脑", " \n  ", now()).unwrap();
        assert_eq!(spaces.text, " \n  ");
        let raw = serde_json::json!({"v":1,"senderId":sender(),"senderName":"电脑","createdAt":"1794900001000","expiresAt":1794986401000i64,"text":"原文"});
        assert!(serde_json::from_value::<Payload>(raw).is_err());
    }
    #[test]
    fn pairing_only_contains_endpoint_and_key_and_requires_explicit_client_enable_elsewhere() {
        let pairing = Pairing {
            endpoint: Endpoint::new("https://clip.kivohub.site/", "fangcun-clipboard").unwrap(),
            key: key(),
        };
        let encoded = pairing.export().unwrap();
        let imported = Pairing::parse(&encoded).unwrap();
        assert_eq!(imported.endpoint.server, DEFAULT_SERVER);
        assert_eq!(imported.endpoint.topic, DEFAULT_TOPIC);
        assert_eq!(imported.key.export(), key().export());
        let bytes = decode(encoded.strip_prefix(PAIR_PREFIX).unwrap(), 6144).unwrap();
        let json: serde_json::Value = serde_json::from_slice(&bytes).unwrap();
        assert!(json.get("token").is_none());
        assert!(json.get("enabled").is_none());
        for server in [
            "http://example.com",
            "https://user:pass@example.com",
            "https://example.com/path",
            "https://example.com?token=bad",
            "https://example.com#x",
        ] {
            assert!(Endpoint::new(server, "topic").is_err());
        }
        for topic in ["", "two words", "a/b"] {
            assert!(Endpoint::new("https://example.com", topic).is_err());
        }
    }
    #[test]
    fn group_limits_expire_bounded_pending_data_and_malformed_durable_state_fails_validation() {
        let large = Payload::new(sender(), "电脑", &"文".repeat(2000), now()).unwrap();
        let mut receiver = ReceiveState::default();
        for i in 0..16 {
            let frames = encrypt_payload(&large, &key(), now()).unwrap();
            assert!(matches!(
                receiver.process(
                    &format!("group-{i}"),
                    &frames[0].to_json().unwrap(),
                    &key(),
                    "abcdefabcdefabcdefabcdefabcdefab",
                    now()
                ),
                ReceiveOutcome::Partial
            ));
        }
        assert_eq!(receiver.pending_count(), 16);
        let extra = encrypt_payload(&large, &key(), now()).unwrap();
        assert!(matches!(
            receiver.process(
                "seventeenth",
                &extra[0].to_json().unwrap(),
                &key(),
                sender(),
                now()
            ),
            ReceiveOutcome::Partial
        ));
        receiver.prune(now() + GROUP_TTL_MS);
        assert_eq!(receiver.pending_count(), 0);
        let mut durable = serde_json::to_value(&receiver).unwrap();
        durable["cursor"] = serde_json::json!("all");
        let restored: ReceiveState = serde_json::from_value(durable).unwrap();
        assert!(restored.validate(now()).is_err());
    }
    #[test]
    fn own_messages_and_unknown_keys_are_acknowledged_but_not_displayed() {
        let frame = encrypt_payload(&payload(), &key(), now())
            .unwrap()
            .remove(0);
        let mut receiver = ReceiveState::default();
        assert!(matches!(
            receiver.process("own-id", &frame.to_json().unwrap(), &key(), sender(), now()),
            ReceiveOutcome::Ignored
        ));
        let other = SharedKey::generate().unwrap();
        assert!(matches!(
            receiver.process(
                "other-key-id",
                &frame.to_json().unwrap(),
                &other,
                sender(),
                now()
            ),
            ReceiveOutcome::Ignored
        ));
        assert_eq!(receiver.cursor.as_deref(), Some("other-key-id"));
    }
    #[test]
    fn same_second_replays_do_not_rewind_cursor_and_expired_events_are_rejected_without_exposure() {
        let large = Payload::new(sender(), "电脑", &"文".repeat(2000), now()).unwrap();
        let frames = encrypt_payload(&large, &key(), now()).unwrap();
        let mut receiver = ReceiveState::default();
        for (i, frame) in frames.iter().enumerate() {
            let id = format!("event-{i}");
            let _ = receiver.process(
                &id,
                &frame.to_json().unwrap(),
                &key(),
                "abcdefabcdefabcdefabcdefabcdefab",
                now(),
            );
        }
        let cursor = receiver.cursor.clone();
        assert!(matches!(
            receiver.process(
                "event-0",
                &frames[0].to_json().unwrap(),
                &key(),
                sender(),
                now()
            ),
            ReceiveOutcome::Duplicate
        ));
        assert!(receiver.cursor == cursor);
        let fresh = encrypt_payload(&payload(), &key(), now())
            .unwrap()
            .remove(0);
        assert!(matches!(
            receiver.process(
                "expired-new-id",
                &fresh.to_json().unwrap(),
                &key(),
                sender(),
                now() + DAY_MS
            ),
            ReceiveOutcome::Rejected(_)
        ));
        assert_eq!(receiver.cursor.as_deref(), Some("expired-new-id"));
        assert_eq!(receiver.pending_count(), 0);
    }
    #[test]
    fn corrupt_duplicate_pending_groups_and_pairing_with_hidden_flags_are_rejected() {
        let large = Payload::new(sender(), "电脑", &"文".repeat(2000), now()).unwrap();
        let frame = encrypt_payload(&large, &key(), now()).unwrap().remove(0);
        let mut receiver = ReceiveState::default();
        let _ = receiver.process(
            "first-part",
            &frame.to_json().unwrap(),
            &key(),
            sender(),
            now(),
        );
        receiver.pending.push(receiver.pending[0].clone());
        assert!(receiver.validate(now()).is_err());
        let wire = serde_json::json!({"v":1,"server":DEFAULT_SERVER,"topic":DEFAULT_TOPIC,"key":key().export(),"enabled":true,"token":"not-imported"});
        let input = format!("{PAIR_PREFIX}{}", URL_SAFE_NO_PAD.encode(wire.to_string()));
        assert!(Pairing::parse(&input).is_err());
        assert!(Endpoint::new("https://@example.com", "topic").is_err());
    }
    #[test]
    fn expired_partial_cache_can_replay_the_same_events_after_a_late_fragment_and_restart() {
        let value = fixture();
        let vector = &value["vectors"][1];
        let frames: Vec<Frame> = serde_json::from_value(vector["frames"].clone()).unwrap();
        let mut receiver = ReceiveState::default();
        let complete = encrypt_payload(&payload(), &key(), now())
            .unwrap()
            .remove(0);
        assert!(matches!(
            receiver.process(
                "already-complete",
                &complete.to_json().unwrap(),
                &key(),
                "abcdefabcdefabcdefabcdefabcdefab",
                now()
            ),
            ReceiveOutcome::Delivered(_)
        ));
        for i in 0..2 {
            assert!(matches!(
                receiver.process(
                    &format!("part-{i}"),
                    &frames[i].to_json().unwrap(),
                    &key(),
                    "abcdefabcdefabcdefabcdefabcdefab",
                    now()
                ),
                ReceiveOutcome::Partial
            ));
        }
        let late = now() + GROUP_TTL_MS + 1;
        assert!(matches!(
            receiver.process(
                "part-2",
                &frames[2].to_json().unwrap(),
                &key(),
                "abcdefabcdefabcdefabcdefabcdefab",
                late
            ),
            ReceiveOutcome::Partial
        ));
        assert!(receiver.pending_recovery());
        assert!(!receiver.event_ids.contains_key("part-0"));
        assert!(!receiver.event_ids.contains_key("part-1"));
        assert!(receiver.event_ids.contains_key("already-complete"));
        let serialized = serde_json::to_vec(&receiver).unwrap();
        receiver = serde_json::from_slice(&serialized).unwrap();
        receiver.validate(late).unwrap();
        assert!(receiver.pending_recovery());
        for i in [3usize, 0] {
            assert!(matches!(
                receiver.process(
                    &format!("part-{i}"),
                    &frames[i].to_json().unwrap(),
                    &key(),
                    "abcdefabcdefabcdefabcdefabcdefab",
                    late
                ),
                ReceiveOutcome::Partial
            ));
        }
        match receiver.process(
            "part-1",
            &frames[1].to_json().unwrap(),
            &key(),
            "abcdefabcdefabcdefabcdefabcdefab",
            late,
        ) {
            ReceiveOutcome::Delivered(message) => assert_eq!(
                message.payload.text,
                vector["expectedPayload"]["text"].as_str().unwrap()
            ),
            _ => panic!("Same-ID replay must reconstruct expired partial work"),
        }
        assert_eq!(receiver.pending_count(), 0);
        assert!(receiver
            .completed
            .contains_key(&format!("{}:{}", complete.kid, complete.mid)));
        assert!(receiver.event_groups.is_empty());
        receiver.validate(late).unwrap();
    }
    #[test]
    fn capacity_evicts_oldest_partial_without_tombstone_and_accepts_new_group_then_recovers_old() {
        let large = Payload::new(sender(), "电脑", &"文".repeat(2000), now()).unwrap();
        let all: Vec<Vec<Frame>> = (0..17)
            .map(|_| encrypt_payload(&large, &key(), now()).unwrap())
            .collect();
        let mut receiver = ReceiveState::default();
        for (i, frames) in all.iter().enumerate() {
            assert!(matches!(
                receiver.process(
                    &format!("group-{i}"),
                    &frames[0].to_json().unwrap(),
                    &key(),
                    "abcdefabcdefabcdefabcdefabcdefab",
                    now() + i as i64
                ),
                ReceiveOutcome::Partial
            ));
        }
        assert_eq!(receiver.pending_count(), 16);
        assert!(receiver.pending_recovery());
        assert!(!receiver
            .pending
            .iter()
            .any(|group| group.mid == all[0][0].mid));
        assert!(receiver
            .pending
            .iter()
            .any(|group| group.mid == all[16][0].mid));
        assert!(!receiver
            .completed
            .contains_key(&format!("{}:{}", all[0][0].kid, all[0][0].mid)));
        assert!(!receiver.event_ids.contains_key("group-0"));
        receiver = serde_json::from_slice(&serde_json::to_vec(&receiver).unwrap()).unwrap();
        receiver.validate(now() + 20).unwrap();
        for group_number in [16usize, 0] {
            let frames = &all[group_number];
            let start = if group_number == 16 { 1 } else { 0 };
            let mut delivered = false;
            for (i, frame) in frames.iter().enumerate().skip(start) {
                let id = if i == 0 {
                    format!("group-{group_number}")
                } else {
                    format!("rest-{group_number}-{i}")
                };
                match receiver.process(
                    &id,
                    &frame.to_json().unwrap(),
                    &key(),
                    "abcdefabcdefabcdefabcdefabcdefab",
                    now() + 30,
                ) {
                    ReceiveOutcome::Delivered(message) => {
                        assert_eq!(message.payload.text, large.text);
                        delivered = true;
                    }
                    ReceiveOutcome::Partial => {}
                    _ => panic!("Expected accepted valid fragment"),
                }
            }
            assert!(delivered);
        }
        receiver.validate(now() + 30).unwrap();
    }
    #[test]
    fn recovery_marker_survives_restart_and_old_ack_cannot_clear_new_eviction() {
        let large = Payload::new(sender(), "电脑", &"文".repeat(2000), now()).unwrap();
        let mut receiver = ReceiveState::default();
        for i in 0..17 {
            let frame = encrypt_payload(&large, &key(), now()).unwrap().remove(0);
            let _ = receiver.process(
                &format!("entry-{i}"),
                &frame.to_json().unwrap(),
                &key(),
                sender(),
                now(),
            );
        }
        let old = receiver
            .recovery_marker()
            .expect("Capacity eviction requires recovery");
        receiver.prune(now() + GROUP_TTL_MS + 1);
        let newer = receiver.recovery_marker().unwrap();
        assert_ne!(old, newer);
        receiver = serde_json::from_slice(&serde_json::to_vec(&receiver).unwrap()).unwrap();
        receiver.acknowledge_recovery(old);
        assert_eq!(receiver.recovery_marker(), Some(newer));
        receiver.acknowledge_recovery(newer);
        assert!(!receiver.pending_recovery());
        receiver.validate(now() + GROUP_TTL_MS + 1).unwrap();
    }
    fn hex_decode(text: &str) -> Vec<u8> {
        (0..text.len())
            .step_by(2)
            .map(|i| u8::from_str_radix(&text[i..i + 2], 16).unwrap())
            .collect()
    }
}
