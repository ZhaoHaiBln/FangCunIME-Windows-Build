"""Black-box AI workbench checks against the real Windows companion frontend.

Uses Linux/host Chromium with deterministic Tauri IPC, microphone and WebAudio
fixtures. It does not exercise Windows WebView2, a physical microphone, provider
APIs, a user's clipboard, or Windows TSF. Start the local Vite server separately.
Only the explicitly supplied loopback HTTP origin can be requested by the page.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import json
from pathlib import Path
import shutil
import sys
import tempfile
import time
import traceback
from urllib.parse import urlsplit
from xml.etree import ElementTree as ET

sys.dont_write_bytecode = True

from playwright.sync_api import expect, sync_playwright


BASE = "http://127.0.0.1:5174/"
OUT = Path(tempfile.gettempdir()) / "fangcun-windows-ai-ui-regression"
SKINS = ("wood_type", "typewriter", "terminal", "brutal")
SETTINGS = {
    "scheme": "flypy", "mixedInput": True, "chinesePunctuation": True,
    "shiftToggleEnglish": True, "capsLockBypass": True, "pageSize": 7,
    "wheelPaging": True, "skin": "wood_type", "themeMode": "light",
    "darkMode": False, "systemDark": False, "firstCandidateSize": 20,
    "keyOpacity": .82, "typingGlow": True, "keyLetterSize": 18,
    "keyLetterBold": False, "clipboardEnabled": True,
    "candidateLayout": "double", "fullWidth": False, "traditional": False,
    "revision": "fixture-settings-1",
}
CONFIG = {
    "enabled": False,
    "text": {"provider": "deepseek", "baseUrl": "https://api.deepseek.com", "model": "deepseek-flash"},
    "speech": {"provider": "openai", "baseUrl": "https://api.openai.com/v1", "model": "gpt-transcribe"},
    "textKeySaved": False, "speechKeySaved": False, "revision": "fixture-ai-1",
}

# Fixtures are deliberately in the browser initialization script rather than a
# production testing hook. Calls remain in memory and reports never dump them.
MOCK = r"""
(() => {
  const initial = __FIXTURE_CONFIG__;
  const clone = value => value === undefined ? null : JSON.parse(JSON.stringify(value));
  const callbacks = new Map(), listeners = new Map(), pending = new Map();
  let serial = 0, revision = 1;
  const scope = (role, route) => role + '|' + route.provider + '|' + new URL(route.baseUrl).origin;
  const saved = new Set();
  for (const role of ['text','speech']) if (initial.ai[role+'KeySaved']) saved.add(scope(role, initial.ai[role]));
  const fixture = {
    settings: clone(initial.settings), ai: clone(initial.ai), calls: [],
    failures: clone(initial.failures || {}), unknownCommands: [],
    deferSave: false, pendingSaves: [],
    count(command) { return this.calls.filter(call => call.command === command).length; },
    latest(command) { return this.calls.filter(call => call.command === command).at(-1)?.args; },
    fail(command, times=1) { this.failures[command] = times; },
    emit(event, payload) {
      for (const id of listeners.get(event) || []) callbacks.get(id)?.({event, id, payload:clone(payload)});
    },
    progress(command, text, id=null, phase=null) {
      const requestId = id || this.latest(command)?.request.id;
      this.emit('ai://progress', {id:requestId, phase:phase || (command === 'ai_generate' ? 'generating' : 'transcribing'), text});
    },
    finish(command, text, success=true, id=null) {
      const requestId = id || this.latest(command)?.request.id;
      const entry = pending.get(requestId);
      if (!entry || entry.command !== command) throw new Error('Fixture has no matching pending request');
      pending.delete(requestId);
      success ? entry.resolve(text) : entry.reject(text || '示例服务暂时不可用');
    },
    rotateRevision() { this.ai.revision = 'fixture-ai-' + (++revision); },
    save(args) {
      if (args.revision !== this.ai.revision) throw 'AI 设置已被其他窗口修改，请重新读取后保存';
      const draft = args.draft;
      for (const role of ['text','speech']) {
        const selected = scope(role, draft[role]);
        if (draft['clear'+role[0].toUpperCase()+role.slice(1)+'Key']) saved.delete(selected);
        else if (typeof draft[role+'Key'] === 'string' && draft[role+'Key'].trim()) saved.add(selected);
      }
      this.ai = {enabled:draft.enabled, text:clone(draft.text), speech:clone(draft.speech),
        textKeySaved:saved.has(scope('text', draft.text)), speechKeySaved:saved.has(scope('speech', draft.speech)),
        revision:'fixture-ai-' + (++revision)};
      return clone(this.ai);
    },
    finishSave(success=true) {
      const entry = this.pendingSaves.shift();
      if (!entry) throw new Error('Fixture has no pending save');
      if (!success) { entry.reject('示例保存失败'); return; }
      try { entry.resolve(this.save(entry.args)); } catch (error) { entry.reject(error); }
    }
  };

  const audio = {
    requests:0, permission:initial.permission || 'allow', resumeDeferred:false,
    streams:[], contexts:[], processors:[], offline:[], pendingPermissions:[], pendingResumes:[], timeShift:0,
    makeStream() {
      const track = {kind:'audio', stops:0, stop() { this.stops++; }};
      const stream = {getTracks:() => [track], getAudioTracks:() => [track]};
      this.streams.push({stream,track}); return stream;
    },
    resolvePermission() {
      const resolve = this.pendingPermissions.shift();
      if (!resolve) throw new Error('Fixture has no microphone permission request');
      resolve(this.makeStream());
    },
    resolveResume() {
      const resolve = this.pendingResumes.shift();
      if (!resolve) throw new Error('Fixture has no resume request');
      resolve();
    },
    feed(frames=960, stereo=false) {
      const processor = this.processors.at(-1);
      if (!processor?.onaudioprocess) throw new Error('Fixture has no active audio processor');
      // Each source value occupies three frames at 48k, so a 16k resampler's
      // sample centers have known values, including both clipping boundaries.
      const values = [-2,-1,0,.5,1,2];
      const first = Float32Array.from({length:frames}, (_,index) => values[Math.floor(index/3) % values.length]);
      const second = Float32Array.from(first);
      if (stereo) for (let index=0;index<second.length;index++) second[index] = first[index] * .5;
      processor.onaudioprocess({inputBuffer:{length:frames, numberOfChannels:stereo ? 2 : 1,
        getChannelData:channel => channel === 0 ? first : second}});
    },
    advance(milliseconds) { this.timeShift += milliseconds; },
    stats() {
      return {requests:this.requests, streams:this.streams.length,
        stops:this.streams.map(entry=>entry.track.stops), contexts:this.contexts.length,
        closes:this.contexts.map(entry=>entry.closes),
        disconnected:this.contexts.map(entry=>entry.nodes.map(node=>node.disconnected)),
        offline:this.offline.map(entry=>({channels:entry.channels,frames:entry.frames,rate:entry.rate,
          sourceRate:entry.buffer?.sampleRate,sourceFrames:entry.buffer?.length}))};
    }
  };
  fixture.audio = audio;
  const originalNow = performance.now.bind(performance);
  Object.defineProperty(performance, 'now', {configurable:true,value:() => originalNow() + audio.timeShift});
  const mediaDevices = {
    async getUserMedia() {
      audio.requests++;
      if (audio.permission === 'deny') throw new DOMException('Fixture microphone denied','NotAllowedError');
      if (audio.permission === 'defer') return await new Promise(resolve => audio.pendingPermissions.push(resolve));
      return audio.makeStream();
    }
  };
  Object.defineProperty(navigator, 'mediaDevices', {configurable:true,value:mediaDevices});
  class AudioContextFixture {
    constructor() { this.sampleRate=48000; this.destination={}; this.closes=0; this.nodes=[]; audio.contexts.push(this); }
    node() { const node={disconnected:0, connect() {}, disconnect() {this.disconnected++;}}; this.nodes.push(node); return node; }
    createMediaStreamSource() { return this.node(); }
    createScriptProcessor() { const node=this.node(); node.onaudioprocess=null; audio.processors.push(node); return node; }
    createGain() { const node=this.node(); node.gain={value:1}; return node; }
    async resume() { if (audio.resumeDeferred) await new Promise(resolve => audio.pendingResumes.push(resolve)); }
    async close() { this.closes++; }
  }
  class OfflineAudioContextFixture {
    constructor(channels,frames,rate) { this.channels=channels; this.frames=frames; this.rate=rate; this.destination={}; audio.offline.push(this); }
    createBuffer(channels,length,sampleRate) {
      const data=Array.from({length:channels},()=>new Float32Array(length));
      return {length,sampleRate,numberOfChannels:channels,getChannelData:channel=>data[channel]};
    }
    createBufferSource() {
      const owner=this;
      return {buffer:null,connect() {},start() { owner.buffer=this.buffer; }};
    }
    async startRendering() {
      if (!this.buffer) throw new Error('Fixture source was not started');
      const input=this.buffer.getChannelData(0), output=new Float32Array(this.frames);
      const ratio=this.buffer.sampleRate/this.rate;
      // Deterministic linear resampling, rather than returning an unrelated
      // canned WAV. The product must supply the captured rate and data.
      for (let index=0;index<output.length;index++) {
        const position=index*ratio, low=Math.floor(position), blend=position-low;
        const a=input[Math.min(low,input.length-1)] || 0;
        const b=input[Math.min(low+1,input.length-1)] || 0;
        output[index]=a+(b-a)*blend;
      }
      return {getChannelData:()=>output};
    }
  }
  Object.defineProperty(window,'AudioContext',{configurable:true,value:AudioContextFixture});
  Object.defineProperty(window,'OfflineAudioContext',{configurable:true,value:OfflineAudioContextFixture});
  window.__aiFixture = fixture;
  globalThis.isTauri = true;
  window.__TAURI_INTERNALS__ = {
    metadata:{currentWindow:{label:'main'},currentWebview:{label:'main'}},
    transformCallback(callback) { const id=++serial; callbacks.set(id,callback); return id; },
    unregisterCallback(id) { callbacks.delete(id); },
    async invoke(command,args={}) {
      fixture.calls.push({command,args:clone(args)});
      if (fixture.failures[command] > 0) { fixture.failures[command]--; throw '示例操作失败'; }
      if (command === 'plugin:event|listen') {
        const ids=listeners.get(args.event) || []; ids.push(args.handler); listeners.set(args.event,ids); return ++serial;
      }
      if (command === 'plugin:event|unlisten') return null;
      if (command === 'ai_get_config') return clone(fixture.ai);
      if (command === 'ai_save_config') {
        if (fixture.deferSave) return await new Promise((resolve,reject)=>fixture.pendingSaves.push({args:clone(args),resolve,reject}));
        return fixture.save(args);
      }
      if (command === 'ai_generate' || command === 'ai_transcribe') {
        return await new Promise((resolve,reject)=>pending.set(args.request.id,{command,resolve,reject}));
      }
      if (command === 'ai_cancel' || command === 'ai_copy_result') return null;
      if (command === 'load_settings') return clone(fixture.settings);
      if (command === 'skin_library_snapshot') return {
        revision:'fixture-skins-1',importEnabled:true,hiddenBuiltins:[],packs:[],activeId:'builtin:'+fixture.settings.skin,active:null,
        builtins:Object.entries({wood_type:'木框活字',typewriter:'经典打字机',terminal:'绿屏终端',brutal:'极客机械'}).map(([id,label])=>({id,label,hidden:false}))};
      if (command === 'patch_settings') {
        fixture.settings={...fixture.settings,...clone(args.patch),revision:'fixture-settings-'+(++serial)};
        fixture.emit('settings://changed',fixture.settings); return clone(fixture.settings);
      }
      if (command === 'reset_settings') { fixture.settings=clone(initial.settings); return clone(fixture.settings); }
      if (command === 'list_clipboard') return [];
      if (command === 'get_clipboard_enabled') return fixture.settings.clipboardEnabled;
      if (command === 'clipboard_persistence_status') return null;
      if (command === 'clipboard_share_snapshot') return {
        revision:'fixture-route-1',viewRevision:'1',server:'https://clipboard.example.invalid',topic:'fixture-topic',
        senderName:'我的电脑',enabled:false,autoSend:false,tokenSaved:false,keySaved:false,keyId:'',
        status:'未连接',error:null,gap:false,needsReset:false,items:[]};
      if (['hide_window','hide_clipboard_window','open_settings_tab','set_english_mode','toggle_statusbar'].includes(command)) return null;
      fixture.unknownCommands.push(command); throw 'Fixture command is not defined';
    }
  };
})();
"""

RESULTS: list[dict] = []
BLOCKED_NETWORK: list[str] = []


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def count(page, command):
    return page.evaluate("command => window.__aiFixture.count(command)", command)


def wait_count(page, command, total):
    page.wait_for_function("([command,total]) => window.__aiFixture.count(command) === total", arg=[command, total])


def tab(page, name):
    page.locator(f'button[data-tab="{name}"]').click()
    pane = page.locator(f'[data-pane="{name}"]')
    expect(pane).to_be_visible()
    pane.evaluate("async el => { await Promise.all(el.getAnimations().filter(a => a.effect?.getTiming().iterations !== Infinity).map(a => a.finished.catch(() => {}))); }")


def mode(page, name):
    page.locator(f'button[data-ai-mode="{name}"]').click()
    expect(page.locator(f'button[data-ai-mode="{name}"]')).to_have_attribute("aria-selected", "true")


def open_config(page):
    if not page.locator("#ai-config").evaluate("el => el.open"):
        page.locator("#ai-open-config").click()
    expect(page.locator("#ai-config-form")).to_be_visible()


def advanced(page, role="text"):
    open_config(page)
    details = page.locator("#ai-config-form .ai-advanced").nth(0 if role == "text" else 1)
    if not details.evaluate("el => el.open"):
        details.locator("summary").click()


def finish(page, command="ai_generate", text="这是中文示例结果。", success=True, request_id=None):
    page.evaluate("args => window.__aiFixture.finish(...args)", [command, text, success, request_id])


def progress(page, text, command="ai_generate", request_id=None, phase=None):
    page.evaluate("args => window.__aiFixture.progress(...args)", [command, text, request_id, phase])


def start_generation(page, selected="write", text="请写一句温暖的问候。"):
    mode(page, selected)
    page.locator("#ai-source").fill(text)
    before = count(page, "ai_generate")
    page.locator("#ai-generate").click()
    wait_count(page, "ai_generate", before + 1)
    expect(page.locator("#ai-cancel")).to_be_enabled()
    return page.evaluate("window.__aiFixture.latest('ai_generate').request.id")


def start_recording(page):
    page.locator("#ai-record").click()
    expect(page.locator("#ai-record")).to_have_text("结束录音")
    expect(page.locator("#ai-meter")).to_be_visible()


def feed_audio(page, frames=960, stereo=False):
    page.evaluate("args => window.__aiFixture.audio.feed(...args)", [frames, stereo])


def stop_recording(page):
    before = count(page, "ai_transcribe")
    page.locator("#ai-record").click()
    wait_count(page, "ai_transcribe", before + 1)
    expect(page.locator("#ai-meter")).to_be_hidden()


def assert_released(page, *, contexts=1, tracks=1):
    state = page.evaluate("window.__aiFixture.audio.stats()")
    check(state["streams"] == tracks and state["stops"] == [1] * tracks,
          "Every acquired microphone track must stop exactly once")
    check(state["contexts"] == contexts and state["closes"] == [1] * contexts,
          "Every created audio context must close exactly once")
    check(all(nodes == [1, 1, 1] for nodes in state["disconnected"]),
          "Recording graph nodes must disconnect exactly once")


@contextmanager
def app(browser, *, enabled=True, keys=True, failures=None, permission="allow", skin="wood_type", theme="light", width=940, height=760):
    settings = {**SETTINGS, "skin": skin, "themeMode": theme, "darkMode": theme == "dark"}
    config = {**CONFIG, "enabled": enabled, "textKeySaved": keys, "speechKeySaved": keys}
    context = browser.new_context(viewport={"width": width, "height": height}, device_scale_factor=1,
                                  reduced_motion="reduce", color_scheme=theme)
    context.add_init_script(MOCK.replace("__FIXTURE_CONFIG__", json.dumps({
        "settings": settings, "ai": config, "failures": failures or {}, "permission": permission,
    }, ensure_ascii=False)))
    expected_origin = urlsplit(BASE)
    def local_only(route):
        url = urlsplit(route.request.url)
        if url.scheme == "http" and (url.hostname, url.port) == (expected_origin.hostname, expected_origin.port):
            route.continue_()
        else:
            # Store only the destination origin, never a query, body or token.
            BLOCKED_NETWORK.append(url.scheme + "://" + (url.hostname or ""))
            route.abort()
    context.route("**/*", local_only)
    page = context.new_page()
    page.set_default_timeout(6500)
    page_errors = []
    page.on("pageerror", lambda error: page_errors.append(type(error).__name__))
    try:
        page.goto(BASE, wait_until="networkidle")
        page.wait_for_function("window.__aiFixture.count('ai_get_config') === 1 && window.__aiFixture.count('clipboard_share_snapshot') >= 1")
        expect(page.locator("#ai-config-state")).not_to_contain_text("正在读取")
        tab(page, "ai")
        yield page
        check(not page_errors, "The frontend produced an uncaught browser exception")
        check(not page.evaluate("window.__aiFixture.unknownCommands.length"), "The IPC fixture encountered an unsupported command")
    finally:
        context.close()


def default_disabled(browser):
    with app(browser, enabled=False, keys=False) as page:
        expect(page.locator("#ai-config-state")).to_contain_text("默认关闭")
        expect(page.locator("#ai-record")).to_be_disabled()
        mode(page, "write")
        page.locator("#ai-source").fill("今天也有好心情。")
        expect(page.locator("#ai-generate")).to_be_disabled()
        page.locator("#ai-source").press("Enter")
        check(count(page, "ai_generate") == count(page, "ai_transcribe") == 0, "Disabled AI must not call a provider")
        check(page.evaluate("window.__aiFixture.audio.requests") == 0, "Disabled AI must not request microphone access")
        check(page.locator("#ai-speech-provider option").evaluate_all("items => items.map(el=>el.value)") == ["openai", "gemini"],
              "Only implemented file-based speech providers may be selectable")


def initial_load_failure(browser):
    with app(browser, failures={"ai_get_config": 1}) as page:
        expect(page.locator("#ai-save-status")).to_contain_text("无法读取")
        expect(page.locator("#ai-record")).to_be_disabled()
        mode(page, "ask")
        expect(page.locator("#ai-generate")).to_be_disabled()
        page.locator("#ai-open-config").click()
        page.locator("#ai-reload").click()
        expect(page.locator("#ai-record")).to_be_enabled()
        check(count(page, "ai_save_config") == 0, "Load failure must not save assumed defaults")


def saved_keys_and_dirty_controls(browser):
    with app(browser) as page:
        open_config(page)
        check(page.locator("#ai-text-key").input_value() == page.locator("#ai-speech-key").input_value() == "",
              "Saved keys must never be filled back into password fields")
        expect(page.locator("#ai-text-key-state")).to_contain_text("不回填")
        mode(page, "rewrite")
        advanced(page)
        page.locator("#ai-text-model").fill("fixture-small-model")
        expect(page.locator("#ai-config-state")).to_contain_text("未保存")
        expect(page.locator("#ai-generate")).to_be_disabled()
        expect(page.locator("#ai-record")).to_be_disabled()
        expect(page.locator("#ai-save")).to_be_enabled()
        check(count(page, "ai_generate") == count(page, "ai_transcribe") == 0, "Unsaved settings must not start an AI operation")


def save_failure_and_success(browser):
    with app(browser, enabled=False, keys=False) as page:
        page.locator("#ai-enabled").check()
        page.locator("#ai-text-key").fill("fixture-text-secret")
        page.locator("#ai-speech-key").fill("fixture-speech-secret")
        page.evaluate("window.__aiFixture.fail('ai_save_config')")
        page.locator("#ai-save").click()
        expect(page.locator("#ai-save-status")).to_contain_text("草稿已保留")
        check(page.locator("#ai-text-key").input_value() != "" and page.locator("#ai-speech-key").input_value() != "",
              "Failed save must preserve both key drafts")
        expect(page.locator("#ai-save")).to_be_enabled()
        page.locator("#ai-save").click()
        expect(page.locator("#ai-save-status")).to_contain_text("已保存")
        check(page.locator("#ai-text-key").input_value() == page.locator("#ai-speech-key").input_value() == "",
              "Successful save must clear key drafts")
        expect(page.locator("#ai-record")).to_be_enabled()
        check(count(page, "ai_generate") == count(page, "ai_transcribe") == 0, "Saving settings alone must not run a paid provider test")
        check(page.evaluate("window.__aiFixture.audio.requests") == 0, "Saving must not activate the microphone")


def pending_save_freezes_configuration(browser):
    with app(browser) as page:
        advanced(page)
        page.locator("#ai-text-model").fill("fixture-fast-model")
        page.evaluate("window.__aiFixture.deferSave=true")
        page.locator("#ai-save").click()
        wait_count(page, "ai_save_config", 1)
        for selector in ("#ai-enabled", "#ai-text-provider", "#ai-text-url", "#ai-text-model", "#ai-text-key",
                         "#ai-speech-provider", "#ai-speech-key", "#ai-clear-text-key", "#ai-reload", "#ai-save"):
            expect(page.locator(selector)).to_be_disabled()
        page.evaluate("window.__aiFixture.finishSave()")
        expect(page.locator("#ai-record")).to_be_enabled()
        advanced(page)
        expect(page.locator("#ai-text-model")).to_have_value("fixture-fast-model")
        expect(page.locator("#ai-text-provider")).to_be_enabled()


def provider_and_origin_key_scope(browser):
    with app(browser) as page:
        open_config(page)
        page.locator("#ai-text-key").fill("fixture-new-secret")
        page.locator("#ai-text-provider").select_option("openai")
        check(page.locator("#ai-text-key").input_value() == "", "Changing provider must clear the unsaved key draft")
        expect(page.locator("#ai-text-key-state")).not_to_contain_text("不回填")
        page.locator("#ai-save").click()
        expect(page.locator("#ai-save-status")).to_contain_text("已保存")
        mode(page, "write")
        expect(page.locator("#ai-generate")).to_be_disabled()
        check(page.evaluate("!window.__aiFixture.ai.textKeySaved && window.__aiFixture.ai.speechKeySaved"),
              "Saving a different provider must not reuse the previous provider's key")
        page.locator("#ai-text-provider").select_option("deepseek")
        page.locator("#ai-save").click()
        expect(page.locator("#ai-generate")).to_be_enabled()
        advanced(page)
        page.locator("#ai-text-key").fill("fixture-origin-secret")
        page.locator("#ai-text-url").fill("https://other.example.invalid/v1")
        check(page.locator("#ai-text-key").input_value() == "", "Changing HTTPS origin must clear the unsaved key draft")
        page.locator("#ai-save").click()
        expect(page.locator("#ai-save-status")).to_contain_text("已保存")
        expect(page.locator("#ai-generate")).to_be_disabled()
        check(page.evaluate("window.__aiFixture.latest('ai_save_config').draft.textKey === null"), "An empty draft must save null rather than an old key")


def revision_conflict_preserves_draft(browser):
    with app(browser) as page:
        advanced(page)
        page.locator("#ai-text-model").fill("fixture-local-draft")
        page.evaluate("window.__aiFixture.rotateRevision()")
        page.locator("#ai-save").click()
        expect(page.locator("#ai-save-status")).to_contain_text("草稿已保留")
        expect(page.locator("#ai-save-status")).to_contain_text("AI 设置已被其他窗口修改，请重新读取后保存")
        expect(page.locator("#ai-text-model")).to_have_value("fixture-local-draft")
        check(page.evaluate("window.__aiFixture.ai.text.model === 'deepseek-flash'"), "A revision conflict must not overwrite the confirmed configuration")
        page.once("dialog", lambda dialog: dialog.accept())
        page.locator("#ai-reload").click()
        expect(page.locator("#ai-text-model")).to_have_value("deepseek-flash")
        expect(page.locator("#ai-config-summary")).to_have_text("已开启")


def custom_advanced_settings_stay_visible(browser):
    with app(browser) as page:
        open_config(page)
        page.locator("#ai-text-provider").select_option("compatible")
        check(page.locator("#ai-text-advanced").evaluate("el=>el.open"),
              "Custom providers must reveal their required address and model")
        expect(page.locator("#ai-text-url")).to_have_value("")
        expect(page.locator("#ai-text-model")).to_have_value("")
        page.locator("#ai-text-url").fill("https://proxy.example.invalid/v1")
        page.locator("#ai-text-model").fill("fixture-compatible-model")
        page.locator("#ai-text-key").fill("fixture-new-secret")
        page.locator("#ai-save").click()
        expect(page.locator("#ai-save-status")).to_contain_text("已保存")
        open_config(page)
        check(page.locator("#ai-text-advanced").evaluate("el=>el.open"),
              "Saved custom endpoints must not disappear behind the advanced disclosure")
        expect(page.locator("#ai-text-url")).to_have_value("https://proxy.example.invalid/v1")


def replace_and_delete_keys_are_explicit(browser):
    with app(browser) as page:
        open_config(page)
        page.locator("#ai-clear-text-key").check()
        page.locator("#ai-text-key").fill("fixture-new-secret")
        expect(page.locator("#ai-clear-text-key")).not_to_be_checked()
        page.locator("#ai-save").click()
        expect(page.locator("#ai-save-status")).to_contain_text("已保存")
        check(page.evaluate("window.__aiFixture.ai.textKeySaved && window.__aiFixture.ai.speechKeySaved"),
              "Entering a replacement key must cancel deletion rather than submit conflicting instructions")
        open_config(page)
        page.locator("#ai-clear-text-key").check()
        page.locator("#ai-save").click()
        expect(page.locator("#ai-save-status")).to_contain_text("已保存")
        check(page.evaluate("!window.__aiFixture.ai.textKeySaved && window.__aiFixture.ai.speechKeySaved"),
              "Explicit text-key deletion must preserve the independent speech key")
        check(count(page, "ai_generate") == count(page, "ai_transcribe") == 0,
              "Replacing or deleting credentials must not run a provider test")


def text_mode_request(browser, selected):
    with app(browser) as page:
        mode(page, selected)
        if selected == "translate":
            page.locator("#ai-target-language").select_option("日语")
        start_generation(page, selected, "请把今天的安排写清楚。")
        check(page.evaluate("mode => { const r=window.__aiFixture.latest('ai_generate').request; return r.mode === mode && r.source === '请把今天的安排写清楚。' && r.targetLanguage === (mode === 'translate' ? '日语' : '中文') && typeof r.id === 'string' && r.id.length>0; }", selected),
              "The selected mode, source and target language must be sent in the real request")
        expect(page.locator("#ai-source")).to_be_disabled()
        expect(page.locator("#ai-generate")).to_be_disabled()
        finish(page)
        expect(page.locator("#ai-result")).to_have_text("这是中文示例结果。")
        expect(page.locator("#ai-source")).to_be_enabled()
        expect(page.locator("#ai-cancel")).to_be_disabled()


def streaming_then_complete(browser):
    with app(browser) as page:
        start_generation(page)
        progress(page, "今天")
        expect(page.locator("#ai-result")).to_have_text("今天")
        progress(page, "今天也有")
        expect(page.locator("#ai-result")).to_have_text("今天也有")
        finish(page, text="今天也有好心情。\n出门记得带伞。")
        expect(page.locator("#ai-result")).to_have_text("今天也有好心情。\n出门记得带伞。")
        expect(page.locator("#ai-copy")).to_be_enabled()
        check(count(page, "ai_copy_result") == 0, "Streaming must not copy or paste without an explicit click")


def cancel_preserves_latest_ignores_late(browser):
    with app(browser) as page:
        request_id = start_generation(page)
        progress(page, "已经显示")
        expect(page.locator("#ai-result")).to_have_text("已经显示")
        # Dispatch both actions in one browser task: cancellation must preserve
        # the piece still inside the production 40ms display throttle.
        page.evaluate("() => { window.__aiFixture.progress('ai_generate','最后一段也保留'); document.querySelector('#ai-cancel').click(); }")
        expect(page.locator("#ai-result")).to_have_text("最后一段也保留")
        wait_count(page, "ai_cancel", 1)
        progress(page, "迟到的片段", request_id=request_id)
        finish(page, text="迟到的完整结果", request_id=request_id)
        page.wait_for_timeout(100)
        expect(page.locator("#ai-result")).to_have_text("最后一段也保留")
        expect(page.locator("#ai-cancel")).to_be_disabled()


def failure_preserves_latest(browser):
    with app(browser) as page:
        start_generation(page)
        page.evaluate("() => { window.__aiFixture.progress('ai_generate','失败前的最后一段'); window.__aiFixture.finish('ai_generate','',false); }")
        expect(page.locator("#ai-operation-error")).to_be_visible()
        expect(page.locator("#ai-result")).to_have_text("失败前的最后一段")
        expect(page.locator("#ai-generate")).to_be_enabled()
        expect(page.locator("#ai-copy")).to_be_enabled()


def safe_local_errors_and_raw_errors(browser):
    with app(browser) as page:
        start_generation(page)
        finish(page, text="AI 服务地址或模型不存在，请检查配置", success=False)
        expect(page.locator("#ai-operation-error")).to_have_text("AI 服务地址或模型不存在，请检查配置")
        start_generation(page)
        finish(page, text="raw-provider-response fixture-text-secret https://example.invalid/?key=fixture-text-secret", success=False)
        expect(page.locator("#ai-operation-error")).to_have_text("生成失败，请检查文字服务配置与网络后重试")
        expect(page.locator("#ai-workbench")).not_to_contain_text("fixture-text-secret")
        expect(page.locator("#ai-workbench")).not_to_contain_text("raw-provider-response")


def unrelated_progress_is_ignored(browser):
    with app(browser) as page:
        request_id = start_generation(page)
        progress(page, "其它窗口内容", request_id="other-window-id")
        progress(page, "不匹配的处理阶段", request_id=request_id, phase="transcribing")
        page.wait_for_timeout(90)
        expect(page.locator("#ai-result")).to_have_text("生成结果会显示在这里")
        finish(page, text="只接受当前任务的结果。")
        expect(page.locator("#ai-result")).to_have_text("只接受当前任务的结果。")


def mode_and_pane_switch_cancel(browser):
    with app(browser) as page:
        first_id = start_generation(page)
        mode(page, "ask")
        wait_count(page, "ai_cancel", 1)
        finish(page, text="旧功能的迟到结果", request_id=first_id)
        expect(page.locator("#ai-result")).to_have_text("生成结果会显示在这里")
        second_id = start_generation(page, "ask")
        tab(page, "input")
        wait_count(page, "ai_cancel", 2)
        finish(page, text="离开后的迟到结果", request_id=second_id)
        tab(page, "ai")
        expect(page.locator("#ai-result")).to_have_text("生成结果会显示在这里")
        expect(page.locator("#ai-cancel")).to_be_disabled()


def voice_wav_and_meter(browser):
    with app(browser) as page:
        start_recording(page)
        feed_audio(page, stereo=True)
        check(int(page.locator("#ai-meter").get_attribute("aria-valuenow")) > 0, "The meter must react to measured audio, even with reduced motion")
        stop_recording(page)
        summary = page.evaluate("""() => {
          const bytes=window.__aiFixture.latest('ai_transcribe').request.wav;
          const view=new DataView(Uint8Array.from(bytes).buffer);
          const ascii=(at,length)=>String.fromCharCode(...bytes.slice(at,at+length));
          return {length:bytes.length,riff:ascii(0,4),wave:ascii(8,4),fmt:ascii(12,4),data:ascii(36,4),
            riffSize:view.getUint32(4,true),formatSize:view.getUint32(16,true),format:view.getUint16(20,true),
            channels:view.getUint16(22,true),rate:view.getUint32(24,true),byteRate:view.getUint32(28,true),
            blockAlign:view.getUint16(32,true),bits:view.getUint16(34,true),dataSize:view.getUint32(40,true),
            pcm:[0,1,2,3,4,5].map(index=>view.getInt16(44+index*2,true))};
        }""")
        check(summary == {"length":684,"riff":"RIFF","wave":"WAVE","fmt":"fmt ","data":"data",
                          "riffSize":676,"formatSize":16,"format":1,"channels":1,"rate":16000,"byteRate":32000,
                          "blockAlign":2,"bits":16,"dataSize":640,"pcm":[-32768,-24576,0,12288,24575,32767]},
              "Captured 48k stereo must downmix and produce a bounded 16k mono PCM16 WAV")
        state = page.evaluate("window.__aiFixture.audio.stats()")
        check(state["offline"] == [{"channels":1,"frames":320,"rate":16000,"sourceRate":48000,"sourceFrames":960}],
              "OfflineAudioContext must receive the actual capture rate and frame count")
        assert_released(page)
        progress(page, "今天", command="ai_transcribe")
        expect(page.locator("#ai-result")).to_have_text("今天")
        finish(page, "ai_transcribe", "今天也有好心情。")
        expect(page.locator("#ai-result")).to_have_text("今天也有好心情。")
        check(count(page, "ai_generate") == 0, "Voice-only transcription must not invoke the text model")


def dictation_chains_current_text_mode(browser):
    with app(browser) as page:
        mode(page, "rewrite")
        page.locator("#ai-source").fill("请写得自然一些。")
        expect(page.locator("#ai-recording-note")).to_contain_text("自动发送")
        start_recording(page)
        feed_audio(page)
        stop_recording(page)
        progress(page, "正在口述", command="ai_transcribe")
        page.wait_for_timeout(70)
        expect(page.locator("#ai-source")).to_have_value("请写得自然一些。")
        finish(page, "ai_transcribe", "今天工作辛苦了。")
        wait_count(page, "ai_generate", 1)
        check(page.evaluate("() => { const r=window.__aiFixture.latest('ai_generate').request; return r.mode==='rewrite' && r.source==='请写得自然一些。\\n今天工作辛苦了。' && r.targetLanguage==='中文'; }"),
              "Dictation must append to the existing draft and continue through the selected text mode")
        finish(page, text="今天忙了一天，辛苦啦。")
        expect(page.locator("#ai-result")).to_have_text("今天忙了一天，辛苦啦。")
        assert_released(page)


def dictation_failure_keeps_source_and_partial(browser):
    with app(browser) as page:
        mode(page, "ask")
        page.locator("#ai-source").fill("原先的问题要保留。")
        start_recording(page)
        feed_audio(page)
        stop_recording(page)
        page.evaluate("() => { window.__aiFixture.progress('ai_transcribe','尚未识别完整的话'); window.__aiFixture.finish('ai_transcribe','',false); }")
        expect(page.locator("#ai-operation-error")).to_be_visible()
        expect(page.locator("#ai-source")).to_have_value("原先的问题要保留。")
        expect(page.locator("#ai-result")).to_have_text("尚未识别完整的话")
        check(count(page, "ai_generate") == 0, "Failed transcription must not generate from a partial draft")
        assert_released(page)


def permission_arrives_after_cancel(browser):
    with app(browser, permission="defer") as page:
        page.locator("#ai-record").click()
        page.wait_for_function("window.__aiFixture.audio.requests === 1")
        page.locator("#ai-cancel").click()
        page.evaluate("window.__aiFixture.audio.resolvePermission()")
        page.wait_for_function("window.__aiFixture.audio.stats().stops[0] === 1")
        assert_released(page, contexts=0)
        check(count(page, "ai_transcribe") == 0, "A cancelled permission request must never upload audio")
        expect(page.locator("#ai-record")).to_be_enabled()


def resume_arrives_after_cancel(browser):
    with app(browser) as page:
        page.evaluate("window.__aiFixture.audio.resumeDeferred=true")
        page.locator("#ai-record").click()
        page.wait_for_function("window.__aiFixture.audio.pendingResumes.length === 1")
        page.locator("#ai-cancel").click()
        page.evaluate("window.__aiFixture.audio.resolveResume()")
        page.wait_for_timeout(60)
        assert_released(page)
        check(count(page, "ai_transcribe") == 0, "A cancelled audio initialization must never upload audio")
        expect(page.locator("#ai-record")).to_be_enabled()


def microphone_denied(browser):
    with app(browser, permission="deny") as page:
        page.locator("#ai-record").click()
        expect(page.locator("#ai-operation-error")).to_be_visible()
        expect(page.locator("#ai-record")).to_be_enabled()
        check(count(page, "ai_transcribe") == 0, "Denied microphone permission must not call transcription")
        check(page.evaluate("window.__aiFixture.audio.stats().streams") == 0, "Denied permission must not create a recording stream")


def empty_recording_not_uploaded(browser):
    with app(browser) as page:
        start_recording(page)
        page.locator("#ai-record").click()
        expect(page.locator("#ai-operation-error")).to_be_visible()
        check(count(page, "ai_transcribe") == 0, "A recording with zero captured frames must not upload an empty WAV")
        assert_released(page)


def cancel_recording_releases_audio(browser):
    with app(browser) as page:
        start_recording(page)
        feed_audio(page)
        page.locator("#ai-cancel").click()
        expect(page.locator("#ai-record")).to_have_text("开始录音")
        assert_released(page)
        check(count(page, "ai_transcribe") == 0, "Cancelled microphone capture must not upload audio")
        expect(page.locator("#ai-meter")).to_be_hidden()


def pane_leave_releases_audio(browser):
    with app(browser) as page:
        start_recording(page)
        feed_audio(page)
        tab(page, "input")
        wait_count(page, "ai_cancel", 1)
        assert_released(page)
        check(count(page, "ai_transcribe") == 0, "Leaving the AI pane must stop capture rather than submit it")
        tab(page, "ai")
        expect(page.locator("#ai-record")).to_have_text("开始录音")


def native_hide_releases_audio(browser):
    with app(browser) as page:
        start_recording(page)
        feed_audio(page)
        page.evaluate("window.__aiFixture.emit('ai://window-hidden', null)")
        wait_count(page, "ai_cancel", 1)
        assert_released(page)
        check(count(page, "ai_transcribe") == 0, "Native hide must stop recording without an upload")
        expect(page.locator("#ai-record")).to_have_text("开始录音")


def recording_sixty_second_limit(browser):
    with app(browser) as page:
        start_recording(page)
        feed_audio(page)
        page.evaluate("window.__aiFixture.audio.advance(60000)")
        wait_count(page, "ai_transcribe", 1)
        assert_released(page)
        page.wait_for_timeout(230)
        check(count(page, "ai_transcribe") == 1, "The 60-second cap must submit exactly one transcription")
        finish(page, "ai_transcribe", "一分钟录音上限的示例。")
        expect(page.locator("#ai-result")).to_have_text("一分钟录音上限的示例。")


def copy_failure_does_not_claim_success(browser):
    with app(browser) as page:
        start_generation(page)
        finish(page, text="请手动粘贴这段示例文字。")
        expect(page.locator("#ai-copy")).to_be_enabled()
        page.evaluate("window.__aiFixture.fail('ai_copy_result')")
        page.locator("#ai-copy").click()
        expect(page.locator("#ai-copy-status")).to_contain_text("复制失败")
        expect(page.locator("#ai-result")).to_have_text("请手动粘贴这段示例文字。")
        expect(page.locator("#ai-copy")).to_be_enabled()
        page.locator("#ai-copy").click()
        expect(page.locator("#ai-copy-status")).to_contain_text("手动粘贴")
        check(page.evaluate("window.__aiFixture.latest('ai_copy_result').text === '请手动粘贴这段示例文字。'"),
              "The explicit copy action must send the visible preview to the native clipboard command")


def keyboard_tabs_and_empty_source(browser):
    with app(browser) as page:
        page.locator('button[data-ai-mode="voice"]').focus()
        page.keyboard.press("ArrowRight")
        expect(page.locator('button[data-ai-mode="write"]')).to_be_focused()
        expect(page.locator('button[data-ai-mode="write"]')).to_have_attribute("aria-selected", "true")
        page.locator("#ai-generate").click()
        expect(page.locator("#ai-operation-error")).to_be_visible()
        check(count(page, "ai_generate") == 0, "Empty text must be rejected before starting a provider request")
        page.locator('button[data-ai-mode="write"]').focus()
        page.keyboard.press("End")
        expect(page.locator('button[data-ai-mode="ask"]')).to_be_focused()
        page.keyboard.press("Home")
        expect(page.locator('button[data-ai-mode="voice"]')).to_be_focused()


def appearance(browser, skin, theme):
    with app(browser, skin=skin, theme=theme, width=760, height=600) as page:
        expect(page.locator("body")).to_have_attribute("data-skin", skin)
        open_config(page)
        advanced(page)
        advanced(page, "speech")
        for selector in ("#ai-text-provider", "#ai-text-url", "#ai-speech-provider", "#ai-speech-url", "#ai-save"):
            control = page.locator(selector)
            control.scroll_into_view_if_needed()
            bounds = control.bounding_box()
            check(bounds is not None and bounds["x"] >= 0 and bounds["x"] + bounds["width"] <= 761,
                  "AI configuration controls must fit the minimum-width window")
        page.locator("#ai-open-config").click()
        start_generation(page, "ask", "怎样让一天过得更从容？")
        finish(page, text="先做一件重要的小事，再给自己留一点休息时间。\n" + "界面换行示例" * 24)
        expect(page.locator("#ai-copy")).to_be_enabled()
        page.locator("#ai-copy").scroll_into_view_if_needed()
        page.locator("#ai-copy").focus()
        expect(page.locator("#ai-copy")).to_be_focused()
        geometry = page.evaluate("""() => ({
          pageWidth:document.documentElement.clientWidth,
          pageScroll:document.documentElement.scrollWidth,
          oversized:[...document.querySelectorAll('#ai-workbench input,#ai-workbench select,#ai-workbench textarea,#ai-workbench pre,#ai-workbench button')]
            .filter(el=>el.getClientRects().length && (el.getBoundingClientRect().left<-.5 || el.getBoundingClientRect().right>innerWidth+.5)).length,
          background:getComputedStyle(document.querySelector('#ai-result')).backgroundColor,
          foreground:getComputedStyle(document.querySelector('#ai-result')).color
        })""")
        check(geometry["pageScroll"] <= geometry["pageWidth"] + 1 and geometry["oversized"] == 0,
              "AI controls and long preview text must not create horizontal overflow")
        check(geometry["foreground"] != geometry["background"], "AI preview text must remain visible in the active palette")
        page.screenshot(path=str(OUT / f"ai-{skin}-{theme}.png"), animations="disabled")


def safe_failure():
    # Real credentials are never read. Keep even fixture password literals out
    # of assertion reports, and do not serialize browser call/WAV histories.
    detail = traceback.format_exc()
    for secret in ("fixture-text-secret", "fixture-speech-secret", "fixture-new-secret", "fixture-origin-secret"):
        detail = detail.replace(secret, "[fixture credential redacted]")
    return detail


def run_case(name, operation):
    started = time.monotonic()
    try:
        operation()
        row = {"name": name, "passed": True, "seconds": round(time.monotonic() - started, 3)}
    except Exception:
        row = {"name": name, "passed": False, "seconds": round(time.monotonic() - started, 3), "error": safe_failure()}
    RESULTS.append(row)
    print(("PASS " if row["passed"] else "FAIL ") + name, flush=True)
    if not row["passed"]:
        print(row["error"], flush=True)


def main():
    global BASE, OUT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", default=BASE, help="Existing local Vite HTTP URL; loopback hosts only")
    parser.add_argument("--output", type=Path, default=OUT, help="Directory for JSON/XML/log and sample-data screenshots")
    parser.add_argument("--browser-executable", default=shutil.which("chromium") or shutil.which("chromium-browser"),
                        help="Chromium executable; defaults to the system or Playwright Chromium")
    args = parser.parse_args()
    parsed = urlsplit(args.base_url)
    if parsed.scheme != "http" or parsed.hostname not in ("127.0.0.1", "localhost", "::1") or parsed.username or parsed.password or parsed.query or parsed.fragment:
        parser.error("--base-url must be a loopback HTTP URL without credentials, query or fragment")
    BASE = args.base_url.rstrip("/") + "/"
    OUT = args.output.resolve()
    OUT.mkdir(parents=True, exist_ok=True)
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(executable_path=args.browser_executable, headless=True,
            args=["--no-sandbox", "--disable-dev-shm-usage"] if sys.platform.startswith("linux") else [])
        cases = [
            ("default_disabled_no_provider_or_microphone", default_disabled),
            ("failed_load_cannot_save_assumed_defaults", initial_load_failure),
            ("saved_keys_never_refill_and_dirty_blocks_operations", saved_keys_and_dirty_controls),
            ("save_rejection_preserves_draft_success_clears_keys", save_failure_and_success),
            ("pending_save_freezes_configuration", pending_save_freezes_configuration),
            ("provider_and_origin_key_scope", provider_and_origin_key_scope),
            ("revision_conflict_preserves_draft", revision_conflict_preserves_draft),
            ("custom_advanced_settings_stay_visible", custom_advanced_settings_stay_visible),
            ("replacement_and_deletion_keep_roles_independent", replace_and_delete_keys_are_explicit),
            *[("text_mode_" + selected, lambda browser, selected=selected: text_mode_request(browser, selected))
              for selected in ("write", "rewrite", "translate", "ask")],
            ("streaming_first_piece_and_complete_final", streaming_then_complete),
            ("cancel_preserves_throttled_piece_ignores_late_result", cancel_preserves_latest_ignores_late),
            ("failure_preserves_throttled_piece", failure_preserves_latest),
            ("safe_local_errors_and_no_raw_error_echo", safe_local_errors_and_raw_errors),
            ("other_window_and_wrong_phase_progress_ignored", unrelated_progress_is_ignored),
            ("mode_and_pane_switch_cancel_old_requests", mode_and_pane_switch_cancel),
            ("voice_meter_and_48k_stereo_to_16k_pcm_wav", voice_wav_and_meter),
            ("dictation_appends_draft_and_chains_selected_mode", dictation_chains_current_text_mode),
            ("dictation_failure_preserves_source_and_partial", dictation_failure_keeps_source_and_partial),
            ("permission_arrives_after_cancel_stops_track", permission_arrives_after_cancel),
            ("audio_resume_arrives_after_cancel_releases_once", resume_arrives_after_cancel),
            ("microphone_denied_no_upload", microphone_denied),
            ("empty_recording_no_upload", empty_recording_not_uploaded),
            ("cancel_recording_releases_audio", cancel_recording_releases_audio),
            ("pane_leave_releases_audio", pane_leave_releases_audio),
            ("native_hide_releases_audio", native_hide_releases_audio),
            ("recording_sixty_second_cap", recording_sixty_second_limit),
            ("copy_failure_retains_preview_no_false_success", copy_failure_does_not_claim_success),
            ("keyboard_mode_navigation_and_empty_source", keyboard_tabs_and_empty_source),
        ]
        for name, operation in cases:
            run_case(name, lambda operation=operation: operation(browser))
        for skin in SKINS:
            for theme in ("light", "dark"):
                run_case("minimum_window_" + skin + "_" + theme,
                         lambda skin=skin, theme=theme: appearance(browser, skin, theme))
        browser.close()
    run_case("no_external_browser_requests", lambda: check(not BLOCKED_NETWORK, "The frontend attempted an external browser request"))
    report = {
        "checked_at": datetime.now(timezone.utc).isoformat(),
        "platform": f"{sys.platform} Chromium, real companion frontend with mock IPC and deterministic fake WebAudio; not Windows/device acceptance",
        "real_provider_or_microphone_or_clipboard_access": False,
        "tests": len(RESULTS), "failures": sum(not row["passed"] for row in RESULTS),
        "blocked_network_origins": sorted(set(BLOCKED_NETWORK)), "results": RESULTS,
    }
    (OUT / "ai-ui-regression.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    suite = ET.Element("testsuite", name="WindowsAiWorkbenchUi", tests=str(report["tests"]),
                       failures=str(report["failures"]), errors="0", skipped="0")
    for row in RESULTS:
        case = ET.SubElement(suite, "testcase", classname="WindowsAiWorkbenchUi", name=row["name"], time=str(row["seconds"]))
        if not row["passed"]:
            ET.SubElement(case, "failure").text = row["error"]
    ET.ElementTree(suite).write(OUT / "ai-ui-regression.xml", encoding="utf-8", xml_declaration=True)
    (OUT / "ai-ui-regression.log").write_text("\n".join(
        ("PASS " if row["passed"] else "FAIL ") + row["name"] + ("\n" + row["error"] if not row["passed"] else "")
        for row in RESULTS) + "\n", encoding="utf-8")
    print(json.dumps({key: report[key] for key in ("tests", "failures", "platform")}, ensure_ascii=False), flush=True)
    raise SystemExit(bool(report["failures"]))


if __name__ == "__main__":
    main()
