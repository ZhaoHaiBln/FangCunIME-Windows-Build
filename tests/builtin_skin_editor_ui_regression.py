"""Builtin edit UI against real TypeScript and deterministic, in-memory Tauri IPC.

Linux Chromium only: no Windows WebView2, native TSF, files, credentials or real AI.
Only loopback frontend assets and intercepted skin PNG fixtures can be requested.
"""
from __future__ import annotations
import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import importlib.util
from io import BytesIO
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
from PIL import Image
spec = importlib.util.spec_from_file_location("builtin_skin_fixture", Path(__file__).with_name("skin_ui_regression.py"))
assert spec and spec.loader
base = importlib.util.module_from_spec(spec); spec.loader.exec_module(base)
BASE = "http://127.0.0.1:5174/"
OUT = Path(tempfile.gettempdir()) / "fangcun-builtin-skin-editor"
RESULTS, BLOCKED = [], []
REAL_SNAPSHOT = None
EDIT_MOCK = r"""
(() => {
 const initial=__EDITOR_CONFIG__,f=window.__aiFixture,s=f.skin,clone=v=>JSON.parse(JSON.stringify(v));
 const e=f.builtin={styles:clone(initial.styles||{}),failure:null,defer:false,pending:[],
  change(command,args){
   if(args.revision!==s.revision)throw '皮肤列表已更新，请重新读取后重试';
   if(this.failure){const value=this.failure;this.failure=null;throw value;}
   if(command==='skin_save_builtin_style'){
    if(Object.keys(args.style).length)this.styles[args.id]=clone(args.style);else delete this.styles[args.id];
    if(args.activate){f.settings.skin=args.id;f.settings.customSkinId='';f.settings.customSkinHash='';s.hidden=s.hidden.filter(id=>id!==args.id);}
   }else delete this.styles[args.id];
   s.rotate();s.publish();return s.snapshot();
  },finish(){const p=this.pending.shift();if(!p)throw Error('No pending edit');try{p.resolve(this.change(p.command,p.args));}catch(error){p.reject(error);}}
 };
 const snapshot=s.snapshot.bind(s);s.snapshot=()=>({...snapshot(),builtinStyles:clone(e.styles)});
 const original=window.__TAURI_INTERNALS__.invoke.bind(window.__TAURI_INTERNALS__);
 window.__TAURI_INTERNALS__.invoke=async(command,args={})=>{
  if(command==='skin_library_snapshot' && initial.realSnapshot){f.calls.push({command,args:clone(args)});return clone(initial.realSnapshot);}
  if(['skin_save_builtin_style','skin_reset_builtin_style'].includes(command)){
   f.calls.push({command,args:clone(args)});
   if(e.defer)return await new Promise((resolve,reject)=>e.pending.push({command,args:clone(args),resolve,reject}));
   return e.change(command,args);
  }
  return original(command,args);
 };
})();
"""


def check(value, text):
    if not value: raise AssertionError(text)


@contextmanager
def app(browser, *, styles=None, active=False, tone="light", width=1000, height=820, real_snapshot=None):
    settings={**base.shared.SETTINGS,"themeMode":tone,"darkMode":tone=="dark","customSkinId":"","customSkinHash":""}
    if real_snapshot: settings["skin"]=real_snapshot["activeId"].removeprefix("builtin:")
    context=browser.new_context(viewport={"width":width,"height":height},reduced_motion="reduce",color_scheme=tone)
    config={"manifest":base.make_manifest(),"hash":base.HASH,"installed":active,"active":active}
    context.add_init_script(base.shared.MOCK.replace("__FIXTURE_CONFIG__",json.dumps({"settings":settings,"ai":base.shared.CONFIG}))+"\n"+base.SKIN_MOCK.replace("__SKIN_CONFIG__",json.dumps(config))+"\n"+EDIT_MOCK.replace("__EDITOR_CONFIG__",json.dumps({"styles":styles or {},"realSnapshot":real_snapshot})))
    origin=urlsplit(BASE)
    def route(request):
        url=urlsplit(request.request.url)
        if url.scheme=="http" and (url.hostname,url.port)==(origin.hostname,origin.port):request.continue_()
        elif url.scheme=="http" and url.hostname=="fcskin.localhost" and url.path.startswith('/'+base.HASH+'/'):
            request.fulfill(status=200,content_type="image/png",body=base.png())
        else: BLOCKED.append(url.scheme+"://"+(url.hostname or ""));request.abort()
    context.route("**/*",route)
    page=context.new_page();page.set_default_timeout(6500);errors=[]
    page.on("pageerror",lambda error:errors.append(type(error).__name__))
    try:
        page.goto(BASE,wait_until="networkidle");base.wait_idle(page);base.tab(page,"theme")
        yield page
        check(not errors,"Uncaught frontend exception")
        check(not page.evaluate("window.__aiFixture.unknownCommands.length"),"Unknown fixture IPC")
    finally:context.close()


def open_editor(page, skin="wood_type"):
    page.locator(f'button[data-builtin-edit="{skin}"]').click()
    expect(page.locator("#builtin-skin-editor")).to_be_visible()
    expect(page.locator("#builtin-editor-title")).to_contain_text(base.BUILTINS[skin])


def fill_color(page, name, value):
    page.locator("#builtin-color-"+name).fill(value)


def details(page, text):
    page.locator("#builtin-skin-editor details").filter(has=page.get_by_text(text,exact=True)).locator("summary").click()


def wait_save(page):
    expect(page.locator("#builtin-skin-editor")).to_be_hidden();base.wait_idle(page)


def original_style(page):
    return page.locator(".dual-cand-showcase .cand-window").first.evaluate("e=>{const s=getComputedStyle(e);return [s.backgroundImage,s.backgroundColor,s.borderRadius,s.boxShadow,s.fontFamily,e.getBoundingClientRect().height]}")


def all_cards(browser):
    with app(browser) as page:
        check(page.locator("#skin-group [data-skin]").count()==4,"No duplicate builtin copies")
        check(page.locator("button[data-builtin-edit]").count()==4,"Every builtin has an edit action")
        check(page.locator("button[data-builtin-reset]:enabled").count()==0,"Unmodified skins cannot be reset")
        original=original_style(page)
        for skin in base.BUILTINS:
            open_editor(page,skin);page.locator("#builtin-editor-cancel").click()
        check(page.evaluate("window.__aiFixture.count('skin_save_builtin_style')")==0,"Opening/cancelling cannot save")
        check(page.evaluate("window.__aiFixture.count('skin_select')")==0,"Editing/cancelling cannot bubble into builtin selection")
        check(original_style(page)==original,"Opening/cancelling must leave original material and geometry unchanged")
        check(page.evaluate("window.__aiFixture.skin.installed")==False,"Builtin editing cannot create local package copies")


def live_tint_cancel(browser, skin, tone):
    with app(browser,tone=tone) as page:
        original=original_style(page);open_editor(page,skin)
        page.locator(f'[data-builtin-tone="{tone}"]').click()
        candidate=page.locator("#builtin-editor-candidate")
        before=Image.open(BytesIO(candidate.screenshot())).convert("RGB")
        material_before=candidate.evaluate("e=>getComputedStyle(e).backgroundImage")
        fill_color(page,"candidateBackground","#1B3FFD")
        after=Image.open(BytesIO(candidate.screenshot())).convert("RGB")
        material_after=candidate.evaluate("e=>getComputedStyle(e).backgroundImage")
        check(before.size==after.size,"Tint may not alter candidate geometry")
        point=(before.width-18,max(8,before.height-15))
        b,a=before.getpixel(point),after.getpixel(point)
        check(sum(abs(x-y) for x,y in zip(a,b))>20,"Actual material pixels must visibly change after tint")
        if skin in ("wood_type","typewriter"):
            check("paper.png" in material_after and "paper.png" in material_before or tone=="dark","Light photo texture must stay in the tinted material")
        if skin=="terminal":check("repeating-linear-gradient" in material_after,"CRT scan lines must remain")
        check(page.evaluate("Object.keys(window.__aiFixture.builtin.styles).length")==0,"Preview must not write settings")
        page.locator("#builtin-editor-cancel").click()
        check(original_style(page)==original,"Cancel must restore untouched global appearance")


def mode_sparse_save(browser):
    with app(browser,tone="dark") as page:
        before=page.evaluate("JSON.stringify(window.__aiFixture.settings)")
        open_editor(page,"typewriter");fill_color(page,"candidateBackground","#123456")
        page.locator('[data-builtin-tone="dark"]').click();fill_color(page,"candidateText","#ABCDEF")
        page.locator("#builtin-editor-save").click();wait_save(page)
        args=page.evaluate("window.__aiFixture.latest('skin_save_builtin_style')")
        check(args['activate'] is True and args['id']=='typewriter',"Save/use must name the original builtin")
        check(args['style']=={'themes':{'light':{'colors':{'candidateBackground':'#123456'}},'dark':{'colors':{'candidateText':'#ABCDEF'}}}},"Only changed fields in both modes may be persisted")
        settings=page.evaluate("window.__aiFixture.settings")
        check(settings['themeMode']=='dark' and settings['skin']=='typewriter',"Preview tone cannot replace user mode")
        old=json.loads(before)
        for key in ('scheme','keyOpacity','firstCandidateSize','typingGlow','keyLetterSize','keyLetterBold'):
            check(settings[key]==old[key],"Save must preserve unrelated preference "+key)
        check(page.locator("#skin-group [data-skin]").count()==4 and page.locator("#skin-local-grid article").count()==0,"Saving must not generate duplicate skin cards")


def invalid_and_failure(browser):
    with app(browser) as page:
        open_editor(page);fill_color(page,"surface","#BAD")
        expect(page.locator("#builtin-editor-save")).to_be_disabled()
        expect(page.locator("#builtin-editor-error")).to_be_visible()
        check(page.evaluate("window.__aiFixture.count('skin_save_builtin_style')")==0,"Invalid hex must not reach backend")
        fill_color(page,"surface","#123456")
        page.evaluate("window.__aiFixture.builtin.failure='raw fixture-secret backend error'")
        page.locator("#builtin-editor-save").click()
        expect(page.locator("#skin-library-error")).to_be_visible()
        expect(page.locator("#skin-library-error")).not_to_contain_text("fixture-secret")
        expect(page.locator("#builtin-color-surface")).to_have_value("#123456")
        check(page.evaluate("Object.keys(window.__aiFixture.builtin.styles).length")==0,"Failed save must keep saved profile untouched")
        expect(page.locator("#builtin-editor-save")).to_be_enabled()


def conflict_rebase(browser):
    with app(browser) as page:
        open_editor(page);fill_color(page,"selection","#654321")
        page.evaluate("window.__aiFixture.skin.rotate()")
        page.locator("#builtin-editor-save").click()
        expect(page.locator("#builtin-editor-save")).to_be_disabled()
        expect(page.locator("#builtin-color-selection")).to_have_value("#654321")
        page.locator("#builtin-editor-reload").click()
        expect(page.locator("#builtin-editor-save")).to_be_enabled()
        expect(page.locator("#builtin-color-selection")).to_have_value("#654321")
        page.locator("#builtin-editor-save").click();wait_save(page)
        check(page.evaluate("window.__aiFixture.builtin.styles.wood_type.themes.light.colors.selection")=="#654321","Explicit re-read must retain the reviewable draft")


def restore_only_one(browser):
    styles={'wood_type':{'themes':{'light':{'colors':{'candidateBackground':'#123456'}}}},'typewriter':{'fontFamily':'mono'}}
    with app(browser,styles=styles) as page:
        before=page.evaluate("JSON.stringify(window.__aiFixture.settings)")
        page.locator('button[data-builtin-reset="wood_type"]').click();base.wait_idle(page)
        saved=page.evaluate("window.__aiFixture.builtin.styles")
        check(saved=={'typewriter':{'fontFamily':'mono'}},"Restore must remove only selected builtin profile")
        after=page.evaluate("window.__aiFixture.settings")
        for key,value in json.loads(before).items():
            if key!='revision':check(after[key]==value,"Restore must preserve setting "+key)
        expect(page.locator('button[data-builtin-reset="wood_type"]')).to_be_disabled()


def font_press_glow(browser):
    with app(browser) as page:
        open_editor(page);details(page,"字体和按压效果")
        page.locator("#builtin-editor-font").select_option("mono")
        check("Consolas" in page.locator("#builtin-editor-candidate .cand-word").first.evaluate("e=>getComputedStyle(e).fontFamily"),"Font must affect actual candidate glyph CSS")
        page.locator("#builtin-editor-use-press").check()
        page.locator("#builtin-editor-press").evaluate("e=>{e.value='1.9';e.dispatchEvent(new Event('input',{bubbles:true}));}")
        button=page.locator(".builtin-press-preview");button.scroll_into_view_if_needed();button.hover();page.mouse.down()
        try:check("1.9px" in button.evaluate("e=>getComputedStyle(e).translate"),"Press offset must affect drawn feedback")
        finally:page.mouse.up()
        details(page,"打字流光颜色")
        colors=['#123456','#234567','#345678','#456789']
        for i,value in enumerate(colors):page.locator('#builtin-glow-'+str(i)).fill(value)
        page.get_by_role('button',name='流光颜色 3恢复默认',exact=True).click()
        for i in (0,1,3):expect(page.locator('#builtin-glow-'+str(i))).to_have_value(colors[i])
        check(page.locator('#builtin-glow-2').input_value()!=colors[2],"One default action must restore its own channel")
        page.locator('#builtin-glow-2').fill(colors[2])
        page.locator("#builtin-editor-save").click();wait_save(page)
        saved=page.evaluate("window.__aiFixture.builtin.styles.wood_type")
        check(saved['fontFamily']=='mono' and saved['pressOffsetDp']==1.9 and saved['themes']['light']['glow']==colors,"Typography, press and all four glow channels must persist")
        css=page.evaluate("(()=>{const s=getComputedStyle(document.documentElement);return ['--glow-a','--glow-b','--fc-builtin-glow-c','--fc-builtin-glow-d'].map(v=>s.getPropertyValue(v).trim());})()")
        check(css==[color+'88' for color in colors],"Every saved glow channel must reach the active renderer")


def local_pack_isolation(browser):
    with app(browser,active=True,styles={'wood_type':{'themes':{'light':{'colors':{'background':'#FF0000'}}}}}) as page:
        expect(page.locator("html")).to_have_attribute("data-local-skin","fixture.green")
        check(page.locator("html").get_attribute("data-builtin-edit") is None,"Local pack must take priority over saved builtin edit")
        open_editor(page)
        check(page.locator("#builtin-editor-material .fc-skin-image").count()==0,"Editor preview cannot inherit the active local package's material PNGs")
        fill_color(page,"surface","#AABBCC");page.locator("#builtin-editor-cancel").click()
        expect(page.locator("html")).to_have_attribute("data-local-skin","fixture.green")


def pending_and_minimum(browser):
    with app(browser,width=640,height=540) as page:
        open_editor(page,"brutal");fill_color(page,"surface","#334455")
        page.evaluate("window.__aiFixture.builtin.defer=true")
        page.locator("#builtin-editor-save").click()
        expect(page.locator("#builtin-editor-cancel")).to_be_disabled()
        expect(page.locator("#builtin-color-surface")).to_be_disabled()
        expect(page.locator('button[data-builtin-edit="typewriter"]')).to_be_disabled()
        page.evaluate("window.__aiFixture.builtin.finish()")
        wait_save(page);open_editor(page,"brutal")
        for control in ('#builtin-color-surface','#builtin-editor-save','#builtin-editor-cancel'):
            target=page.locator(control);target.scroll_into_view_if_needed();box=target.bounding_box()
            check(box and box['x']>=0 and box['x']+box['width']<=640 and box['y']>=0 and box['y']+box['height']<=540,"Minimum window control must remain reachable: "+control)
        page.screenshot(path=str(OUT/'builtin-editor-minimum.png'))



def real_snapshot_values(browser):
    snapshot=REAL_SNAPSHOT
    check(snapshot is not None,"Real backend JSON must be supplied")
    with app(browser,real_snapshot=snapshot) as page:
        check(page.locator("#skin-group [data-skin]").count()==4,"Real snapshot must retain four original builtin cards")
        for skin in base.BUILTINS:
            profile=snapshot.get('builtinStyles',{}).get(skin,{})
            open_editor(page,skin)
            expect(page.locator('#builtin-editor-font')).to_have_value(profile.get('fontFamily',''))
            for tone in ('light','dark'):
                page.locator('[data-builtin-tone="'+tone+'"]').click()
                value=profile.get('themes',{}).get(tone,{})
                for key,color in value.get('colors',{}).items():expect(page.locator('#builtin-color-'+key)).to_have_value(color.upper())
                for index,color in enumerate(value.get('glow',[])):expect(page.locator('#builtin-glow-'+str(index))).to_have_value(color.upper())
            page.locator('#builtin-editor-cancel').click()
        check(page.evaluate("window.__aiFixture.count('skin_save_builtin_style')")==0,"Reading actual snapshots must never write them back")
        check(page.evaluate("window.__aiFixture.count('skin_reset_builtin_style')")==0,"Reading actual snapshots must never reset a builtin")


def run_case(name, call):
    try:call();RESULTS.append({'name':name,'passed':True})
    except Exception:RESULTS.append({'name':name,'passed':False,'error':traceback.format_exc().replace('fixture-secret','[sample redacted]')})


def main():
    global BASE,OUT,REAL_SNAPSHOT
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--base-url',default=BASE);parser.add_argument('--output',type=Path,default=OUT);parser.add_argument('--browser-executable',default=shutil.which('chromium') or shutil.which('chromium-browser'));parser.add_argument('--real-snapshot',type=Path);parser.add_argument('--only-real-snapshot',action='store_true')
    args=parser.parse_args();parsed=urlsplit(args.base_url)
    if parsed.scheme!='http' or parsed.hostname not in ('127.0.0.1','localhost','::1') or parsed.username or parsed.password or parsed.query or parsed.fragment:parser.error('Only loopback frontend origins are allowed')
    BASE=args.base_url.rstrip('/')+'/';OUT=args.output.resolve();OUT.mkdir(parents=True,exist_ok=True)
    if args.only_real_snapshot and not args.real_snapshot:parser.error('--only-real-snapshot requires --real-snapshot')
    if args.real_snapshot:REAL_SNAPSHOT=json.loads(args.real_snapshot.read_text())
    with sync_playwright() as playwright:
        browser=playwright.chromium.launch(executable_path=args.browser_executable,headless=True,args=['--no-sandbox','--disable-dev-shm-usage'] if sys.platform.startswith('linux') else [])
        cases=[('all_four_edit_without_copies',all_cards),('mode_sparse_save_preserves_preferences',mode_sparse_save),('invalid_and_failed_save_preserve_draft',invalid_and_failure),('cas_explicit_rebase_preserves_draft',conflict_rebase),('restore_only_one_builtin',restore_only_one),('font_press_and_four_glow_channels',font_press_glow),('active_local_pack_isolation',local_pack_isolation),('pending_controls_and_minimum_window',pending_and_minimum)]
        if args.only_real_snapshot:cases=[]
        for name,call in cases:run_case(name,lambda call=call:call(browser))
        if not args.only_real_snapshot:
            for skin in base.BUILTINS:
                for tone in ('light','dark'):run_case('material_tint_cancel_'+skin+'_'+tone,lambda skin=skin,tone=tone:live_tint_cancel(browser,skin,tone))
        if REAL_SNAPSHOT is not None:run_case('real_backend_snapshot_values',lambda:real_snapshot_values(browser))
        browser.close()
    run_case('no_external_requests',lambda:check(not BLOCKED,'External request outside controlled origins'))
    report={'tests':len(RESULTS),'failures':sum(not item['passed'] for item in RESULTS),'checkedAt':datetime.now(timezone.utc).isoformat(),'platform':'Linux Chromium with actual TypeScript, mocked IPC/PNG; no WebView2 or native TSF acceptance','blockedOrigins':sorted(set(BLOCKED)),'results':RESULTS}
    (OUT/'builtin-skin-editor-ui.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
    (OUT/'builtin-skin-editor-ui.log').write_text('\n'.join(('PASS ' if item['passed'] else 'FAIL ')+item['name']+('\n'+item['error'] if not item['passed'] else '') for item in RESULTS)+'\n')
    suite=ET.Element('testsuite',name='BuiltinSkinEditor',tests=str(report['tests']),failures=str(report['failures']),errors='0',skipped='0')
    for item in RESULTS:
        case=ET.SubElement(suite,'testcase',name=item['name'])
        if not item['passed']:ET.SubElement(case,'failure').text=item['error']
    ET.ElementTree(suite).write(OUT/'builtin-skin-editor-ui.xml',encoding='utf-8',xml_declaration=True)
    print(json.dumps({key:report[key] for key in ('tests','failures','platform')},ensure_ascii=False));raise SystemExit(bool(report['failures']))

if __name__=='__main__':main()
