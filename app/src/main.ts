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
  candidateLayout: string;
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
  skin: "green_terminal",
  candidateLayout: "horizontal",
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

const appMode = new URLSearchParams(window.location.search).get("mode") ?? "main";
let state: AppSettings = { ...DEFAULT_SETTINGS };
let clipItems: ClipboardItem[] = [];
let clipEnabled = true;
let clipQuery = "";
let altvQuery = "";
let previewDarkCand = false;
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
  const filtered = q
    ? clipItems.filter((item) => item.text.toLowerCase().includes(q))
    : clipItems;

  container.innerHTML = "";
  if (filtered.length === 0) {
    const empty = document.createElement("div");
    empty.className = "clip-empty-state";
    empty.textContent = q ? "没有匹配的剪贴板内容" : "暂无剪贴板记录 — 复制任意文本后自动收录";
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
  // Scheme cards
  document.querySelectorAll<HTMLButtonElement>("[data-scheme]").forEach((btn) => {
    btn.classList.toggle("is-selected", btn.dataset.scheme === state.scheme);
  });

  // Skin, Layout & Mode buttons
  document.querySelectorAll<HTMLButtonElement>("[data-skin]").forEach((btn) => {
    btn.classList.toggle("is-selected", btn.dataset.skin === state.skin);
  });
  document.querySelectorAll<HTMLButtonElement>("[data-layout]").forEach((btn) => {
    btn.classList.toggle("is-selected", btn.dataset.layout === (state.candidateLayout || "horizontal"));
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

  // Live Candidate Window Preview
  const candWin = document.getElementById("cand-window-preview")!;
  const isCrafted =
    state.skin === "green_terminal" ||
    state.skin === "wood_type" ||
    state.skin === "classic_typewriter";
  const isDark =
    !isCrafted &&
    (previewDarkCand ||
      state.themeMode === "dark" ||
      state.skin === "deep_blue" ||
      (state.themeMode === "system" && state.systemDark));
  candWin.classList.toggle("is-dark", isDark);
  candWin.classList.toggle("is-vertical", state.candidateLayout === "vertical");
  candWin.dataset.skin = state.skin;

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
  } else if (q.includes("皮肤") || q.includes("深色") || q.includes("浅色") || q.includes("珊瑚")) {
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
  if (btn?.dataset.skin) void save({ skin: btn.dataset.skin });
});
document.getElementById("layout-group")?.addEventListener("click", (e) => {
  const btn = (e.target as HTMLElement).closest<HTMLButtonElement>("[data-layout]");
  if (btn?.dataset.layout) void save({ candidateLayout: btn.dataset.layout });
});
document.getElementById("mode-group")!.addEventListener("click", (e) => {
  const btn = (e.target as HTMLElement).closest<HTMLButtonElement>("[data-mode]");
  if (btn?.dataset.mode) void save({ themeMode: btn.dataset.mode });
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
  const order = [
    "green_terminal",
    "wood_type",
    "classic_typewriter",
    "warm_grid",
    "native_light",
    "mint",
    "deep_blue",
  ];
  const idx = order.indexOf(state.skin);
  const nextSkin = order[(idx + 1) % order.length];
  void save({ skin: nextSkin });
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
