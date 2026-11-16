import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";

export interface AppSettings {
  scheme: string;
  mixedInput: boolean;
  chinesePunctuation: boolean;
  skin: string;
  themeMode: string;
  darkMode: boolean;
  systemDark: boolean;
  firstCandidateSize: number;
  keyOpacity: number;
  typingGlow: boolean;
  keyLetterSize: number;
  keyLetterBold: boolean;
}

export interface ClipboardItem {
  id: string;
  text: string;
  pinned: boolean;
  createdAtMs: number;
  source: string;
}

interface SkinTheme {
  bg: string;
  surface: string;
  subtle: string;
  border: string;
  text: string;
  secondary: string;
  accent: string;
  accentSoft: string;
  keyFunc: string;
  glowA: string;
  glowB: string;
}

const SKIN_THEMES: Record<string, { light: SkinTheme; dark: SkinTheme }> = {
  native_light: {
    light: {
      bg: "#f5f6f9",
      surface: "#ffffff",
      subtle: "#eef0f5",
      border: "#dfe3ec",
      text: "#181a20",
      secondary: "#687082",
      accent: "#5e72e4",
      accentSoft: "rgba(94, 114, 228, 0.12)",
      keyFunc: "#e6e9f2",
      glowA: "rgba(94, 114, 228, 0.28)",
      glowB: "rgba(140, 160, 255, 0.14)",
    },
    dark: {
      bg: "#12151c",
      surface: "#1a1e28",
      subtle: "#222735",
      border: "#2c3345",
      text: "#edf0f7",
      secondary: "#949db3",
      accent: "#7c8ff8",
      accentSoft: "rgba(124, 143, 248, 0.16)",
      keyFunc: "#252b3b",
      glowA: "rgba(124, 143, 248, 0.30)",
      glowB: "rgba(94, 114, 228, 0.15)",
    },
  },
  warm_grid: {
    light: {
      bg: "#f3ece3",
      surface: "#fcf8f2",
      subtle: "#efe6da",
      border: "#dfd3c3",
      text: "#2c221b",
      secondary: "#7a6858",
      accent: "#a94b2b",
      accentSoft: "rgba(169, 75, 43, 0.12)",
      keyFunc: "#e9dfd2",
      glowA: "rgba(169, 75, 43, 0.25)",
      glowB: "rgba(217, 130, 92, 0.14)",
    },
    dark: {
      bg: "#1b1613",
      surface: "#251f1b",
      subtle: "#2f2722",
      border: "#3e332c",
      text: "#f4ebe1",
      secondary: "#b2a090",
      accent: "#df7955",
      accentSoft: "rgba(223, 121, 85, 0.16)",
      keyFunc: "#332a24",
      glowA: "rgba(223, 121, 85, 0.28)",
      glowB: "rgba(169, 75, 43, 0.15)",
    },
  },
  mint: {
    light: {
      bg: "#edf5f1",
      surface: "#fafdfb",
      subtle: "#e3efe9",
      border: "#cde0d7",
      text: "#152821",
      secondary: "#547366",
      accent: "#1f8a68",
      accentSoft: "rgba(31, 138, 104, 0.12)",
      keyFunc: "#dbeae3",
      glowA: "rgba(31, 138, 104, 0.25)",
      glowB: "rgba(77, 184, 148, 0.14)",
    },
    dark: {
      bg: "#101b17",
      surface: "#172520",
      subtle: "#1e3029",
      border: "#2a4138",
      text: "#e7f3ee",
      secondary: "#8eb0a2",
      accent: "#42be94",
      accentSoft: "rgba(66, 190, 148, 0.16)",
      keyFunc: "#21362e",
      glowA: "rgba(66, 190, 148, 0.28)",
      glowB: "rgba(31, 138, 104, 0.15)",
    },
  },
  deep_blue: {
    light: {
      bg: "#111a2b",
      surface: "#18243a",
      subtle: "#1f2e4a",
      border: "#2c3f63",
      text: "#eef3fc",
      secondary: "#94a6c8",
      accent: "#f28b66",
      accentSoft: "rgba(242, 139, 102, 0.16)",
      keyFunc: "#243554",
      glowA: "rgba(242, 139, 102, 0.30)",
      glowB: "rgba(96, 142, 224, 0.16)",
    },
    dark: {
      bg: "#0e1524",
      surface: "#151f33",
      subtle: "#1c2942",
      border: "#283959",
      text: "#eef3fc",
      secondary: "#94a6c8",
      accent: "#f28b66",
      accentSoft: "rgba(242, 139, 102, 0.16)",
      keyFunc: "#21304d",
      glowA: "rgba(242, 139, 102, 0.30)",
      glowB: "rgba(96, 142, 224, 0.16)",
    },
  },
};

const SCHEME_LABELS: Record<string, string> = {
  flypy: "小鹤双拼",
  pinyin: "全拼",
  ninekey: "九宫格",
};

const SKIN_LABELS: Record<string, string> = {
  native_light: "原生轻白",
  warm_grid: "米白田字格",
  mint: "薄荷绿",
  deep_blue: "深蓝珊瑚",
};

const DEFAULT_SETTINGS: AppSettings = {
  scheme: "flypy",
  mixedInput: true,
  chinesePunctuation: true,
  skin: "native_light",
  themeMode: "system",
  darkMode: false,
  systemDark: false,
  firstCandidateSize: 20,
  keyOpacity: 0.82,
  typingGlow: true,
  keyLetterSize: 18,
  keyLetterBold: false,
};

let state: AppSettings = { ...DEFAULT_SETTINGS };
let clipItems: ClipboardItem[] = [];
let clipEnabled = true;
let clipQuery = "";
let toastTimer: number | undefined;

function showToast(msg: string): void {
  const el = document.getElementById("toast");
  if (!el) return;
  el.textContent = msg;
  el.classList.add("is-visible");
  if (toastTimer !== undefined) window.clearTimeout(toastTimer);
  toastTimer = window.setTimeout(() => el.classList.remove("is-visible"), 1600);
}

function resolveDarkMode(s: AppSettings): boolean {
  if (s.themeMode === "dark") return true;
  if (s.themeMode === "light") return false;
  return s.systemDark;
}

function applyThemeTokens(s: AppSettings): void {
  const dark = resolveDarkMode(s);
  const skinSet = SKIN_THEMES[s.skin] ?? SKIN_THEMES.native_light!;
  const theme = dark ? skinSet.dark : skinSet.light;
  const root = document.documentElement.style;
  root.setProperty("--bg", theme.bg);
  root.setProperty("--surface", theme.surface);
  root.setProperty("--surface-subtle", theme.subtle);
  root.setProperty("--border", theme.border);
  root.setProperty("--text", theme.text);
  root.setProperty("--secondary", theme.secondary);
  root.setProperty("--accent", theme.accent);
  root.setProperty("--accent-soft", theme.accentSoft);
  root.setProperty("--key-func", theme.keyFunc);
  root.setProperty("--glow-a", theme.glowA);
  root.setProperty("--glow-b", theme.glowB);
  const keyAlpha = Math.max(0.4, Math.min(1.0, s.keyOpacity));
  const keyBg = dark
    ? `rgba(38, 45, 60, ${keyAlpha.toFixed(2)})`
    : `rgba(255, 255, 255, ${keyAlpha.toFixed(2)})`;
  root.setProperty("--key-bg", keyBg);
}

function formatTime(ms: number): string {
  if (!ms) return "刚刚";
  const d = new Date(ms);
  const hh = String(d.getHours()).padStart(2, "0");
  const mm = String(d.getMinutes()).padStart(2, "0");
  return `${hh}:${mm}`;
}

function renderClipboard(): void {
  const countEl = document.getElementById("clip-tab-count");
  if (countEl) countEl.textContent = String(clipItems.length);

  const watchBtn = document.getElementById("clip-toggle-watch");
  if (watchBtn) {
    watchBtn.textContent = clipEnabled ? "✓ 监听中" : "已暂停";
    watchBtn.classList.toggle("is-selected", clipEnabled);
  }

  const listEl = document.getElementById("clip-list");
  if (!listEl) return;

  const q = clipQuery.trim().toLowerCase();
  const filtered = q
    ? clipItems.filter((item) => item.text.toLowerCase().includes(q))
    : clipItems;

  listEl.innerHTML = "";
  if (filtered.length === 0) {
    const empty = document.createElement("div");
    empty.className = "clip-empty";
    empty.textContent = q
      ? "没有匹配的剪贴板记录"
      : "暂无剪贴板记录 — 复制任意文本后将自动出现在这里";
    listEl.appendChild(empty);
    return;
  }

  for (const item of filtered) {
    const row = document.createElement("div");
    row.className = `clip-item${item.pinned ? " is-pinned" : ""}`;

    const body = document.createElement("div");
    body.className = "clip-item-body";

    const textEl = document.createElement("div");
    textEl.className = "clip-text";
    textEl.textContent = item.text;

    const meta = document.createElement("div");
    meta.className = "clip-meta";
    if (item.pinned) {
      const pinTag = document.createElement("span");
      pinTag.className = "clip-pin-tag";
      pinTag.textContent = "已置顶";
      meta.appendChild(pinTag);
    }
    const info = document.createElement("span");
    info.textContent = `${formatTime(item.createdAtMs)} · ${item.text.length} 字 · ${item.source}`;
    meta.appendChild(info);

    body.appendChild(textEl);
    body.appendChild(meta);

    const ops = document.createElement("div");
    ops.className = "clip-item-ops";

    const copyBtn = document.createElement("button");
    copyBtn.className = "clip-op-btn is-primary";
    copyBtn.type = "button";
    copyBtn.textContent = "复制";
    copyBtn.addEventListener("click", async () => {
      try {
        clipItems = await invoke<ClipboardItem[]>("copy_clipboard_item", { id: item.id });
        renderClipboard();
        showToast("已复制到系统剪贴板");
      } catch {
        await navigator.clipboard?.writeText(item.text).catch(() => {});
        showToast("已复制到剪贴板");
      }
    });

    const pinBtn = document.createElement("button");
    pinBtn.className = "clip-op-btn";
    pinBtn.type = "button";
    pinBtn.textContent = item.pinned ? "取消置顶" : "置顶";
    pinBtn.addEventListener("click", async () => {
      try {
        clipItems = await invoke<ClipboardItem[]>("toggle_pin_clipboard", { id: item.id });
      } catch {
        clipItems = clipItems.map((c) => (c.id === item.id ? { ...c, pinned: !c.pinned } : c));
      }
      renderClipboard();
    });

    const delBtn = document.createElement("button");
    delBtn.className = "clip-op-btn";
    delBtn.type = "button";
    delBtn.textContent = "删除";
    delBtn.addEventListener("click", async () => {
      try {
        clipItems = await invoke<ClipboardItem[]>("delete_clipboard_item", { id: item.id });
      } catch {
        clipItems = clipItems.filter((c) => c.id !== item.id);
      }
      renderClipboard();
    });

    ops.appendChild(copyBtn);
    ops.appendChild(pinBtn);
    ops.appendChild(delBtn);

    row.appendChild(body);
    row.appendChild(ops);
    listEl.appendChild(row);
  }
}

function render(): void {
  applyThemeTokens(state);

  document.querySelectorAll<HTMLButtonElement>("[data-scheme]").forEach((el) => {
    el.classList.toggle("is-selected", el.dataset.scheme === state.scheme);
  });
  document.querySelectorAll<HTMLButtonElement>("[data-skin]").forEach((el) => {
    el.classList.toggle("is-selected", el.dataset.skin === state.skin);
  });
  document.querySelectorAll<HTMLButtonElement>("[data-mode]").forEach((el) => {
    el.classList.toggle("is-selected", el.dataset.mode === state.themeMode);
  });

  const mixedBtn = document.getElementById("toggle-mixed")!;
  mixedBtn.textContent = state.mixedInput ? "✓ 中英混输：开启" : "中英混输：关闭";
  mixedBtn.classList.toggle("is-selected", state.mixedInput);

  const punctBtn = document.getElementById("toggle-punct")!;
  punctBtn.textContent = state.chinesePunctuation ? "✓ 中文标点：开启" : "中文标点：关闭";
  punctBtn.classList.toggle("is-selected", state.chinesePunctuation);

  const glowBtn = document.getElementById("toggle-glow")!;
  glowBtn.textContent = state.typingGlow ? "✓ 打字流光特效：开启" : "打字流光特效：关闭";
  glowBtn.classList.toggle("is-selected", state.typingGlow);

  const boldBtn = document.getElementById("toggle-bold")!;
  boldBtn.textContent = state.keyLetterBold ? "✓ 键帽字母加粗：开启" : "键帽字母加粗：常规";
  boldBtn.classList.toggle("is-selected", state.keyLetterBold);

  document.getElementById("first-val")!.textContent = `${state.firstCandidateSize} pt`;
  document.getElementById("letter-val")!.textContent = `${state.keyLetterSize} pt`;
  document.getElementById("opacity-val")!.textContent = `${Math.round(state.keyOpacity * 100)}%`;

  const firstCand = document.getElementById("preview-first-cand")!;
  firstCand.style.fontSize = `${state.firstCandidateSize}px`;

  document.querySelectorAll<HTMLElement>(".key-letter").forEach((k) => {
    k.style.fontSize = `${state.keyLetterSize}px`;
    k.style.fontWeight = state.keyLetterBold ? "700" : "400";
  });

  const modeKey = document.getElementById("preview-mode-key")!;
  modeKey.textContent = state.mixedInput ? "中英混输" : (SCHEME_LABELS[state.scheme] ?? "小鹤双拼");

  const summary = document.getElementById("status-summary")!;
  summary.textContent = `${SCHEME_LABELS[state.scheme] ?? "小鹤双拼"} · ${
    state.mixedInput ? "中英混输" : "纯中文"
  } · ${SKIN_LABELS[state.skin] ?? "原生轻白"}`;
}

async function save(patch: Partial<AppSettings>): Promise<void> {
  state = { ...state, ...patch };
  render();
  try {
    state = await invoke<AppSettings>("save_settings", { settings: state });
    render();
  } catch {
    // Allow standalone web preview when not hosted inside Tauri.
  }
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

document.getElementById("filter-tabs")!.addEventListener("click", (e) => {
  const btn = (e.target as HTMLElement).closest<HTMLButtonElement>("[data-tab]");
  if (!btn) return;
  const tab = btn.dataset.tab ?? "all";
  document.querySelectorAll<HTMLButtonElement>("[data-tab]").forEach((el) => {
    el.classList.toggle("is-active", el === btn);
  });
  document.querySelectorAll<HTMLElement>(".bento-card").forEach((card) => {
    const show = tab === "all" || card.dataset.category === tab;
    card.classList.toggle("is-hidden", !show);
  });
});

document.getElementById("preview-card")!.addEventListener("click", (e) => {
  const rect = (e.currentTarget as HTMLElement).getBoundingClientRect();
  triggerGlow(e.clientX - rect.left, e.clientY - rect.top);
});

document.getElementById("scheme-group")!.addEventListener("click", (e) => {
  const btn = (e.target as HTMLElement).closest<HTMLButtonElement>("[data-scheme]");
  if (btn?.dataset.scheme) void save({ scheme: btn.dataset.scheme });
});

document.getElementById("skin-group")!.addEventListener("click", (e) => {
  const btn = (e.target as HTMLElement).closest<HTMLButtonElement>("[data-skin]");
  if (btn?.dataset.skin) void save({ skin: btn.dataset.skin });
});

document.getElementById("mode-group")!.addEventListener("click", (e) => {
  const btn = (e.target as HTMLElement).closest<HTMLButtonElement>("[data-mode]");
  if (btn?.dataset.mode) void save({ themeMode: btn.dataset.mode });
});

document.getElementById("toggle-mixed")!.addEventListener("click", () => {
  void save({ mixedInput: !state.mixedInput });
});
document.getElementById("toggle-punct")!.addEventListener("click", () => {
  void save({ chinesePunctuation: !state.chinesePunctuation });
});
document.getElementById("toggle-glow")!.addEventListener("click", () => {
  void save({ typingGlow: !state.typingGlow });
});
document.getElementById("toggle-bold")!.addEventListener("click", () => {
  void save({ keyLetterBold: !state.keyLetterBold });
});

const clamp = (v: number, lo: number, hi: number) => Math.max(lo, Math.min(hi, v));
document.getElementById("first-minus")!.addEventListener("click", () => {
  void save({ firstCandidateSize: clamp(state.firstCandidateSize - 1, 16, 30) });
});
document.getElementById("first-plus")!.addEventListener("click", () => {
  void save({ firstCandidateSize: clamp(state.firstCandidateSize + 1, 16, 30) });
});
document.getElementById("letter-minus")!.addEventListener("click", () => {
  void save({ keyLetterSize: clamp(state.keyLetterSize - 1, 14, 30) });
});
document.getElementById("letter-plus")!.addEventListener("click", () => {
  void save({ keyLetterSize: clamp(state.keyLetterSize + 1, 14, 30) });
});
document.getElementById("opacity-minus")!.addEventListener("click", () => {
  void save({ keyOpacity: clamp(Math.round((state.keyOpacity - 0.05) * 20) / 20, 0.4, 1.0) });
});
document.getElementById("opacity-plus")!.addEventListener("click", () => {
  void save({ keyOpacity: clamp(Math.round((state.keyOpacity + 0.05) * 20) / 20, 0.4, 1.0) });
});

document.getElementById("clip-search")!.addEventListener("input", (e) => {
  clipQuery = (e.target as HTMLInputElement).value;
  renderClipboard();
});

document.getElementById("clip-toggle-watch")!.addEventListener("click", async () => {
  try {
    clipEnabled = await invoke<boolean>("set_clipboard_enabled", { enabled: !clipEnabled });
  } catch {
    clipEnabled = !clipEnabled;
  }
  renderClipboard();
  showToast(clipEnabled ? "已恢复剪贴板监听" : "已暂停剪贴板监听");
});

document.getElementById("clip-clear")!.addEventListener("click", async () => {
  try {
    clipItems = await invoke<ClipboardItem[]>("clear_clipboard", { keepPinned: true });
  } catch {
    clipItems = clipItems.filter((c) => c.pinned);
  }
  renderClipboard();
  showToast("已清空未置顶剪贴板记录");
});

document.getElementById("btn-reset")!.addEventListener("click", async () => {
  try {
    state = await invoke<AppSettings>("reset_settings");
  } catch {
    state = { ...DEFAULT_SETTINGS };
  }
  render();
  showToast("已恢复默认设置");
});

document.getElementById("btn-close")!.addEventListener("click", () => {
  void invoke("hide_window").catch(() => {
    showToast("窗口已最小化到系统托盘");
  });
});

async function init(): Promise<void> {
  try {
    state = await invoke<AppSettings>("load_settings");
    clipItems = await invoke<ClipboardItem[]>("list_clipboard");
    clipEnabled = await invoke<boolean>("get_clipboard_enabled");
    await listen<ClipboardItem[]>("clipboard://changed", (event) => {
      clipItems = event.payload;
      renderClipboard();
    });
  } catch {
    // Standalone web preview fallback
  }
  render();
  renderClipboard();
}

void init();
