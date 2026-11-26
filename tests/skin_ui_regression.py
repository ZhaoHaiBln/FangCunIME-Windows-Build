"""Real Windows companion TypeScript with deterministic skin-library IPC and PNG fixtures.

Only the supplied loopback Vite origin and intercepted fcskin fixture URLs are
allowed. This does not exercise Windows WebView2, Win32 file selection, ZIP/PNG
validation in Rust, native TSF painting, a real user's files, clipboard or AI.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import time
import traceback
from urllib.parse import urlsplit
from xml.etree import ElementTree as ET
import zlib

sys.dont_write_bytecode = True
from playwright.sync_api import expect, sync_playwright

spec = importlib.util.spec_from_file_location("fangcun_ai_fixture", Path(__file__).with_name("ai_ui_regression.py"))
assert spec and spec.loader
shared = importlib.util.module_from_spec(spec)
spec.loader.exec_module(shared)
BASE = "http://127.0.0.1:5174/"
OUT = Path(tempfile.gettempdir()) / "fangcun-windows-skin-ui-regression"
HASH = "a" * 64
BASES = {"mechanical": "wood_type", "typewriter": "typewriter", "mahjong": "terminal", "brutal": "brutal"}
COLOR_NAMES = ["background", "surface", "border", "function", "text", "muted", "accent", "onAccent", "pressed", "toolbar", "candidateBackground", "candidateText", "selection", "onSelection", "keyText", "keyHint", "functionText", "codeText"]
LIGHT = dict(zip(COLOR_NAMES, ["#EDF4ED", "#E2ECE4", "#557766", "#C9DCCF", "#172A1D", "#4D6557", "#316B46", "#FFFFFF", "#B4CFBC", "#D5E5D8", "#EAF6E9", "#16351E", "#3B764C", "#FAFFFA", "#203F29", "#45634F", "#1B4127", "#285D36"]))
DARK = dict(zip(COLOR_NAMES, ["#111E16", "#1A3021", "#668F71", "#284B34", "#E2F6E6", "#A1C6AA", "#7BC38E", "#101E14", "#3A6948", "#1E3B28", "#152C1E", "#D2F2DA", "#6FA982", "#112319", "#D7EEDD", "#90B99C", "#E0F9E7", "#B3E4C0"]))
ROLES = ["deck", "toolbar", "code", "candidates", "selection", "keyNormal", "keyFunction", "keyAction"]
FILES = {role: "".join("_" + c.lower() if c.isupper() else c for c in role) + ".png" for role in ROLES}
SCALES = dict(zip(ROLES, ["tile", "contain", "stretch", "nineSlice", "nineSlice", "nineSlice", "stretch", "contain"]))


def make_manifest(base="mechanical", images=True, name="草木方寸"):
    return {"format": "fangcun.skin", "schemaVersion": 1, "id": "fixture.green", "name": name,
            "version": 3, "author": "本地示例作者", "license": "测试样例", "baseSkin": base, "assetScale": 2,
            "fontFamily": "sans", "pressOffsetDp": 1.5, "preview": "preview.png",
            "themes": {tone: {"colors": colors, "glow": ["#63B77A", "#A6D86B", "#66C8BF", "#C1B6E8"],
                              "images": {role: {"file": tone + "/" + FILES[role], "scale": SCALES[role],
                                                "insets": [4, 4, 4, 4] if SCALES[role] == "nineSlice" else [0, 0, 0, 0]}
                                         for role in ROLES} if images else {}}
                       for tone, colors in (("light", LIGHT), ("dark", DARK))}}


SKIN_MOCK = r"""
(() => {
  const initial=__SKIN_CONFIG__, f=window.__aiFixture, clone=v=>JSON.parse(JSON.stringify(v));
  const base={mechanical:'wood_type',typewriter:'typewriter',mahjong:'terminal',brutal:'brutal'};
  let revision=1;
  const s=f.skin={revision:'skin-1',importEnabled:true,hidden:[],installed:initial.installed,
    manifest:clone(initial.manifest),hash:initial.hash,importOutcome:'success',failure:null,defer:false,pending:[],deferReads:false,reads:[],
    rotate(){this.revision='skin-'+(++revision);},
    activeId(){return this.installed && f.settings.customSkinId===this.manifest.id ? 'local:'+this.manifest.id : 'system:default';},
    snapshot(){
      const urls={light:{},dark:{}};
      for(const tone of ['light','dark'])for(const [role,image]of Object.entries(this.manifest.themes[tone].images))urls[tone][role]='http://fcskin.localhost/'+this.hash+'/'+image.file;
      return clone({revision:this.revision,importEnabled:this.importEnabled,hiddenBuiltins:this.hidden,
        builtins:[],
        packs:this.installed?[{id:this.manifest.id,hash:this.hash,name:this.manifest.name,version:this.manifest.version,author:this.manifest.author,baseSkin:this.manifest.baseSkin,previewUrl:'http://fcskin.localhost/'+this.hash+'/preview.png'}]:[],
        activeId:this.activeId(),active:this.activeId().startsWith('local:')?{id:this.manifest.id,hash:this.hash,manifest:this.manifest,imageUrls:urls}:null});
    },
    publish(){f.settings.revision='settings-skin-'+revision;f.emit('settings://changed',f.settings);f.emit('skin-library://changed',{revision:this.revision});},
    fallback(){f.settings.customSkinId='';f.settings.customSkinHash='';f.settings.skin='system_default';},
    change(command,args){
      if(args.revision!==this.revision)throw '皮肤列表已更新，请重新读取后重试';
      if(this.failure){const error=this.failure;this.failure=null;throw error;}
      if(command==='skin_import_pick'){
        if(!this.importEnabled)throw '本地皮肤导入已关闭';
        if(this.importOutcome==='cancel')return null;
        if(this.importOutcome==='fail')throw '示例 ZIP 校验失败';
        this.installed=true;
      }else if(command==='skin_set_import_enabled')this.importEnabled=args.enabled;
      else if(command==='skin_select'){
        if(args.id.startsWith('local:')){
          if(!this.installed)throw '皮肤包不存在，请重新读取';
          f.settings.customSkinId=this.manifest.id;f.settings.customSkinHash=this.hash;f.settings.skin=base[this.manifest.baseSkin];
        }else if(args.id==='system:default'){this.fallback();}else throw '皮肤选择无效';
      }else if(command==='skin_delete'){
        if(this.activeId().startsWith('local:'))this.fallback();
        this.installed=false;
      }
      this.rotate();this.publish();return this.snapshot();
    },
    finish(){const job=this.pending.shift();if(!job)throw Error('No pending skin mutation');try{job.resolve(this.change(job.command,job.args));}catch(e){job.reject(e);}},
    finishRead(){const job=this.reads.shift();if(!job)throw Error('No pending skin read');job.resolve(job.value);}
  };
  if(initial.active){f.settings.skin=base[s.manifest.baseSkin];f.settings.customSkinId=s.manifest.id;f.settings.customSkinHash=s.hash;}
  const original=window.__TAURI_INTERNALS__.invoke.bind(window.__TAURI_INTERNALS__);
  window.__TAURI_INTERNALS__.invoke=async(command,args={})=>{
    if(command==='skin_library_snapshot'){
      f.calls.push({command,args:clone(args)});
      if(s.failure==='read'){s.failure=null;throw '示例读取失败';}
      if(s.deferReads)return await new Promise(resolve=>s.reads.push({resolve,value:s.snapshot()}));
      return s.snapshot();
    }
    if(['skin_set_import_enabled','skin_select','skin_delete','skin_import_pick'].includes(command)){
      f.calls.push({command,args:clone(args)});
      if(s.defer)return await new Promise((resolve,reject)=>s.pending.push({command,args:clone(args),resolve,reject}));
      return s.change(command,args);
    }
    return original(command,args);
  };
})();
"""

RESULTS = []
BLOCKED = []


def png():
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 12, 12, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress((b"\0" + bytes([112, 157, 121]) * 12) * 12)) + chunk(b"IEND", b"")


def check(value, message):
    if not value:
        raise AssertionError(message)


def wait_idle(page):
    expect(page.locator("#skin-library-status")).not_to_contain_text("正在")
    expect(page.locator("#skin-library-reload")).to_be_enabled()


def tab(page, name):
    page.locator(f'[data-tab="{name}"]').click()
    expect(page.locator(f'[data-pane="{name}"]')).to_be_visible()


def management(page):
    tab(page, "theme")


def installed_button(page):
    return page.locator('[data-local-id="fixture.green"]')


def select_local(page):
    tab(page, "theme")
    installed_button(page).click()
    expect(page.locator("html")).to_have_attribute("data-local-skin", "fixture.green")
    wait_idle(page)


def selected_default(page):
    expect(page.locator("#skin-use-default")).to_have_attribute("aria-pressed", "true")
    expect(page.locator("html")).to_have_attribute("data-ime-theme", "system_default")


@contextmanager
def app(browser, *, installed=False, active=False, base="mechanical", images=True, name="草木方寸", tone="light", window_mode="main", width=940, height=760):
    settings = {**shared.SETTINGS, "themeMode": tone, "darkMode": tone == "dark", "customSkinId": "", "customSkinHash": ""}
    context = browser.new_context(viewport={"width": width, "height": height}, reduced_motion="reduce", color_scheme=tone)
    context.add_init_script(shared.MOCK.replace("__FIXTURE_CONFIG__", json.dumps({"settings": settings, "ai": shared.CONFIG}, ensure_ascii=False)) + "\n" + SKIN_MOCK.replace("__SKIN_CONFIG__", json.dumps({"manifest": make_manifest(base, images, name), "hash": HASH, "installed": installed, "active": active}, ensure_ascii=False)))
    origin = urlsplit(BASE)
    def route(request):
        url = urlsplit(request.request.url)
        if url.scheme == "http" and (url.hostname, url.port) == (origin.hostname, origin.port):
            request.continue_()
        elif url.scheme == "http" and url.hostname == "fcskin.localhost" and url.path.startswith("/" + HASH + "/") and url.path.endswith(".png"):
            request.fulfill(status=200, content_type="image/png", body=png(), headers={"Cache-Control": "public,max-age=31536000,immutable"})
        else:
            BLOCKED.append(url.scheme + "://" + (url.hostname or "")); request.abort()
    context.route("**/*", route)
    page = context.new_page(); page.set_default_timeout(6500)
    errors = []; page.on("pageerror", lambda error: errors.append(type(error).__name__))
    try:
        page.goto(BASE + ("?mode=" + window_mode if window_mode != "main" else ""), wait_until="networkidle")
        page.wait_for_function("window.__aiFixture.count('skin_library_snapshot')>=1")
        if window_mode == "main":
            wait_idle(page); tab(page, "theme")
        if active:
            expect(page.locator("html")).to_have_attribute("data-local-skin", "fixture.green")
        yield page
        check(not errors, "Frontend raised an uncaught exception")
        check(not page.evaluate("window.__aiFixture.unknownCommands.length"), "IPC fixture encountered an unknown command")
    finally:
        context.close()


def system_defaults_only(browser):
    with app(browser) as page:
        check(page.locator("#skin-group,button[data-skin],#builtin-skin-editor,#skin-builtin-management").count()==0,"No bundled choices, editor or restore controls may remain")
        selected_default(page)
        expect(page.locator("html")).not_to_have_attribute("data-local-skin", "fixture.green")
        check(page.locator(".fc-skin-image").count()==0,"Default must have no bundled material layers")
        expect(page.locator("#skin-import-enabled")).to_be_checked()
        expect(page.locator("#skin-import")).to_be_enabled()
        expect(page.locator("#skin-local-empty")).to_be_visible()


def reload_does_not_restore_bundled_skins(browser):
    with app(browser) as page:
        for _ in range(2):
            page.locator("#skin-library-reload").click();wait_idle(page)
            check(page.locator("#skin-group,[data-builtin],#builtin-skin-editor").count()==0,"Reload cannot resurrect retired skin controls")
            selected_default(page)
        check(page.evaluate("window.__aiFixture.calls.every(call=>!call.command.includes('builtin'))"),"Retired builtin IPC must never be invoked")


def default_preserves_preferences(browser):
    with app(browser,installed=True,active=True) as page:
        preserved=page.evaluate("(() => {const s=window.__aiFixture.settings;return [s.themeMode,s.firstCandidateSize,s.keyOpacity,s.typingGlow,s.scheme,s.keyLetterSize,s.keyLetterBold];})()")
        page.locator("#skin-use-default").click();wait_idle(page)
        selected_default(page)
        check(page.evaluate("(() => {const s=window.__aiFixture.settings;return [s.themeMode,s.firstCandidateSize,s.keyOpacity,s.typingGlow,s.scheme,s.keyLetterSize,s.keyLetterBold];})()") == preserved,
              "Changing only the selected file must preserve mode/input/typography/effects preferences")
        expect(installed_button(page)).to_be_visible()
        check(page.evaluate("!window.__aiFixture.settings.customSkinId && !window.__aiFixture.settings.customSkinHash"),"Explicit default must clear both local identity fields")


def import_cancel_and_no_autoapply(browser):
    with app(browser) as page:
        page.evaluate("window.__aiFixture.skin.importOutcome='cancel'")
        page.locator("#skin-import").click(); wait_idle(page)
        expect(page.locator("#skin-library-status")).to_contain_text("已取消")
        check(page.locator("#skin-local-grid article").count() == 0, "Native picker cancellation must not register a pack")
        page.evaluate("window.__aiFixture.skin.importOutcome='success'")
        page.locator("#skin-import").click(); wait_idle(page)
        expect(installed_button(page)).to_be_visible()
        expect(installed_button(page)).to_contain_text("版本 3")
        selected_default(page)
        check(page.evaluate("document.documentElement.dataset.localSkin === undefined"), "Import must never auto-apply the pack")
        select_local(page)


def escaped_pack_labels(browser):
    name = "<img src=x onerror=alert(1)>"
    with app(browser, installed=True, name=name) as page:
        expect(installed_button(page)).to_contain_text(name)
        check(page.locator("#skin-local-grid img").count() == 1, "Untrusted pack names must be text, not added HTML elements")


def disabled_import_keeps_installed_usable(browser):
    with app(browser, installed=True) as page:
        page.locator("#skin-import-enabled").uncheck(); wait_idle(page)
        expect(page.locator("#skin-import")).to_be_disabled()
        expect(installed_button(page)).to_be_enabled()
        select_local(page)
        expect(page.locator("#skin-import-enabled")).not_to_be_checked()


def delete_active_clears_all_materials(browser):
    with app(browser, installed=True, active=True) as page:
        page.once("dialog", lambda dialog: dialog.accept())
        page.locator(".skin-local-delete").click(); wait_idle(page)
        expect(page.locator("#skin-local-empty")).to_be_visible(); selected_default(page)
        check(page.evaluate("document.querySelectorAll('[data-local-skin],.fc-skin-image').length===0"),
              "Deleting the active pack must remove all custom variables, targets and material layers")
        check(page.evaluate("!document.documentElement.style.getPropertyValue('--fc-background') && !document.documentElement.style.getPropertyValue('--ime-deck')"),
              "Returning to default must restore CSS inheritance rather than retain local token overrides")


def failure_preserves_confirmed_state(browser):
    with app(browser,installed=True,active=True) as page:
        page.evaluate("window.__aiFixture.skin.failure='raw provider text fixture-secret'")
        page.locator("#skin-use-default").click();wait_idle(page)
        expect(page.locator("#skin-use-default")).to_have_attribute("aria-pressed","false")
        expect(page.locator("#skin-library-error")).to_contain_text("原皮肤与列表已保留")
        expect(page.locator("#skin-library-error")).not_to_contain_text("fixture-secret")
        expect(page.locator("html")).to_have_attribute("data-local-skin","fixture.green")


def cas_requires_explicit_reload(browser):
    with app(browser, installed=True) as page:
        page.evaluate("window.__aiFixture.skin.rotate()")
        installed_button(page).click(); wait_idle(page)
        expect(page.locator("#skin-library-error")).to_have_text("皮肤列表已更新，请重新读取后重试")
        expect(installed_button(page)).to_be_disabled()
        selected_default(page)
        page.locator("#skin-library-reload").click(); wait_idle(page)
        expect(page.locator("#skin-library-error")).to_be_hidden()
        expect(installed_button(page)).to_be_enabled()


def pending_mutation_freezes_library(browser):
    with app(browser, installed=True) as page:
        management(page); page.evaluate("window.__aiFixture.skin.defer=true")
        installed_button(page).click()
        page.wait_for_function("window.__aiFixture.skin.pending.length===1")
        for selector in ("#skin-import", "#skin-import-enabled", "#skin-library-reload", "#skin-use-default", "#skin-local-grid button"):
            for control in page.locator(selector).all():
                expect(control).to_be_disabled()
        page.evaluate("window.__aiFixture.skin.finish()"); wait_idle(page)
        expect(page.locator("html")).to_have_attribute("data-local-skin", "fixture.green")


def delayed_snapshot_does_not_restore_stale_pack(browser):
    with app(browser, installed=True, active=True) as page:
        page.evaluate("window.__aiFixture.skin.deferReads=true;window.__aiFixture.emit('skin-library://changed',{})")
        page.wait_for_function("window.__aiFixture.skin.reads.length===1")
        page.evaluate("""() => {
          const f=window.__aiFixture;f.settings.skin='system_default';f.settings.customSkinId='';f.settings.customSkinHash='';
          f.skin.rotate();f.emit('settings://changed',f.settings);f.emit('skin-library://changed',{});
        }""")
        page.wait_for_function("document.documentElement.dataset.localSkin===undefined")
        page.evaluate("window.__aiFixture.skin.deferReads=false;window.__aiFixture.skin.finishRead()")
        wait_idle(page)
        selected_default(page)
        check(page.evaluate("window.__aiFixture.count('skin_library_snapshot')>=3 && document.querySelectorAll('.fc-skin-image').length===0"),
              "A change received during a delayed read must trigger a fresh snapshot, never revive stale material")


def user_mode_changes_and_default_revert(browser):
    with app(browser, installed=True, active=True) as page:
        for tone, palette in (("dark", DARK), ("light", LIGHT)):
            page.locator(f'#mode-group [data-mode="{tone}"]').click()
            page.wait_for_function("color=>document.documentElement.style.getPropertyValue('--fc-background')===color", arg=palette["background"])
            expect(installed_button(page)).to_have_attribute("aria-pressed", "true")
        page.locator('#skin-use-default').click();wait_idle(page)
        selected_default(page)
        check(page.evaluate("document.querySelectorAll('[data-local-skin],.fc-skin-image').length===0"),
              "Selecting default must clear custom material without deleting the installed pack")
        expect(installed_button(page)).to_be_visible()
        expect(page.locator('#mode-group [data-mode="light"]')).to_have_attribute("aria-pressed", "true")


def colors_images_modes(browser, base, tone):
    with app(browser, installed=True, active=True, base=base, tone=tone) as page:
        palette = DARK if tone == "dark" else LIGHT
        check(page.evaluate("document.documentElement.style.getPropertyValue('--fc-background')") == palette["background"], "Local shell must use the current explicit light/dark palette")
        check(page.evaluate("[...document.querySelectorAll('.fc-skin-image')].every(el=>!el.style.cssText.includes('/skin-materials/'))"), "Local material layers must not reuse old builtin photographs")
        roles = page.evaluate("[...new Set([...document.querySelectorAll('.fc-skin-image')].map(el=>el.dataset.role))].sort()")
        check(roles == sorted(ROLES), "Every supported material role must reach a real component")
        for mode_name, values in (("light", LIGHT), ("dark", DARK)):
            preview = page.locator(f'.dual-cand-showcase .cand-window[data-ime-mode="{mode_name}"]')
            check(preview.evaluate("el=>el.style.getPropertyValue('--fc-candidate-background')") == values["candidateBackground"], "Both candidate showcases must use their own mode rather than inherit the shell tone")
            check(preview.evaluate("el=>getComputedStyle(el).backgroundImage") == "none", "Omitted builtin wood/paper/CRT backgrounds must be suppressed under local materials")
            check(preview.locator(":scope > .fc-skin-image").get_attribute("data-role") == "candidates", "The candidate texture must apply beneath text and selection")
        if base == "mechanical":
            page.screenshot(path=str(OUT / f"skin-main-{tone}.png"), animations="disabled")


def scale_roles_and_tiny_nineslice(browser):
    with app(browser, installed=True, active=True) as page:
        page.wait_for_function("document.querySelector('.fc-skin-image[data-role=deck]')?.style.getPropertyValue('--fc-natural-size')")
        check(page.locator('.fc-skin-image[data-role="deck"]').first.evaluate("el=>el.style.backgroundRepeat") == "repeat", "Tile images must repeat at assetScale-adjusted natural size")
        check(page.locator('.fc-skin-image[data-role="toolbar"]').first.evaluate("el=>el.style.backgroundSize") == "contain", "Contain materials must preserve image aspect ratio")
        check(page.locator('.fc-skin-image[data-role="code"]').first.evaluate("el=>el.style.backgroundSize") == "100% 100%", "Stretch materials must fill the component")
        tab(page, "effects")
        target = page.locator("#first-minus")
        target.evaluate("el=>Object.assign(el.style,{width:'2px',height:'2px',minWidth:'0px',minHeight:'0px',padding:'0',borderWidth:'0'})")
        page.wait_for_timeout(80)
        check(target.evaluate("el=>{const layer=el.querySelector('.fc-skin-image');const values=getComputedStyle(layer).borderImageWidth.split(' ').map(parseFloat);return values.every(v=>v<=1.01)&&layer.style.borderImageSlice.includes('fill');}"),
              "Nine-slice edges must shrink together when the target is smaller than its source edges")


def colors_only_no_old_textures(browser):
    with app(browser, installed=True, active=True, base="typewriter", images=False) as page:
        check(page.locator(".fc-skin-image").count() == 0, "Missing image roles must be drawn using this pack's colors")
        check(page.locator(".cand-window").first.evaluate("el=>getComputedStyle(el).backgroundImage") == "none", "Color-only packs must not reveal a builtin paper material")
        expect(page.locator("#first-val")).to_have_text("20 px")


def ai_mutations_keep_textures_without_stream_churn(browser):
    with app(browser, installed=True, active=True) as page:
        tab(page, "ai"); page.locator('[data-ai-mode="write"]').click()
        expect(page.locator("#ai-generate > .fc-skin-image")).to_have_attribute("data-role", "keyAction")
        page.evaluate("window.__stableSkinLayer=document.querySelector('#ai-generate > .fc-skin-image'); for(let i=0;i<100;i++)document.querySelector('#ai-result').textContent='示例逐字输出'+i")
        page.wait_for_timeout(80)
        check(page.evaluate("window.__stableSkinLayer.isConnected"), "Streaming output updates must not rebuild unrelated skin textures")


def other_windows(browser, window_mode, tone):
    with app(browser, installed=True, active=True, window_mode=window_mode, tone=tone, width=340 if window_mode == "clipboard" else 218, height=450 if window_mode == "clipboard" else 32) as page:
        selector = ".altv-popup" if window_mode == "clipboard" else ".floating-statusbar"
        check(page.locator(selector + " > .fc-skin-image").count() == 1, "Independent companion windows must use the same installed pack materials")
        check(page.evaluate("window.__aiFixture.count('skin_select')===0 && window.__aiFixture.count('skin_import_pick')===0"), "Read-only windows must not mutate skin preferences on startup")
        page.screenshot(path=str(OUT / f"skin-{window_mode}-{tone}.png"), animations="disabled")


def minimum_window(browser, tone):
    with app(browser, installed=True, active=True, name="非常长的本地皮肤名字用于验证小窗口不会挤出边界", tone=tone, width=760, height=600) as page:
        installed_button(page).scroll_into_view_if_needed()
        check(page.evaluate("document.documentElement.scrollWidth<=innerWidth && [...document.querySelectorAll('#skin-local-grid button,#skin-use-default,#skin-import')].filter(el=>el.getClientRects().length).every(el=>el.getBoundingClientRect().right<=innerWidth+1)"), "Skin management controls must fit the minimum companion window")
        page.screenshot(path=str(OUT / f"skin-minimum-{tone}.png"), animations="disabled")


def run_case(name, operation):
    start = time.monotonic()
    try:
        operation(); row = {"name": name, "passed": True, "seconds": round(time.monotonic() - start, 3)}
    except Exception:
        row = {"name": name, "passed": False, "seconds": round(time.monotonic() - start, 3), "error": traceback.format_exc().replace("fixture-secret", "[sample redacted]")}
    RESULTS.append(row); print(("PASS " if row["passed"] else "FAIL ") + name, flush=True)
    if not row["passed"]:
        print(row["error"], flush=True)


def main():
    global BASE, OUT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", default=BASE)
    parser.add_argument("--output", type=Path, default=OUT)
    parser.add_argument("--browser-executable", default=shutil.which("chromium") or shutil.which("chromium-browser"))
    args = parser.parse_args(); url = urlsplit(args.base_url)
    if url.scheme != "http" or url.hostname not in ("127.0.0.1", "localhost", "::1") or url.username or url.password or url.query or url.fragment:
        parser.error("Only a loopback Vite HTTP URL is allowed")
    BASE = args.base_url.rstrip("/") + "/"; OUT = args.output.resolve(); OUT.mkdir(parents=True, exist_ok=True)
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(executable_path=args.browser_executable, headless=True, args=["--no-sandbox", "--disable-dev-shm-usage"] if sys.platform.startswith("linux") else [])
        cases = [("system_default_without_bundled_catalog", system_defaults_only), ("reload_cannot_restore_bundled_skins", reload_does_not_restore_bundled_skins),
                 ("explicit_default_preserves_other_preferences", default_preserves_preferences), ("picker_cancel_and_import_no_autoapply", import_cancel_and_no_autoapply),
                 ("pack_names_are_text_nodes", escaped_pack_labels), ("import_disabled_existing_pack_usable", disabled_import_keeps_installed_usable),
                 ("delete_active_reverts_and_clears_custom_materials", delete_active_clears_all_materials), ("failure_keeps_confirmed_state", failure_preserves_confirmed_state),
                 ("cas_requires_explicit_reload", cas_requires_explicit_reload), ("pending_mutation_locks_library_controls", pending_mutation_freezes_library),
                 ("delayed_read_cannot_restore_stale_pack", delayed_snapshot_does_not_restore_stale_pack), ("user_mode_changes_and_default_revert", user_mode_changes_and_default_revert),
                 ("four_image_scales_and_small_nineslice", scale_roles_and_tiny_nineslice), ("color_only_pack_has_no_builtin_material", colors_only_no_old_textures),
                 ("ai_button_updates_keep_textures_without_stream_churn", ai_mutations_keep_textures_without_stream_churn)]
        for name, operation in cases:
            run_case(name, lambda operation=operation: operation(browser))
        for base in BASES:
            for tone in ("light", "dark"):
                run_case("palette_materials_" + base + "_" + tone, lambda base=base, tone=tone: colors_images_modes(browser, base, tone))
        for window_mode in ("clipboard", "statusbar"):
            for tone in ("light", "dark"):
                run_case("independent_" + window_mode + "_" + tone, lambda window_mode=window_mode, tone=tone: other_windows(browser, window_mode, tone))
        for tone in ("light", "dark"):
            run_case("minimum_window_" + tone, lambda tone=tone: minimum_window(browser, tone))
        browser.close()
    run_case("no_external_requests", lambda: check(not BLOCKED, "Frontend attempted an external request outside controlled fixture resources"))
    report = {"checkedAt": datetime.now(timezone.utc).isoformat(), "platform": "Linux Chromium; real frontend with deterministic mock IPC/PNG only, not Windows or native acceptance", "tests": len(RESULTS), "failures": sum(not row["passed"] for row in RESULTS), "blockedOrigins": sorted(set(BLOCKED)), "results": RESULTS}
    (OUT / "skin-ui-regression.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    suite = ET.Element("testsuite", name="SkinLibraryUi", tests=str(report["tests"]), failures=str(report["failures"]), errors="0", skipped="0")
    for row in RESULTS:
        case = ET.SubElement(suite, "testcase", classname="SkinLibraryUi", name=row["name"], time=str(row["seconds"]))
        if not row["passed"]:
            ET.SubElement(case, "failure").text = row["error"]
    ET.ElementTree(suite).write(OUT / "skin-ui-regression.xml", encoding="utf-8", xml_declaration=True)
    (OUT / "skin-ui-regression.log").write_text("\n".join(("PASS " if row["passed"] else "FAIL ") + row["name"] + ("\n" + row["error"] if not row["passed"] else "") for row in RESULTS) + "\n")
    print(json.dumps({key: report[key] for key in ("tests", "failures", "platform")}, ensure_ascii=False), flush=True)
    raise SystemExit(bool(report["failures"]))


if __name__ == "__main__":
    main()
