import { invoke, isTauri } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import "./skins.css";

type Tone = "light" | "dark";
type Role = "deck" | "toolbar" | "code" | "candidates" | "selection" | "keyNormal" | "keyFunction" | "keyAction";
type ColorName = "background" | "surface" | "border" | "function" | "text" | "muted" | "accent" | "onAccent" | "pressed" | "toolbar" | "candidateBackground" | "candidateText" | "selection" | "onSelection" | "keyText" | "keyHint" | "functionText" | "codeText";
interface SkinImage { file: string; scale: "stretch" | "contain" | "tile" | "nineSlice"; insets: [number, number, number, number]; }
interface SkinTheme { colors: Record<ColorName, string>; glow: [string, string, string, string]; images: Partial<Record<Role, SkinImage>>; }
interface Manifest { format: "fangcun.skin"; schemaVersion: 1; id: string; name: string; version: number; author?: string; license?: string; baseSkin: "mechanical" | "typewriter" | "mahjong" | "brutal"; assetScale: number; fontFamily: "sans" | "serif" | "mono"; pressOffsetDp: number; preview: string; themes: Record<Tone, SkinTheme>; }
interface PackInfo { id: string; hash: string; name: string; version: number; author: string; baseSkin: Manifest["baseSkin"]; previewUrl: string; unavailable?: boolean; }
interface ActivePack { id?: string; hash: string; manifest: Manifest; imageUrls: Record<Tone, Partial<Record<Role, string>>>; }
interface Library { revision: string; importEnabled: boolean; packs: PackInfo[]; activeId: string; active: ActivePack | null; warning?: string | null; }
interface AppliedTheme { skin: string; dark: boolean; themeMode: string; customSkinId?: string; customSkinHash?: string; }

const native = isTauri();
const mode = new URLSearchParams(location.search).get("mode") ?? "main";
const main = mode === "main";
const roles: Role[] = ["deck", "toolbar", "code", "candidates", "selection", "keyNormal", "keyFunction", "keyAction"];
const colorNames: ColorName[] = ["background", "surface", "border", "function", "text", "muted", "accent", "onAccent", "pressed", "toolbar", "candidateBackground", "candidateText", "selection", "onSelection", "keyText", "keyHint", "functionText", "codeText"];
const variables: Record<ColorName, string[]> = {
  background: ["--ime-board-bg", "--fc-background"], surface: ["--ime-key-bg", "--ime-win-bg", "--fc-surface"],
  border: ["--ime-border", "--ime-win-outer-border"], function: ["--ime-fn-key-bg"], text: ["--ime-text"],
  muted: ["--ime-subtext"], accent: ["--ime-accent"], onAccent: ["--ime-accent-text"], pressed: ["--ime-key-pressed-bg"],
  toolbar: ["--ime-toolbar-bg", "--ime-win-header-bg"], candidateBackground: ["--fc-candidate-background"],
  candidateText: ["--candidate-ink", "--fc-candidate-text"], selection: ["--ime-cand-active-bg"], onSelection: ["--ime-cand-active-text"],
  keyText: ["--fc-key-text"], keyHint: ["--fc-key-hint"], functionText: ["--fc-function-text"], codeText: ["--fc-code-text"],
};
const ownedVariables = [...Object.values(variables).flat(), "--ime-deck", "--ime-paper", "--ime-screen", "--ime-crt-code", "--glow-a", "--glow-b", "--fc-font", "--fc-press-offset"];
const CAS_ERROR = "皮肤列表已更新，请重新读取后重试";
const knownErrors = new Set([
  CAS_ERROR, "本地皮肤导入已关闭",
  "皮肤导入已关闭，已安装的皮肤仍可使用", "皮肤导入已关闭，未安装", "本地皮肤不存在",
  "皮肤包不存在，请重新读取", "同名皮肤已存在，请先删除旧包再导入",
  "皮肤文件不可用，原选择和文件已保留", "皮肤库数据无效，已保留原文件", "请选择 .fcskin 皮肤包",
  "皮肤包格式不正确，请使用方寸 .fcskin v1 皮肤包", "皮肤素材过大，未导入", "皮肤包展开后过大，未导入",
  "皮肤图片过大，未导入", "皮肤图片总像素过大，未导入", "皮肤九切片边距不正确，未导入",
  "皮肤图片不是有效的普通 PNG", "皮肤 PNG 尺寸或编码不符合 v1 规范", "暂不支持动画皮肤图片",
  "皮肤 PNG 校验失败，未导入", "无法解码皮肤 PNG", "无法打开选中的皮肤包，请重试",
  "无法读取选中的皮肤包", "本机文件选择目前仅支持 Windows", "无法打开皮肤文件选择器",
]);
const el = <T extends HTMLElement>(id: string) => document.getElementById(id) as T | null;
let library: Library | null = null;
let theme: AppliedTheme = { skin: document.documentElement.dataset.imeTheme ?? "system_default", dark: document.documentElement.dataset.imeMode === "dark", themeMode: "system" };
let busy = false, mustReload = false, pendingRefresh = false, disposed = false;
let requestSequence = 0;
let notice = native ? "正在读取皮肤列表…" : "界面预览 · 本地导入需在方寸桌面伴侣中使用";
let failure = "";
let applyQueued = false;
const unlisteners: Array<() => void> = [];
const decorated = new Set<HTMLElement>();
const contentObserver = new MutationObserver(records => {
  const changesSurface = records.some(record => {
    const target = record.target instanceof HTMLElement ? record.target : null;
    if (record.type === "attributes") return !!target?.matches(".nav-item,.clip-view-tabs button,.ai-modes button,.cand-pill,.scheme-card");
    if (target?.matches("button,.fb-lang-badge,.sb-btn")) return true;
    return [...record.addedNodes].some(node => node instanceof HTMLElement && (node.matches("button,.clip-card-item") || !!node.querySelector("button")));
  });
  if (changesSurface) scheduleApply();
});
const observer = new ResizeObserver(entries => {
  for (const entry of entries) {
    const target = entry.target as HTMLElement;
    const layer = target.querySelector<HTMLElement>(":scope > .fc-skin-image");
    if (layer?.dataset.insets) fitSlices(layer, target, layer.dataset.insets, Number(layer.dataset.assetScale));
  }
});

function allowedUrl(raw: string | undefined, hash: string, path?: string): string | null {
  if (!raw || !/^[a-f0-9]{64}$/.test(hash)) return null;
  try {
    const url = new URL(raw);
    if (url.origin !== "http://fcskin.localhost" || url.username || url.password || url.search || url.hash) return null;
    if (!new RegExp("^/" + hash + "/(?:[a-z0-9_-]+/)*[a-z0-9_-]+\\.png$").test(url.pathname)) return null;
    return !path || url.pathname === "/" + hash + "/" + path ? url.href : null;
  } catch { return null; }
}
function validActive(active: ActivePack): boolean {
  const manifest = active.manifest;
  return !!manifest && manifest.format === "fangcun.skin" && manifest.schemaVersion === 1 && /^[a-f0-9]{64}$/.test(active.hash)
    && Number.isInteger(manifest.assetScale) && manifest.assetScale >= 1 && manifest.assetScale <= 4
    && ["sans", "serif", "mono"].includes(manifest.fontFamily)
    && Number.isFinite(manifest.pressOffsetDp) && manifest.pressOffsetDp >= 0 && manifest.pressOffsetDp <= 2
    && (["light", "dark"] as const).every(tone => {
      const value = manifest.themes?.[tone];
      return !!value && colorNames.every(name => /^#[0-9a-f]{6}$/i.test(value.colors?.[name] ?? ""))
        && value.glow?.length === 4 && value.glow.every(color => /^#[0-9a-f]{6}$/i.test(color));
    });
}
function clearTarget(target: HTMLElement): void {
  ownedVariables.forEach(name => target.style.removeProperty(name));
  delete target.dataset.localSkin;
  target.querySelectorAll<HTMLElement>(":scope > .fc-skin-image").forEach(layer => layer.remove());
  target.classList.remove("fc-skin-surface"); observer.unobserve(target); decorated.delete(target);
}
function clearCustom(): void {
  clearTarget(document.documentElement);
  [...decorated].forEach(clearTarget);
  document.querySelectorAll<HTMLElement>(".cand-window[data-local-skin]").forEach(clearTarget);
}
function applyColors(target: HTMLElement, active: ActivePack, tone: Tone): void {
  const value = active.manifest.themes[tone], colors = value.colors;
  target.dataset.localSkin = active.manifest.id;
  for (const name of colorNames) for (const variable of variables[name]) target.style.setProperty(variable, colors[name]);
  target.style.setProperty("--ime-deck", colors.background); target.style.setProperty("--ime-paper", colors.candidateBackground);
  target.style.setProperty("--ime-screen", colors.candidateBackground); target.style.setProperty("--ime-crt-code", colors.codeText);
  target.style.setProperty("--glow-a", value.glow[0] + "75"); target.style.setProperty("--glow-b", value.glow[1] + "65");
  target.style.setProperty("--fc-font", active.manifest.fontFamily === "mono" ? '"Cascadia Mono",Consolas,monospace' : active.manifest.fontFamily === "serif" ? '"SimSun","Songti SC",Georgia,serif' : '"Microsoft YaHei UI","Segoe UI",system-ui,sans-serif');
  target.style.setProperty("--fc-press-offset", active.manifest.pressOffsetDp + "px");
}
function fitSlices(layer: HTMLElement, target: HTMLElement, raw: string, scale: number): void {
  const [left = 0, top = 0, right = 0, bottom = 0] = raw.split(",").map(Number);
  const box = target.getBoundingClientRect();
  const ratio = Math.max(0, Math.min(1, box.width / Math.max(1, (left + right) / scale), box.height / Math.max(1, (top + bottom) / scale)));
  layer.style.borderImageWidth = [top, right, bottom, left].map(value => value * ratio / scale + "px").join(" ");
}
function imageRole(target: HTMLElement, active: ActivePack, tone: Tone, role: Role): void {
  const descriptor = active.manifest.themes[tone].images?.[role];
  const url = descriptor && allowedUrl(active.imageUrls?.[tone]?.[role], active.hash, descriptor.file);
  if (!descriptor || !url || !["stretch", "contain", "tile", "nineSlice"].includes(descriptor.scale)
      || !Array.isArray(descriptor.insets) || descriptor.insets.length !== 4 || !descriptor.insets.every(value => Number.isInteger(value) && value >= 0 && value <= 2048)) return;
  target.querySelectorAll<HTMLElement>(":scope > .fc-skin-image").forEach(layer => layer.remove());
  const layer = document.createElement("span"); layer.className = "fc-skin-image"; layer.setAttribute("aria-hidden", "true"); layer.dataset.role = role;
  layer.style.clipPath = "inset(0 round " + getComputedStyle(target).borderRadius + ")";
  if (descriptor.scale === "nineSlice") {
    const [left, top, right, bottom] = descriptor.insets;
    layer.style.borderStyle = "solid"; layer.style.borderWidth = "0"; layer.style.borderImageSource = `url(${JSON.stringify(url)})`;
    layer.style.borderImageSlice = `${top} ${right} ${bottom} ${left} fill`; layer.style.borderImageRepeat = "stretch";
    layer.dataset.insets = descriptor.insets.join(","); layer.dataset.assetScale = String(active.manifest.assetScale);
    fitSlices(layer, target, layer.dataset.insets, active.manifest.assetScale); observer.observe(target);
  } else {
    layer.style.backgroundImage = `url(${JSON.stringify(url)})`;
    layer.style.backgroundRepeat = descriptor.scale === "tile" ? "repeat" : "no-repeat";
    layer.style.backgroundPosition = "center";
    layer.style.backgroundSize = descriptor.scale === "stretch" ? "100% 100%" : descriptor.scale === "contain" ? "contain" : "var(--fc-natural-size)";
    if (descriptor.scale === "tile") {
      const image = new Image();
      image.onload = () => { if (layer.isConnected) layer.style.setProperty("--fc-natural-size", `${image.naturalWidth / active.manifest.assetScale}px ${image.naturalHeight / active.manifest.assetScale}px`); };
      image.src = url;
    }
  }
  target.classList.add("fc-skin-surface"); target.prepend(layer); decorated.add(target);
}
function decorate(selector: string, active: ActivePack, tone: Tone, role: Role): void {
  document.querySelectorAll<HTMLElement>(selector).forEach(target => {
    imageRole(target, active, tone, role);
  });
}
function applyActive(): void {
  contentObserver.disconnect();
  clearCustom();
  const active = library?.active;
  const matchesSelection = active && validActive(active)
    && library?.activeId === "local:" + active.manifest.id
    && (theme.customSkinId === undefined || (theme.customSkinId === active.manifest.id
      && (!theme.customSkinHash || theme.customSkinHash === active.hash)));
  if (!active || !matchesSelection) {
    const light = el("showcase-light-label"), dark = el("showcase-dark-label");
    if (light) light.textContent = "系统默认 · 浅色候选窗";
    if (dark) dark.textContent = "系统默认 · 深色候选窗";
    return;
  }
  const tone: Tone = theme.dark ? "dark" : "light";
  applyColors(document.documentElement, active, tone);
  decorate(".win-shell,.altv-popup", active, tone, "deck");
  decorate(".sidebar,.floating-statusbar,.workspace-footer", active, tone, "toolbar");
  decorate(".stepper-btn,.scheme-card,.shortcut-row", active, tone, "keyNormal");
  decorate(".mini-pill,.mode-pill,.btn-restore,.clip-icon-btn,.fb-tool-btn,.fb-lang-badge,.sb-btn,.ai-secondary,.ai-modes button", active, tone, "keyFunction");
  decorate(".btn-finish,.ai-primary,.skin-action-primary", active, tone, "keyAction");
  decorate(".nav-item.is-active,.clip-view-tabs button[aria-selected=true]", active, tone, "selection");
  document.querySelectorAll<HTMLElement>(".cand-window").forEach(preview => {
    const previewTone: Tone = preview.dataset.imeMode === "dark" ? "dark" : "light";
    applyColors(preview, active, previewTone); decorated.add(preview);
    imageRole(preview, active, previewTone, "candidates");
    preview.querySelectorAll<HTMLElement>(".cand-top-row").forEach(target => imageRole(target, active, previewTone, "code"));
    preview.querySelectorAll<HTMLElement>(".cand-pill.is-selected").forEach(target => imageRole(target, active, previewTone, "selection"));
  });
  const light = el("showcase-light-label"), dark = el("showcase-dark-label");
  if (light) light.textContent = active.manifest.name + " · 浅色候选窗";
  if (dark) dark.textContent = active.manifest.name + " · 深色候选窗";
  // Other workspaces render new buttons/items independently. Reapply only after those DOM updates,
  // and disconnect while decorating so our own image layers cannot trigger a mutation loop.
  contentObserver.observe(document.body, { childList: true, subtree: true, attributes: true, attributeFilter: ["class", "aria-selected"] });
}
function scheduleApply(): void {
  if (disposed || applyQueued) return;
  applyQueued = true;
  queueMicrotask(() => { applyQueued = false; if (!disposed) applyActive(); });
}

function message(): void {
  const status = el("skin-library-status"), error = el("skin-library-error");
  if (status) status.textContent = busy ? "正在处理皮肤，请稍候…" : notice;
  if (error) { error.textContent = failure; error.hidden = !failure; }
  const enabled = el<HTMLInputElement>("skin-import-enabled");
  if (enabled) { enabled.checked = library?.importEnabled ?? true; enabled.disabled = !native || !library || busy || mustReload; }
  const importButton = el<HTMLButtonElement>("skin-import");
  if (importButton) importButton.disabled = !native || !library || busy || mustReload || !library.importEnabled;
  const reload = el<HTMLButtonElement>("skin-library-reload"); if (reload) reload.disabled = !native || busy;
  document.querySelectorAll<HTMLButtonElement>("#skin-use-default,#skin-local-grid button").forEach(button => { button.disabled = !native || !library || busy || mustReload || button.dataset.unavailable === "true"; });

}
function selection(): void {
  if (!library) return;
  const useDefault = el<HTMLButtonElement>("skin-use-default");
  const isDefault = library.activeId === "system:default";
  useDefault?.classList.toggle("is-active", isDefault);
  useDefault?.setAttribute("aria-pressed", String(isDefault));
  const currentName = el("skin-current-name");
  if (currentName) currentName.textContent = "当前：" + (library.active?.manifest.name ?? "系统默认");
  document.querySelectorAll<HTMLButtonElement>("#skin-local-grid [data-local-id]").forEach(button => {
    const selected = library!.activeId === "local:" + button.dataset.localId;
    button.classList.toggle("is-selected", selected); button.setAttribute("aria-pressed", String(selected));
    const marker = button.querySelector<HTMLElement>(".skin-local-use"); if (marker) marker.textContent = button.dataset.unavailable === "true" ? "素材不可用，可删除后重新导入" : selected ? "正在使用" : "使用皮肤";
  });
}
function renderLibrary(): void {
  if (main && library) {
    const grid = el("skin-local-grid");
    if (grid) {
      grid.replaceChildren();
      for (const pack of library.packs) {
        const card = document.createElement("article"); card.className = "skin-local-card";
        const use = document.createElement("button"); use.type = "button"; use.className = "skin-local-select"; use.dataset.localId = pack.id;
        if (pack.unavailable) use.dataset.unavailable = "true";
        const url = allowedUrl(pack.previewUrl, pack.hash);
        if (url) { const image = document.createElement("img"); image.src = url; image.alt = ""; image.loading = "lazy"; image.className = "skin-local-preview"; use.append(image); }
        const name = document.createElement("strong"); name.textContent = pack.name;
        const meta = document.createElement("span"); meta.className = "skin-library-note"; meta.textContent = `版本 ${pack.version}` + (pack.author ? " · " + pack.author : "");
        const action = document.createElement("span"); action.className = "skin-local-use"; action.textContent = "使用皮肤";
        use.append(name, meta, action); use.addEventListener("click", () => { void mutate("skin_select", { id: "local:" + pack.id }, "已使用本地皮肤"); });
        const remove = document.createElement("button"); remove.type = "button"; remove.className = "mini-pill skin-local-delete"; remove.textContent = "删除"; remove.setAttribute("aria-label", "删除皮肤“" + pack.name + "”");
        remove.addEventListener("click", () => { if (window.confirm("删除“" + pack.name + "”？正在使用时会先切回系统默认外观。")) void mutate("skin_delete", { id: "local:" + pack.id }, "已删除本地皮肤"); });
        card.append(use, remove); grid.append(card);
      }
    }
    const empty = el("skin-local-empty"); if (empty) empty.hidden = library.packs.length > 0;
  }
  selection(); message(); applyActive();
}
async function reload(explicit = false): Promise<void> {
  if (!native || disposed) return;
  if (busy) { pendingRefresh = true; return; }
  const sequence = ++requestSequence; busy = true; message();
  try {
    const snapshot = await invoke<Library>("skin_library_snapshot");
    if (disposed || sequence !== requestSequence) return;
    library = snapshot; if (explicit) { mustReload = false; failure = ""; }
    notice = snapshot.warning ? "部分皮肤素材不可用，可删除后重新导入；当前可用外观已保留" : "皮肤列表已读取 · 导入后需点选才会使用"; renderLibrary();
  } catch { if (!disposed && sequence === requestSequence) failure = "无法读取皮肤列表；已保留当前外观，请重新读取"; }
  finally { if (sequence === requestSequence) {
    busy = false; message();
    const refresh = pendingRefresh; pendingRefresh = false;
    if (refresh && !mustReload && !disposed) void reload();
  } }
}
async function mutate(command: string, args: object, success: string, expectedRevision?: string): Promise<boolean> {
  if (!main || !native || !library || busy || mustReload || disposed) return false;
  const sequence = ++requestSequence; busy = true; failure = ""; message();
  let committed = false;
  try {
    const result = await invoke<Library | null>(command, { ...args, revision: expectedRevision ?? library.revision });
    if (disposed || sequence !== requestSequence) return false;
    if (result) { library = result; notice = success; committed = true; renderLibrary(); }
    else notice = "已取消导入，当前皮肤未更改";
  } catch (error) {
    if (!disposed && sequence === requestSequence) {
      mustReload = error === CAS_ERROR;
      failure = typeof error === "string" && knownErrors.has(error) ? error : "操作未完成，原皮肤与列表已保留，请重试";
      notice = mustReload ? "请点击“重新读取”后继续" : "当前皮肤未更改";
    }
  } finally {
    if (sequence === requestSequence) {
      busy = false; message();
      const refresh = pendingRefresh; pendingRefresh = false;
      if (refresh && committed && !mustReload) void reload();
    }
  }
  return committed;
}

window.addEventListener("fangcun:theme-applied", event => { theme = (event as CustomEvent<AppliedTheme>).detail; selection(); scheduleApply(); });
el("skin-use-default")?.addEventListener("click", () => {
  void mutate("skin_select", { id: "system:default" }, "已使用系统默认外观");
});
el("skin-library-reload")?.addEventListener("click", () => { void reload(true); });
el("skin-import")?.addEventListener("click", () => { void mutate("skin_import_pick", {}, "已导入皮肤，请点选使用"); });
el<HTMLInputElement>("skin-import-enabled")?.addEventListener("change", event => { const enabled = (event.target as HTMLInputElement).checked; message(); void mutate("skin_set_import_enabled", { enabled }, enabled ? "已允许导入本地皮肤" : "已关闭新导入，已安装皮肤仍可使用"); });
window.addEventListener("pagehide", () => { disposed = true; requestSequence++; observer.disconnect(); contentObserver.disconnect(); unlisteners.splice(0).forEach(stop => stop()); });
async function connect(): Promise<void> {
  if (!native) { message(); return; }
  try {
    const stop = await listen("skin-library://changed", () => { if (mustReload) return; if (busy) pendingRefresh = true; else void reload(); });
    if (disposed) { stop(); return; } unlisteners.push(stop);
  } catch { /* Explicit reload is still available if event subscription fails. */ }
  await reload();
}
void connect();
