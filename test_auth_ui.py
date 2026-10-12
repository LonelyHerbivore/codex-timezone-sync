import json,base64,pathlib,os,shutil
from playwright.sync_api import sync_playwright
html=(pathlib.Path(__file__).parent/'dashboard.html').read_text(encoding='utf-8')
browser_candidates=[os.getenv('CODEX_TZ_CHROMIUM',''),shutil.which('chromium') or '',shutil.which('chromium-browser') or '',r'C:\Program Files\Google\Chrome\Application\chrome.exe',r'C:\Program Files\Microsoft\Edge\Application\msedge.exe']
browser_path=next((p for p in browser_candidates if p and pathlib.Path(p).exists()),None)
with sync_playwright() as p:
 b=p.chromium.launch(headless=True,executable_path=browser_path,args=['--no-sandbox'])
 for stored in [True,False]:
  page=b.new_page()
  page.evaluate('''() => {window.__store={};Object.defineProperty(window,'localStorage',{value:{getItem:k=>window.__store[k]??null,setItem:(k,v)=>window.__store[k]=v,clear:()=>window.__store={}}});}''')
  if stored:
   raw=json.dumps({'state':{'managementKey':'test-secret-key','rememberPassword':True}})
   page.evaluate('''raw=>{const key=new TextEncoder().encode('cli-proxy-api-webui::secure-storage|'+location.host+'|'+navigator.userAgent);const bytes=new TextEncoder().encode(raw);let s='';for(let i=0;i<bytes.length;i++)s+=String.fromCharCode(bytes[i]^key[i%key.length]);localStorage.setItem('cli-proxy-auth','enc::v1::'+btoa(s));}''',raw)
  page.evaluate('''() => {window.__calls=[];window.fetch=async(url,opt)=>{window.__calls.push(opt.headers.Authorization);const data=url.endsWith('/status')?{ok:true,available:true,timezone:'America/Los_Angeles',settings:{enabled:true,logging:true},age_seconds:1}:url.endsWith('/logs')?{events:[]}:url.endsWith('/stats')?{success:0,failure:0}:url.endsWith('/failures')?{records:[]}:{records:[]};return {ok:true,json:async()=>data};};}''')
  page.set_content(html)
  if not stored:
   page.wait_for_timeout(250)
   assert page.locator('#zone').inner_text()=='—'
   page.locator('#management-key').fill('test-secret-key')
   page.locator('#connect').click()
  page.locator('#zone').get_by_text('America/Los_Angeles').wait_for(timeout=5000)
  assert page.evaluate("window.__calls.includes('Bearer test-secret-key')")
  if not stored:assert page.evaluate("localStorage.getItem('cli-proxy-auth')") is None
  page.close()
 print('PASS: saved CPA login reused automatically; manual fallback; no plugin secret persistence')
 b.close()
