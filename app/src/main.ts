import { invoke } from "@tauri-apps/api/core";
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
  themeMode: string;
  darkMode: boolean;
  systemDark: boolean;
  firstCandidateSize: number;
  keyOpacity: number;
  typingGlow: boolean;
  keyLetterSize: number;
  keyLetterBold: boolean;
  clipboardEnabled?: boolean;
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
  skin: "square_cream",
  themeMode: "system",
  darkMode: false,
  systemDark: false,
  firstCandidateSize: 20,
  keyOpacity: 0.82,
  typingGlow: true,
  keyLetterSize: 18,
  keyLetterBold: false,
  clipboardEnabled: true,
};

type SkinId = "square_cream" | "square_aluminum" | "square_amber" | "square_jade";
type SkinPalette = Record<string, string>;
const SKIN_ORDER: SkinId[] = ["square_cream", "square_aluminum", "square_amber", "square_jade"];
const SKIN_LABELS: Record<SkinId, string> = {
  square_cream: "奶油方格", square_aluminum: "铝银机械", square_amber: "墨黑琥珀", square_jade: "翡翠磨砂",
};
function canonicalSkin(value: string): SkinId {
  if (SKIN_ORDER.includes(value as SkinId)) return value as SkinId;
  return ({ warm_grid: "square_cream", native_light: "square_aluminum", deep_blue: "square_amber", mint: "square_jade" } as Record<string, SkinId>)[value] ?? "square_cream";
}
function accentForeground(hex: string): string {
  const channels = [1, 3, 5].map((offset) => parseInt(hex.slice(offset, offset + 2), 16) / 255)
    .map((value) => value <= .04045 ? value / 12.92 : Math.pow((value + .055) / 1.055, 2.4));
  const luminance = .2126 * channels[0]! + .7152 * channels[1]! + .0722 * channels[2]!;
  return 1.05 / (luminance + .05) >= (luminance + .05) / .05 ? "#FFFFFF" : "#000000";
}
function skinPalette(skin: SkinId, dark: boolean): SkinPalette {
  // Eight shared tokens in native/Android order: base, cap, edge, function, ink, secondary, accent, pressed.
  type Tokens = readonly [string, string, string, string, string, string, string, string];
  const values: Record<SkinId, readonly [Tokens, Tokens]> = {
    square_cream: [
      ["#F0EDE4", "#FFFEFA", "#D2CFC4", "#E7E4DB", "#292D29", "#62665D", "#315F37", "#E0E8D9"],
      ["#242723", "#383D34", "#5F6959", "#454B3F", "#F5F3EA", "#B8C0AD", "#B4D49B", "#4D5C44"],
    ],
    square_aluminum: [
      ["#CFD4DA", "#EFF2F5", "#929DA9", "#D4D9E0", "#232C36", "#4F5C69", "#075EAC", "#D1E4F3"],
      ["#232A32", "#3A4551", "#657588", "#465463", "#F2F6FA", "#B0BECE", "#9BC8FA", "#3D5D7C"],
    ],
    square_amber: [
      ["#E5E0D7", "#F9F5EC", "#B7AA93", "#D8D0C0", "#302C24", "#695C48", "#955200", "#F1DFC1"],
      ["#20211F", "#383A35", "#68695E", "#45473F", "#F8F4E8", "#C0BDAD", "#F1AD3E", "#5C4B31"],
    ],
    square_jade: [
      ["#DCECE4", "#F0F8F1", "#B0CBBE", "#DBEAE0", "#233E32", "#526C5D", "#156448", "#C7E4D4"],
      ["#1D2D27", "#304D3E", "#607F6C", "#3D5B4A", "#F0F8EE", "#B4CCBA", "#A0D9B6", "#416850"],
    ],
  };
  const [base, cap, edge, functional, ink, secondary, accent, pressed] = values[skin][dark ? 1 : 0];
  return {
    "bg-main": base, "bg-sidebar": functional, surface: cap, "surface-elevated": cap,
    "surface-subtle": functional, border: edge, "border-strong": edge, text: ink,
    "text-secondary": secondary, "text-muted": secondary, accent,
    "accent-hover": `color-mix(in srgb, ${accent} 86%, ${dark ? "white" : "black"})`,
    "on-accent": accentForeground(accent), "accent-soft": pressed,
    "glow-a": `color-mix(in srgb, ${accent} 22%, transparent)`,
    "glow-b": `color-mix(in srgb, ${pressed} 32%, transparent)`,
  };
}
function mixHex(first: string, second: string, fraction: number): string {
  return "#" + [1, 3, 5].map((offset) => {
    const a = parseInt(first.slice(offset, offset + 2), 16);
    const b = parseInt(second.slice(offset, offset + 2), 16);
    return Math.round(a + (b - a) * fraction).toString(16).padStart(2, "0");
  }).join("");
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
  for (const [name, value] of Object.entries(skinPalette(skin, dark))) root.style.setProperty(`--${name}`, value);
  root.style.setProperty("--shadow-soft", dark ? "0 3px 12px rgba(0,0,0,.14)" : "0 3px 12px rgba(32,35,29,.055)");
  root.style.setProperty("--shadow-float", dark ? "0 8px 24px rgba(0,0,0,.32)" : "0 8px 24px rgba(32,35,29,.13)");
  document.querySelectorAll<HTMLElement>(".cand-window").forEach((preview) => {
    const tone = preview.id === "cand-window-preview" ? (previewDarkCand ?? dark) : preview.dataset.tone === "dark";
    const palette = skinPalette(skin, tone);
    preview.classList.toggle("is-dark", tone);
    preview.dataset.skin = skin;
    for (const name of ["surface", "surface-subtle", "border", "text", "text-secondary", "accent", "on-accent"]) {
      preview.style.setProperty(`--cand-${name}`, palette[name]!);
    }
    preview.style.setProperty("--cand-surface", palette["bg-main"]!);
    preview.style.setProperty("--cand-surface-subtle", mixHex(palette["bg-main"]!, palette["surface-subtle"]!, .28));
  });
  for (const [id, selected] of [["btn-preview-light", !(previewDarkCand ?? dark)], ["btn-preview-dark", previewDarkCand ?? dark]] as const) {
    document.getElementById(id)?.classList.toggle("is-active", selected);
  }
  const lightLabel = document.getElementById("showcase-light-label");
  const darkLabel = document.getElementById("showcase-dark-label");
  if (lightLabel) lightLabel.textContent = `${SKIN_LABELS[skin]} · 浅色候选窗`;
  if (darkLabel) darkLabel.textContent = `${SKIN_LABELS[skin]} · 深色候选窗`;
}

const appMode = new URLSearchParams(window.location.search).get("mode") ?? "main";
let state: AppSettings = { ...DEFAULT_SETTINGS };
let clipItems: ClipboardItem[] = [];
let clipEnabled = true;
let clipQuery = "";
let altvQuery = "";
let previewDarkCand: boolean | null = null;
let previewLangEng = false;
let toastTimer: number | undefined;
let shiftToastTimer: number | undefined;

function showToast(msg: string): void {
  const el = document.getElementById("toast");
  if (!el) return;
  el.textContent = msg;
  el.classList.add("is-visible");
  if (toastTimer !== undefined) window.clearTimeout(toastTimer);
  toastTimer = window.setTimeout(() => el.classList.remove("is-visible"), 1600);
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

function buildClipboardRow(item: ClipboardItem): HTMLElement {
  const card = document.createElement("div");
  card.className = `clip-card-item${item.pinned ? " is-pinned" : ""}`;

  const main = document.createElement("div");
  main.className = "clip-card-main";

  const txt = document.createElement("div");
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
  pinBtn.textContent = "📌";
  pinBtn.addEventListener("click", async (e) => {
    e.stopPropagation();
    try {
      clipItems = await invoke<ClipboardItem[]>("toggle_pin_clipboard", { id: item.id });
    } catch {
      clipItems = clipItems.map((c) => (c.id === item.id ? { ...c, pinned: !c.pinned } : c));
    }
    renderClipboard();
  });

  const delBtn = document.createElement("button");
  delBtn.type = "button";
  delBtn.className = "clip-icon-btn";
  delBtn.title = "删除";
  delBtn.textContent = "×";
  delBtn.addEventListener("click", async (e) => {
    e.stopPropagation();
    try {
      clipItems = await invoke<ClipboardItem[]>("delete_clipboard_item", { id: item.id });
    } catch {
      clipItems = clipItems.filter((c) => c.id !== item.id);
    }
    renderClipboard();
  });

  const sendBtn = document.createElement("button");
  sendBtn.type = "button";
  sendBtn.className = "clip-icon-btn share-local-send";
  sendBtn.title = "发送到已配对设备";
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

  card.addEventListener("click", async () => {
    try {
      clipItems = await invoke<ClipboardItem[]>("copy_clipboard_item", { id: item.id });
      renderClipboard();
      showToast("已复制到剪贴板");
    } catch {
      await navigator.clipboard?.writeText(item.text).catch(() => {});
      showToast("已复制到剪贴板");
    }
    if (appMode === "clipboard") {
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

  container.innerHTML = "";
  const remote = container.id === "altv-list" && altvShared
    ? (shareState?.items ?? []).filter((item) => !q || item.text.toLowerCase().includes(q) || item.senderName.toLowerCase().includes(q))
    : [];
  for (const item of remote) container.appendChild(buildSharedRow(item));
  if (filtered.length === 0 && remote.length === 0) {
    const empty = document.createElement("div");
    empty.className = "clip-empty-state";
    empty.textContent = q ? "没有匹配的剪贴板内容"
      : container.id === "altv-list" && altvShared ? "暂无共享内容，请从另一台已配对设备发送"
      : "暂无剪贴板记录 — 复制任意文本后自动收录";
    container.appendChild(empty);
    return;
  }

  for (const item of filtered) {
    container.appendChild(buildClipboardRow(item));
  }
}

function renderClipboard(): void {
  const countEl = document.getElementById("clip-tab-count");
  if (countEl) countEl.textContent = String(clipItems.length);

  for (const prefix of ["clip", "altv"]) {
    const txt = document.getElementById(`${prefix}-watch-text`);
    const dot = document.getElementById(`${prefix}-watch-dot`);
    if (txt) txt.textContent = clipEnabled ? "监听中" : "已暂停";
    if (dot) dot.classList.toggle("is-on", clipEnabled);
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
  });

  // Skin & Mode buttons
  document.querySelectorAll<HTMLButtonElement>("[data-skin]").forEach((btn) => {
    btn.classList.toggle("is-selected", btn.dataset.skin === state.skin);
    btn.setAttribute("aria-pressed", String(btn.dataset.skin === state.skin));
  });
  document.querySelectorAll<HTMLButtonElement>("[data-mode]").forEach((btn) => {
    btn.classList.toggle("is-selected", btn.dataset.mode === state.themeMode);
  });

  // Switches
  (document.getElementById("sw-mixed") as HTMLInputElement).checked = state.mixedInput;
  (document.getElementById("sw-punct") as HTMLInputElement).checked = state.chinesePunctuation;
  (document.getElementById("sw-shift-toggle") as HTMLInputElement).checked = state.shiftToggleEnglish;
  (document.getElementById("sw-capslock") as HTMLInputElement).checked = state.capsLockBypass;
  (document.getElementById("sw-wheel") as HTMLInputElement).checked = state.wheelPaging;
  (document.getElementById("sw-glow") as HTMLInputElement).checked = state.typingGlow;

  const pageSelect = document.getElementById("sel-page-size") as HTMLSelectElement;
  pageSelect.value = String(state.pageSize || 7);

  // Metrics
  document.getElementById("first-val")!.textContent = `${state.firstCandidateSize} pt`;

  // The candidate preview shares the active native skin and its selected light/dark variant.
  const firstWord = document.getElementById("preview-first-word")!;
  firstWord.style.fontSize = `${Math.max(14, state.firstCandidateSize - 4)}px`;

  // Floating status bar
  const fbPunct = document.getElementById("fb-punct");
  if (fbPunct) fbPunct.textContent = state.chinesePunctuation ? "“。" : "\".";

  const genSummary = document.getElementById("gen-scheme-summary");
  if (genSummary) genSummary.textContent = SCHEME_LABELS[state.scheme] ?? "小鹤双拼";
}

async function save(patch: Partial<AppSettings>): Promise<void> {
  state = { ...state, ...patch };
  render();
  try {
    state = await invoke<AppSettings>("save_settings", { settings: state });
    render();
  } catch {
    // Allow standalone browser preview outside Tauri.
  }
}

function switchTab(tab: string): void {
  document.querySelectorAll<HTMLButtonElement>("#sidebar-nav [data-tab]").forEach((btn) => {
    btn.classList.toggle("is-active", btn.dataset.tab === tab);
  });
  document.querySelectorAll<HTMLElement>(".tab-pane").forEach((pane) => {
    pane.classList.toggle("is-active", pane.dataset.pane === tab);
  });
}

function triggerGlow(x: number, y: number): void {
  if (!state.typingGlow) return;
  const layer = document.getElementById("glow-layer")!;
  const pulse = document.createElement("div");
  pulse.className = "glow-pulse";
  pulse.style.left = `${x}px`;
  pulse.style.top = `${y}px`;
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

// Sidebar Search (auto-jumps to matching tab)
document.getElementById("settings-search")!.addEventListener("input", (e) => {
  const q = (e.target as HTMLInputElement).value.trim().toLowerCase();
  if (!q) return;
  if (q.includes("剪贴") || q.includes("clip") || q.includes("alt")) {
    switchTab("clipboard");
  } else if (q.includes("皮肤") || q.includes("深色") || q.includes("浅色") || q.includes("琥珀") || q.includes("翡翠") || q.includes("机械") || q.includes("奶油")) {
    switchTab("theme");
  } else if (q.includes("字号") || q.includes("流光") || q.includes("透明")) {
    switchTab("effects");
  } else if (q.includes("状态栏") || q.includes("常规")) {
    switchTab("general");
  } else {
    switchTab("input");
  }
});

// Preview Stage Interactions
document.getElementById("preview-card")!.addEventListener("click", (e) => {
  const rect = (e.currentTarget as HTMLElement).getBoundingClientRect();
  triggerGlow(e.clientX - rect.left, e.clientY - rect.top);
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
    void save({ scheme: btn.dataset.scheme });
    showToast(`已切换为${SCHEME_LABELS[btn.dataset.scheme] ?? "小鹤双拼"}`);
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
document.getElementById("sel-page-size")!.addEventListener("change", (e) => {
  const val = Number((e.target as HTMLSelectElement).value) || 7;
  void save({ pageSize: val });
  showToast(`每页候选词数量已设为 ${val}`);
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
  void save({ chinesePunctuation: !state.chinesePunctuation });
  showToast(state.chinesePunctuation ? "已切换为中文标点 (“。)" : "已切换为英文标点 (\".)");
});

// Floating Status Bar Interactive Buttons
document.getElementById("fb-lang")!.addEventListener("click", () => {
  showShiftCaretToast();
  void invoke("set_english_mode", { english: previewLangEng }).catch(() => {});
  renderFloatingStatusBar();
  showToast(previewLangEng ? "已切换为英文模式" : "已切换为中文模式");
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
  void invoke("toggle_statusbar").catch(() => {});
});

// Theme & Skin Controls
document.getElementById("skin-group")!.addEventListener("click", (e) => {
  const btn = (e.target as HTMLElement).closest<HTMLButtonElement>("[data-skin]");
  if (btn?.dataset.skin) {
    const skin = canonicalSkin(btn.dataset.skin);
    previewDarkCand = null;
    void save({ skin, themeMode: skin === "square_amber" ? "dark" : "light" });
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
  void save({ typingGlow: (e.target as HTMLInputElement).checked });
});

const clamp = (v: number, lo: number, hi: number) => Math.max(lo, Math.min(hi, v));
document.getElementById("first-minus")!.addEventListener("click", () => {
  void save({ firstCandidateSize: clamp(state.firstCandidateSize - 1, 16, 30) });
});
document.getElementById("first-plus")!.addEventListener("click", () => {
  void save({ firstCandidateSize: clamp(state.firstCandidateSize + 1, 16, 30) });
});

// Clipboard Controls
async function setClipboardWatch(want: boolean): Promise<void> {
  try {
    clipEnabled = await invoke<boolean>("set_clipboard_enabled", { enabled: want });
  } catch {
    clipEnabled = want;
  }
  renderClipboard();
  showToast(clipEnabled ? "已开启剪贴板监听" : "已暂停剪贴板监听");
}

async function clearUnpinnedClipboard(): Promise<void> {
  try {
    clipItems = await invoke<ClipboardItem[]>("clear_clipboard", { keepPinned: true });
  } catch {
    clipItems = clipItems.filter((c) => c.pinned);
  }
  renderClipboard();
  showToast("已清空未置顶剪贴板记录");
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
  const fbLang = document.getElementById("fb-lang");
  if (sbLang) {
    sbLang.textContent = previewLangEng ? "英" : "中";
    sbLang.classList.toggle("is-english", previewLangEng);
  }
  if (fbLang) {
    fbLang.textContent = previewLangEng ? "英" : "中";
  }
  if (sbPunct) {
    sbPunct.textContent = state.chinesePunctuation ? "“。" : ".,";
    sbPunct.classList.toggle("is-muted", !state.chinesePunctuation);
  }
}

document.getElementById("sb-lang")?.addEventListener("click", () => {
  previewLangEng = !previewLangEng;
  void invoke("set_english_mode", { english: previewLangEng }).catch(() => {});
  renderFloatingStatusBar();
});
document.getElementById("sb-punct")?.addEventListener("click", () => {
  void save({ chinesePunctuation: !state.chinesePunctuation }).then(() => {
    renderFloatingStatusBar();
  });
});
document.getElementById("sb-skin")?.addEventListener("click", () => {
  const idx = SKIN_ORDER.indexOf(canonicalSkin(state.skin));
  const nextSkin = SKIN_ORDER[(idx + 1) % SKIN_ORDER.length];
  previewDarkCand = null;
  void save({ skin: nextSkin, themeMode: nextSkin === "square_amber" ? "dark" : "light" });
});
document.getElementById("sb-settings")?.addEventListener("click", () => {
  void invoke("open_settings_tab", { tab: "input" }).catch(() => {});
});

// Footer Buttons
document.getElementById("btn-reset")!.addEventListener("click", async () => {
  try {
    state = await invoke<AppSettings>("reset_settings");
  } catch {
    state = { ...DEFAULT_SETTINGS };
  }
  render();
  renderFloatingStatusBar();
  showToast("已恢复默认设置");
});

document.getElementById("btn-close")!.addEventListener("click", () => {
  void invoke("hide_window").catch(() => {
    showToast("已完成保存并最小化到系统托盘");
  });
});

async function init(): Promise<void> {
  if (appMode === "clipboard") {
    document.body.classList.add("mode-clipboard");
    openAltVPopup(true);
  } else if (appMode === "statusbar") {
    document.body.classList.add("mode-statusbar");
    document.getElementById("floating-statusbar")?.classList.remove("is-hidden");
  }

  try {
    state = await invoke<AppSettings>("load_settings");
    clipItems = await invoke<ClipboardItem[]>("list_clipboard");
    clipEnabled = await invoke<boolean>("get_clipboard_enabled");
    await listen<ClipboardItem[]>("clipboard://changed", (event) => {
      clipItems = event.payload;
      renderClipboard();
    });
    await listen<boolean>("clipboard://enabled-changed", (event) => {
      clipEnabled = event.payload;
      renderClipboard();
    });
    await listen<AppSettings>("settings://changed", (event) => {
      state = event.payload;
      render();
      renderFloatingStatusBar();
    });
    await listen<boolean>("ime://english-changed", (event) => {
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
    // Standalone web preview fallback
  }
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
  list.replaceChildren();
  for (const item of shareState?.items ?? []) list.appendChild(buildSharedRow(item));
  if (!list.childElementCount) {
    const empty = document.createElement("div"); empty.className = "clip-empty-state";
    empty.textContent = "暂无共享内容。配对并开启后，可从另一台设备发送文字。"; list.appendChild(empty);
  }
}
function buildSharedRow(item: SharedItem): HTMLElement {
  const card = document.createElement("div"); card.className = "clip-card-item";
  const main = document.createElement("div"); main.className = "clip-card-main";
  const text = document.createElement("div"); text.className = "clip-card-text"; text.textContent = item.text;
  const source = document.createElement("div"); source.className = "share-source";
  source.textContent = `${item.senderName} · 共享 · ${formatRelativeTime(item.createdAt)}`;
  main.append(text, source);
  const remove = document.createElement("button"); remove.type = "button"; remove.className = "clip-icon-btn";
  remove.textContent = "×"; remove.title = "删除本机共享记录"; remove.setAttribute("aria-label", "删除本机共享记录");
  remove.addEventListener("click", async (event) => {
    event.stopPropagation(); await shareAction(() => invoke<ShareSnapshot>("clipboard_share_remove", { id: item.id }));
  });
  card.append(main, remove);
  card.addEventListener("click", async () => {
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
  try {
    await listen<ShareSnapshot>("clipboard-share://changed", (event) => applyShareSnapshot(event.payload));
    applyShareSnapshot(await invoke<ShareSnapshot>("clipboard_share_snapshot"));
  } catch { shareUiError = "共享组件不可用，请在最新方寸桌面伴侣内打开"; }
  renderShare();
}
void initClipboardShare();
