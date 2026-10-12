#!/usr/bin/env python3
"""Headless browser UI smoke test: isolated data URL + mocked management fetch."""
import pathlib
import os
import shutil
from playwright.sync_api import sync_playwright
ROOT=pathlib.Path(__file__).resolve().parent
html=(ROOT/'dashboard.html').read_bytes()
browser_candidates=[
    os.getenv("CODEX_TZ_CHROMIUM",""),
    shutil.which("chromium") or "",
    shutil.which("chromium-browser") or "",
    shutil.which("google-chrome") or "",
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
]
browser_path=next((p for p in browser_candidates if p and pathlib.Path(p).exists()),None)
with sync_playwright() as p:
    browser=p.chromium.launch(headless=True,executable_path=browser_path,args=['--no-sandbox'])
    page=browser.new_page(viewport={'width':1536,'height':1024},device_scale_factor=1)
    errors=[];page.on('pageerror',lambda exc:errors.append(str(exc)))
    page.evaluate('''
        window.__mockCalls=[];
        window.__mockConfig={enabled:true,logging:true,debug:false};
        window.fetch=async(url,options={})=>{
          if(options.headers?.Authorization!=='Bearer test-secret-key')throw Error('missing management authentication');
          window.__mockCalls.push({url,method:options.method||'GET'});
          let response={};
          if(url.endsWith('/status'))response={ok:true,available:true,timezone:'America/Los_Angeles',date:'2026-10-12',abbreviation:'PDT',offset:'-07:00',ip:'66.92.223.4',age_seconds:90,settings:window.__mockConfig};
          else if(url.endsWith('/logs'))response={ok:true,events:[{timestamp:1791786000,result:'rewritten',timezone:'America/Los_Angeles',timezone_before:'Asia/Shanghai'},{timestamp:1791785700,result:'skipped',timezone:'America/Los_Angeles'}]};
          else if(url.endsWith('/models'))response={ok:true,matched:1,mismatched:1,unknown:1,records:[{timestamp:1791786000,request_id:'test-1',requested_model:'gpt-6-astra',routed_model:'gpt-6-astra',upstream_model:'gpt-5.6-sol',timezone_before:'Asia/Shanghai',timezone_after:'America/Los_Angeles',status:'mismatched',channel:'sse'},{timestamp:1791786001,request_id:'test-2',requested_model:'gpt-6-astra',routed_model:'gpt-6-astra',upstream_model:'gpt-6-astra',status:'matched',channel:'response'}]};
          else if(url.endsWith('/activity/clear'))response={ok:true};
          else if(url.endsWith('/stats'))response={ok:true,success:17,failure:2};
          else if(url.endsWith('/stats/clear'))response={ok:true};
          else if(url.endsWith('/egress/refresh'))response={ok:true,timezone:'America/Los_Angeles',ip:'66.92.223.4'};
          else if(url.endsWith('/failures'))response={ok:true,records:[{id:'123456',timestamp:1791786000,reason:'state_unavailable',size:38}]};
          else if(url.endsWith('/failures/view'))response={ok:true,reason:'state_unavailable',kind:'decoded-body',body_base64:btoa('{"input":[{"content":"example request"}]}')};
          else if(url.endsWith('/failures/clear'))response={ok:true};
          else if(url.endsWith('/settings')){Object.assign(window.__mockConfig,JSON.parse(options.body));response={ok:true};}
          else if(url.endsWith('/selftest'))response={ok:true,timezone:'America/Los_Angeles',mode:'dry-run'};
          else throw Error('unknown endpoint: '+url);
          return new Response(JSON.stringify(response),{status:200,headers:{'Content-Type':'application/json'}});
        };
        undefined;
    ''')
    page.set_content(html.decode('utf-8'))
    assert page.title()=='Codex Timezone Sync'
    page.locator('#management-key').fill('test-secret-key')
    page.locator('#connect').click()
    page.locator('#zone').get_by_text('America/Los_Angeles').wait_for()
    assert page.locator('#ip').inner_text()=='66.92.223.4'
    assert '运行中' in page.locator('#sync-state').inner_text()
    page.screenshot(path=str(ROOT/'preview.png'),full_page=True)
    assert page.locator('[data-view="log"]').count()==0
    assert page.locator('[data-view="models"]').count()==0
    assert 'Asia/Shanghai' in page.locator('#recent-body').inner_text()
    assert 'gpt-5.6-sol' in page.locator('#recent-body').inner_text()
    assert page.locator('#recent-body tr').count()>=2
    assert page.locator('#count-success').inner_text()=='17'
    assert page.locator('#count-failure').inner_text()=='2'
    page.locator('#probe-egress').click()
    page.get_by_text('出口时区检测完成',exact=False).wait_for()
    assert '出口时区不可用' in page.locator('#failure-body').inner_text()
    page.locator('#failure-body button').first.click()
    assert 'example request' in page.locator('#failure-json').inner_text()
    page.locator('#clear-failures').click()
    assert '暂无校正失败日志' in page.locator('#failure-body').inner_text()
    page.locator('#clear-stats').click()
    page.get_by_text('校正统计已清空').wait_for()
    assert page.locator('#count-success').inner_text()=='0'
    assert page.locator('#recent-body tr').first.locator('td').nth(1).inner_text()=='Codex'
    page.locator('#clear-activity').click()
    page.get_by_text('最近活动已清空').wait_for()
    assert '暂无活动记录' in page.locator('#recent-body').inner_text()
    page.locator('#toggle-log').uncheck()
    page.locator('#save').click()
    page.get_by_text('配置已保存并立即生效').wait_for()
    assert page.evaluate('window.__mockConfig.logging') is False
    page.locator('#selftest').click()
    page.get_by_text('本地自检通过',exact=False).wait_for()
    page.locator('[data-view="settings"]').click()
    assert page.get_by_text('状态有效期').is_visible()
    assert not errors,errors
    assert len(page.evaluate('window.__mockCalls'))>=5
    page.set_viewport_size({'width':390,'height':844})
    assert page.evaluate('document.documentElement.scrollWidth <= window.innerWidth + 2'), 'Mobile layout overflows horizontally'
    page.locator('[data-view="overview"]').click()
    assert page.locator('#refresh').is_visible()
    print('browser UI PASS: merged activity, old timezone, model data, settings, selftest, mobile')
    browser.close()
