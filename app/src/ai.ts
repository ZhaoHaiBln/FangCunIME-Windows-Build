import { invoke, isTauri } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import "./ai.css";

type Provider = "openai" | "gemini" | "deepseek" | "volcengine" | "siliconflow" | "compatible";
type Mode = "voice" | "write" | "rewrite" | "translate" | "ask";
interface Route { provider: Provider; baseUrl: string; model: string; }
interface Config { enabled: boolean; text: Route; speech: Route; textKeySaved: boolean; speechKeySaved: boolean; revision: string; }
interface Progress { id: string; phase: "generating" | "transcribing"; text: string; }
interface Capture {
  stream: MediaStream; context: AudioContext; source: MediaStreamAudioSourceNode;
  processor: ScriptProcessorNode; mute: GainNode; chunks: Float32Array[];
  samples: number; rate: number; startedAt: number; timer: number; released: boolean;
}
interface Operation { id: string; kind: "requesting" | "recording" | "transcribing" | "generating"; mode: Mode; capture?: Capture; source: string; }
const MAX_SOURCE = 8000;
const MAX_SECONDS = 60;
const TEXT_DEFAULTS: Record<Provider, Route> = {
  deepseek: { provider: "deepseek", baseUrl: "https://api.deepseek.com", model: "deepseek-flash" },
  openai: { provider: "openai", baseUrl: "https://api.openai.com/v1", model: "gpt-4o-mini" },
  gemini: { provider: "gemini", baseUrl: "https://generativelanguage.googleapis.com/v1beta", model: "gemini-3.5-flash-lite" },
  volcengine: { provider: "volcengine", baseUrl: "https://ark.cn-beijing.volces.com/api/v3", model: "doubao-seed-2-0-mini-260428" },
  siliconflow: { provider: "siliconflow", baseUrl: "https://api.siliconflow.cn/v1", model: "Qwen/Qwen2.5-7B-Instruct" },
  compatible: { provider: "compatible", baseUrl: "", model: "" },
};
const SPEECH_DEFAULTS: Partial<Record<Provider, Route>> = {
  openai: { provider: "openai", baseUrl: "https://api.openai.com/v1", model: "gpt-transcribe" },
  gemini: { provider: "gemini", baseUrl: "https://generativelanguage.googleapis.com/v1beta", model: "gemini-3.5-transcribe" },
};
const LABELS: Record<Provider, string> = { openai: "OpenAI", gemini: "Gemini", deepseek: "DeepSeek", volcengine: "火山豆包", siliconflow: "硅基流动", compatible: "自定义兼容服务" };
// Only known local messages reach the UI. Raw provider responses and credentials never do.
const USER_ERRORS = new Set([
  "AI 设置已被其他窗口修改，请重新读取后保存", "保存的 AI 地址过多，请删除旧地址的凭证后重试",
  "请填写不含密钥的 HTTPS 服务地址", "AI 服务地址格式不正确",
  "AI 服务地址须使用 HTTPS，且不要包含账户、密钥、查询参数或片段", "请填写有效的 AI 模型名称",
  "Windows 语音目前支持 OpenAI 或 Gemini 录音转写，请选择对应文件模型",
  "文字 AI 请选择文字模型，录音转写模型仅用于语音", "Gemini 模型名称不正确",
  "API Key 格式不正确，请只填写密钥内容", "无法加密 AI 凭证，请检查当前 Windows 账户",
  "无法读取 AI 凭证，请在当前 Windows 账户重新保存", "AI 凭证数据无效，请重新保存",
  "AI 凭证需要 Windows 账户保护，不会明文保存", "请先为当前文字服务地址保存 API Key",
  "请先为当前语音服务地址保存 API Key", "无法连接 AI 服务，请检查网络和服务地址",
  "AI 服务返回的结果格式不正确，请检查模型和服务地址", "AI 服务发生重定向，请检查服务地址",
  "AI 请求未被接受，请检查模型及服务配置", "AI 凭证无效或没有权限，请检查 API Key",
  "AI 服务地址或模型不存在，请检查配置", "AI 请求超时，请稍后重试",
  "AI 请求过多或额度不足，请稍后重试", "AI 服务暂时不可用，请稍后重试",
  "AI 响应等待超时，请重试", "AI 连接提前结束，结果未完整返回，请重试",
  "AI 输出达到模型长度上限，请缩短要求后重新生成", "AI 未能完整生成正文，请调整内容或模型后重试",
]);
const commandError = (error: unknown, fallback: string): string => typeof error === "string" && USER_ERRORS.has(error) ? error : fallback;
const pane = document.getElementById("ai-workbench");
if (pane && (!new URLSearchParams(location.search).get("mode") || new URLSearchParams(location.search).get("mode") === "main")) startWorkbench(pane);

function startWorkbench(pane: HTMLElement): void {
  const native = isTauri();
  const el = <T extends HTMLElement>(id: string): T => document.getElementById(id) as T;
  const input = (id: string) => el<HTMLInputElement>(id);
  const select = (id: string) => el<HTMLSelectElement>(id);
  const button = (id: string) => el<HTMLButtonElement>(id);
  const source = el<HTMLTextAreaElement>("ai-source");
  const configPanel = el<HTMLDetailsElement>("ai-config");
  let config: Config | null = null;
  let mode: Mode = "voice";
  let dirty = false;
  let saving = false;
  let current: Operation | null = null;
  let output = "";
  let resultLabel = "识别结果";
  let outputTimer: number | undefined;
  let outputRevision = 0;
  let copying = false;
  let pendingProgress: { operation: Operation; text: string } | null = null;
  let sequence = 0;
  let disposed = false;
  const unlisten: Array<() => void> = [];
  const makeId = () => "fc-ai-" + Date.now().toString(36) + "-" + (++sequence).toString(36) + "-" + crypto.getRandomValues(new Uint32Array(1))[0]!.toString(36);
  const owns = (operation: Operation) => !disposed && current === operation;
  const route = (role: "text" | "speech"): Route => ({ provider: select("ai-" + role + "-provider").value as Provider,
    baseUrl: input("ai-" + role + "-url").value.trim(), model: input("ai-" + role + "-model").value.trim() });
  const origin = (url: string): string => { try { const value = new URL(url); return value.protocol === "https:" && !value.username && !value.password && !value.search && !value.hash ? value.origin : ""; } catch { return ""; } };
  const sameScope = (a: Route, b: Route) => a.provider === b.provider && !!origin(a.baseUrl) && origin(a.baseUrl) === origin(b.baseUrl);
  const savedKey = (role: "text" | "speech") => !!config && sameScope(route(role), config[role]) && (role === "text" ? config.textKeySaved : config.speechKeySaved) && !input("ai-clear-" + role + "-key").checked;
  const setState = (text: string, phase = "idle") => { el("ai-operation-state").textContent = text; el("ai-status-light").dataset.phase = phase; };
  const setError = (text: string) => { el("ai-operation-error").textContent = text; el("ai-operation-error").hidden = !text; };
  const showOutput = (text: string, caption = resultLabel) => { output = text; outputRevision++; resultLabel = caption; el("ai-result-label").textContent = caption;
    el("ai-result").textContent = text || (mode === "voice" ? "识别结果会显示在这里" : "生成结果会显示在这里"); button("ai-copy").disabled = !native || !text || copying;
    el("ai-copy-status").textContent = ""; };
  function clearProgress(): void { if (outputTimer !== undefined) window.clearTimeout(outputTimer); outputTimer = undefined; pendingProgress = null; }
  function applyProgress(operation: Operation, text: string): void {
    // Partial ASR belongs in the result area; it must not replace the user's existing draft.
    showOutput(text, operation.kind === "transcribing" && operation.mode !== "voice" ? "口述识别中" : resultLabel);
  }
  function flushProgress(operation: Operation): void {
    const pending = pendingProgress;
    clearProgress();
    if (pending?.operation === operation && owns(operation)) applyProgress(operation, pending.text);
  }
  function showProgress(operation: Operation, text: string): void {
    pendingProgress = { operation, text };
    if (outputTimer !== undefined) return;
    outputTimer = window.setTimeout(() => { outputTimer = undefined; const next = pendingProgress; pendingProgress = null;
      if (!next || !owns(next.operation)) return;
      applyProgress(next.operation, next.text);
    }, 40);
  }
  function updateCount(): void { el("ai-source-count").textContent = source.value.length + " / " + MAX_SOURCE + " 字"; }
  function refresh(): void {
    const busy = current !== null;
    const ready = native && !!config && config.enabled && !dirty && !saving;
    const textReady = ready && !!config?.textKeySaved;
    const speechReady = ready && !!config?.speechKeySaved;
    el<HTMLFormElement>("ai-config-form").querySelectorAll<HTMLInputElement | HTMLSelectElement>("input, select").forEach(field => {
      field.disabled = !native || !config || saving;
    });
    button("ai-save").disabled = !native || !config || saving || busy || !dirty;
    button("ai-reload").disabled = !native || saving || busy;
    button("ai-generate").hidden = mode === "voice";
    button("ai-generate").disabled = !textReady || busy;
    button("ai-generate").textContent = { voice: "生成文字", write: "生成文字", rewrite: "开始改写", translate: "开始翻译", ask: "获取回答" }[mode];
    button("ai-record").disabled = current?.kind === "recording" ? false : !speechReady || busy || (mode !== "voice" && !textReady);
    button("ai-record").textContent = current?.kind === "recording" ? "结束录音" : mode === "voice" ? "开始录音" : "口述输入";
    button("ai-cancel").disabled = !busy;
    source.disabled = busy;
    el("ai-source-block").hidden = mode === "voice";
    el("ai-language-row").hidden = mode !== "translate";
    select("ai-target-language").disabled = busy;
    el("ai-record-time").hidden = current?.kind !== "recording";
    el("ai-meter").hidden = current?.kind !== "recording";
    el("ai-recording-note").textContent = mode === "voice"
      ? "音频在结束录音后发送至所选语音服务，结果先留在本页。"
      : "口述会加入输入内容；结束后识别，再自动发送至文字服务处理。结果先留在本页。";
    el("ai-source-label").textContent = { voice: "输入内容", write: "帮写要求", rewrite: "需要改写的内容", translate: "需要翻译的内容", ask: "你的问题" }[mode];
    const status = !native ? "界面预览 · AI 需在方寸桌面伴侣中使用" : !config ? "AI 组件尚未连接，请重新载入设置" : saving ? "正在保存 AI 设置…"
      : dirty ? "有未保存的 AI 设置，保存后才能开始" : !config.enabled ? "AI 默认关闭 · 需要时开启并保存" : "AI 已开启 · 文字与语音分别配置";
    el("ai-config-state").textContent = status;
    el("ai-config-summary").textContent = dirty ? "未保存" : config?.enabled ? "已开启" : "默认关闭";
    for (const role of ["text", "speech"] as const) {
      const stored = savedKey(role);
      const changed = !!config && !sameScope(route(role), config[role]);
      el("ai-" + role + "-key-state").textContent = input("ai-" + role + "-key").value ? "新 Key 尚未保存"
        : input("ai-clear-" + role + "-key").checked ? "保存时删除当前地址的 Key" : stored ? "已加密保存 · 不回填" : changed ? "保存后检查当前地址的 Key" : "未保存";
    }
    el("ai-service-note").textContent = config ? (mode === "voice" ? "语音：" + LABELS[config.speech.provider]
      : "文字：" + LABELS[config.text.provider] + " · 语音：" + LABELS[config.speech.provider]) + " · 配置只在保存后生效" : "先保存服务设置，再开始使用";
    updateCount();
  }
  function release(capture: Capture | undefined): void {
    if (!capture || capture.released) return;
    capture.released = true;
    window.clearInterval(capture.timer);
    capture.processor.onaudioprocess = null;
    for (const node of [capture.source, capture.processor, capture.mute]) { try { node.disconnect(); } catch { /* Already disconnected. */ } }
    capture.stream.getTracks().forEach(track => track.stop());
    void capture.context.close().catch(() => {});
    el("ai-meter").setAttribute("aria-valuenow", "0"); el("ai-meter").style.setProperty("--ai-level", "0%");
  }
  function cancel(reason = "已取消，未完成内容保留在本页"): void {
    const operation = current;
    if (operation) flushProgress(operation);
    current = null; clearProgress();
    if (operation) { release(operation.capture); operation.capture = undefined;
      if (native) void invoke("ai_cancel", { id: operation.id }).catch(() => {}); }
    if (operation) {
      if (operation.kind === "transcribing" && output) showOutput(output, "识别原文（未完成）");
      setState(reason);
    }
    refresh();
  }
  function markDirty(): void { cancel("配置已修改，当前操作已停止"); dirty = true; el("ai-save-status").textContent = "修改尚未保存"; refresh(); }
  function revealAdvanced(role: "text" | "speech"): void {
    const value = route(role);
    const defaults = role === "text" ? TEXT_DEFAULTS[value.provider] : SPEECH_DEFAULTS[value.provider];
    // A custom/proxy endpoint must stay visible when the saved configuration is loaded.
    el<HTMLDetailsElement>("ai-" + role + "-advanced").open = !defaults || value.provider === "compatible"
      || value.baseUrl.replace(/\/+$/, "") !== defaults.baseUrl.replace(/\/+$/, "") || value.model !== defaults.model;
  }
  function fill(value: Config): void {
    config = value; input("ai-enabled").checked = value.enabled;
    for (const role of ["text", "speech"] as const) {
      select("ai-" + role + "-provider").value = value[role].provider;
      input("ai-" + role + "-url").value = value[role].baseUrl; input("ai-" + role + "-model").value = value[role].model;
      input("ai-" + role + "-key").value = ""; input("ai-clear-" + role + "-key").checked = false;
      revealAdvanced(role);
    }
    dirty = false; refresh();
  }
  async function load(): Promise<void> {
    if (!native || saving) return;
    if (dirty && !window.confirm("重新载入会放弃未保存的 AI 设置，继续吗？")) return;
    saving = true; refresh();
    try { const value = await invoke<Config>("ai_get_config"); fill(value); el("ai-save-status").textContent = "已载入保存的 AI 设置"; configPanel.open = !value.enabled || !value.textKeySaved || !value.speechKeySaved; }
    catch { el("ai-save-status").textContent = "无法读取 AI 设置，请重新载入；普通打字不受影响"; }
    finally { saving = false; refresh(); }
  }
  el<HTMLFormElement>("ai-config-form").addEventListener("submit", async event => {
    event.preventDefault(); if (!native || !config || saving || current || !dirty) return;
    const text = route("text"), speech = route("speech");
    if (![text, speech].every(value => !!origin(value.baseUrl) && !!value.model)) { el("ai-save-status").textContent = "请在高级设置中填写有效的 HTTPS 地址和模型"; return; }
    saving = true; refresh();
    try {
      const value = await invoke<Config>("ai_save_config", { draft: { enabled: input("ai-enabled").checked, text, speech,
        textKey: input("ai-text-key").value.trim() || null, speechKey: input("ai-speech-key").value.trim() || null,
        clearTextKey: input("ai-clear-text-key").checked, clearSpeechKey: input("ai-clear-speech-key").checked }, revision: config.revision });
      fill(value); el("ai-save-status").textContent = "AI 设置已保存；密钥不回填。未进行付费生成或录音测试。";
      if (value.enabled && value.textKeySaved && value.speechKeySaved) configPanel.open = false;
    } catch (error) { el("ai-save-status").textContent = commandError(error, "保存失败") + "；草稿已保留，可重试或重新载入已保存设置"; }
    finally { saving = false; refresh(); }
  });
  button("ai-open-config").addEventListener("click", () => { configPanel.open = !configPanel.open; });
  button("ai-reload").addEventListener("click", () => { void load(); });
  for (const role of ["text", "speech"] as const) {
    select("ai-" + role + "-provider").addEventListener("change", () => {
      const selected = select("ai-" + role + "-provider").value as Provider;
      const defaults = role === "text" ? TEXT_DEFAULTS[selected] : SPEECH_DEFAULTS[selected];
      if (!defaults) return;
      input("ai-" + role + "-url").value = defaults.baseUrl; input("ai-" + role + "-model").value = defaults.model;
      input("ai-" + role + "-key").value = ""; input("ai-clear-" + role + "-key").checked = false; revealAdvanced(role); markDirty();
    });
    let draftOrigin = "";
    input("ai-" + role + "-url").addEventListener("focus", () => { draftOrigin = origin(input("ai-" + role + "-url").value); });
    input("ai-" + role + "-url").addEventListener("input", () => {
      const next = origin(input("ai-" + role + "-url").value);
      if (next && next !== draftOrigin) { input("ai-" + role + "-key").value = ""; draftOrigin = next; }
      markDirty();
    });
    input("ai-" + role + "-model").addEventListener("input", markDirty);
    input("ai-" + role + "-key").addEventListener("input", () => {
      if (input("ai-" + role + "-key").value) input("ai-clear-" + role + "-key").checked = false;
      markDirty();
    });
    input("ai-clear-" + role + "-key").addEventListener("change", () => { if (input("ai-clear-" + role + "-key").checked) input("ai-" + role + "-key").value = ""; markDirty(); });
  }
  input("ai-enabled").addEventListener("change", markDirty);
  source.addEventListener("input", updateCount);
  function chooseMode(next: Mode): void {
    if (mode !== next) { cancel("已切换功能"); mode = next; setError(""); resultLabel = next === "voice" ? "识别结果" : "生成结果"; showOutput(""); }
    pane.querySelectorAll<HTMLButtonElement>("[data-ai-mode]").forEach(item => { const selected = item.dataset.aiMode === mode; item.setAttribute("aria-selected", String(selected)); item.tabIndex = selected ? 0 : -1; });
    refresh();
  }
  el("ai-modes").addEventListener("click", event => { const item = (event.target as HTMLElement).closest<HTMLButtonElement>("[data-ai-mode]"); if (item) chooseMode(item.dataset.aiMode as Mode); });
  el("ai-modes").addEventListener("keydown", event => {
    if (!["ArrowRight", "ArrowLeft", "Home", "End"].includes(event.key)) return;
    const items = Array.from(pane.querySelectorAll<HTMLButtonElement>("[data-ai-mode]"));
    const index = items.findIndex(item => item.dataset.aiMode === mode);
    const next = event.key === "Home" ? 0 : event.key === "End" ? items.length - 1 : (index + (event.key === "ArrowRight" ? 1 : -1) + items.length) % items.length;
    event.preventDefault(); chooseMode(items[next]!.dataset.aiMode as Mode); items[next]!.focus();
  });
  async function generate(operation?: Operation): Promise<void> {
    const text = source.value.trim();
    if (!native || !config?.enabled || dirty || saving || !config.textKeySaved || mode === "voice") return;
    if (!text || text.length > MAX_SOURCE) { setError("请填写 1–8000 字的内容后再生成"); return; }
    if (current && current !== operation) return;
    const job: Operation = { id: makeId(), kind: "generating", mode, source: text }; current = job; clearProgress();
    setError(""); el("ai-copy-status").textContent = ""; showOutput("", "生成结果"); setState("正在生成，结果先留在此页", "generating"); refresh();
    try {
      const result = await invoke<string>("ai_generate", { request: { id: job.id, mode, source: text, targetLanguage: mode === "translate" ? select("ai-target-language").value : "中文" } });
      if (!owns(job)) return; clearProgress(); showOutput(result); setState("生成完成");
    } catch (error) { if (owns(job)) { flushProgress(job); setError(commandError(error, "生成失败，请检查文字服务配置与网络后重试")); setState("生成未完成", "error"); } }
    finally { if (owns(job)) { current = null; refresh(); } }
  }
  button("ai-generate").addEventListener("click", () => { void generate(); });
  button("ai-cancel").addEventListener("click", () => cancel());
  async function finishRecording(operation: Operation): Promise<void> {
    if (!owns(operation) || operation.kind !== "recording" || !operation.capture) return;
    const capture = operation.capture; operation.capture = undefined; operation.kind = "transcribing"; release(capture);
    setState("正在识别，录音已停止", "transcribing"); refresh();
    try {
      const wav = await wavFromCapture(capture); capture.chunks = [];
      if (!owns(operation)) return;
      const transcript = await invoke<string>("ai_transcribe", { request: { id: operation.id, wav: Array.from(wav) } });
      if (!owns(operation)) return; clearProgress();
      if (operation.mode === "voice") { showOutput(transcript, "识别结果"); current = null; setState("识别完成"); refresh(); }
      else {
        source.value = (operation.source.trim() ? operation.source.trim() + "\n" : "") + transcript; updateCount();
        if (source.value.length > MAX_SOURCE) { showOutput(transcript, "口述原文"); current = null; setError("识别文字已保留；内容超过 8000 字，请精简后再生成"); setState("语音已识别，等待精简"); refresh(); }
        else { current = null; await generate(); }
      }
    } catch (error) { if (owns(operation)) { flushProgress(operation); current = null; if (output) showOutput(output, "识别原文（未完成）"); setError(commandError(error, "语音识别未完成，请检查麦克风、语音服务配置与网络后重试")); setState("识别未完成", "error"); refresh(); } }
    finally { capture.chunks = []; }
  }
  async function record(): Promise<void> {
    if (current?.kind === "recording") { await finishRecording(current); return; }
    if (!native || !config?.enabled || !config.speechKeySaved || dirty || saving || current || (mode !== "voice" && !config.textKeySaved)) return;
    const operation: Operation = { id: makeId(), kind: "requesting", mode, source: source.value }; current = operation;
    setError(""); showOutput("", mode === "voice" ? "识别结果" : "生成结果"); setState("正在请求麦克风权限…", "requesting"); refresh();
    let stream: MediaStream | undefined;
    let context: AudioContext | undefined;
    let capture: Capture | undefined;
    try {
      if (!navigator.mediaDevices?.getUserMedia || !window.AudioContext || !window.OfflineAudioContext) throw new Error("AUDIO_UNAVAILABLE");
      stream = await navigator.mediaDevices.getUserMedia({ audio: { channelCount: 1, echoCancellation: true, noiseSuppression: true }, video: false });
      if (!owns(operation)) { stream.getTracks().forEach(track => track.stop()); return; }
      context = new AudioContext();
      if (!Number.isFinite(context.sampleRate) || context.sampleRate < 8000 || context.sampleRate > 192000) throw new Error("AUDIO_FORMAT");
      const audioSource = context.createMediaStreamSource(stream), processor = context.createScriptProcessor(4096, 1, 1), mute = context.createGain(); mute.gain.value = 0;
      capture = { stream, context, source: audioSource, processor, mute, chunks: [], samples: 0, rate: context.sampleRate, startedAt: performance.now(), timer: 0, released: false };
      operation.capture = capture;
      const activeCapture = capture;
      processor.onaudioprocess = event => {
        if (!owns(operation) || operation.kind !== "recording") return;
        const available = Math.floor(activeCapture.rate * MAX_SECONDS) - activeCapture.samples;
        const length = Math.min(available, event.inputBuffer.length); if (length <= 0) return;
        const samples = new Float32Array(length); let energy = 0;
        for (let channel = 0; channel < event.inputBuffer.numberOfChannels; channel++) { const input = event.inputBuffer.getChannelData(channel);
          for (let index = 0; index < length; index++) samples[index] = samples[index]! + (Number.isFinite(input[index]) ? input[index]! : 0) / event.inputBuffer.numberOfChannels; }
        for (let index = 0; index < length; index++) energy += samples[index]! * samples[index]!;
        activeCapture.chunks.push(samples); activeCapture.samples += length;
        const rms = Math.sqrt(energy / length); const level = Math.round(Math.max(0, Math.min(1, (20 * Math.log10(Math.max(rms, .00001)) + 60) / 60)) * 100);
        el("ai-meter").setAttribute("aria-valuenow", String(level)); el("ai-meter").style.setProperty("--ai-level", level + "%");
        if (activeCapture.samples >= Math.floor(activeCapture.rate * MAX_SECONDS)) queueMicrotask(() => { void finishRecording(operation); });
      };
      audioSource.connect(processor); processor.connect(mute); mute.connect(context.destination);
      await context.resume();
      if (!owns(operation)) { release(activeCapture); return; }
      operation.kind = "recording"; activeCapture.startedAt = performance.now();
      activeCapture.timer = window.setInterval(() => { if (!owns(operation) || operation.kind !== "recording") return;
        const seconds = Math.min(MAX_SECONDS, Math.floor((performance.now() - activeCapture.startedAt) / 1000)); el("ai-record-time").textContent = seconds + " / 60 秒";
        if (seconds >= MAX_SECONDS) void finishRecording(operation);
      }, 100);
      el("ai-record-time").textContent = "0 / 60 秒"; setState("正在录音，结束后识别", "recording"); refresh();
    } catch {
      if (capture) release(capture);
      else { stream?.getTracks().forEach(track => track.stop()); if (context) void context.close().catch(() => {}); }
      if (owns(operation)) { current = null; setError("无法开始录音，请检查 Windows 麦克风权限后重试"); setState("录音未开始", "error"); refresh(); }
    }
  }
  button("ai-record").addEventListener("click", () => { void record(); });
  button("ai-copy").addEventListener("click", async () => {
    if (!native || !output || copying) return;
    const text = output, revision = outputRevision; copying = true; button("ai-copy").disabled = true;
    el("ai-copy-status").textContent = "正在复制…";
    try { await invoke("ai_copy_result", { text }); if (revision === outputRevision) el("ai-copy-status").textContent = "已复制，请到目标输入框手动粘贴"; }
    catch { if (revision === outputRevision) el("ai-copy-status").textContent = "复制失败，内容仍在本页，请重试"; }
    finally { copying = false; button("ai-copy").disabled = !output || !native; }
  });
  const observer = new MutationObserver(() => { if (!pane.classList.contains("is-active") && current) cancel("已离开 AI，当前操作已停止"); });
  observer.observe(pane, { attributes: true, attributeFilter: ["class"] });
  document.addEventListener("visibilitychange", () => { if (document.hidden) cancel("窗口已隐藏，当前操作已停止"); });
  document.getElementById("btn-close")?.addEventListener("click", () => cancel("正在关闭窗口，当前操作已停止"));
  window.addEventListener("pagehide", () => { cancel("窗口已关闭"); disposed = true; observer.disconnect(); unlisten.splice(0).forEach(stop => stop()); });
  async function connect(): Promise<void> {
    if (!native) { configPanel.open = true; refresh(); return; }
    const keepListener = (stop: () => void) => { if (disposed) stop(); else unlisten.push(stop); };
    try { keepListener(await listen<Progress>("ai://progress", event => {
      const operation = current; if (!operation || event.payload.id !== operation.id || typeof event.payload.text !== "string") return;
      if ((operation.kind === "generating" && event.payload.phase === "generating") || (operation.kind === "transcribing" && event.payload.phase === "transcribing")) showProgress(operation, event.payload.text);
    }));
      if (disposed) return;
      keepListener(await listen("ai://window-hidden", () => cancel("窗口已隐藏，当前操作已停止"))); }
    catch { /* Final invoke results still work if the event bridge is unavailable. */ }
    if (!disposed) await load();
  }
  chooseMode("voice"); void connect();
}

async function wavFromCapture(capture: Capture): Promise<Uint8Array> {
  if (!capture.samples) throw new Error("EMPTY_AUDIO");
  const frames = Math.min(16000 * MAX_SECONDS, Math.max(1, Math.round(capture.samples * 16000 / capture.rate)));
  const offline = new OfflineAudioContext(1, frames, 16000);
  const audio = offline.createBuffer(1, capture.samples, capture.rate); const channel = audio.getChannelData(0); let offset = 0;
  for (const chunk of capture.chunks) { channel.set(chunk, offset); offset += chunk.length; }
  const source = offline.createBufferSource(); source.buffer = audio; source.connect(offline.destination); source.start();
  const rendered = await offline.startRendering(); const samples = rendered.getChannelData(0);
  const bytes = new Uint8Array(44 + frames * 2), view = new DataView(bytes.buffer);
  function ascii(at: number, value: string): void { for (let index = 0; index < value.length; index++) view.setUint8(at + index, value.charCodeAt(index)); }
  ascii(0, "RIFF"); view.setUint32(4, bytes.length - 8, true); ascii(8, "WAVE"); ascii(12, "fmt "); view.setUint32(16, 16, true); view.setUint16(20, 1, true); view.setUint16(22, 1, true); view.setUint32(24, 16000, true); view.setUint32(28, 32000, true); view.setUint16(32, 2, true); view.setUint16(34, 16, true); ascii(36, "data"); view.setUint32(40, frames * 2, true);
  for (let index = 0; index < frames; index++) { const value = Math.max(-1, Math.min(1, samples[index] || 0)); view.setInt16(44 + index * 2, Math.round(value * (value < 0 ? 32768 : 32767)), true); }
  return bytes;
}
