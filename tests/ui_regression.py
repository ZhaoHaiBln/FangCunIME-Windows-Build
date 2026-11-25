"""Black-box Windows companion frontend checks with a deterministic Tauri IPC fixture.

Runs the real Vite/TypeScript frontend in Linux Chromium. It does not run Windows
WebView2, the Rust backend, TSF, real clipboard contents, or a remote relay.
"""
from __future__ import annotations

import argparse
import json
import shutil
import sys
import tempfile
import time
import traceback
from contextlib import contextmanager
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import urlsplit
from xml.etree import ElementTree as ET

from playwright.sync_api import expect, sync_playwright


OUT = Path(tempfile.gettempdir()) / "fangcun-windows-ui-regression"
BASE = "http://127.0.0.1:5174/"
SKINS = ("wood_type", "typewriter", "terminal", "brutal")
SETTINGS = {
    "scheme": "flypy", "mixedInput": True, "chinesePunctuation": True,
    "shiftToggleEnglish": True, "capsLockBypass": True, "pageSize": 7,
    "wheelPaging": True, "skin": "wood_type", "themeMode": "system",
    "darkMode": False, "systemDark": False, "firstCandidateSize": 20,
    "keyOpacity": .82, "typingGlow": True, "keyLetterSize": 18,
    "keyLetterBold": False, "clipboardEnabled": True,
    "candidateLayout": "double", "fullWidth": False, "traditional": False,
}
ITEMS = [
    {"id": "local-1", "text": "今天也有好心情", "pinned": False,
     "createdAtMs": 1795480000000, "source": "fixture"},
    {"id": "local-2", "text": "晚上一起吃饭，然后去散步。\n这里是一段较长的剪贴板预览文字，用来检查窄窗口里的换行和操作按钮。",
     "pinned": True, "createdAtMs": 1795479900000, "source": "fixture"},
    {"id": "local-3", "text": '<img src="fixture.invalid" onerror="window.__fixture.injected=true"> 文字按原样显示',
     "pinned": False, "createdAtMs": 1795479800000, "source": "fixture"},
]

MOCK = r"""
(() => {
  const initial = __FIXTURE_CONFIG__;
  const clone = value => value === undefined ? null : JSON.parse(JSON.stringify(value));
  let serial = 0;
  const callbacks = new Map();
  const listeners = new Map();
  const fixture = {
    settings: clone(initial.settings), items: clone(initial.items), enabled: true,
    calls: [], failures: initial.failures || {}, deferSaves: false, pendingSaves: [],
    injected: false,
    share: {
      revision: 'fixture-route-1', viewRevision: '1',
      server: 'https://clipboard.example.invalid', topic: 'fixture-topic',
      senderName: '我的电脑', enabled: true, autoSend: false,
      tokenSaved: true, keySaved: true, keyId: '1111111111111111',
      status: '已连接 · 示例设备', error: null, gap: false, needsReset: false,
      items: [{id:'shared-1',senderId:'phone',senderName:'我的手机',
        createdAt:1795480000000,receivedAt:1795480000100,
        expiresAt:1795566400000,text:'来自手机的共享文字'}]
    },
    count(command) {return this.calls.filter(call => call.command === command).length;},
    fail(command, count=1) {this.failures[command] = count;},
    emit(event, payload) {
      for (const id of listeners.get(event) || []) {
        callbacks.get(id)?.({event, id, payload:clone(payload)});
      }
    },
    finishSave(success=true) {
      const request = this.pendingSaves.shift();
      if (!request) throw new Error('No pending save in fixture');
      if (!success) {request.reject('FIXTURE_SAVE_DENIED'); return;}
      this.settings = request.command === 'patch_settings'
        ? {...this.settings, ...clone(request.args.patch)} : clone(request.args.settings);
      this.enabled = this.settings.clipboardEnabled;
      this.emit('settings://changed', this.settings);
      request.resolve(clone(this.settings));
    }
  };
  window.__fixture = fixture;
  globalThis.isTauri = true;
  window.__TAURI_INTERNALS__ = {
    metadata: {currentWindow:{label:initial.mode}, currentWebview:{label:initial.mode}},
    transformCallback(callback) {const id = ++serial; callbacks.set(id, callback); return id;},
    unregisterCallback(id) {callbacks.delete(id);},
    async invoke(command, args={}) {
      fixture.calls.push({command,args:clone(args)});
      if (fixture.failures[command] > 0) {
        fixture.failures[command]--; throw 'FIXTURE_COMMAND_DENIED';
      }
      if (command === 'plugin:event|listen') {
        const ids = listeners.get(args.event) || [];
        ids.push(args.handler); listeners.set(args.event, ids); return ++serial;
      }
      if (command === 'plugin:event|unlisten') return null;
      if (command === 'load_settings') return clone(fixture.settings);
      if (command === 'save_settings' || command === 'patch_settings') {
        if (fixture.deferSaves) {
          return await new Promise((resolve,reject) => fixture.pendingSaves.push({command,args:clone(args),resolve,reject}));
        }
        fixture.settings = command === 'patch_settings'
          ? {...fixture.settings, ...clone(args.patch)} : clone(args.settings);
        fixture.enabled = fixture.settings.clipboardEnabled;
        fixture.emit('settings://changed', fixture.settings);
        return clone(fixture.settings);
      }
      if (command === 'reset_settings') {
        fixture.settings = clone(initial.settings); return clone(fixture.settings);
      }
      if (command === 'list_clipboard') return clone(fixture.items);
      if (command === 'get_clipboard_enabled') return fixture.enabled;
      if (command === 'clipboard_persistence_status') return null;
      if (command === 'set_clipboard_enabled') {fixture.enabled=args.enabled; fixture.settings.clipboardEnabled=args.enabled; fixture.emit('clipboard://enabled-changed',fixture.enabled); fixture.emit('settings://changed',fixture.settings); return fixture.enabled;}
      if (command === 'copy_clipboard_item') return clone(fixture.items);
      if (command === 'toggle_pin_clipboard') {
        fixture.items=fixture.items.map(item => item.id===args.id ? {...item,pinned:!item.pinned}:item);
        return clone(fixture.items);
      }
      if (command === 'delete_clipboard_item') {
        fixture.items=fixture.items.filter(item=>item.id!==args.id); return clone(fixture.items);
      }
      if (command === 'clear_clipboard') {
        fixture.items=fixture.items.filter(item=>args.keepPinned && item.pinned); return clone(fixture.items);
      }
      if (command === 'clipboard_share_snapshot') return clone(fixture.share);
      if (command === 'clipboard_share_remove') {
        fixture.share.items=fixture.share.items.filter(item=>item.id!==args.id);
        fixture.share.viewRevision=String(Number(fixture.share.viewRevision)+1);
        return clone(fixture.share);
      }
      if (command === 'clipboard_share_copy' || command === 'clipboard_share_send_item') return null;
      if (['hide_window','hide_clipboard_window','open_settings_tab','set_english_mode','toggle_statusbar'].includes(command)) return null;
      throw 'UNHANDLED_FIXTURE_COMMAND: '+command;
    }
  };
})();
"""


RESULTS = []
UNEXPECTED_NETWORK = []


def check(condition, message):
    if not condition:
        raise AssertionError(message)


@contextmanager
def app(browser, *, skin="wood_type", mode="system", system_dark=False,
        window="main", width=940, height=760, reduced=True, failures=None,
        native=True):
    settings = {**SETTINGS, "skin": skin, "themeMode": mode, "systemDark": system_dark}
    context = browser.new_context(viewport={"width": width, "height": height},
                                  device_scale_factor=1,
                                  reduced_motion="reduce" if reduced else "no-preference")
    if native:
        data = dict(settings=settings, items=ITEMS, failures=failures or {}, mode=window)
        context.add_init_script(MOCK.replace("__FIXTURE_CONFIG__", json.dumps(data, ensure_ascii=False)))
    def allow_local(route):
        if route.request.url.startswith(BASE):
            route.continue_()
        else:
            UNEXPECTED_NETWORK.append(route.request.url)
            route.abort()
    context.route("**/*", allow_local)
    page = context.new_page()
    page.set_default_timeout(6000)
    try:
        page.goto(BASE + ("" if window == "main" else "?mode=" + window), wait_until="networkidle")
        if native:
            page.wait_for_function("window.__fixture.count('load_settings') >= 1 && window.__fixture.count('clipboard_share_snapshot') === 1")
            page.wait_for_timeout(50)
        yield page
    finally:
        context.close()


def count(page, command):
    return page.evaluate("command => window.__fixture.count(command)", command)


def wait_count(page, command, total):
    page.wait_for_function("([command,total]) => window.__fixture.count(command) === total", arg=[command,total])


def tab(page, name):
    page.locator(f'[data-tab="{name}"]').click()
    pane=page.locator(f'[data-pane="{name}"]')
    expect(pane).to_be_visible()
    # Measure physical press only after the independent page-entry animation ends.
    pane.evaluate("async el => { await Promise.all(el.getAnimations().filter(a => a.effect?.getTiming().iterations !== Infinity).map(a => a.finished.catch(() => {}))); }")


def shot(page, name):
    page.screenshot(path=str(OUT / (name + ".png")), animations="disabled")


def assert_same_bounds(a, b, label):
    check(a is not None and b is not None, label + " requires nonempty bounds")
    for key in ("x", "y", "width", "height"):
        check(abs(a[key] - b[key]) <= .05, f"{label}: {key} moved {a[key]} -> {b[key]}")


def no_horizontal_overflow(page):
    check(page.evaluate("document.documentElement.scrollWidth <= innerWidth"), "Page has horizontal overflow")
    check(page.evaluate("document.body.scrollWidth <= innerWidth"), "Body has horizontal overflow")


def appearance(browser, skin, mode):
    with app(browser, skin=skin, mode=mode) as page:
        check(page.locator("html").get_attribute("data-ime-theme") == skin, "Skin not applied")
        check(page.locator("html").get_attribute("data-ime-mode") == mode, "Light/dark not applied")
        no_horizontal_overflow(page)
        tab(page, "theme")
        expect(page.locator(f'button[data-skin="{skin}"]')).to_have_attribute("aria-pressed", "true")
        expect(page.locator(f'[data-mode="{mode}"]')).to_have_attribute("aria-pressed", "true")
        check(page.locator(".cand-window").count() == 3, "All candidate previews must remain")
        shot(page, f"theme-{skin}-{mode}")
        tab(page, "clipboard")
        expect(page.locator("#clip-list .clip-card-main")).to_have_count(3)
        check(page.locator("#clip-list .clip-card-text img").count() == 0, "Clipboard text must remain inert")
        for button in page.locator("#clip-list .clip-icon-btn").all():
            box=button.bounding_box()
            check(box and box["width"] >= 28 and box["height"] >= 28, "Icon action target is too small")
        shot(page, f"clipboard-{skin}-{mode}")


def skin_preserves_mode(browser):
    with app(browser, mode="system", system_dark=True) as page:
        tab(page, "theme")
        for mode in ("system", "light", "dark"):
            page.locator(f'[data-mode="{mode}"]').click()
            for skin in SKINS:
                page.locator(f'button[data-skin="{skin}"]').click()
                page.wait_for_function("([skin,mode]) => window.__fixture.settings.skin===skin && window.__fixture.settings.themeMode===mode", arg=[skin,mode])
                expect(page.locator(f'[data-mode="{mode}"]')).to_have_attribute("aria-pressed", "true")
                actual = page.locator("html").get_attribute("data-ime-mode")
                check(actual == ("light" if mode == "light" else "dark"), "Skin changed effective mode")


def keyboard_and_action_separation(browser):
    with app(browser) as page:
        tab(page, "clipboard")
        page.locator("#clip-search").focus()
        page.keyboard.press("Tab")
        expect(page.locator("#clip-list .clip-card-main").first).to_be_focused()
        page.keyboard.press("Enter")
        wait_count(page,"copy_clipboard_item",1)
        page.locator("#clip-search").focus();page.keyboard.press("Tab");page.keyboard.press("Space")
        wait_count(page,"copy_clipboard_item",2)
        first=page.locator("#clip-list .clip-card-item").first
        pin=first.get_by_role("button",name="置顶该条",exact=True)
        pin.click()
        wait_count(page,"toggle_pin_clipboard",1)
        expect(page.locator("#clip-list .clip-card-item").first.get_by_role("button",name="取消置顶",exact=True)).to_have_attribute("aria-pressed","true")
        page.locator("#clip-list .clip-card-item").nth(2).get_by_role("button",name="删除剪贴板记录",exact=True).click()
        wait_count(page,"delete_clipboard_item",1)
        expect(page.locator("#clip-list .clip-card-main")).to_have_count(2)
        check(count(page,"copy_clipboard_item")==2,"Pin/delete bubbled into copy")
        check(page.locator("#clip-list button button").count()==0,"Nested interactive buttons")
        page.locator("#clip-shared-tab").click()
        shared=page.locator("#share-list .clip-card-main")
        expect(shared).to_have_count(1)
        check(shared.evaluate("el=>el.tagName")=="BUTTON","Shared copy must be native button")
        shared.focus();page.keyboard.press("Enter")
        wait_count(page,"clipboard_share_copy",1)
        page.locator("#share-list").get_by_role("button",name="删除本机共享记录",exact=True).click()
        wait_count(page,"clipboard_share_remove",1)
        check(count(page,"clipboard_share_copy")==1,"Shared delete bubbled into copy")


def clipboard_rejections(browser):
    with app(browser) as page:
        tab(page,"clipboard")
        first=page.locator("#clip-list .clip-card-item").first
        for command,label,message in [
            ("toggle_pin_clipboard","置顶该条","置顶操作失败"),
            ("delete_clipboard_item","删除剪贴板记录","删除失败"),
        ]:
            page.evaluate("command=>window.__fixture.fail(command)",command)
            first.get_by_role("button",name=label,exact=True).click()
            expect(page.locator("#toast")).to_contain_text(message)
            expect(page.locator("#clip-list .clip-card-main")).to_have_count(3)
            expect(first.get_by_role("button",name="置顶该条",exact=True)).to_have_attribute("aria-pressed","false")
        page.evaluate("window.__fixture.fail('set_clipboard_enabled')")
        page.locator("#clip-toggle-watch").click()
        expect(page.locator("#toast")).to_contain_text("监听设置失败")
        expect(page.locator("#clip-watch-text")).to_have_text("监听中")
        page.evaluate("window.__fixture.fail('clear_clipboard')")
        page.locator("#clip-clear").click()
        expect(page.locator("#toast")).to_contain_text("清空失败")
        expect(page.locator("#clip-list .clip-card-main")).to_have_count(3)
        check(count(page,"copy_clipboard_item")==0,"Action rejection must not copy")
        shot(page,"clipboard-rejected-action")


def altv_copy_rejection_and_success(browser):
    with app(browser,window="clipboard",width=340,height=450) as page:
        no_horizontal_overflow(page)
        popup=page.locator(".altv-popup")
        expect(popup).to_be_visible()
        page.evaluate("window.__fixture.fail('copy_clipboard_item')")
        main=page.locator("#altv-list .clip-card-main").first
        main.click()
        expect(page.locator("#toast")).to_contain_text("复制失败")
        check(count(page,"hide_clipboard_window")==0,"Rejected copy must not dismiss AltV")
        expect(page.locator("#altv-list .clip-card-main")).to_have_count(3)
        expect(popup).to_be_visible()
        shot(page,"altv-copy-rejected")
        main.focus();page.keyboard.press("Enter")
        wait_count(page,"copy_clipboard_item",2)
        wait_count(page,"hide_clipboard_window",1)
        shot(page,"altv-copy-success")


def serial_saves_and_stale_event(browser):
    with app(browser) as page:
        page.evaluate("window.__fixture.deferSaves=true")
        page.locator("#sw-mixed").uncheck()
        wait_count(page,"patch_settings",1)
        page.locator("#sw-punct").uncheck()
        page.locator("#sel-page-size").select_option("9")
        check(count(page,"patch_settings")==1,"Rapid saves must be serialized")
        expect(page.locator("#sidebar-status-text")).to_contain_text("正在保存")
        page.evaluate("window.__fixture.emit('settings://changed', window.__fixture.settings)")
        expect(page.locator("#sw-mixed")).not_to_be_checked()
        expect(page.locator("#sw-punct")).not_to_be_checked()
        expect(page.locator("#sel-page-size")).to_have_value("9")
        for _ in range(6):
            if page.locator("#sidebar-status-text").inner_text() == "设置已保存":
                break
            page.wait_for_function("window.__fixture.pendingSaves.length===1")
            check(page.evaluate("window.__fixture.pendingSaves.length")==1,"Only one IPC save may be in-flight")
            page.evaluate("window.__fixture.finishSave(true)")
            page.wait_for_function("window.__fixture.pendingSaves.length===1 || document.getElementById('sidebar-status-text').textContent==='设置已保存'")
            expect(page.locator("#sw-punct")).not_to_be_checked()
            expect(page.locator("#sel-page-size")).to_have_value("9")
        expect(page.locator("#sidebar-status-text")).to_have_text("设置已保存")
        check(count(page,"patch_settings")>=2,"Follow-up changed values were not persisted")
        settings=page.evaluate("window.__fixture.settings")
        check(not settings["mixedInput"] and not settings["chinesePunctuation"] and settings["pageSize"]==9,"Rapid saves lost changed fields")
        check(settings["skin"]=="wood_type" and settings["themeMode"]=="system" and settings["shiftToggleEnglish"],"Rapid saves changed unrelated fields")
        expect(page.locator("#sw-punct")).not_to_be_checked()
        expect(page.locator("#sel-page-size")).to_have_value("9")


def save_rejection(browser):
    with app(browser,mode="dark") as page:
        tab(page,"theme")
        page.evaluate("window.__fixture.fail('patch_settings')")
        page.locator('button[data-skin="terminal"]').click()
        expect(page.locator("#sidebar-status-text")).to_contain_text("保存失败")
        page.wait_for_function("document.documentElement.dataset.imeTheme==='wood_type'")
        check(page.evaluate("window.__fixture.settings.skin")=="wood_type","Rejected theme must not be committed")
        check(page.locator("html").get_attribute("data-ime-mode")=="dark","Rejected save reset mode")
        check("已切换" not in page.locator("#toast").inner_text(),"Failed save reported success")
        shot(page,"settings-save-rejected")


def initial_load_rejection(browser):
    with app(browser, failures={"load_settings":100}) as page:
        switch = page.locator("#sw-mixed")
        if switch.is_enabled():
            switch.click()
        else:
            expect(switch).to_be_disabled()
        page.wait_for_timeout(80)
        check(count(page,"patch_settings")==0,"Failed load must not write defaults over existing settings")
        expect(page.locator("#sidebar-status-text")).to_contain_text("失败")
        check(count(page,"hide_window")==0,"Failed load must not hide window")


def patches_preserve_other_window_fields(browser):
    with app(browser) as page:
        page.evaluate("window.__fixture.deferSaves=true")
        page.locator("#sw-mixed").uncheck()
        wait_count(page,"patch_settings",1)
        check(page.evaluate("Object.keys(window.__fixture.pendingSaves[0].args.patch).join(',')") == "mixedInput",
              "One changed control must send only its owned field")
        page.evaluate("""() => {
          const f=window.__fixture;
          f.settings.skin='terminal'; f.settings.themeMode='dark';
          f.settings.clipboardEnabled=false; f.enabled=false;
          f.emit('settings://changed',f.settings);
          f.emit('clipboard://enabled-changed',true); // delayed older boolean payload
          f.finishSave(true);
        }""")
        expect(page.locator("#sidebar-status-text")).to_have_text("设置已保存")
        settings=page.evaluate("window.__fixture.settings")
        check(not settings["mixedInput"] and settings["skin"]=="terminal" and settings["themeMode"]=="dark",
              "A patch must retain another window's skin/mode changes")
        check(not settings["clipboardEnabled"],"A patch must preserve the committed clipboard preference")
        check(count(page,"save_settings")==0,"The UI must not fall back to a stale whole-settings write")
        tab(page,"clipboard")
        expect(page.locator("#clip-watch-text")).to_have_text("已暂停")


def clipboard_events_reload_committed_state(browser):
    with app(browser) as page:
        tab(page,"clipboard")
        page.evaluate("""() => {
          const f=window.__fixture;
          f.enabled=false; f.settings.clipboardEnabled=false;
          f.emit('clipboard://enabled-changed',true);
        }""")
        expect(page.locator("#clip-watch-text")).to_have_text("已暂停")
        page.evaluate("window.__fixture.emit('clipboard://persistence-error','<private filesystem path>')")
        expect(page.locator("#toast")).to_contain_text("历史保存失败")
        check("private filesystem" not in page.locator("#toast").inner_text(),"Persistence error must not expose raw payload")
        page.evaluate("window.__fixture.fail('set_clipboard_enabled')")
        page.locator("#clip-toggle-watch").click()
        expect(page.locator("#toast")).to_contain_text("监听设置失败")
        check(not page.evaluate("window.__fixture.settings.clipboardEnabled"),"Rejected native save must retain preference")
        expect(page.locator("#clip-watch-text")).to_have_text("已暂停")


def press_feedback_fixed_bounds(browser):
    with app(browser,reduced=False) as page:
        button=page.locator("#btn-close")
        before=button.bounding_box()
        check(before and before["width"]>0 and before["height"]>0,"Need real finish button bounds")
        page.mouse.move(before["x"]+before["width"]/2,before["y"]+before["height"]/2)
        page.wait_for_timeout(180)
        idle=button.evaluate("el=>({shadow:getComputedStyle(el).boxShadow,background:getComputedStyle(el).background,transform:getComputedStyle(el).transform})")
        page.mouse.down();page.wait_for_timeout(180)
        pressed=button.evaluate("el=>({shadow:getComputedStyle(el).boxShadow,background:getComputedStyle(el).background,transform:getComputedStyle(el).transform})")
        assert_same_bounds(before,button.bounding_box(),"Finish press")
        check(idle!=pressed,"Finish button has no visible press feedback")
        check(count(page,"hide_window")==0,"Mouse down must not complete action")
        shot(page,"finish-pressed")
        page.mouse.up()
        wait_count(page,"hide_window",1)
        assert_same_bounds(before,button.bounding_box(),"Finish release")
    with app(browser,reduced=False) as page:
        tab(page,"clipboard")
        button=page.locator("#clip-list .clip-card-main").first
        before=button.bounding_box()
        page.mouse.move(before["x"]+before["width"]/2,before["y"]+before["height"]/2)
        page.wait_for_timeout(180)
        idle=button.evaluate("el=>getComputedStyle(el).boxShadow")
        page.mouse.down();page.wait_for_timeout(180)
        pressed=button.evaluate("el=>getComputedStyle(el).boxShadow")
        assert_same_bounds(before,button.bounding_box(),"Copy press")
        check(idle!=pressed,"Copy button has no visible press feedback")
        check(count(page,"copy_clipboard_item")==0,"Copy must wait for pointer release")
        shot(page,"clipboard-copy-pressed")
        page.mouse.up();wait_count(page,"copy_clipboard_item",1)


def minimum_window_and_keyboard_focus(browser):
    with app(browser,width=760,height=600) as page:
        for pane in ("general","input","theme","effects","clipboard","ai","about"):
            tab(page,pane);no_horizontal_overflow(page)
            close=page.locator("#btn-close").bounding_box()
            check(close and close["x"]>=0 and close["y"]>=0 and close["x"]+close["width"]<=760 and close["y"]+close["height"]<=600,"Fixed action not reachable in minimum window")
        tab(page,"clipboard")
        page.locator("#clip-search").focus();page.keyboard.press("Tab")
        first=page.locator("#clip-list .clip-card-main").first
        expect(first).to_be_focused()
        focus=first.evaluate("el=>({width:getComputedStyle(el).outlineWidth,style:getComputedStyle(el).outlineStyle})")
        check(float(focus["width"].replace("px",""))>=1 and focus["style"]!="none","Keyboard focus must be visible")
        shot(page,"minimum-window-keyboard-focus")
    with app(browser,window="statusbar",width=218,height=32) as page:
        no_horizontal_overflow(page)
        for button in page.locator("#floating-statusbar button").all():
            box=button.bounding_box()
            check(box and box["x"]>=0 and box["y"]>=0 and box["x"]+box["width"]<=218.1 and box["y"]+box["height"]<=32.1,"Status action exceeds native bounds")
        shot(page,"statusbar-compact")


def reduced_motion_and_preview_mode(browser):
    with app(browser) as page:
        button=page.locator("#btn-close")
        durations=button.evaluate("el=>getComputedStyle(el).transitionDuration.split(',').map(Number.parseFloat)")
        check(max(durations)<=.011,"Reduced motion must suppress transition duration")
        page.locator("#preview-card").click()
        check(page.locator(".glow-pulse").count()==0,"Reduced motion must not spawn glow pulses")
    with app(browser,native=False) as page:
        expect(page.locator("#sidebar-status-text")).to_contain_text("界面预览")
        check("实时同步" not in page.locator("#sidebar-status-text").inner_text(),"Standalone preview must not claim native sync")
        tab(page,"theme");page.locator('button[data-skin="brutal"]').click()
        check(page.locator("html").get_attribute("data-ime-theme")=="brutal","Standalone local theme preview stopped working")
        expect(page.locator("#sidebar-status-text")).to_contain_text("界面预览")


def run_case(name, operation):
    started=time.monotonic()
    try:
        operation()
        RESULTS.append(dict(name=name,passed=True,seconds=round(time.monotonic()-started,3)))
        print("PASS",name,flush=True)
    except Exception:
        detail=traceback.format_exc()
        RESULTS.append(dict(name=name,passed=False,seconds=round(time.monotonic()-started,3),error=detail))
        print("FAIL",name,detail,flush=True)


def main():
    global BASE, OUT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", default=BASE, help="Local Vite URL; only loopback hosts are allowed")
    parser.add_argument("--output", type=Path, default=OUT, help="Directory for JSON/XML and real frontend screenshots")
    parser.add_argument("--browser-executable", default=shutil.which("chromium") or shutil.which("chromium-browser"),
                        help="Optional Chromium executable; otherwise use Playwright's installed Chromium")
    args = parser.parse_args()
    parsed = urlsplit(args.base_url)
    if parsed.scheme != "http" or parsed.hostname not in ("127.0.0.1", "localhost", "::1") or parsed.username or parsed.password or parsed.query or parsed.fragment:
        parser.error("--base-url must be a local loopback HTTP URL without credentials, query or fragment")
    BASE = args.base_url.rstrip("/") + "/"
    OUT = args.output.resolve()
    OUT.mkdir(parents=True,exist_ok=True)
    with sync_playwright() as p:
        browser=p.chromium.launch(executable_path=args.browser_executable,headless=True,
                                  args=["--no-sandbox","--disable-dev-shm-usage"] if sys.platform.startswith("linux") else [])
        for skin in SKINS:
            for mode in ("light","dark"):
                run_case("appearance_"+skin+"_"+mode,lambda skin=skin,mode=mode:appearance(browser,skin,mode))
        for name,operation in [
            ("skin_preserves_all_three_modes",skin_preserves_mode),
            ("clipboard_keyboard_and_action_separation",keyboard_and_action_separation),
            ("clipboard_rejections_preserve_state",clipboard_rejections),
            ("altv_copy_failure_does_not_dismiss",altv_copy_rejection_and_success),
            ("serial_saves_and_stale_settings_event",serial_saves_and_stale_event),
            ("save_rejection_restores_confirmed_theme",save_rejection),
            ("minimal_patches_preserve_other_window_edits",patches_preserve_other_window_fields),
            ("clipboard_events_read_committed_state",clipboard_events_reload_committed_state),
            ("initial_load_failure_cannot_write_defaults",initial_load_rejection),
            ("press_feedback_keeps_touch_bounds",press_feedback_fixed_bounds),
            ("minimum_window_and_keyboard_focus",minimum_window_and_keyboard_focus),
            ("reduced_motion_and_standalone_preview",reduced_motion_and_preview_mode),
        ]:
            run_case(name,lambda operation=operation:operation(browser))
        browser.close()
    run_case("no_external_browser_requests",lambda:check(not UNEXPECTED_NETWORK,"Unexpected external requests: "+repr(UNEXPECTED_NETWORK)))
    result=dict(checked_at=datetime.now(timezone.utc).isoformat(),
        platform=f"{sys.platform} Chromium renders actual Windows companion frontend with mock Tauri IPC; not Windows WebView2/TSF/device acceptance",
        real_clipboard_or_remote_relay_access=False,
        tests=len(RESULTS),failures=sum(not r["passed"] for r in RESULTS),results=RESULTS,
        unexpected_network=UNEXPECTED_NETWORK)
    (OUT/"ui-regression.json").write_text(json.dumps(result,ensure_ascii=False,indent=2)+"\n")
    suite=ET.Element("testsuite",name="WindowsCompanionUi",tests=str(result["tests"]),failures=str(result["failures"]),errors="0",skipped="0")
    for row in RESULTS:
        case=ET.SubElement(suite,"testcase",classname="WindowsCompanionUi",name=row["name"],time=str(row["seconds"]))
        if not row["passed"]:ET.SubElement(case,"failure").text=row["error"]
    ET.ElementTree(suite).write(OUT/"ui-regression.xml",encoding="utf-8",xml_declaration=True)
    print(json.dumps({k:result[k] for k in ("tests","failures","platform")},ensure_ascii=False),flush=True)
    raise SystemExit(bool(result["failures"]))


if __name__ == "__main__":
    main()
