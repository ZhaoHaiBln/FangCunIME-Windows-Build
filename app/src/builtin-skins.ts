/** Editable builtin preferences. These are overlays, never local-package identities. */
export type BuiltinId = "wood_type" | "typewriter" | "terminal" | "brutal";
type Tone = "light" | "dark";
type Font = "sans" | "serif" | "mono";
export type BuiltinColor = "background" | "surface" | "border" | "function" | "text" | "muted" | "accent" | "onAccent" | "pressed" | "toolbar" | "candidateBackground" | "candidateText" | "selection" | "onSelection" | "keyText" | "keyHint" | "functionText" | "codeText";
export interface BuiltinTone { colors?: Partial<Record<BuiltinColor, string>>; glow?: string[]; }
export interface BuiltinStyle { fontFamily?: Font; pressOffsetDp?: number; themes?: Partial<Record<Tone, BuiltinTone>>; }
interface Library { revision: string; builtinStyles?: Partial<Record<BuiltinId, BuiltinStyle>>; }
interface Theme { skin: string; dark: boolean; customSkinId?: string; }
interface EditorHost {
  native: boolean;
  library(): Library | null;
  busy(): boolean;
  blocked(): boolean;
  mutate(command: string, args: object, success: string, expectedRevision?: string): Promise<boolean>;
  reload(): Promise<void>;
}
const names: Record<BuiltinId, string> = { wood_type: "木框活字", typewriter: "经典打字机", terminal: "绿屏终端", brutal: "极客机械" };
const fields: Record<BuiltinColor, [string, string]> = {
  background: ["底板材质颜色", "--ime-board-bg"], surface: ["普通键帽颜色", "--ime-key-bg"],
  border: ["边框", "--ime-border"], function: ["功能键颜色", "--ime-fn-key-bg"],
  text: ["界面文字", "--ime-text"], muted: ["辅助文字", "--ime-subtext"],
  accent: ["强调色", "--ime-accent"], onAccent: ["强调色上的文字", "--ime-accent-text"],
  pressed: ["按下颜色", "--ime-key-pressed-bg"], toolbar: ["工具栏颜色", "--ime-toolbar-bg"],
  candidateBackground: ["候选栏材质颜色", "--ime-win-bg"], candidateText: ["候选文字", "--candidate-ink"],
  selection: ["候选选中背景", "--ime-cand-active-bg"], onSelection: ["候选选中文字", "--ime-cand-active-text"],
  keyText: ["键帽文字", "--ime-text"], keyHint: ["键帽副标", "--ime-subtext"],
  functionText: ["功能键文字", "--ime-text"], codeText: ["拼音文字", "--ime-crt-code"],
};
const css: Record<BuiltinColor, string[]> = {
  background: ["--ime-board-bg"], surface: ["--ime-key-bg", "--ime-win-bg"], border: ["--ime-border", "--ime-win-outer-border"],
  function: ["--ime-fn-key-bg"], text: ["--ime-text"], muted: ["--ime-subtext"], accent: ["--ime-accent"],
  onAccent: ["--ime-accent-text"], pressed: ["--ime-key-pressed-bg"], toolbar: ["--ime-toolbar-bg", "--ime-win-header-bg"],
  candidateBackground: ["--fc-builtin-candidate-background"], candidateText: ["--candidate-ink", "--fc-builtin-candidate-text"],
  selection: ["--ime-cand-active-bg", "--fc-builtin-selection"], onSelection: ["--ime-cand-active-text", "--fc-builtin-on-selection"],
  keyText: ["--fc-builtin-key-text"], keyHint: ["--fc-builtin-key-hint"], functionText: ["--fc-builtin-function-text"], codeText: ["--ime-crt-code", "--fc-builtin-code-text"],
};
const additionalVariables = ["--ime-deck", "--ime-paper", "--ime-screen", "--glow-a", "--glow-b", "--fc-builtin-glow-c", "--fc-builtin-glow-d", "--fc-builtin-font", "--fc-builtin-press", "--fc-builtin-key-material"];
const variables = [...new Set([...Object.values(css).flat(), ...additionalVariables])];
const clone = <T>(value: T): T => JSON.parse(JSON.stringify(value)) as T;
const id = <T extends HTMLElement>(name: string) => document.getElementById(name) as T | null;
const colorValid = (value: unknown): value is string => typeof value === "string" && /^#[0-9a-f]{6}$/i.test(value);
const targets = new Set<HTMLElement>();
const defaults = new Map<string, { colors: Record<BuiltinColor, string>; deck: string; paper: string; screen: string; cap: string; glow: string[] }>();

function defaultsFor(skin: BuiltinId, tone: Tone) {
  const key = skin + ":" + tone;
  const cached = defaults.get(key);
  if (cached) return cached;
  const probe = document.createElement("div");
  probe.className = "cand-window skin-builtin-probe"; probe.dataset.imeTheme = skin; probe.dataset.imeMode = tone;
  const cap = document.createElement("i"), swatch = document.createElement("span");
  swatch.className = "skin-swatch"; swatch.dataset.imeTheme = skin; swatch.dataset.imeMode = tone; swatch.append(cap); probe.append(swatch); document.body.append(probe);
  const style = getComputedStyle(probe);
  const colors = {} as Record<BuiltinColor, string>;
  for (const [name, [, variable]] of Object.entries(fields)) {
    let value = style.getPropertyValue(variable).trim();
    if (!colorValid(value)) value = style.getPropertyValue(name === "muted" || name === "keyHint" ? "--ime-subtext" : "--ime-text").trim();
    colors[name as BuiltinColor] = colorValid(value) ? value.toUpperCase() : "#000000";
  }
  // The metal-rimmed black caps have cream legends; their ink is separate
  // from the dark text printed on the typewriter's paper display.
  if (skin === "typewriter") { colors.keyText = "#FFF1D2"; colors.keyHint = "#CDBA9B"; }
  const glowA = style.getPropertyValue("--glow-a").trim().slice(0, 7), glowB = style.getPropertyValue("--glow-b").trim().slice(0, 7);
  const result = { colors, deck: style.getPropertyValue("--ime-deck").trim(), paper: style.getPropertyValue("--ime-paper").trim(), screen: style.getPropertyValue("--ime-screen").trim(), cap: getComputedStyle(cap).backgroundImage,
    glow: [glowA, glowB, glowA, glowB].map(value => colorValid(value) ? value.toUpperCase() : "#69A976") };
  probe.remove(); defaults.set(key, result); return result;
}
function validStyle(profile: BuiltinStyle): boolean {
  if (!profile || typeof profile !== "object" || Array.isArray(profile)) return false;
  if (Object.keys(profile).some(key => !["fontFamily", "pressOffsetDp", "themes"].includes(key))) return false;
  if (profile.fontFamily !== undefined && !["sans", "serif", "mono"].includes(profile.fontFamily)) return false;
  if (profile.pressOffsetDp !== undefined && (!Number.isFinite(profile.pressOffsetDp) || profile.pressOffsetDp < 0 || profile.pressOffsetDp > 2)) return false;
  if (profile.themes !== undefined && (!profile.themes || typeof profile.themes !== "object" || Array.isArray(profile.themes))) return false;
  return Object.entries(profile.themes ?? {}).every(([tone, value]) => ["light", "dark"].includes(tone)
    && !!value && typeof value === "object" && !Array.isArray(value)
    && Object.keys(value).every(key => ["colors", "glow"].includes(key))
    && (value.colors === undefined || !!value.colors && typeof value.colors === "object" && !Array.isArray(value.colors)
      && Object.entries(value.colors).every(([name, color]) => Object.hasOwn(fields, name) && colorValid(color)))
    && (value.glow === undefined || Array.isArray(value.glow) && value.glow.length === 4 && value.glow.every(colorValid)));
}
function clearTarget(target: HTMLElement): void {
  for (const variable of variables) target.style.removeProperty(variable);
  for (const key of Object.keys(target.dataset)) if (key.startsWith("builtin")) delete target.dataset[key];
}
export function clearBuiltinStyles(): void { for (const target of targets) clearTarget(target); targets.clear(); }
function tint(color: string, material: string): string { return `linear-gradient(${color}99,${color}99)` + (material && material !== "none" ? "," + material : ""); }
function applyTarget(target: HTMLElement, skin: BuiltinId, tone: Tone, profile: BuiltinStyle): void {
  if (!validStyle(profile)) return;
  const base = defaultsFor(skin, tone), colors = profile.themes?.[tone]?.colors ?? {};
  let changed = false;
  for (const [name, color] of Object.entries(colors) as [BuiltinColor, string][]) {
    if (color.toUpperCase() === base.colors[name]) continue;
    changed = true; target.dataset["builtin" + name[0]!.toUpperCase() + name.slice(1)] = "true";
    for (const variable of css[name]) target.style.setProperty(variable, color);
    if (name === "background" && base.deck) target.style.setProperty("--ime-deck", tint(color, base.deck));
    if (name === "surface") target.style.setProperty("--fc-builtin-key-material", tint(color, base.cap));
    if (name === "candidateBackground") {
      if (base.paper) target.style.setProperty("--ime-paper", tint(color, base.paper));
      if (base.screen) target.style.setProperty("--ime-screen", tint(color, base.screen));
    }
  }
  const glow = profile.themes?.[tone]?.glow;
  if (glow) {
    changed = true; target.dataset.builtinGlow = "true";
    ["--glow-a", "--glow-b", "--fc-builtin-glow-c", "--fc-builtin-glow-d"].forEach((variable, index) => target.style.setProperty(variable, glow[index]! + "88"));
  }
  if (profile.fontFamily) {
    changed = true; target.dataset.builtinFont = "true";
    target.style.setProperty("--fc-builtin-font", profile.fontFamily === "mono" ? '"Cascadia Mono",Consolas,monospace' : profile.fontFamily === "serif" ? '"SimSun","Songti SC",Georgia,serif' : '"Microsoft YaHei UI","Segoe UI",system-ui,sans-serif');
  }
  if (profile.pressOffsetDp !== undefined) { changed = true; target.dataset.builtinPress = "true"; target.style.setProperty("--fc-builtin-press", profile.pressOffsetDp + "px"); }
  if (changed) { target.dataset.builtinEdit = skin; targets.add(target); }
}
export function applyBuiltinStyles(library: Library | null, theme: Theme): void {
  clearBuiltinStyles();
  if (!theme.customSkinId && Object.hasOwn(names, theme.skin)) {
    const skin = theme.skin as BuiltinId, profile = library?.builtinStyles?.[skin];
    if (profile) applyTarget(document.documentElement, skin, theme.dark ? "dark" : "light", profile);
    document.querySelectorAll<HTMLElement>(".cand-window:not(.builtin-editor-preview)").forEach(target => {
      const targetSkin = (target.dataset.imeTheme ?? skin) as BuiltinId, targetProfile = library?.builtinStyles?.[targetSkin];
      if (targetProfile && Object.hasOwn(names, targetSkin)) applyTarget(target, targetSkin, target.dataset.imeMode === "dark" ? "dark" : "light", targetProfile);
    });
  }
  document.querySelectorAll<HTMLElement>("#skin-group .skin-swatch").forEach(target => {
    const skin = target.dataset.imeTheme as BuiltinId, profile = library?.builtinStyles?.[skin];
    if (profile && Object.hasOwn(names, skin)) applyTarget(target, skin, target.dataset.imeMode === "dark" ? "dark" : "light", profile);
  });
}

export function installBuiltinEditor(host: EditorHost) {
  let current: { id: BuiltinId; draft: BuiltinStyle; original: string; revision: string; tone: Tone } | null = null;
  let note = "", error = "";
  const section = id("builtin-skin-editor");
  function invalid() { return current !== null && !validStyle(current.draft); }
  function dirty() { return current !== null && JSON.stringify(current.draft) !== current.original; }
  function colorRow(label: string, value: string, description: string, onChange: (value: string) => void, onReset: () => void) {
    const row = document.createElement("div"); row.className = "builtin-color-field";
    const name = document.createElement("label"), input = document.createElement("input"), sample = document.createElement("span"), reset = document.createElement("button");
    input.id = description; input.type = "text"; input.value = value; input.maxLength = 7; input.spellcheck = false;
    input.autocomplete = "off"; input.setAttribute("aria-label", label); input.setAttribute("aria-invalid", String(!colorValid(value)));
    name.htmlFor = input.id; name.textContent = label; sample.className = "builtin-color-sample"; sample.setAttribute("aria-hidden", "true");
    if (colorValid(value)) sample.style.background = value;
    reset.type = "button"; reset.className = "builtin-field-reset"; reset.textContent = "默认"; reset.setAttribute("aria-label", label + "恢复默认");
    input.addEventListener("input", () => { onChange(input.value); sample.style.background = colorValid(input.value) ? input.value : "transparent"; input.setAttribute("aria-invalid", String(!colorValid(input.value))); update(false); });
    reset.addEventListener("click", () => { onReset(); update(); });
    row.append(name, sample, input, reset); return row;
  }
  function clean(profile: BuiltinStyle): BuiltinStyle {
    const result = clone(profile);
    for (const tone of ["light", "dark"] as Tone[]) {
      const value = result.themes?.[tone];
      if (!value) continue;
      if (value.colors && !Object.keys(value.colors).length) delete value.colors;
      if (!value.colors && !value.glow) delete result.themes?.[tone];
    }
    if (result.themes && !Object.keys(result.themes).length) delete result.themes;
    return result;
  }
  function editTone(): BuiltinTone {
    if (!current) return {};
    current.draft.themes ??= {}; return current.draft.themes[current.tone] ??= {};
  }
  function preview(): void {
    if (!current) return;
    const target = id("builtin-editor-material"), candidate = id("builtin-editor-candidate"), keys = target?.querySelector<HTMLElement>(".skin-swatch");
    if (!target || !candidate || !keys) return;
    target.dataset.imeTheme = current.id; target.dataset.imeMode = current.tone;
    candidate.dataset.imeTheme = current.id; candidate.dataset.imeMode = current.tone;
    keys.dataset.imeTheme = current.id; keys.dataset.imeMode = current.tone;
    const safe = clean(clone(current.draft));
    for (const value of Object.values(safe.themes ?? {})) {
      if (value.colors) for (const name of Object.keys(value.colors) as BuiltinColor[]) if (!colorValid(value.colors[name])) delete value.colors[name];
      if (value.glow && !value.glow.every(colorValid)) delete value.glow;
    }
    for (const surface of [target, candidate, keys]) { clearTarget(surface); applyTarget(surface, current.id, current.tone, safe); }
  }
  function update(rebuild = true): void {
    if (!section) return;
    section.hidden = !current;
    document.querySelectorAll<HTMLButtonElement>("button[data-builtin-edit],button[data-builtin-reset]").forEach(button => {
      button.disabled = !host.native || !host.library() || host.busy() || host.blocked() || current !== null;
      if (button.dataset.builtinReset) button.disabled ||= !host.library()?.builtinStyles?.[button.dataset.builtinReset as BuiltinId];
    });
    if (!current) return;
    const title = id("builtin-editor-title"); if (title) title.textContent = "编辑 · " + names[current.id];
    const conflict = current.revision !== host.library()?.revision || host.blocked();
    const status = id("builtin-editor-status"), failure = id("builtin-editor-error");
    if (status) status.textContent = host.busy() ? "正在保存，请稍候…" : conflict ? "已保存版本有变化；草稿保留，请重新读取后再保存。" : note || (dirty() ? "草稿尚未保存" : "原拟物材质保留；修改只在保存后生效");
    if (failure) { failure.textContent = invalid() ? "颜色请填写 #RRGGBB（例如 #315B42），按压范围为 0–2。" : error; failure.hidden = !failure.textContent; }
    section.querySelectorAll<HTMLInputElement | HTMLSelectElement | HTMLButtonElement>("input,select,button").forEach(control => { control.disabled = host.busy(); });
    const save = id<HTMLButtonElement>("builtin-editor-save"); if (save) save.disabled = !host.native || host.busy() || conflict || invalid();
    const refresh = id<HTMLButtonElement>("builtin-editor-reload"); if (refresh) refresh.disabled = !host.native || host.busy();
    const pressControl = id<HTMLInputElement>("builtin-editor-press"); if (pressControl) pressControl.disabled = host.busy() || current.draft.pressOffsetDp === undefined;
    document.querySelectorAll<HTMLButtonElement>("[data-builtin-tone]").forEach(button => button.setAttribute("aria-pressed", String(button.dataset.builtinTone === current!.tone)));
    if (rebuild) {
      const common = id("builtin-editor-colors"), advanced = id("builtin-editor-advanced"), glow = id("builtin-editor-glow");
      common?.replaceChildren(); advanced?.replaceChildren(); glow?.replaceChildren();
      const base = defaultsFor(current.id, current.tone);
      const commonNames: BuiltinColor[] = ["background", "surface", "function", "toolbar", "candidateBackground", "candidateText", "selection", "onSelection"];
      for (const [name, [label]] of Object.entries(fields) as [BuiltinColor, [string, string]][]) {
        const color = current.draft.themes?.[current.tone]?.colors?.[name] ?? base.colors[name];
        const row = colorRow(label, color, "builtin-color-" + name, value => {
          const tone = editTone(); tone.colors ??= {};
          if (colorValid(value) && value.toUpperCase() === base.colors[name]) delete tone.colors[name];
          else tone.colors[name] = colorValid(value) ? value.toUpperCase() : value;
          current!.draft = clean(current!.draft); error = ""; note = "";
        }, () => { if (current?.draft.themes?.[current.tone]?.colors) delete current.draft.themes[current.tone]!.colors![name]; current!.draft = clean(current!.draft); });
        (commonNames.includes(name) ? common : advanced)?.append(row);
      }
      for (let index = 0; index < 4; index++) {
        const value = current.draft.themes?.[current.tone]?.glow?.[index] ?? base.glow[index]!;
        glow?.append(colorRow("流光颜色 " + (index + 1), value, "builtin-glow-" + index, color => {
          const tone = editTone(); tone.glow ??= [...base.glow]; tone.glow[index] = colorValid(color) ? color.toUpperCase() : color;
          if (tone.glow.every((entry, i) => entry === base.glow[i])) delete tone.glow;
          current!.draft = clean(current!.draft); error = ""; note = "";
        }, () => {
          const tone = current?.draft.themes?.[current.tone];
          if (tone?.glow) { tone.glow[index] = base.glow[index]!; if (tone.glow.every((entry, i) => entry === base.glow[i])) delete tone.glow; }
          current!.draft = clean(current!.draft);
        }));
      }
      const font = id<HTMLSelectElement>("builtin-editor-font"); if (font) font.value = current.draft.fontFamily ?? "";
      const press = id<HTMLInputElement>("builtin-editor-press"), enabled = id<HTMLInputElement>("builtin-editor-use-press"), output = id("builtin-editor-press-value");
      if (press) { press.value = String(current.draft.pressOffsetDp ?? 1.5); press.disabled = host.busy() || current.draft.pressOffsetDp === undefined; }
      if (enabled) enabled.checked = current.draft.pressOffsetDp !== undefined;
      if (output) output.textContent = current.draft.pressOffsetDp === undefined ? "使用皮肤默认" : current.draft.pressOffsetDp + " dp";
    }
    preview();
  }
  function open(skin: BuiltinId): void {
    if (!host.native || host.busy() || host.blocked() || !host.library() || current) return;
    const draft = clone(host.library()!.builtinStyles?.[skin] ?? {});
    current = { id: skin, draft, original: JSON.stringify(draft), revision: host.library()!.revision, tone: "light" };
    note = ""; error = ""; update(); section?.scrollIntoView({ block: "start" }); id("builtin-editor-title")?.focus({ preventScroll: true });
  }
  function render(): void {
    document.querySelectorAll<HTMLElement>("#skin-group .builtin-skin-card").forEach(card => {
      const button = card.querySelector<HTMLElement>("[data-skin]"); card.hidden = !!button?.hidden;
    });
    update(false);
  }
  document.querySelectorAll<HTMLButtonElement>("button[data-builtin-edit]").forEach(button => button.addEventListener("click", event => {
    event.stopPropagation(); // Never let the skin selector treat an edit as a selection.
    open(button.dataset.builtinEdit as BuiltinId);
  }));
  document.querySelectorAll<HTMLButtonElement>("button[data-builtin-reset]").forEach(button => button.addEventListener("click", event => {
    event.stopPropagation();
    if (current || host.busy()) return;
    void host.mutate("skin_reset_builtin_style", { id: button.dataset.builtinReset }, "已恢复这款皮肤原样");
  }));
  document.querySelectorAll<HTMLButtonElement>("[data-builtin-tone]").forEach(button => button.addEventListener("click", () => { if (current && !host.busy()) { current.tone = button.dataset.builtinTone as Tone; update(); } }));
  id("builtin-editor-cancel")?.addEventListener("click", () => { if (!host.busy()) { current = null; error = ""; note = ""; update(); } });
  id<HTMLSelectElement>("builtin-editor-font")?.addEventListener("change", event => {
    if (!current) return;
    const value = (event.target as HTMLSelectElement).value; if (value) current.draft.fontFamily = value as Font; else delete current.draft.fontFamily;
    note = ""; update(false);
  });
  id<HTMLInputElement>("builtin-editor-use-press")?.addEventListener("change", event => { if (current) { if ((event.target as HTMLInputElement).checked) current.draft.pressOffsetDp = 1.5; else delete current.draft.pressOffsetDp; update(); } });
  id<HTMLInputElement>("builtin-editor-press")?.addEventListener("input", event => { if (current) { current.draft.pressOffsetDp = Number((event.target as HTMLInputElement).value); const output = id("builtin-editor-press-value"); if (output) output.textContent = current.draft.pressOffsetDp + " dp"; update(false); } });
  id("builtin-editor-defaults")?.addEventListener("click", () => { if (current && !host.busy()) { current.draft = {}; note = "草稿已恢复原样；点保存后才会生效"; error = ""; update(); } });
  id("builtin-editor-reload")?.addEventListener("click", () => { void (async () => {
    if (!current || host.busy()) return;
    await host.reload(); if (!current) return;
    if (!host.blocked() && host.library()) { current.revision = host.library()!.revision; note = "已重新读取，草稿未变；再次保存将使用当前草稿。"; error = ""; update(false); }
  })(); });
  id("builtin-editor-save")?.addEventListener("click", () => { void (async () => {
    if (!current || invalid() || host.busy() || host.blocked()) return;
    const state = current, saved = await host.mutate("skin_save_builtin_style", { id: state.id, style: clean(state.draft), activate: true }, "已保存并使用这款皮肤", state.revision);
    if (saved && current === state) { current = null; update(); }
    else if (current === state) { error = "保存未完成，草稿仍在；请查看皮肤列表提示。"; update(false); }
  })(); });
  return { render, preview };
}
