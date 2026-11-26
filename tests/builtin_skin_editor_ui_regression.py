"""Retired builtin data must remain inert in the file-only Windows skin UI.

The historical filename remains a test entrypoint. Runs real TypeScript with
mocked IPC and PNGs in Linux Chromium; no WebView2, real files or native TSF.
"""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import importlib.util
import json
from pathlib import Path
import shutil
import sys
import tempfile
import traceback
from urllib.parse import urlsplit
from xml.etree import ElementTree as ET
sys.dont_write_bytecode = True
from playwright.sync_api import expect, sync_playwright
spec = importlib.util.spec_from_file_location("file_skin_fixture", Path(__file__).with_name("skin_ui_regression.py"))
assert spec and spec.loader
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)
BASE = "http://127.0.0.1:5174/"
OUT = Path(tempfile.gettempdir()) / "fangcun-file-only-skins"
RESULTS = []
REAL_SNAPSHOT = None


def check(value, message):
    if not value:
        raise AssertionError(message)


def refresh(page, change):
    page.evaluate(change)
    page.locator("#skin-library-reload").click()
    base.wait_idle(page)


def inject_retired_snapshot(page):
    refresh(page, """() => {
      const f=window.__aiFixture,s=f.skin,original=s.snapshot.bind(s);
      s.snapshot=()=>({...original(),hiddenBuiltins:['wood_type'],
        builtins:['wood_type','typewriter','terminal','brutal'].map(id=>({id,label:id,hidden:false})),
        builtinStyles:{wood_type:{fontFamily:'serif',pressOffsetDp:2,themes:{light:{colors:{background:'#FF0000',selection:'#0000FF'},glow:['#FF0000','#FF0000','#FF0000','#FF0000']},dark:{colors:{background:'#FF0000'}}}}}});
    }""")


def no_retired_controls(browser):
    with base.app(browser) as page:
        inject_retired_snapshot(page)
        check(page.locator("#skin-group,button[data-skin],[data-builtin-edit],[data-builtin-reset],#builtin-skin-editor,#skin-builtin-management").count()==0,
              "A stale snapshot must not restore retired builtin cards, editors or management controls")
        check(page.evaluate("window.__aiFixture.calls.every(call=>!call.command.includes('builtin'))"),
              "A file-only page cannot invoke any retired builtin mutation")
        base.selected_default(page)


def retired_styles_are_inert(browser, tone):
    with base.app(browser,tone=tone) as page:
        inject_retired_snapshot(page)
        root=page.locator("html")
        check(root.evaluate("el=>getComputedStyle(el).getPropertyValue('--ime-board-bg').trim()") == ("#181818" if tone=="dark" else "#F3F3F3"),
              "Retired color overrides must not affect the neutral fallback")
        check(root.evaluate("el=>!el.hasAttribute('data-builtin-edit')&&!el.style.getPropertyValue('--fc-builtin-font')&&!el.style.getPropertyValue('--glow-a')"),
              "Retired font/glow profiles must remain inactive")
        check(page.locator(".fc-skin-image").count()==0,"Default cannot use bundled photographs")


def active_file_isolated_from_retired_styles(browser):
    with base.app(browser,installed=True,active=True) as page:
        inject_retired_snapshot(page)
        expect(page.locator("html")).to_have_attribute("data-local-skin","fixture.green")
        check(page.evaluate("document.documentElement.style.getPropertyValue('--fc-background')")==base.LIGHT['background'],
              "Retired builtin colors must not replace an active file palette")
        check(page.evaluate("document.querySelectorAll('.fc-skin-image').length>0"),"The active file must retain its own images")


def cancelled_delete_preserves_file(browser):
    with base.app(browser,installed=True,active=True) as page:
        page.once("dialog",lambda dialog:dialog.dismiss())
        page.locator(".skin-local-delete").click()
        check(page.evaluate("window.__aiFixture.count('skin_delete')")==0,"Cancelling deletion must not mutate the library")
        expect(base.installed_button(page)).to_have_attribute("aria-pressed","true")
        expect(page.locator("html")).to_have_attribute("data-local-skin","fixture.green")


def failed_delete_preserves_file(browser):
    with base.app(browser,installed=True,active=True) as page:
        page.evaluate("window.__aiFixture.skin.failure='fixture-secret raw deletion error'")
        page.once("dialog",lambda dialog:dialog.accept())
        page.locator(".skin-local-delete").click();base.wait_idle(page)
        expect(base.installed_button(page)).to_have_attribute("aria-pressed","true")
        expect(page.locator("html")).to_have_attribute("data-local-skin","fixture.green")
        expect(page.locator("#skin-library-error")).not_to_contain_text("fixture-secret")
        check(page.evaluate("window.__aiFixture.skin.installed"),"A rejected delete cannot remove the registered file")


def read_failure_preserves_current_material(browser):
    with base.app(browser,installed=True,active=True) as page:
        page.evaluate("window.__aiFixture.skin.failure='read'")
        page.locator("#skin-library-reload").click();base.wait_idle(page)
        expect(page.locator("#skin-library-error")).to_contain_text("已保留当前外观")
        expect(page.locator("html")).to_have_attribute("data-local-skin","fixture.green")
        check(page.evaluate("document.querySelectorAll('.fc-skin-image').length>0"),"A failed metadata read cannot blank the last confirmed file")


def default_conflict_requires_reload(browser):
    with base.app(browser,installed=True,active=True) as page:
        page.evaluate("window.__aiFixture.skin.rotate()")
        page.locator("#skin-use-default").click();base.wait_idle(page)
        expect(page.locator("#skin-library-error")).to_have_text("皮肤列表已更新，请重新读取后重试")
        expect(page.locator("#skin-use-default")).to_be_disabled()
        expect(page.locator("html")).to_have_attribute("data-local-skin","fixture.green")
        page.locator("#skin-library-reload").click();base.wait_idle(page)
        page.locator("#skin-use-default").click();base.wait_idle(page)
        base.selected_default(page)
        expect(base.installed_button(page)).to_be_visible()


def invalid_active_metadata_uses_neutral_fallback(browser):
    with base.app(browser,installed=True,active=True) as page:
        refresh(page,"delete window.__aiFixture.skin.manifest.themes.light.colors.surface")
        check(page.evaluate("!document.documentElement.dataset.localSkin&&document.querySelectorAll('.fc-skin-image').length===0"),
              "Invalid active metadata must clear the old material instead of partially applying colors")
        expect(page.locator("#showcase-light-label")).to_contain_text("系统默认")
        check(page.locator("html").evaluate("el=>getComputedStyle(el).getPropertyValue('--ime-board-bg').trim()")=='#F3F3F3',
              "Invalid metadata cannot expose a retired builtin fallback")


def rejected_image_origins_never_load(browser):
    with base.app(browser,installed=True,active=True) as page:
        refresh(page,"""() => {const s=window.__aiFixture.skin,original=s.snapshot.bind(s);
          s.snapshot=()=>{const v=original();v.active.imageUrls.light.keyNormal='https://fixture.invalid/key.png';return v;};
        }""")
        check(page.locator('.fc-skin-image[data-role="keyNormal"]').count()==0,"Untrusted image URLs must not reach DOM image layers")
        expect(page.locator("html")).to_have_attribute("data-local-skin","fixture.green")
        check(page.locator(".fc-skin-image").count()>0,"Rejecting one image must preserve valid file roles")


def mismatched_active_identity_is_inert(browser):
    with base.app(browser,installed=True,active=True) as page:
        refresh(page,"""() => {const s=window.__aiFixture.skin,original=s.snapshot.bind(s);
          s.snapshot=()=>({...original(),activeId:'local:fixture.other'});
        }""")
        check(page.evaluate("!document.documentElement.dataset.localSkin&&document.querySelectorAll('.fc-skin-image').length===0"),
              "A metadata snapshot with a different selected ID cannot restore stale file images")


def real_snapshot_is_file_only(browser):
    snapshot=REAL_SNAPSHOT
    check(snapshot is not None,"Actual backend snapshot is required")
    check(snapshot['builtins']==[] and snapshot.get('builtinStyles',{})=={} and snapshot.get('hiddenBuiltins',[])==[],
          "Actual backend snapshot must publish no retired builtin choices or overlays")
    check(snapshot['activeId']=='system:default' and snapshot['active'] is None,
          "The supplied actual empty backend snapshot must use the explicit system default")
    with base.app(browser) as page:
        raw=json.dumps(snapshot,ensure_ascii=False)
        refresh(page,"window.__aiFixture.skin.snapshot=()=>JSON.parse("+json.dumps(raw)+")")
        base.selected_default(page)
        check(page.locator("#skin-group,#builtin-skin-editor,[data-builtin-edit]").count()==0,"Actual empty backend state cannot create builtin controls")
        check(page.evaluate("window.__aiFixture.count('skin_select')===0"),"Reading the actual backend snapshot must not write a selection")


def run_case(name,operation):
    try:
        operation();RESULTS.append({'name':name,'passed':True})
    except Exception:
        RESULTS.append({'name':name,'passed':False,'error':traceback.format_exc().replace('fixture-secret','[sample redacted]')})


def main():
    global BASE,OUT,REAL_SNAPSHOT
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base-url',default=BASE);parser.add_argument('--output',type=Path,default=OUT)
    parser.add_argument('--browser-executable',default=shutil.which('chromium') or shutil.which('chromium-browser'))
    parser.add_argument('--real-snapshot',type=Path);parser.add_argument('--only-real-snapshot',action='store_true')
    args=parser.parse_args();parsed=urlsplit(args.base_url)
    if parsed.scheme!='http' or parsed.hostname not in ('127.0.0.1','localhost','::1') or parsed.username or parsed.password or parsed.query or parsed.fragment:
        parser.error('Only loopback frontend origins are allowed')
    if args.only_real_snapshot and not args.real_snapshot:parser.error('--only-real-snapshot requires --real-snapshot')
    BASE=args.base_url.rstrip('/')+'/';base.BASE=BASE
    OUT=args.output.resolve();OUT.mkdir(parents=True,exist_ok=True)
    if args.real_snapshot:REAL_SNAPSHOT=json.loads(args.real_snapshot.read_text())
    with sync_playwright() as playwright:
        browser=playwright.chromium.launch(executable_path=args.browser_executable,headless=True,args=['--no-sandbox','--disable-dev-shm-usage'] if sys.platform.startswith('linux') else [])
        cases=[('retired_snapshot_cannot_restore_controls',no_retired_controls),
               ('retired_light_styles_are_inert',lambda browser:retired_styles_are_inert(browser,'light')),
               ('retired_dark_styles_are_inert',lambda browser:retired_styles_are_inert(browser,'dark')),
               ('active_file_ignores_retired_overrides',active_file_isolated_from_retired_styles),
               ('cancelled_delete_preserves_file',cancelled_delete_preserves_file),
               ('failed_delete_preserves_confirmed_file',failed_delete_preserves_file),
               ('failed_read_preserves_confirmed_material',read_failure_preserves_current_material),
               ('default_conflict_needs_explicit_reload',default_conflict_requires_reload),
               ('invalid_active_metadata_uses_neutral_default',invalid_active_metadata_uses_neutral_fallback),
               ('untrusted_image_origins_are_not_loaded',rejected_image_origins_never_load),
               ('mismatched_selected_id_cannot_apply_material',mismatched_active_identity_is_inert)]
        if args.only_real_snapshot:cases=[]
        for name,operation in cases:run_case(name,lambda operation=operation:operation(browser))
        if REAL_SNAPSHOT is not None:run_case('actual_backend_file_only_snapshot',lambda:real_snapshot_is_file_only(browser))
        browser.close()
    run_case('no_external_requests',lambda:check(not base.BLOCKED,'External request outside controlled origins'))
    report={'tests':len(RESULTS),'failures':sum(not row['passed'] for row in RESULTS),'checkedAt':datetime.now(timezone.utc).isoformat(),
            'platform':'Linux Chromium with actual TypeScript, mock IPC/PNG; no WebView2 or native TSF acceptance','blockedOrigins':sorted(set(base.BLOCKED)),'results':RESULTS}
    (OUT/'builtin-skin-editor-ui.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
    (OUT/'builtin-skin-editor-ui.log').write_text('\n'.join(('PASS ' if row['passed'] else 'FAIL ')+row['name']+('\n'+row['error'] if not row['passed'] else '') for row in RESULTS)+'\n')
    suite=ET.Element('testsuite',name='RetiredBuiltinAndFileOnlySkinUi',tests=str(report['tests']),failures=str(report['failures']),errors='0',skipped='0')
    for row in RESULTS:
        case=ET.SubElement(suite,'testcase',name=row['name'])
        if not row['passed']:ET.SubElement(case,'failure').text=row['error']
    ET.ElementTree(suite).write(OUT/'builtin-skin-editor-ui.xml',encoding='utf-8',xml_declaration=True)
    print(json.dumps({key:report[key] for key in ('tests','failures','platform')},ensure_ascii=False));raise SystemExit(bool(report['failures']))

if __name__=='__main__':main()
