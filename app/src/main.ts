import { invoke, isTauri } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";

export interface AppSettings {
  scheme: string;
  mixedInput: boolean;
  chinesePunctuation: boolean;
  shiftToggleEnglish: boolean;
  capsLockBypass: boolean;
  pageSize: number;
  wheelPaging: boolean;
  skin: string;
  customSkinId?: string;
  customSkinHash?: string;
  themeMode: string;
  darkMode: boolean;
  systemDark: boolean;
  firstCandidateSize: number;
  keyOpacity: number;
  typingGlow: boolean;
  keyLetterSize: number;
  keyLetterBold: boolean;
  clipboardEnabled?: boolean;
  candidateLayout: "double" | "single" | "vertical";
  fullWidth: boolean;
  traditional: boolean;
  readonly revision?: string;
}

export interface ClipboardItem {
  id: string;
  text: string;
  pinned: boolean;
  createdAtMs: number;
  source: string;
}

interface SharedItem {
  id: string; senderId: string; senderName: string; createdAt: number; receivedAt: number; expiresAt: number; text: string;
}
interface ShareSnapshot {
  revision: string; viewRevision: string; server: string; topic: string; senderName: string;
  enabled: boolean; autoSend: boolean; tokenSaved: boolean; keySaved: boolean; keyId: string;
  status: string; error: string | null; gap: boolean; needsReset: boolean; items: SharedItem[];
}
let shareState: ShareSnapshot | null = null;
let shareFormDirty = false;
let shareBusy = false;
let sharePausing = false;
let lastShareViewRevision = 0n;
let shareDraftOrigin = "";
let shareUiError: string | null = null;
let altvShared = false;

const SCHEME_LABELS: Record<string, string> = {
  flypy: "小鹤双拼",
  pinyin: "全拼",
  ninekey: "自定义 / 九宫格",
};

const DEFAULT_SETTINGS: AppSettings = {
  scheme: "flypy",
  mixedInput: true,
  chinesePunctuation: true,
  shiftToggleEnglish: true,
  capsLockBypass: true,
  pageSize: 7,
  wheelPaging: true,
  skin: "wood_type",
  themeMode: "system",
  darkMode: false,
  systemDark: false,
  firstCandidateSize: 20,
  keyOpacity: 0.82,
  typingGlow: true,
  keyLetterSize: 18,
  keyLetterBold: false,
  clipboardEnabled: true,
  candidateLayout: "double",
  fullWidth: false,
  traditional: false,
};

type SkinId = "wood_type" | "typewriter" | "terminal" | "brutal";
const SKIN_ORDER: SkinId[] = ["wood_type", "typewriter", "terminal", "brutal"];
const SKIN_LABELS: Record<SkinId, string> = {
  wood_type: "木框活字", typewriter: "经典打字机", terminal: "绿屏终端", brutal: "极客机械",
};
function canonicalSkin(value: string): SkinId {
  if (SKIN_ORDER.includes(value as SkinId)) return value as SkinId;
  const legacy: Record<string, SkinId> = {
    swiss: "wood_type", metro: "terminal", eink: "typewriter",
    warm_grid: "wood_type", native_light: "terminal", deep_blue: "brutal", mint: "typewriter",
    square_cream: "wood_type", square_aluminum: "terminal", square_amber: "brutal", square_jade: "typewriter",
  };
  return Object.prototype.hasOwnProperty.call(legacy, value) ? legacy[value]! : "wood_type";
}

function effectiveDark(): boolean {
  return state.themeMode === "dark" || (state.themeMode === "system" && state.systemDark);
}
function applySkinTheme(): void {
  const skin = canonicalSkin(state.skin);
  const dark = effectiveDark();
  const root = document.documentElement;
  document.body.dataset.skin = skin;
  document.body.dataset.theme = dark ? "dark" : "light";
  root.style.colorScheme = dark ? "dark" : "light";
  root.dataset.imeTheme = skin;
  root.dataset.imeMode = dark ? "dark" : "light";
  document.querySelectorAll<HTMLElement>(".cand-window").forEach((preview) => {
    const tone = preview.id === "cand-window-preview" ? (previewDarkCand ?? dark) : preview.dataset.tone === "dark";
    preview.dataset.imeTheme = skin;
    preview.dataset.candidateLayout = state.candidateLayout;
    preview.dataset.imeMode = tone ? "dark" : "light";
    preview.classList.toggle("is-dark", tone);
    preview.querySelectorAll<HTMLElement>(".cand-num").forEach((node, index) => {
      node.textContent = skin === "brutal" ? `[${index + 1}]` : String(index + 1);
    });
  });
  for (const [id, selected] of [["btn-preview-light", !(previewDarkCand ?? dark)], ["btn-preview-dark", previewDarkCand ?? dark]] as const) {
    document.getElementById(id)?.classList.toggle("is-active", selected);
    document.getElementById(id)?.setAttribute("aria-pressed", String(selected));
  }
  const lightLabel = document.getElementById("showcase-light-label");
  const darkLabel = document.getElementById("showcase-dark-label");
  if (lightLabel) lightLabel.textContent = `${SKIN_LABELS[skin]} · 浅色候选窗`;
  if (darkLabel) darkLabel.textContent = `${SKIN_LABELS[skin]} · 深色候选窗`;
}

const appMode = new URLSearchParams(window.location.search).get("mode") ?? "main";
const nativeApp = isTauri();
let state: AppSettings = { ...DEFAULT_SETTINGS };
let confirmedSettings: AppSettings = { ...DEFAULT_SETTINGS };
let initialSettingsLoaded = !nativeApp;
let finishInitialSettingsLoad: () => void = () => {};
const initialSettingsReady = new Promise<void>((resolve) => { finishInitialSettingsLoad = resolve; });
interface SettingsRequest {
  patch: Partial<AppSettings>;
  reset: boolean;
  successMessage?: string;
  resolve: (saved: boolean) => void;
}
const settingsQueue: SettingsRequest[] = [];
let settingsPump: Promise<void> | null = null;
let settingsReadSequence = 0;
let settingsRefreshPending = false;
let lastSettingsFailed = false;
let closingSettings = false;
let clipItems: ClipboardItem[] = [];
let clipEnabled = true;
let clipboardBusy = false;
let clipboardEventRevision = 0;
let clipboardEnabledReadSequence = 0;
let clipQuery = "";
let altvQuery = "";
let previewDarkCand: boolean | null = null;
let previewLangEng = false;
let languageBusy = false;
let englishEventRevision = 0;
let toastTimer: number | undefined;
let shiftToastTimer: number | undefined;

function showToast(msg: string): void {
  const el = document.getElementById("toast");
  if (!el) return;
  el.setAttribute("role", "status");
  el.setAttribute("aria-live", "polite");
  el.textContent = msg;
  el.classList.add("is-visible");
  if (toastTimer !== undefined) window.clearTimeout(toastTimer);
  toastTimer = window.setTimeout(() => el.classList.remove("is-visible"), 1600);
}

function setSettingsStatus(status: "loading" | "saving" | "saved" | "error" | "preview"): void {
  const label = document.getElementById("sidebar-status-text");
  if (label) {
    label.setAttribute("role", "status");
    label.setAttribute("aria-live", "polite");
    label.textContent = {
      loading: "正在读取设置…",
      saving: "正在保存…",
      saved: "设置已保存",
      error: "保存失败，请重试",
      preview: "界面预览 · 仅本页生效",
    }[status];
  }
  document.querySelector<HTMLElement>(".sidebar-footer")?.setAttribute("data-status", status);
  document.querySelector<HTMLElement>(".sync-dot")?.setAttribute("data-status", status);
}

function renderPendingSettings(): void {
  state = settingsQueue.reduce<AppSettings>((visible, request) => ({ ...visible, ...request.patch }),
    { ...confirmedSettings });
  render();
  renderFloatingStatusBar();
}

/** Read disk instead of applying event payloads which may arrive after a newer save. */
async function refreshConfirmedSettings(): Promise<void> {
  if (!nativeApp || !initialSettingsLoaded) return;
  if (settingsPump || settingsQueue.length) {
    settingsReadSequence++;
    settingsRefreshPending = true;
    return;
  }
  const sequence = ++settingsReadSequence;
  try {
    const loaded = await invoke<AppSettings>("load_settings");
    if (sequence === settingsReadSequence && !settingsPump && !settingsQueue.length) {
      confirmedSettings = loaded;
      renderPendingSettings();
    }
  } catch {
    // A failed refresh must not replace confirmed values with defaults.
  }
}

async function runSettingsQueue(): Promise<void> {
  await initialSettingsReady;
  let failed = false;
  if (!initialSettingsLoaded) {
    try {
      confirmedSettings = await invoke<AppSettings>("load_settings");
      initialSettingsLoaded = true;
    } catch { /* Never save defaults over settings which could not be read. */ }
  }
  do {
    while (settingsQueue.length) {
      const request = settingsQueue[0]!;
      let saved = false;
      if (initialSettingsLoaded) {
        try {
          const result = request.reset
            ? await invoke<AppSettings>("reset_settings")
            : await invoke<AppSettings>("patch_settings", { patch: request.patch });
          confirmedSettings = result;
          saved = true;
        } catch {
          // Keep the last confirmed values, then reapply later queued edits.
        }
      }
      settingsQueue.shift();
      renderPendingSettings();
      if (saved && request.successMessage) showToast(request.successMessage);
      if (!saved) { failed = true; showToast("保存失败，请重试"); }
      request.resolve(saved);
    }
    if (settingsRefreshPending && initialSettingsLoaded) {
      settingsRefreshPending = false;
      const sequence = ++settingsReadSequence;
      try {
        const loaded = await invoke<AppSettings>("load_settings");
        if (sequence === settingsReadSequence) {
          confirmedSettings = loaded;
          renderPendingSettings();
        }
      } catch { /* The successful save receipt remains authoritative. */ }
    }
  } while (settingsQueue.length || (settingsRefreshPending && initialSettingsLoaded));
  lastSettingsFailed = failed || !initialSettingsLoaded;
  setSettingsStatus(lastSettingsFailed ? "error" : "saved");
}

function enqueueSettings(patch: Partial<AppSettings>, successMessage?: string, reset = false): Promise<boolean> {
  if (!nativeApp) {
    confirmedSettings = { ...confirmedSettings, ...patch };
    renderPendingSettings();
    setSettingsStatus("preview");
    if (successMessage) showToast(`界面预览：${successMessage}（仅本页）`);
    return Promise.resolve(true);
  }
  settingsReadSequence++;
  settingsRefreshPending = true;
  const result = new Promise<boolean>((resolve) => {
    settingsQueue.push({ patch: { ...patch }, reset, successMessage, resolve });
  });
  setSettingsStatus("saving");
  renderPendingSettings();
  startSettingsPump();
  return result;
}

function startSettingsPump(): void {
  if (settingsPump) return;
  settingsPump = runSettingsQueue().finally(() => {
    settingsPump = null;
    if (settingsQueue.length) startSettingsPump();
    else if (settingsRefreshPending) {
      settingsRefreshPending = false;
      void refreshConfirmedSettings();
    }
  });
}

function showShiftCaretToast(englishMode?: boolean): void {
  if (typeof englishMode === "boolean") {
    previewLangEng = englishMode;
  } else {
    previewLangEng = !previewLangEng;
  }
  const toastEl = document.getElementById("shift-caret-toast");
  const labelEl = document.getElementById("shift-toast-label");
  const fbLang = document.getElementById("fb-lang");
  if (labelEl) labelEl.textContent = previewLangEng ? "英" : "中";
  if (fbLang) fbLang.textContent = previewLangEng ? "英" : "中";
  if (!toastEl) return;
  toastEl.classList.add("is-visible");
  if (shiftToastTimer !== undefined) window.clearTimeout(shiftToastTimer);
  shiftToastTimer = window.setTimeout(() => {
    toastEl.classList.remove("is-visible");
  }, 1150);
}

async function requestEnglishMode(english: boolean, announce = false): Promise<void> {
  if (languageBusy) return;
  languageBusy = true;
  for (const id of ["fb-lang", "sb-lang"]) document.getElementById(id)?.setAttribute("aria-disabled", "true");
  const revision = englishEventRevision;
  try {
    if (nativeApp) await invoke("set_english_mode", { english });
    if (revision === englishEventRevision) previewLangEng = english;
    showShiftCaretToast(previewLangEng);
    renderFloatingStatusBar();
    if (!nativeApp) showToast(`界面预览：${previewLangEng ? "英文" : "中文"}模式`);
    else if (announce) showToast(previewLangEng ? "已切换为英文模式" : "已切换为中文模式");
  } catch { showToast("语言切换失败，请重试"); }
  finally {
    languageBusy = false;
    for (const id of ["fb-lang", "sb-lang"]) document.getElementById(id)?.setAttribute("aria-disabled", "false");
  }
}

function formatRelativeTime(ms: number): string {
  if (!ms) return "刚刚";
  const diffSec = Math.max(0, Math.floor((Date.now() - ms) / 1000));
  if (diffSec < 45) return "刚刚";
  const diffMin = Math.floor(diffSec / 60);
  if (diffMin < 60) return `${diffMin} 分钟前`;
  const diffHr = Math.floor(diffMin / 60);
  if (diffHr < 24) return `${diffHr} 小时前`;
  const d = new Date(ms);
  return `${String(d.getHours()).padStart(2, "0")}:${String(d.getMinutes()).padStart(2, "0")}`;
}

/** Keep keyboard focus on the same item/action when a clipboard snapshot rebuilds the list. */
function replaceClipboardRows(container: HTMLElement, build: () => void): void {
  const focused = document.activeElement instanceof HTMLElement ? document.activeElement : null;
  const oldRow = focused?.closest<HTMLElement>("[data-clipboard-id]");
  const ownedFocus = !!focused && !!oldRow && container.contains(focused);
  const id = oldRow?.dataset.clipboardId;
  const action = focused?.dataset.clipboardAction;
  const index = oldRow ? Array.from(container.children).indexOf(oldRow) : -1;
  container.replaceChildren();
  container.setAttribute("role", "list");
  build();
  if (!ownedFocus) return;
  const rows = Array.from(container.querySelectorAll<HTMLElement>("[data-clipboard-id]"));
  const row = rows.find((candidate) => candidate.dataset.clipboardId === id)
    ?? rows[Math.min(Math.max(index, 0), rows.length - 1)];
  const button = row && (Array.from(row.querySelectorAll<HTMLButtonElement>("[data-clipboard-action]"))
    .find((candidate) => candidate.dataset.clipboardAction === action && !candidate.disabled)
    ?? row.querySelector<HTMLButtonElement>(".clip-card-main"));
  if (button) button.focus({ preventScroll: true });
  else {
    const search = document.getElementById(container.id === "altv-list" ? "altv-search" : "clip-search");
    if (search && !search.closest("[hidden]")) search.focus({ preventScroll: true });
  }
}

async function clipboardAction<T>(
  operation: () => Promise<T>,
  apply: (result: T, revision: number) => void,
  preview: () => void,
  errorMessage: string,
  successMessage?: string,
): Promise<boolean> {
  if (clipboardBusy) return false;
  clipboardBusy = true;
  renderClipboard();
  try {
    if (nativeApp) {
      const revision = clipboardEventRevision;
      apply(await operation(), revision);
      if (successMessage) showToast(successMessage);
    } else {
      preview();
      showToast("界面预览 · 操作仅在本页演示");
    }
    return true;
  } catch {
    showToast(errorMessage);
    return false;
  } finally {
    clipboardBusy = false;
    renderClipboard();
  }
}

function applyClipboardItems(items: ClipboardItem[], revision: number): void {
  // A newer clipboard event may have arrived while this operation's receipt was in flight.
  if (revision === clipboardEventRevision) clipItems = items;
}

function buildClipboardRow(item: ClipboardItem): HTMLElement {
  const card = document.createElement("div");
  card.className = `clip-card-item${item.pinned ? " is-pinned" : ""}`;
  card.dataset.clipboardId = item.id;
  card.setAttribute("role", "listitem");

  const main = document.createElement("button");
  main.type = "button";
  main.className = "clip-card-main";
  main.dataset.clipboardAction = "copy";
  main.setAttribute("aria-label", `复制剪贴板内容：${item.text.slice(0, 160)}`);
  main.setAttribute("aria-disabled", String(clipboardBusy));
  main.title = item.text;

  const txt = document.createElement("span");
  txt.className = "clip-card-text";
  txt.textContent = item.text;
  main.appendChild(txt);

  const right = document.createElement("div");
  right.className = "clip-card-right";

  const actions = document.createElement("div");
  actions.className = "clip-card-actions";

  const pinBtn = document.createElement("button");
  pinBtn.type = "button";
  pinBtn.className = `clip-icon-btn${item.pinned ? " is-pinned" : ""}`;
  pinBtn.title = item.pinned ? "取消置顶" : "置顶该条";
  pinBtn.dataset.clipboardAction = "pin";
  pinBtn.setAttribute("aria-label", pinBtn.title);
  pinBtn.setAttribute("aria-pressed", String(item.pinned));
  pinBtn.setAttribute("aria-disabled", String(clipboardBusy));
  const pinIcon = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  pinIcon.setAttribute("viewBox", "0 0 20 20");
  pinIcon.setAttribute("width", "16"); pinIcon.setAttribute("height", "16");
  pinIcon.setAttribute("aria-hidden", "true");
  const pinPath = document.createElementNS("http://www.w3.org/2000/svg", "path");
  pinPath.setAttribute("d", "M7 3h6l-1 5 3 3H5l3-3-1-5ZM10 11v6");
  pinPath.setAttribute("fill", "none"); pinPath.setAttribute("stroke", "currentColor");
  pinPath.setAttribute("stroke-width", "1.4"); pinIcon.appendChild(pinPath); pinBtn.appendChild(pinIcon);
  pinBtn.addEventListener("click", async (e) => {
    e.stopPropagation();
    await clipboardAction(() => invoke<ClipboardItem[]>("toggle_pin_clipboard", { id: item.id }),
      applyClipboardItems,
      () => { clipItems = clipItems.map((entry) => entry.id === item.id ? { ...entry, pinned: !entry.pinned } : entry); },
      "置顶操作失败，请重试");
  });

  const delBtn = document.createElement("button");
  delBtn.type = "button";
  delBtn.className = "clip-icon-btn";
  delBtn.title = "删除";
  delBtn.dataset.clipboardAction = "delete";
  delBtn.setAttribute("aria-label", "删除剪贴板记录");
  delBtn.setAttribute("aria-disabled", String(clipboardBusy));
  delBtn.textContent = "×";
  delBtn.addEventListener("click", async (e) => {
    e.stopPropagation();
    await clipboardAction(() => invoke<ClipboardItem[]>("delete_clipboard_item", { id: item.id }),
      applyClipboardItems, () => { clipItems = clipItems.filter((entry) => entry.id !== item.id); },
      "删除失败，请重试");
  });

  const sendBtn = document.createElement("button");
  sendBtn.type = "button";
  sendBtn.className = "clip-icon-btn share-local-send";
  sendBtn.title = "发送到已配对设备";
  sendBtn.dataset.clipboardAction = "send";
  sendBtn.setAttribute("aria-label", "发送到已配对设备");
  sendBtn.textContent = "↗";
  sendBtn.disabled = !shareState?.enabled || shareFormDirty || shareBusy;
  sendBtn.addEventListener("click", async (event) => {
    event.stopPropagation();
    const revision = shareState?.revision;
    if (!revision) return;
    const ok = await shareAction(() => invoke("clipboard_share_send_item", { id: item.id, revision }));
    if (ok) showToast("已加入发送队列");
  });
  actions.appendChild(sendBtn);
  actions.appendChild(pinBtn);
  actions.appendChild(delBtn);

  const timeEl = document.createElement("span");
  timeEl.className = "clip-card-time";
  timeEl.textContent = formatRelativeTime(item.createdAtMs);

  right.appendChild(actions);
  right.appendChild(timeEl);

  card.appendChild(main);
  card.appendChild(right);

  main.addEventListener("click", async () => {
    const copied = await clipboardAction(() => invoke<ClipboardItem[]>("copy_clipboard_item", { id: item.id }),
      applyClipboardItems, () => {}, "复制失败，请重试", "已复制到剪贴板");
    if (copied && nativeApp && appMode === "clipboard") {
      void invoke("hide_clipboard_window").catch(() => {});
    }
  });

  return card;
}

function renderListInto(container: HTMLElement | null, query: string): void {
  if (!container) return;
  const q = query.trim().toLowerCase();
  const local = container.id === "altv-list" && altvShared ? [] : clipItems;
  const filtered = q
    ? local.filter((item) => item.text.toLowerCase().includes(q))
    : local;

  const remote = container.id === "altv-list" && altvShared
    ? (shareState?.items ?? []).filter((item) => !q || item.text.toLowerCase().includes(q) || item.senderName.toLowerCase().includes(q))
    : [];
  replaceClipboardRows(container, () => {
    for (const item of remote) container.appendChild(buildSharedRow(item));
    if (filtered.length === 0 && remote.length === 0) {
      const empty = document.createElement("div");
      empty.className = "clip-empty-state";
      empty.textContent = q ? "没有匹配的剪贴板内容"
        : container.id === "altv-list" && altvShared ? "暂无共享内容，请从另一台已配对设备发送"
        : !nativeApp ? "界面预览：剪贴板记录会在方寸桌面伴侣中显示"
        : clipEnabled ? "暂无剪贴板记录 — 复制文字后会自动收录"
        : "剪贴板监听已暂停，开启后记录新复制的文字";
      container.appendChild(empty);
      return;
    }
    for (const item of filtered) container.appendChild(buildClipboardRow(item));
  });
}

function renderClipboard(): void {
  const countEl = document.getElementById("clip-tab-count");
  if (countEl) countEl.textContent = String(clipItems.length);

  for (const prefix of ["clip", "altv"]) {
    const txt = document.getElementById(`${prefix}-watch-text`);
    const dot = document.getElementById(`${prefix}-watch-dot`);
    if (txt) txt.textContent = nativeApp ? (clipEnabled ? "监听中" : "已暂停") : "界面预览";
    if (dot) dot.classList.toggle("is-on", nativeApp && clipEnabled);
    document.getElementById(`${prefix}-toggle-watch`)?.setAttribute("aria-pressed", String(clipEnabled));
    document.getElementById(`${prefix}-toggle-watch`)?.setAttribute("aria-disabled", String(clipboardBusy));
    document.getElementById(`${prefix}-clear`)?.setAttribute("aria-disabled", String(clipboardBusy));
  }

  const swGenClip = document.getElementById("sw-gen-clip") as HTMLInputElement | null;
  if (swGenClip) swGenClip.checked = clipEnabled;

  renderListInto(document.getElementById("clip-list"), clipQuery);
  renderListInto(document.getElementById("altv-list"), altvQuery);
}

function render(): void {
  state.skin = canonicalSkin(state.skin);
  applySkinTheme();
  // Scheme cards
  document.querySelectorAll<HTMLButtonElement>("[data-scheme]").forEach((btn) => {
    btn.classList.toggle("is-selected", btn.dataset.scheme === state.scheme);
    btn.setAttribute("aria-pressed", String(btn.dataset.scheme === state.scheme));
  });

  // Skin & Mode buttons
  document.querySelectorAll<HTMLButtonElement>("button[data-skin]").forEach((btn) => {
    btn.classList.toggle("is-selected", btn.dataset.skin === state.skin);
    btn.setAttribute("aria-pressed", String(btn.dataset.skin === state.skin));
  });
  document.querySelectorAll<HTMLButtonElement>("[data-mode]").forEach((btn) => {
    btn.classList.toggle("is-selected", btn.dataset.mode === state.themeMode);
    btn.setAttribute("aria-pressed", String(btn.dataset.mode === state.themeMode));
  });

  // Switches
  (document.getElementById("sw-mixed") as HTMLInputElement).checked = state.mixedInput;
  (document.getElementById("sw-punct") as HTMLInputElement).checked = state.chinesePunctuation;
  (document.getElementById("sw-shift-toggle") as HTMLInputElement).checked = state.shiftToggleEnglish;
  (document.getElementById("sw-capslock") as HTMLInputElement).checked = state.capsLockBypass;
  (document.getElementById("sw-wheel") as HTMLInputElement).checked = state.wheelPaging;
  (document.getElementById("sw-glow") as HTMLInputElement).checked = state.typingGlow;

  (document.getElementById("sel-candidate-layout") as HTMLSelectElement).value = state.candidateLayout;
  const pageSelect = document.getElementById("sel-page-size") as HTMLSelectElement;
  pageSelect.value = String(state.pageSize || 7);

  // Metrics
  document.getElementById("first-val")!.textContent = `${state.firstCandidateSize} px`;

  // The candidate preview shares the active native skin and its selected light/dark variant.
  const firstWord = document.getElementById("preview-first-word")!;
  firstWord.style.fontSize = `${state.firstCandidateSize}px`;

  // Floating status bar
  const fbPunct = document.getElementById("fb-punct");
  if (fbPunct) fbPunct.textContent = state.chinesePunctuation ? "“。" : "\".";

  const genSummary = document.getElementById("gen-scheme-summary");
  if (genSummary) genSummary.textContent = SCHEME_LABELS[state.scheme] ?? "小鹤双拼";
  window.dispatchEvent(new CustomEvent("fangcun:theme-applied", { detail: {
    skin: state.skin, dark: effectiveDark(), themeMode: state.themeMode,
    customSkinId: state.customSkinId, customSkinHash: state.customSkinHash,
  } }));
}

function save(patch: Partial<AppSettings>, successMessage?: string): Promise<boolean> {
  return enqueueSettings(patch, successMessage);
}

function switchTab(tab: string): void {
  if (!Array.from(document.querySelectorAll<HTMLElement>(".tab-pane")).some((pane) => pane.dataset.pane === tab)) return;
  document.querySelectorAll<HTMLButtonElement>("#sidebar-nav [data-tab]").forEach((btn) => {
    btn.classList.toggle("is-active", btn.dataset.tab === tab);
    if (btn.dataset.tab === tab) btn.setAttribute("aria-current", "page");
    else btn.removeAttribute("aria-current");
  });
  document.querySelectorAll<HTMLElement>(".tab-pane").forEach((pane) => {
    pane.classList.toggle("is-active", pane.dataset.pane === tab);
  });
}

function triggerGlow(x: number, y: number): void {
  if (!state.typingGlow || window.matchMedia("(prefers-reduced-motion: reduce)").matches) return;
  const layer = document.getElementById("glow-layer")!;
  const pulse = document.createElement("div");
  pulse.className = "glow-pulse";
  pulse.style.left = `${x}px`;
  pulse.style.top = `${y}px`;
  if (layer.childElementCount >= 6) layer.firstElementChild?.remove();
  layer.appendChild(pulse);
  pulse.addEventListener("animationend", () => pulse.remove());
}

function openAltVPopup(show: boolean): void {
  const overlay = document.getElementById("altv-overlay");
  if (!overlay) return;
  overlay.classList.toggle("is-hidden", !show);
  if (show) {
    const input = document.getElementById("altv-search") as HTMLInputElement | null;
    input?.focus();
  }
}

// Sidebar Navigation
document.getElementById("sidebar-nav")!.addEventListener("click", (e) => {
  const btn = (e.target as HTMLElement).closest<HTMLButtonElement>("[data-tab]");
  if (!btn?.dataset.tab) return;
  switchTab(btn.dataset.tab);
});

// Keep search local and navigate only when the query has a known destination.
const settingsSearch = document.getElementById("settings-search")!;
const searchFeedback = document.createElement("p");
searchFeedback.id = "settings-search-feedback";
searchFeedback.className = "settings-search-feedback";
searchFeedback.setAttribute("role", "status");
searchFeedback.setAttribute("aria-live", "polite");
searchFeedback.hidden = true;
settingsSearch.setAttribute("aria-describedby", searchFeedback.id);
settingsSearch.parentElement?.appendChild(searchFeedback);
settingsSearch.addEventListener("input", (e) => {
  const q = (e.target as HTMLInputElement).value.trim().toLowerCase();
  searchFeedback.hidden = true;
  searchFeedback.textContent = "";
  if (!q) return;
  const targets: [string, string[]][] = [
    ["clipboard", ["剪贴", "clip", "alt", "共享", "配对"]],
    ["theme", ["皮肤", "外观", "深色", "浅色", "跟随系统", "木框", "活字", "打字机", "终端", "极客", "机械", "wood", "typewriter", "terminal", "brutal"]],
    ["effects", ["字号", "流光", "透明", "特效", "字体"]],
    ["general", ["状态栏", "常规"]],
    ["about", ["关于", "版本", "配置文件"]],
    ["ai", ["ai", "智能", "翻译", "润色"]],
    ["input", ["输入", "方案", "小鹤", "双拼", "全拼", "九宫", "候选", "分页", "滚轮", "标点", "混输", "shift", "caps", "page", "wheel"]],
  ];
  const matched = targets.find(([, words]) => words.some((word) => q.includes(word)));
  if (matched) switchTab(matched[0]);
  else { searchFeedback.textContent = "没有找到匹配设置"; searchFeedback.hidden = false; }
});

// Preview Stage Interactions
document.getElementById("preview-card")!.addEventListener("click", (e) => {
  const rect = document.getElementById("cand-window-preview")!.getBoundingClientRect();
  const inside = e.clientX >= rect.left && e.clientX <= rect.right && e.clientY >= rect.top && e.clientY <= rect.bottom;
  triggerGlow(inside ? e.clientX - rect.left : rect.width / 2, inside ? e.clientY - rect.top : rect.height / 2);
});

document.getElementById("btn-preview-light")!.addEventListener("click", () => {
  previewDarkCand = false;
  document.getElementById("btn-preview-light")!.classList.add("is-active");
  document.getElementById("btn-preview-dark")!.classList.remove("is-active");
  render();
});

document.getElementById("btn-preview-dark")!.addEventListener("click", () => {
  previewDarkCand = true;
  document.getElementById("btn-preview-dark")!.classList.add("is-active");
  document.getElementById("btn-preview-light")!.classList.remove("is-active");
  render();
});

document.getElementById("btn-preview-shift")!.addEventListener("click", () => {
  showShiftCaretToast();
});

document.getElementById("btn-open-clip-popup")!.addEventListener("click", () => {
  openAltVPopup(true);
});

// Scheme Cards
document.getElementById("scheme-group")!.addEventListener("click", (e) => {
  const btn = (e.target as HTMLElement).closest<HTMLButtonElement>("[data-scheme]");
  if (btn?.dataset.scheme) {
    void save({ scheme: btn.dataset.scheme }, `已切换为${SCHEME_LABELS[btn.dataset.scheme] ?? "小鹤双拼"}`);
  }
});

// Input Behavior Switches & Page Size
document.getElementById("sw-mixed")!.addEventListener("change", (e) => {
  void save({ mixedInput: (e.target as HTMLInputElement).checked });
});
document.getElementById("sw-punct")!.addEventListener("change", (e) => {
  void save({ chinesePunctuation: (e.target as HTMLInputElement).checked });
});
document.getElementById("sw-shift-toggle")!.addEventListener("change", (e) => {
  void save({ shiftToggleEnglish: (e.target as HTMLInputElement).checked });
});
document.getElementById("sw-capslock")!.addEventListener("change", (e) => {
  void save({ capsLockBypass: (e.target as HTMLInputElement).checked });
});
document.getElementById("sel-candidate-layout")!.addEventListener("change", (event) => {
  const candidateLayout = (event.target as HTMLSelectElement).value;
  if (candidateLayout === "double" || candidateLayout === "single" || candidateLayout === "vertical") {
    void save({ candidateLayout });
  }
});

document.getElementById("sel-page-size")!.addEventListener("change", (e) => {
  const val = Number((e.target as HTMLSelectElement).value) || 7;
  void save({ pageSize: val }, `每页候选词数量已设为 ${val}`);
});
document.getElementById("sw-wheel")!.addEventListener("change", (e) => {
  void save({ wheelPaging: (e.target as HTMLInputElement).checked });
});

// Shortcut Card Interactive Buttons
document.getElementById("sc-alt-v")!.addEventListener("click", () => {
  openAltVPopup(true);
});
document.getElementById("sc-shift")!.addEventListener("click", () => {
  showShiftCaretToast();
});
document.getElementById("sc-ctrl-dot")!.addEventListener("click", () => {
  const chinesePunctuation = !state.chinesePunctuation;
  void save({ chinesePunctuation }, chinesePunctuation ? "已切换为中文标点 (“。)" : "已切换为英文标点 (\".)");
});

// Floating Status Bar Interactive Buttons
document.getElementById("fb-lang")!.addEventListener("click", () => {
  void requestEnglishMode(!previewLangEng, true);
});
document.getElementById("fb-punct")!.addEventListener("click", () => {
  void save({ chinesePunctuation: !state.chinesePunctuation });
});
document.getElementById("fb-skin")!.addEventListener("click", () => {
  switchTab("theme");
});
document.getElementById("fb-settings")!.addEventListener("click", () => {
  switchTab("input");
});
document.getElementById("btn-toggle-statusbar")?.addEventListener("click", () => {
  if (!nativeApp) { showToast("界面预览：独立状态栏在方寸桌面伴侣中显示"); return; }
  void invoke("toggle_statusbar").catch(() => showToast("状态栏操作失败，请重试"));
});

// Theme & Skin Controls
document.getElementById("skin-group")!.addEventListener("click", (e) => {
  const btn = (e.target as HTMLElement).closest<HTMLButtonElement>("[data-skin]");
  if (btn?.dataset.skin) {
    const selection = new CustomEvent("fangcun:skin-select", { cancelable: true, detail: { id: "builtin:" + btn.dataset.skin } });
    if (!window.dispatchEvent(selection)) return;
    const skin = canonicalSkin(btn.dataset.skin);
    previewDarkCand = null;
    void save({ skin });
  }
});
document.getElementById("mode-group")!.addEventListener("click", (e) => {
  const btn = (e.target as HTMLElement).closest<HTMLButtonElement>("[data-mode]");
  if (btn?.dataset.mode) {
    previewDarkCand = null;
    void save({ themeMode: btn.dataset.mode });
  }
});

// Effects & Metrics Controls
document.getElementById("sw-glow")!.addEventListener("change", (e) => {
  const enabled = (e.target as HTMLInputElement).checked;
  if (!enabled) document.getElementById("glow-layer")?.replaceChildren();
  void save({ typingGlow: enabled });
});

const clamp = (v: number, lo: number, hi: number) => Math.max(lo, Math.min(hi, v));
document.getElementById("first-minus")!.addEventListener("click", () => {
  void save({ firstCandidateSize: clamp(state.firstCandidateSize - 1, 16, 30) });
});
document.getElementById("first-plus")!.addEventListener("click", () => {
  void save({ firstCandidateSize: clamp(state.firstCandidateSize + 1, 16, 30) });
});

// Clipboard Controls
/** Ignore delayed boolean event payloads; read the current committed runtime. */
async function refreshClipboardEnabled(): Promise<boolean> {
  if (!nativeApp) return true;
  const sequence = ++clipboardEnabledReadSequence;
  try {
    const enabled = await invoke<boolean>("get_clipboard_enabled");
    if (sequence !== clipboardEnabledReadSequence) return false;
    clipEnabled = enabled;
    confirmedSettings = { ...confirmedSettings, clipboardEnabled: enabled };
    renderClipboard();
    return true;
  } catch {
    if (sequence === clipboardEnabledReadSequence) showToast("无法读取监听状态，请重试");
    return false;
  }
}

async function setClipboardWatch(want: boolean): Promise<void> {
  const saved = await clipboardAction(() => invoke<boolean>("set_clipboard_enabled", { enabled: want }),
    () => {}, () => { clipEnabled = want; confirmedSettings = { ...confirmedSettings, clipboardEnabled: want }; },
    "监听设置失败，请重试");
  if (saved && nativeApp && await refreshClipboardEnabled()) showToast(clipEnabled ? "已开启剪贴板监听" : "已暂停剪贴板监听");
}

async function clearUnpinnedClipboard(): Promise<void> {
  await clipboardAction(() => invoke<ClipboardItem[]>("clear_clipboard", { keepPinned: true }),
    applyClipboardItems, () => { clipItems = clipItems.filter((entry) => entry.pinned); },
    "清空失败，请重试", "已清空未置顶剪贴板记录");
}

document.getElementById("clip-search")!.addEventListener("input", (e) => {
  clipQuery = (e.target as HTMLInputElement).value;
  renderClipboard();
});
document.getElementById("altv-search")!.addEventListener("input", (e) => {
  altvQuery = (e.target as HTMLInputElement).value;
  renderClipboard();
});

document.getElementById("clip-toggle-watch")!.addEventListener("click", () => void setClipboardWatch(!clipEnabled));
document.getElementById("altv-toggle-watch")!.addEventListener("click", () => void setClipboardWatch(!clipEnabled));
document.getElementById("sw-gen-clip")!.addEventListener("change", (e) => {
  void setClipboardWatch((e.target as HTMLInputElement).checked);
});

document.getElementById("clip-clear")!.addEventListener("click", () => void clearUnpinnedClipboard());
document.getElementById("altv-clear")!.addEventListener("click", () => void clearUnpinnedClipboard());

function closeAltVOrHide(): void {
  if (appMode === "clipboard") {
    void invoke("hide_clipboard_window").catch(() => {});
  } else {
    openAltVPopup(false);
  }
}

document.getElementById("altv-close")!.addEventListener("click", closeAltVOrHide);
document.getElementById("altv-open-settings")!.addEventListener("click", () => {
  if (appMode === "clipboard") {
    void invoke("open_settings_tab", { tab: "clipboard" }).catch(() => {});
  } else {
    openAltVPopup(false);
    switchTab("clipboard");
  }
});
document.getElementById("altv-overlay")!.addEventListener("click", (e) => {
  if (e.target === e.currentTarget && appMode !== "clipboard") openAltVPopup(false);
});

window.addEventListener("keydown", (e) => {
  if (e.key === "Escape") closeAltVOrHide();
  if (appMode === "main" && e.altKey && (e.key === "v" || e.key === "V")) {
    e.preventDefault();
    openAltVPopup(true);
  }
});

// Floating Status Bar Controls (Mockup 2 #3)
function renderFloatingStatusBar(): void {
  const sbLang = document.getElementById("sb-lang");
  const sbPunct = document.getElementById("sb-punct");
  const widthIcon = document.querySelector<HTMLElement>("#sb-width .sharp-icon");
  widthIcon?.setAttribute("data-icon", state.fullWidth ? "full-width" : "half-width");
  document.getElementById("sb-width")?.setAttribute("aria-pressed", String(state.fullWidth));
  const script = document.getElementById("sb-script");
  if (script) script.textContent = state.traditional ? "繁" : "简";
  const fbLang = document.getElementById("fb-lang");
  if (sbLang) {
    sbLang.textContent = previewLangEng ? "EN" : "中";
    sbLang.classList.toggle("is-english", previewLangEng);
  }
  if (fbLang) {
    fbLang.textContent = previewLangEng ? "英" : "中";
  }
  if (sbPunct) {
    sbPunct.textContent = state.chinesePunctuation ? "。，" : ".,";
    sbPunct.classList.toggle("is-muted", !state.chinesePunctuation);
  }
}

document.getElementById("sb-lang")?.addEventListener("click", () => {
  void requestEnglishMode(!previewLangEng);
});
document.getElementById("sb-punct")?.addEventListener("click", () => {
  void save({ chinesePunctuation: !state.chinesePunctuation }).then(() => {
    renderFloatingStatusBar();
  });
});
document.getElementById("sb-width")?.addEventListener("click", () => {
  void save({ fullWidth: !state.fullWidth }).then(renderFloatingStatusBar);
});
document.getElementById("sb-script")?.addEventListener("click", () => {
  void save({ traditional: !state.traditional }).then(renderFloatingStatusBar);
});
document.getElementById("sb-settings")?.addEventListener("click", () => {
  void invoke("open_settings_tab", { tab: "input" }).catch(() => {});
});

// Footer Buttons
document.getElementById("btn-reset")!.addEventListener("click", () => {
  void enqueueSettings({ ...DEFAULT_SETTINGS, systemDark: confirmedSettings.systemDark }, "已恢复默认设置", true);
});

document.getElementById("btn-close")!.addEventListener("click", async () => {
  if (!nativeApp) { showToast("界面预览 · 设置仅在本页生效"); return; }
  if (closingSettings) return;
  closingSettings = true;
  try {
    while (settingsPump) await settingsPump;
    if (lastSettingsFailed || !initialSettingsLoaded) { showToast("设置未保存，请重试"); return; }
    await invoke("hide_window");
  } catch { showToast("关闭失败，请重试"); }
  finally { closingSettings = false; }
});

async function init(): Promise<void> {
  if (appMode === "clipboard") {
    document.body.classList.add("mode-clipboard");
    openAltVPopup(true);
  } else if (appMode === "statusbar") {
    document.body.classList.add("mode-statusbar");
    document.getElementById("floating-statusbar")?.classList.remove("is-hidden");
  }
  switchTab(document.querySelector<HTMLButtonElement>("#sidebar-nav .is-active")?.dataset.tab ?? "input");
  setSettingsStatus(nativeApp ? "loading" : "preview");
  if (!nativeApp) {
    const systemTheme = window.matchMedia("(prefers-color-scheme: dark)");
    confirmedSettings.systemDark = systemTheme.matches;
    systemTheme.addEventListener("change", (event) => {
      confirmedSettings.systemDark = event.matches;
      renderPendingSettings();
    });
    finishInitialSettingsLoad();
    renderPendingSettings();
    renderClipboard();
    return;
  }
  try {
    confirmedSettings = await invoke<AppSettings>("load_settings");
    initialSettingsLoaded = true;
    renderPendingSettings();
    setSettingsStatus(settingsQueue.length ? "saving" : "saved");
  } catch {
    lastSettingsFailed = true;
    setSettingsStatus("error");
    showToast("无法读取设置，请重试");
  } finally { finishInitialSettingsLoad(); }
  try {
    clipItems = await invoke<ClipboardItem[]>("list_clipboard");
    await refreshClipboardEnabled();
  } catch { showToast("剪贴板暂时不可用，请重试"); }
  try {
    await listen<ClipboardItem[]>("clipboard://changed", (event) => {
      clipboardEventRevision++;
      clipItems = event.payload;
      renderClipboard();
    });
    await listen<boolean>("clipboard://enabled-changed", () => {
      void refreshClipboardEnabled();
      void refreshConfirmedSettings();
    });
    await listen("clipboard://persistence-error", () => {
      showToast("剪贴板历史保存失败，请检查存储后重试");
    });
    await listen<AppSettings>("settings://changed", () => { void refreshConfirmedSettings(); });
    await listen<boolean>("ime://english-changed", (event) => {
      englishEventRevision++;
      previewLangEng = event.payload;
      renderFloatingStatusBar();
    });
    await listen("ui://clipboard-opened", () => {
      if (appMode === "clipboard") {
        openAltVPopup(true);
      }
    });
    await listen<string>("ui://switch-tab", (event) => {
      if (appMode === "main" && event.payload) {
        switchTab(event.payload);
      }
    });
    await listen("ui://open-clipboard", () => {
      if (appMode === "main") {
        openAltVPopup(true);
      }
    });
  } catch {
    showToast("界面同步暂时不可用，请重新打开方寸设置");
  }
  // Catch changes between the initial snapshot and listener registration.
  await refreshConfirmedSettings();
  await refreshClipboardEnabled();
  try {
    if (await invoke<string | null>("clipboard_persistence_status")) {
      showToast("剪贴板历史保存失败，请检查存储后重试");
    }
  } catch { /* Existing operation/status messages remain visible. */ }
  render();
  renderClipboard();
  renderFloatingStatusBar();
}

void init();

function selectAltVView(shared: boolean): void {
  altvShared = shared;
  document.getElementById("altv-local-tab")!.setAttribute("aria-selected", String(!shared));
  document.getElementById("altv-shared-tab")!.setAttribute("aria-selected", String(shared));
  const localActions = document.getElementById("altv-clear")!.closest(".clip-footer-left") as HTMLElement | null;
  if (localActions) localActions.hidden = shared;
  renderClipboard();
}
function shareInput(id: string): HTMLInputElement { return document.getElementById(id) as HTMLInputElement; }
function shareOrigin(value: string): string {
  try { const url = new URL(value); return url.protocol === "https:" ? url.origin : ""; } catch { return ""; }
}
function selectClipboardView(shared: boolean): void {
  document.getElementById("clip-local-panel")!.hidden = shared;
  document.getElementById("clip-share-panel")!.hidden = !shared;
  document.getElementById("clip-local-tab")!.setAttribute("aria-selected", String(!shared));
  document.getElementById("clip-shared-tab")!.setAttribute("aria-selected", String(shared));
}
function shareError(message: string): void {
  const element = document.getElementById("share-error")!;
  element.textContent = message; element.hidden = !message;
}
function applyShareSnapshot(value: ShareSnapshot): void {
  if (!/^\d+$/.test(value.viewRevision)) { shareError("共享组件版本不匹配，请更新桌面伴侣"); return; }
  const revision = BigInt(value.viewRevision);
  if (revision < lastShareViewRevision) return;
  lastShareViewRevision = revision;
  const initial = shareState === null;
  shareState = value;
  if (initial && (!value.keySaved || !value.tokenSaved || value.needsReset)) {
    (document.getElementById("share-settings") as HTMLDetailsElement).open = true;
  }
  renderShare();
  renderClipboard();
}
function renderShare(): void {
  const status = document.getElementById("share-status");
  if (!status) return;
  status.textContent = shareState?.status ?? "共享组件尚未连接";
  shareInput("share-enabled").checked = shareState?.enabled ?? false;
  shareInput("share-auto").checked = shareState?.autoSend ?? false;
  if (shareState && !shareFormDirty) {
    shareInput("share-server").value = shareState.server;
    shareInput("share-topic").value = shareState.topic;
    shareInput("share-name").value = shareState.senderName;
    shareDraftOrigin = shareOrigin(shareState.server);
  }
  const originMatches = !!shareState && shareOrigin(shareInput("share-server").value) === shareOrigin(shareState.server);
  document.getElementById("share-token-state")!.textContent = shareState?.tokenSaved && originMatches
    ? "已加密保存（不回显）" : originMatches ? "未保存" : "新站点需要本机独立 Token";
  document.getElementById("share-key-state")!.textContent = shareState?.keySaved
    ? `已加密保存 · ${shareState.keyId.slice(0, 8)}` : "未保存";
  document.getElementById("share-gap")!.hidden = !shareState?.gap;
  shareError(shareUiError ?? shareState?.error ?? "");
  const enabled = !!shareState?.enabled && !shareFormDirty && !shareBusy;
  shareInput("share-enabled").disabled = shareBusy || shareFormDirty || !shareState || !!shareState.needsReset;
  shareInput("share-auto").disabled = shareBusy || shareFormDirty || !shareState || !!shareState.needsReset;
  (document.getElementById("share-send-current") as HTMLButtonElement).disabled = !enabled;
  for (const id of ["share-save", "share-import", "share-new-key", "share-export", "share-reset"]) {
    (document.getElementById(id) as HTMLButtonElement).disabled = shareBusy || !shareState;
  }
  (document.getElementById("share-export") as HTMLButtonElement).disabled ||= !shareState?.keySaved || shareFormDirty;
  (document.getElementById("share-new-key") as HTMLButtonElement).disabled ||= shareFormDirty || !!shareState?.needsReset;
  const list = document.getElementById("share-list")!;
  replaceClipboardRows(list, () => {
    for (const item of shareState?.items ?? []) list.appendChild(buildSharedRow(item));
    if (!list.childElementCount) {
      const empty = document.createElement("div"); empty.className = "clip-empty-state";
      empty.textContent = "暂无共享内容。配对并开启后，可从另一台设备发送文字。"; list.appendChild(empty);
    }
  });
}
function buildSharedRow(item: SharedItem): HTMLElement {
  const card = document.createElement("div"); card.className = "clip-card-item";
  card.dataset.clipboardId = item.id; card.setAttribute("role", "listitem");
  const main = document.createElement("button"); main.type = "button"; main.className = "clip-card-main";
  main.dataset.clipboardAction = "copy";
  main.setAttribute("aria-label", `复制共享内容：${item.text.slice(0, 160)}`);
  main.setAttribute("aria-disabled", String(shareBusy)); main.title = item.text;
  const text = document.createElement("span"); text.className = "clip-card-text"; text.textContent = item.text;
  const source = document.createElement("span"); source.className = "share-source";
  source.textContent = `${item.senderName} · 共享 · ${formatRelativeTime(item.createdAt)}`;
  main.append(text, source);
  const remove = document.createElement("button"); remove.type = "button"; remove.className = "clip-icon-btn";
  remove.dataset.clipboardAction = "remove-share";
  remove.setAttribute("aria-disabled", String(shareBusy));
  remove.textContent = "×"; remove.title = "删除本机共享记录"; remove.setAttribute("aria-label", "删除本机共享记录");
  remove.addEventListener("click", async (event) => {
    event.stopPropagation(); await shareAction(() => invoke<ShareSnapshot>("clipboard_share_remove", { id: item.id }));
  });
  card.append(main, remove);
  main.addEventListener("click", async () => {
    if (await shareAction(() => invoke("clipboard_share_copy", { id: item.id }))) {
      showToast("已复制共享内容");
      if (appMode === "clipboard") void invoke("hide_clipboard_window").catch(() => {});
    }
  });
  return card;
}
async function shareAction(operation: () => Promise<unknown>): Promise<boolean> {
  if (shareBusy) return false;
  shareBusy = true; shareUiError = null; renderShare(); renderClipboard();
  try {
    const result = await operation();
    if (result && typeof result === "object" && "viewRevision" in result) applyShareSnapshot(result as ShareSnapshot);
    return true;
  } catch (error) {
    // Rust returns allowlisted local errors only. Never render provider response bodies or inputs.
    const message = typeof error === "string" ? error : "共享操作失败，请检查设置";
    showToast(message);
    try { applyShareSnapshot(await invoke<ShareSnapshot>("clipboard_share_snapshot")); } catch { /* Preserve visible state. */ }
    shareUiError = message;
    return false;
  } finally { shareBusy = false; renderShare(); renderClipboard(); }
}
function clearShareSecrets(): void {
  shareInput("share-token").value = ""; shareInput("share-key").value = "";
}
function dirtyShareForm(): void {
  shareFormDirty = true;
  const origin = shareOrigin(shareInput("share-server").value);
  if (origin !== shareDraftOrigin) {
    // An old origin's *draft* token is not authorization for a newly pasted server either.
    shareInput("share-token").value = ""; shareDraftOrigin = origin;
  }
  if (shareState?.enabled && !sharePausing) {
    sharePausing = true;
    void invoke<ShareSnapshot>("clipboard_share_enabled", { enabled: false })
      .then(applyShareSnapshot).catch(() => showToast("请先关闭共享再修改连接信息"))
      .finally(() => { sharePausing = false; renderShare(); });
  }
  renderShare(); renderClipboard();
}
async function initClipboardShare(): Promise<void> {
  document.getElementById("altv-local-tab")!.addEventListener("click", () => selectAltVView(false));
  document.getElementById("altv-shared-tab")!.addEventListener("click", () => selectAltVView(true));
  document.getElementById("clip-local-tab")!.addEventListener("click", () => selectClipboardView(false));
  document.getElementById("clip-shared-tab")!.addEventListener("click", () => selectClipboardView(true));
  document.getElementById("share-open-settings")!.addEventListener("click", () => { switchTab("clipboard"); selectClipboardView(true); });
  for (const id of ["share-server", "share-topic", "share-name", "share-token", "share-key"]) {
    shareInput(id).addEventListener("input", dirtyShareForm);
  }
  shareInput("share-enabled").addEventListener("change", async () => {
    const enabled = shareInput("share-enabled").checked;
    await shareAction(() => invoke<ShareSnapshot>("clipboard_share_enabled", { enabled }));
  });
  shareInput("share-auto").addEventListener("change", async () => {
    const enabled = shareInput("share-auto").checked;
    await shareAction(() => invoke<ShareSnapshot>("clipboard_share_auto_send", { enabled }));
  });
  document.getElementById("share-send-current")!.addEventListener("click", async () => {
    if (!shareState) return;
    if (await shareAction(() => invoke("clipboard_share_send_current", { revision: shareState!.revision }))) showToast("已加入发送队列");
  });
  document.getElementById("share-save")!.addEventListener("click", async () => {
    const draft = { server: shareInput("share-server").value, topic: shareInput("share-topic").value,
      senderName: shareInput("share-name").value, token: shareInput("share-token").value || null,
      key: shareInput("share-key").value || null };
    if (await shareAction(() => invoke<ShareSnapshot>("clipboard_share_save", { draft }))) {
      shareFormDirty = false; clearShareSecrets(); renderShare(); showToast("已保存，开启共享后连接");
    }
  });
  document.getElementById("share-new-key")!.addEventListener("click", async () => {
    if (shareState?.keySaved && !window.confirm("生成新密钥后，其他设备需重新导入配对资料。继续吗？")) return;
    if (await shareAction(() => invoke<ShareSnapshot>("clipboard_share_generate_key"))) { clearShareSecrets(); showToast("新密钥已加密保存"); }
  });
  document.getElementById("share-export")!.addEventListener("click", async () => {
    if (await shareAction(() => invoke("clipboard_share_export"))) showToast("配对资料已复制，请只粘贴到自己的设备");
  });
  document.getElementById("share-import")!.addEventListener("click", async () => {
    const field = document.getElementById("share-pairing") as HTMLInputElement;
    if (await shareAction(() => invoke<ShareSnapshot>("clipboard_share_import", { pairing: field.value }))) {
      field.value = ""; shareFormDirty = false; clearShareSecrets(); renderShare(); showToast("配对资料已保存，请检查本机 Token 后开启共享");
    }
  });
  document.getElementById("share-reset")!.addEventListener("click", async () => {
    if (!window.confirm("重置本机共享配置和共享记录？原本机剪贴板历史保留。")) return;
    if (await shareAction(() => invoke<ShareSnapshot>("clipboard_share_reset"))) {
      shareFormDirty = false; clearShareSecrets(); renderShare();
    }
  });
  if (!nativeApp) {
    shareUiError = "界面预览：跨设备共享需在方寸桌面伴侣中使用";
    renderShare();
    return;
  }
  try {
    await listen<ShareSnapshot>("clipboard-share://changed", (event) => applyShareSnapshot(event.payload));
    applyShareSnapshot(await invoke<ShareSnapshot>("clipboard_share_snapshot"));
  } catch { shareUiError = "共享组件不可用，请在最新方寸桌面伴侣内打开"; }
  renderShare();
}
void initClipboardShare();
