//! HTTPS ntfy transport. Tokens stay in sensitive Authorization headers; no server body is logged.
use crate::sync_protocol::{valid_event_id, Endpoint, Frame, MAX_FRAME_BYTES};
use futures_util::StreamExt;
use reqwest::{
    header::{HeaderValue, ACCEPT, AUTHORIZATION, CONTENT_TYPE, RETRY_AFTER},
    redirect::Policy,
    Client, Response, StatusCode,
};
use std::time::{Duration, SystemTime};
use tokio::sync::watch;

pub const MAX_EVENT_BYTES: usize = 65_536;
pub const MAX_POLL_BYTES: usize = 8 * 1_048_576;
pub const MAX_POLL_MESSAGES: usize = 2048;

#[derive(Clone)]
pub struct CancelToken {
    sender: watch::Sender<bool>,
}
impl Default for CancelToken {
    fn default() -> Self {
        Self::new()
    }
}
impl CancelToken {
    pub fn new() -> Self {
        let (sender, _) = watch::channel(false);
        Self { sender }
    }
    pub fn cancel(&self) {
        self.sender.send_replace(true);
    }
    pub fn is_cancelled(&self) -> bool {
        *self.sender.borrow()
    }
    pub async fn cancelled(&self) {
        let mut receiver = self.sender.subscribe();
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

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NtfyError {
    Auth,
    RateLimited(u64),
    CursorExpired,
    Truncated,
    Cancelled,
    Network,
    InvalidResponse,
    Configuration,
    CallbackFailed,
}
impl NtfyError {
    pub fn safe_message(self) -> &'static str {
        match self {
            Self::Auth => "中转服务令牌无效或无该话题读写权限",
            Self::RateLimited(_) => "中转服务暂时限流，稍后自动重试",
            Self::CursorExpired => "中转服务缓存已变化，需要有限回溯",
            Self::Truncated => "共享记录超过恢复上限，部分内容可能缺失",
            Self::Cancelled => "共享已停止",
            Self::Network => "无法连接中转服务，请检查网络与地址",
            Self::InvalidResponse => "中转服务返回格式异常，已停止本次连接",
            Self::Configuration => "共享地址、话题或令牌配置不正确",
            Self::CallbackFailed => "共享内容未能安全保存到本机，已停止接收",
        }
    }
    pub fn retry_after(self) -> Option<Duration> {
        if let Self::RateLimited(seconds) = self {
            Some(Duration::from_secs(seconds))
        } else {
            None
        }
    }
}
impl std::fmt::Display for NtfyError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.safe_message())
    }
}
impl std::error::Error for NtfyError {}
pub type Result<T> = std::result::Result<T, NtfyError>;

#[derive(Clone)]
pub struct NtfyMessage {
    pub id: String,
    pub message: String,
}
pub struct PollBatch {
    pub messages: Vec<NtfyMessage>,
    pub truncated: bool,
    pub raw_bytes: usize,
}
#[derive(Clone)]
pub struct NtfyClient {
    client: Client,
    endpoint: Endpoint,
    authorization: HeaderValue,
}
impl NtfyClient {
    pub fn new(server: &str, topic: &str, token: &str) -> Result<Self> {
        let endpoint = Endpoint::new(server, topic).map_err(|_| NtfyError::Configuration)?;
        if token.is_empty() || token.len() > 8192 || !token.bytes().all(|c| (33..=126).contains(&c))
        {
            return Err(NtfyError::Configuration);
        }
        let mut authorization = HeaderValue::from_str(&format!("Bearer {token}"))
            .map_err(|_| NtfyError::Configuration)?;
        authorization.set_sensitive(true);
        let client = Client::builder()
            .https_only(true)
            .redirect(Policy::none())
            .connect_timeout(Duration::from_secs(15))
            .build()
            .map_err(|_| NtfyError::Configuration)?;
        Ok(Self {
            client,
            endpoint,
            authorization,
        })
    }
    pub async fn publish(&self, frame: &Frame, cancel: &CancelToken) -> Result<()> {
        let json = frame.to_json().map_err(|_| NtfyError::Configuration)?;
        if json.len() > MAX_FRAME_BYTES {
            return Err(NtfyError::Configuration);
        }
        let response = self
            .send(
                self.client
                    .post(self.url(None, None)?)
                    .header(CONTENT_TYPE, "text/plain; charset=utf-8")
                    .body(json.clone())
                    .timeout(Duration::from_secs(30)),
                false,
                cancel,
            )
            .await?;
        let acknowledgement = read_bounded(response, MAX_EVENT_BYTES, cancel).await?;
        validate_publish_ack(&acknowledgement, &json)?;
        if cancel.is_cancelled() {
            return Err(NtfyError::Cancelled);
        }
        Ok(())
    }
    /// One bounded request only. The owner decides whether to do its one permitted 24h fallback.
    pub async fn poll(
        &self,
        since: Option<&str>,
        recovery_day: bool,
        cancel: &CancelToken,
    ) -> Result<PollBatch> {
        self.poll_with_budget(
            since,
            recovery_day,
            cancel,
            MAX_POLL_MESSAGES,
            MAX_POLL_BYTES,
        )
        .await
    }
    pub async fn poll_with_budget(
        &self,
        since: Option<&str>,
        recovery_day: bool,
        cancel: &CancelToken,
        max_messages: usize,
        max_bytes: usize,
    ) -> Result<PollBatch> {
        let max_messages = max_messages.min(MAX_POLL_MESSAGES);
        let max_bytes = max_bytes.min(MAX_POLL_BYTES);
        if max_messages == 0 || max_bytes == 0 {
            return Err(NtfyError::Truncated);
        }
        let since = if recovery_day { Some("24h") } else { since };
        let mut url = self.url(Some("json"), if recovery_day { None } else { since })?;
        {
            let mut query = url.query_pairs_mut();
            query.append_pair("poll", "1");
            if recovery_day {
                query.append_pair("since", "24h");
            }
        }
        let response = self
            .send(
                self.client
                    .get(url)
                    .header(ACCEPT, "application/x-ndjson")
                    .timeout(Duration::from_secs(60)),
                since.is_some(),
                cancel,
            )
            .await?;
        let truncated = response
            .headers()
            .get("X-Messages-Truncated")
            .and_then(|v| v.to_str().ok())
            .is_some_and(|v| v == "1");
        let bytes = read_bounded(response, max_bytes, cancel).await?;
        let mut messages = Vec::new();
        for line in bytes.split(|b| *b == b'\n') {
            if cancel.is_cancelled() {
                return Err(NtfyError::Cancelled);
            }
            if line.is_empty() || line == b"\r" {
                continue;
            }
            if line.len() > MAX_EVENT_BYTES {
                return Err(NtfyError::InvalidResponse);
            }
            if let Some(message) = parse_message(line)? {
                if messages.len() >= max_messages {
                    return Err(NtfyError::Truncated);
                }
                messages.push(message);
            }
        }
        Ok(PollBatch {
            messages,
            truncated,
            raw_bytes: bytes.len(),
        })
    }
    /// No total-duration deadline: each parsed event is delivered immediately; cancellation drops IO.
    pub async fn stream<F>(
        &self,
        since: Option<&str>,
        cancel: &CancelToken,
        mut on_message: F,
    ) -> Result<()>
    where
        F: FnMut(NtfyMessage) -> Result<()>,
    {
        let url = self.url(Some("sse"), since)?;
        let response = self
            .send(
                self.client.get(url).header(ACCEPT, "text/event-stream"),
                since.is_some(),
                cancel,
            )
            .await?;
        if !response
            .headers()
            .get(CONTENT_TYPE)
            .and_then(|v| v.to_str().ok())
            .is_some_and(|v| v.to_ascii_lowercase().starts_with("text/event-stream"))
        {
            return Err(NtfyError::InvalidResponse);
        }
        let mut stream = response.bytes_stream();
        let mut decoder = SseDecoder::default();
        loop {
            let chunk = tokio::select! {
                _=cancel.cancelled()=>return Err(NtfyError::Cancelled),
                item=tokio::time::timeout(Duration::from_secs(90),stream.next())=>item.map_err(|_|NtfyError::Network)?,
            };
            match chunk {
                Some(Ok(bytes)) => {
                    for event in decoder.feed(&bytes)? {
                        if cancel.is_cancelled() {
                            return Err(NtfyError::Cancelled);
                        }
                        if let Some(message) = parse_message(&event)? {
                            on_message(message)?;
                        }
                    }
                }
                Some(Err(_)) => return Err(NtfyError::Network),
                None => {
                    for event in decoder.finish()? {
                        if cancel.is_cancelled() {
                            return Err(NtfyError::Cancelled);
                        }
                        if let Some(message) = parse_message(&event)? {
                            on_message(message)?;
                        }
                    }
                    return Ok(());
                }
            }
        }
    }
    fn url(&self, suffix: Option<&str>, since: Option<&str>) -> Result<reqwest::Url> {
        if since.is_some_and(|id| !valid_event_id(id)) {
            return Err(NtfyError::Configuration);
        }
        let mut url = reqwest::Url::parse(&format!(
            "{}/{}{}",
            self.endpoint.server,
            self.endpoint.topic,
            suffix.map(|s| format!("/{s}")).unwrap_or_default()
        ))
        .map_err(|_| NtfyError::Configuration)?;
        if let Some(id) = since {
            url.query_pairs_mut().append_pair("since", id);
        }
        Ok(url)
    }
    async fn send(
        &self,
        request: reqwest::RequestBuilder,
        has_cursor: bool,
        cancel: &CancelToken,
    ) -> Result<Response> {
        if cancel.is_cancelled() {
            return Err(NtfyError::Cancelled);
        }
        let response = tokio::select! {
            _=cancel.cancelled()=>return Err(NtfyError::Cancelled),
            response=tokio::time::timeout(Duration::from_secs(30),request.header(AUTHORIZATION,self.authorization.clone()).send())=>
                response.map_err(|_|NtfyError::Network)?.map_err(|_|NtfyError::Network)?,
        };
        if let Some(error) = status_error(
            response.status(),
            response
                .headers()
                .get(RETRY_AFTER)
                .and_then(|v| v.to_str().ok()),
            has_cursor,
        ) {
            return Err(error);
        }
        Ok(response)
    }
}

fn status_error(status: StatusCode, retry: Option<&str>, has_cursor: bool) -> Option<NtfyError> {
    if status.is_success() {
        return None;
    }
    Some(match status.as_u16() {
        401 | 403 => NtfyError::Auth,
        429 => NtfyError::RateLimited(retry_after_seconds(retry, SystemTime::now())),
        400 | 404 if has_cursor => NtfyError::CursorExpired,
        300..=399 => NtfyError::InvalidResponse,
        _ => NtfyError::Network,
    })
}
fn retry_after_seconds(value: Option<&str>, now: SystemTime) -> u64 {
    let Some(value) = value.map(str::trim) else {
        return 60;
    };
    let seconds = if !value.is_empty() && value.bytes().all(|byte| byte.is_ascii_digit()) {
        value.parse::<u64>().unwrap_or(u64::MAX)
    } else if let Ok(date) = httpdate::parse_http_date(value) {
        date.duration_since(now)
            .map(|duration| {
                duration
                    .as_secs()
                    .saturating_add(u64::from(duration.subsec_nanos() > 0))
            })
            .unwrap_or(0)
    } else {
        60
    };
    seconds.clamp(1, 86_400)
}

async fn read_bounded(response: Response, max: usize, cancel: &CancelToken) -> Result<Vec<u8>> {
    if response
        .content_length()
        .is_some_and(|len| len > max as u64)
    {
        return Err(NtfyError::Truncated);
    }
    let mut result = Vec::new();
    let mut stream = response.bytes_stream();
    loop {
        let next = tokio::select! {_=cancel.cancelled()=>return Err(NtfyError::Cancelled),next=stream.next()=>next};
        match next {
            Some(Ok(bytes)) => {
                if bytes.len() > max.saturating_sub(result.len()) {
                    return Err(NtfyError::Truncated);
                }
                result.extend_from_slice(&bytes);
            }
            Some(Err(_)) => return Err(NtfyError::Network),
            None => return Ok(result),
        }
    }
}
fn validate_publish_ack(bytes: &[u8], expected: &str) -> Result<()> {
    let json: serde_json::Value =
        serde_json::from_slice(bytes).map_err(|_| NtfyError::InvalidResponse)?;
    let correct = json.get("event").and_then(|value| value.as_str()) == Some("message")
        && json
            .get("id")
            .and_then(|value| value.as_str())
            .is_some_and(valid_event_id)
        && json.get("message").and_then(|value| value.as_str()) == Some(expected)
        && !json.get("attachment").is_some_and(|value| !value.is_null());
    if correct {
        Ok(())
    } else {
        Err(NtfyError::InvalidResponse)
    }
}

fn parse_message(bytes: &[u8]) -> Result<Option<NtfyMessage>> {
    if bytes.len() > MAX_EVENT_BYTES {
        return Err(NtfyError::InvalidResponse);
    }
    let json: serde_json::Value =
        serde_json::from_slice(bytes).map_err(|_| NtfyError::InvalidResponse)?;
    let event = json
        .get("event")
        .and_then(|v| v.as_str())
        .ok_or(NtfyError::InvalidResponse)?;
    if event != "message" {
        return Ok(None);
    }
    let id = json
        .get("id")
        .and_then(|v| v.as_str())
        .filter(|id| valid_event_id(id))
        .ok_or(NtfyError::InvalidResponse)?;
    // Attachments are never downloaded. A missing/non-text message is acknowledged as an invalid frame.
    let message = json.get("message").and_then(|v| v.as_str()).unwrap_or("");
    Ok(Some(NtfyMessage {
        id: id.to_string(),
        message: message.to_string(),
    }))
}

#[derive(Default)]
struct SseDecoder {
    line: Vec<u8>,
    data: Vec<u8>,
    event_bytes: usize,
}
impl SseDecoder {
    fn feed(&mut self, bytes: &[u8]) -> Result<Vec<Vec<u8>>> {
        let mut events = Vec::new();
        for byte in bytes {
            self.event_bytes += 1;
            if self.event_bytes > MAX_EVENT_BYTES {
                return Err(NtfyError::InvalidResponse);
            }
            if *byte == b'\n' {
                self.end_line(&mut events)?;
            } else {
                self.line.push(*byte);
            }
        }
        Ok(events)
    }
    fn end_line(&mut self, events: &mut Vec<Vec<u8>>) -> Result<()> {
        if self.line.last() == Some(&b'\r') {
            self.line.pop();
        }
        if self.line.is_empty() {
            if !self.data.is_empty() {
                events.push(std::mem::take(&mut self.data));
            }
            self.event_bytes = 0;
        } else if self.line.starts_with(b"data:") {
            let mut value = &self.line[5..];
            if value.first() == Some(&b' ') {
                value = &value[1..];
            }
            if !self.data.is_empty() {
                self.data.push(b'\n');
            }
            self.data.extend_from_slice(value);
        }
        self.line.clear();
        Ok(())
    }
    fn finish(&mut self) -> Result<Vec<Vec<u8>>> {
        let mut events = Vec::new();
        if !self.line.is_empty() {
            self.end_line(&mut events)?;
        }
        if !self.data.is_empty() {
            events.push(std::mem::take(&mut self.data));
        }
        Ok(events)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn requests_keep_tokens_in_headers_and_never_use_all_or_cleartext() {
        let client =
            NtfyClient::new("https://example.com/", "test-topic", "private-test-token").unwrap();
        let url = client.url(Some("sse"), Some("same-second-id")).unwrap();
        assert_eq!(
            url.as_str(),
            "https://example.com/test-topic/sse?since=same-second-id"
        );
        assert!(!url.as_str().contains("private-test-token"));
        assert!(client.authorization.is_sensitive());
        assert_eq!(
            client.url(Some("sse"), Some("all")).unwrap_err(),
            NtfyError::Configuration
        );
        assert!(NtfyClient::new("http://example.com", "topic", "test").is_err());
        assert!(NtfyClient::new("https://example.com/v1", "topic", "test").is_err());
        assert!(NtfyClient::new("https://example.com", "topic", "bad\nkey").is_err());
    }
    #[test]
    fn status_errors_are_local_safe_and_distinguish_auth_cursor_and_backoff() {
        assert_eq!(
            status_error(StatusCode::FORBIDDEN, None, true),
            Some(NtfyError::Auth)
        );
        assert_eq!(
            status_error(StatusCode::BAD_REQUEST, None, true),
            Some(NtfyError::CursorExpired)
        );
        assert_eq!(
            status_error(StatusCode::TOO_MANY_REQUESTS, Some("99999"), false),
            Some(NtfyError::RateLimited(86400))
        );
        assert_eq!(
            status_error(StatusCode::TEMPORARY_REDIRECT, None, false),
            Some(NtfyError::InvalidResponse)
        );
        assert!(!NtfyError::InvalidResponse
            .safe_message()
            .contains("private-test-token"));
    }
    #[test]
    fn slow_sse_dispatches_complete_events_before_eof_and_handles_split_utf8() {
        let mut decoder = SseDecoder::default();
        let json = r#"{"event":"message","id":"event-1","message":"你好"}"#;
        let event = format!("data: {json}\r\n\r\n");
        let mut ready = Vec::new();
        for byte in event.as_bytes() {
            ready.extend(decoder.feed(&[*byte]).unwrap());
        }
        assert_eq!(ready.len(), 1);
        let message = parse_message(&ready[0]).unwrap().unwrap();
        assert_eq!(message.id, "event-1");
        assert_eq!(message.message, "你好");
        assert!(decoder.finish().unwrap().is_empty());
        assert!(parse_message(br#"{"event":"keepalive","id":"keepalive"}"#)
            .unwrap()
            .is_none());
        let mut long = SseDecoder::default();
        assert!(long.feed(&vec![b'x'; MAX_EVENT_BYTES + 1]).is_err());
    }
    #[tokio::test]
    async fn exhausted_recovery_budget_never_starts_a_network_request() {
        let client =
            NtfyClient::new("https://does-not-exist.invalid", "topic", "test-token").unwrap();
        let cancel = CancelToken::new();
        assert!(matches!(
            client.poll_with_budget(None, false, &cancel, 0, 1024).await,
            Err(NtfyError::Truncated)
        ));
        assert!(matches!(
            client.poll_with_budget(None, true, &cancel, 2048, 0).await,
            Err(NtfyError::Truncated)
        ));
    }
    #[test]
    fn retry_after_accepts_http_dates_and_seconds_without_shortening_long_server_delays() {
        let now = SystemTime::UNIX_EPOCH + Duration::from_secs(1_794_900_000);
        let date = httpdate::fmt_http_date(now + Duration::from_secs(18_000));
        assert_eq!(retry_after_seconds(Some(&date), now), 18_000);
        let past = httpdate::fmt_http_date(now - Duration::from_secs(60));
        assert_eq!(retry_after_seconds(Some(&past), now), 1);
        assert_eq!(retry_after_seconds(Some("7200"), now), 7200);
        assert_eq!(
            retry_after_seconds(Some("99999999999999999999999999"), now),
            86_400
        );
        let far = httpdate::fmt_http_date(now + Duration::from_secs(172_800));
        assert_eq!(retry_after_seconds(Some(&far), now), 86_400);
        assert_eq!(retry_after_seconds(Some("invalid"), now), 60);
    }
    #[test]
    fn publish_requires_real_matching_ntfy_ack_not_proxy_html_or_attachment() {
        let expected = "encrypted-frame-only";
        let valid = serde_json::json!({"event":"message","id":"valid-event","message":expected});
        assert!(validate_publish_ack(&serde_json::to_vec(&valid).unwrap(), expected).is_ok());
        assert_eq!(
            validate_publish_ack(b"<html>proxy page</html>", expected),
            Err(NtfyError::InvalidResponse)
        );
        let wrong =
            serde_json::json!({"event":"message","id":"valid-event","message":"changed-body"});
        assert_eq!(
            validate_publish_ack(&serde_json::to_vec(&wrong).unwrap(), expected),
            Err(NtfyError::InvalidResponse)
        );
        let attached = serde_json::json!({"event":"message","id":"valid-event","message":expected,"attachment":{"url":"https://untrusted.invalid"}});
        assert_eq!(
            validate_publish_ack(&serde_json::to_vec(&attached).unwrap(), expected),
            Err(NtfyError::InvalidResponse)
        );
    }

    #[tokio::test]
    async fn cancellation_is_sticky_and_wakes_active_waiters() {
        let cancel = CancelToken::new();
        let cloned = cancel.clone();
        let waiting = tokio::spawn(async move {
            cloned.cancelled().await;
        });
        tokio::task::yield_now().await;
        cancel.cancel();
        tokio::time::timeout(Duration::from_secs(1), waiting)
            .await
            .unwrap()
            .unwrap();
        tokio::time::timeout(Duration::from_secs(1), cancel.cancelled())
            .await
            .unwrap();
        assert!(cancel.is_cancelled());
    }
}
