#!/usr/bin/env python3
"""Contract tests against the built C ABI .so (no Codex credentials)."""
import base64
import ctypes as ct
import json
import os
import pathlib
import stat
import socket
import sys
import threading
import time
import tempfile
import unittest
from datetime import datetime, timedelta, timezone

ROOT = pathlib.Path(__file__).resolve().parent

class Buffer(ct.Structure):
    _fields_ = [('ptr', ct.c_void_p), ('len', ct.c_size_t)]

Call = ct.CFUNCTYPE(ct.c_int, ct.c_char_p, ct.c_void_p, ct.c_size_t, ct.POINTER(Buffer))
Free = ct.CFUNCTYPE(None, ct.c_void_p, ct.c_size_t)
Shutdown = ct.CFUNCTYPE(None)

class API(ct.Structure):
    _fields_ = [('abi_version', ct.c_uint32), ('call', Call), ('free_buffer', Free), ('shutdown', Shutdown)]

LIB = ct.CDLL(str(ROOT / ('codex-timezone-sync.dll' if sys.platform == 'win32' else 'codex-timezone-sync.dylib' if sys.platform == 'darwin' else 'codex-timezone-sync.so')))
LIB.cliproxy_plugin_init.argtypes = [ct.c_void_p, ct.POINTER(API)]
LIB.cliproxy_plugin_init.restype = ct.c_int
API_OBJ = API()
assert LIB.cliproxy_plugin_init(None, ct.byref(API_OBJ)) == 0
assert API_OBJ.abi_version == 1

def call(method, payload=None):
    encoded = b'' if payload is None else json.dumps(payload, ensure_ascii=False).encode()
    mem = ct.create_string_buffer(encoded)
    response = Buffer()
    status = API_OBJ.call(method.encode(), mem, len(encoded), ct.byref(response))
    assert status == 0, status
    try:
        return json.loads(ct.string_at(response.ptr, response.len))
    finally:
        API_OBJ.free_buffer(response.ptr, response.len)

def invoke(body, fmt='codex', wrap=False):
    envelope = {'ToFormat': fmt, 'Body': base64.b64encode(json.dumps(body, ensure_ascii=False).encode()).decode()}
    if wrap:
        envelope = {'request': envelope}
    res = call('request.intercept_after', envelope)
    assert res['ok']
    enc = res.get('result', {}).get('Body')
    return json.loads(base64.b64decode(enc)) if enc else None

class TestTimezonePlugin(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.path = pathlib.Path(cls.temp.name) / 'timezone.state'
        cls.path.write_text('America/Los_Angeles|2026-10-11|PDT|-07:00\n66.92.223.4\n')
        os.environ['CODEX_TZ_STATE_FILE'] = str(cls.path)
        cls.config = pathlib.Path(cls.temp.name) / 'timezone.config'
        os.environ['CODEX_TZ_CONFIG_FILE'] = str(cls.config)
        cls.logfile = pathlib.Path(cls.temp.name) / 'timezone.events'
        os.environ['CODEX_TZ_LOG_FILE'] = str(cls.logfile)
        cls.statsfile = pathlib.Path(cls.temp.name) / 'timezone.stats'
        os.environ['CODEX_TZ_STATS_FILE'] = str(cls.statsfile)
        cls.socketpath = pathlib.Path(cls.temp.name) / 'refresh.sock'
        os.environ['CODEX_TZ_REFRESH_SOCKET'] = str(cls.socketpath)
        cls.failfile = pathlib.Path(cls.temp.name) / 'timezone.failures'
        os.environ['CODEX_TZ_FAILURE_FILE'] = str(cls.failfile)

    @classmethod
    def tearDownClass(cls):
        os.environ.pop('CODEX_TZ_STATE_FILE', None)
        os.environ.pop('CODEX_TZ_CONFIG_FILE', None)
        os.environ.pop('CODEX_TZ_LOG_FILE', None)
        os.environ.pop('CODEX_TZ_STATS_FILE', None)
        os.environ.pop('CODEX_TZ_REFRESH_SOCKET', None)
        os.environ.pop('CODEX_TZ_FAILURE_FILE', None)
        cls.temp.cleanup()

    def test_register(self):
        reg = call('plugin.register')
        self.assertTrue(reg['result']['capabilities']['request_interceptor'])
        self.assertEqual(reg['result']['schema_version'], 1)

    def test_management_panel_registration_and_html(self):
        reg = call('plugin.register')['result']
        self.assertTrue(reg['capabilities']['management_api'])
        menus = call('management.register')['result']['Resources']
        self.assertEqual(menus[0]['Path'], '/panel')
        page = call('management.handle', {'Method':'GET', 'Path':'/v0/resource/plugins/codex-timezone-sync/panel'})['result']
        self.assertEqual(page['StatusCode'], 200)
        self.assertEqual(page['Headers']['Content-Type'], ['text/html; charset=utf-8'])
        html = base64.b64decode(page['Body']).decode('utf-8')
        self.assertIn('Codex Timezone Sync', html)
        self.assertNotIn('SECRET', html)
        bad = call('management.handle', {'Method':'GET', 'Path':'/unknown'})['result']
        self.assertEqual(bad['StatusCode'], 404)

    def _management(self, path, method='GET', data=None):
        request={'Path':f'/v0/management/plugins/codex-timezone-sync/{path}', 'Method':method}
        if data is not None:
            request['Body']=base64.b64encode(json.dumps(data).encode()).decode()
        response=call('management.handle',request)['result']
        return response['StatusCode'],json.loads(base64.b64decode(response['Body']))

    def test_activity_clear_is_durable(self):
        self.config.unlink(missing_ok=True)
        self.logfile.unlink(missing_ok=True)
        self.assertIsNotNone(invoke({'input': []}))
        self.assertTrue(self._management('logs')[1]['events'])
        self.assertEqual(self._management('activity/clear','POST'),(200,{'ok':True}))
        self.assertEqual(self._management('logs')[1]['events'],[])
        self.assertEqual(self._management('models')[1]['records'],[])
        self.assertEqual(self.logfile.read_text(),'')

    def test_persistent_counters_and_reset(self):
        self.config.unlink(missing_ok=True)
        self.statsfile.unlink(missing_ok=True)
        self.assertEqual(self._management('stats')[1]['success'],0)
        self.assertIsNotNone(invoke({'input':[]}))
        self.assertIsNone(invoke({'instructions':'[CPAMC_TIMEZONE_V1] already synced'}))
        self.assertEqual(self._management('stats')[1],{'ok':True,'success':1,'failure':0})
        self.path.rename(self.path.with_suffix('.missing'))
        try:
            self.assertIsNone(invoke({'input':[]}))
        finally:
            self.path.with_suffix('.missing').rename(self.path)
        self.assertEqual(self._management('stats')[1]['failure'],1)
        self.assertEqual(self._management('stats/clear','POST'),(200,{'ok':True}))
        self.assertEqual(self._management('stats')[1],{'ok':True,'success':0,'failure':0})

    def test_manual_egress_refresh_invokes_helper_and_reads_new_state(self):
        before=self.path.read_text()
        try:
            code,result=self._management('egress/refresh','POST')
            self.assertEqual(code,503)
            self.assertFalse(result['ok'])
            if os.name == 'nt':
                request=pathlib.Path(str(self.socketpath)+'.request')
                ready=pathlib.Path(str(self.socketpath)+'.ready')
                ready.touch()
                def serve_once():
                    deadline=time.monotonic()+8
                    while not request.exists() and time.monotonic()<deadline:
                        time.sleep(.05)
                    self.assertTrue(request.exists())
                    nonce=request.read_text(encoding='ascii').strip()
                    request.unlink(missing_ok=True)
                    self.path.write_text('America/New_York|2026-10-12|EDT|-04:00\n198.51.100.9\n')
                    pathlib.Path(str(self.socketpath)+'.'+nonce+'.reply').write_text('OK\n')
                thread=threading.Thread(target=serve_once,daemon=True)
                thread.start()
                try:
                    code,result=self._management('egress/refresh','POST')
                    thread.join(timeout=3)
                finally:
                    ready.unlink(missing_ok=True)
                    request.unlink(missing_ok=True)
            else:
                with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as server:
                    server.bind(str(self.socketpath))
                    server.listen(1)
                    def serve_once():
                        conn,_=server.accept()
                        with conn:
                            self.assertEqual(conn.recv(32),b'REFRESH\n')
                            self.path.write_text('America/New_York|2026-10-12|EDT|-04:00\n198.51.100.9\n')
                            conn.sendall(b'OK\n')
                    thread=threading.Thread(target=serve_once,daemon=True)
                    thread.start()
                    code,result=self._management('egress/refresh','POST')
                    thread.join(timeout=3)
            self.assertEqual(code,200)
            self.assertEqual(result['timezone'],'America/New_York')
            self.assertEqual(result['ip'],'198.51.100.9')
        finally:
            self.path.write_text(before)
            self.socketpath.unlink(missing_ok=True)

    def test_failure_log_only_on_failed_rewrites_and_full_body_view(self):
        self.config.unlink(missing_ok=True)
        self.failfile.unlink(missing_ok=True)
        self.assertIsNotNone(invoke({'input':[{'content':'normal message'}]}))
        self.assertEqual(self._management('failures')[1]['records'],[])
        previous=self.path.read_text()
        original={'input':[{'role':'user','content':'example failed request'}],'model':'codex-test'}
        try:
            self.path.unlink()
            self.assertIsNone(invoke(original))
        finally:
            self.path.write_text(previous)
        code,result=self._management('failures')
        self.assertEqual(code,200)
        self.assertEqual(len(result['records']),1)
        failure=result['records'][0]
        self.assertEqual(failure['reason'],'state_unavailable')
        if os.name != 'nt':
            self.assertEqual(stat.S_IMODE(self.failfile.stat().st_mode),0o600)
        code,view=self._management('failures/view','POST',{'id':failure['id']})
        self.assertEqual(code,200)
        self.assertEqual(view['kind'],'decoded-body')
        self.assertEqual(json.loads(base64.b64decode(view['body_base64'])),original)
        self.assertEqual(self._management('failures/clear','POST'),(200,{'ok':True}))
        self.assertEqual(self._management('failures')[1]['records'],[])
        self.assertEqual(self._management('failures/view','POST',{'id':failure['id']})[0],404)

    def test_failure_log_invalid_base64(self):
        self.config.unlink(missing_ok=True)
        self.failfile.unlink(missing_ok=True)
        call('request.intercept_after',{'ToFormat':'codex','Body':'not-base64!'})
        row=self._management('failures')[1]['records'][0]
        self.assertEqual(row['reason'],'invalid_base64')
        detail=self._management('failures/view','POST',{'id':row['id']})[1]
        self.assertEqual(base64.b64decode(detail['body_base64']),b'not-base64!')

    def test_status_and_config_persistence(self):
        self.config.unlink(missing_ok=True)
        code, status=self._management('status')
        self.assertEqual(code,200)
        self.assertTrue(status['available'])
        self.assertEqual(status['timezone'],'America/Los_Angeles')
        self.assertEqual(status['ip'],'66.92.223.4')
        self.assertEqual(status['settings'],{'enabled':True,'logging':True,'debug':False})
        code, saved=self._management('settings','PUT',{'enabled':False,'debug':True})
        self.assertEqual(code,200)
        self.assertFalse(saved['settings']['enabled'])
        self.assertTrue(saved['settings']['logging'])
        self.assertTrue(saved['settings']['debug'])
        self.assertIsNone(invoke({'input':[]}))
        code,status=self._management('status')
        self.assertFalse(status['settings']['enabled'])
        self.assertEqual(code,200)
        self.assertEqual(self._management('settings','PUT',{'enabled':'false'})[0],400)
        self.assertEqual(self._management('settings','PUT',{'garbage':True})[0],400)
        self.assertEqual(self._management('status')[1]['settings'],status['settings'])
        self.assertEqual(self._management('settings','PUT',{'enabled':True, 'debug':False})[0],200)
        self.assertIn('America/Los_Angeles',invoke({'input':[]})['instructions'])

    def test_logging_sanitizes_request_and_obeys_switch(self):
        self.config.unlink(missing_ok=True)
        self.logfile.unlink(missing_ok=True)
        secret='user-prompt-private-example sk-abcdefg'
        self.assertIsNotNone(invoke({'input':[{'role':'user','content':secret}]}))
        code,logs=self._management('logs')
        self.assertEqual(code,200)
        self.assertEqual(len(logs['events']),1)
        self.assertEqual(logs['events'][0]['result'],'rewritten')
        self.assertEqual(logs['events'][0]['timezone'],'America/Los_Angeles')
        self.assertNotIn(secret,self.logfile.read_text())
        self.assertNotIn('input',self.logfile.read_text())
        self.assertEqual(self._management('settings','PUT',{'logging':False})[0],200)
        invoke({'input':[{'role':'user','content':'next private message'}]})
        self.assertEqual(len(self._management('logs')[1]['events']),1)
        self.assertEqual(self._management('settings','PUT',{'logging':True})[0],200)
        invoke({'input':[]})
        self.assertEqual(len(self._management('logs')[1]['events']),2)

    def test_log_recovery_from_corrupt_records(self):
        self.logfile.write_text('invalid-data\n1791786000\trewritten\tAmerica/Los_Angeles\n')
        code, data=self._management('logs')
        self.assertEqual(code, 200)
        self.assertEqual(len(data['events']), 1)
        self.assertEqual(data['events'][0]['result'], 'rewritten')

    @unittest.skipIf(os.name == 'nt', 'symlink creation requires extra privilege on Windows')
    def test_no_symlink_follow_for_event_read(self):
        self.config.unlink(missing_ok=True)
        protected=pathlib.Path(self.temp.name) / 'private.log'
        protected.write_text('do not expose this')
        self.logfile.unlink(missing_ok=True)
        self.logfile.symlink_to(protected)
        try:
            self.assertEqual(self._management('logs')[1]['events'], [])
            invoke({'input':[]})
            self.assertEqual(protected.read_text(), 'do not expose this')
        finally:
            self.logfile.unlink(missing_ok=True)

    def test_selftest_is_dry_run_and_respects_disabled(self):
        self.config.unlink(missing_ok=True)
        self.assertEqual(self._management('selftest','POST')[0],200)
        self.assertEqual(self._management('selftest','POST')[1]['mode'],'dry-run')
        self.assertEqual(self._management('settings','PUT',{'enabled':False})[0],200)
        self.assertEqual(self._management('selftest','POST')[0],409)
        self.assertEqual(self._management('settings','PUT',{'enabled':True})[0],200)
        original=self.path.read_bytes()
        self.path.unlink()
        try:
            self.assertEqual(self._management('selftest','POST')[0],503)
        finally:
            self.path.write_bytes(original)

    def test_before_auth_is_noop(self):
        self.assertEqual(call('request.intercept_before', {})['result'], {})

    def test_native_instructions_unmodified_except_append(self):
        req = {'model':'gpt-5.5','instructions':'Behave nicely.\n原样保留中文和表情 😀','input':[{'role':'user','content':'hi'}]}
        out = invoke(req)
        self.assertEqual(out['input'], req['input'])
        self.assertIn('原样保留中文和表情 😀', out['instructions'])
        self.assertIn('Current date: ' + datetime.now(timezone(timedelta(hours=-7))).strftime('%Y-%m-%d'), out['instructions'])
        self.assertIn('America/Los_Angeles', out['instructions'])
        self.assertIn('UTC-07:00', out['instructions'])

    def test_native_absent_instructions(self):
        req={'model':'gpt-5.5','input':[{'role':'user','content':'hello'}]}
        out=invoke(req,wrap=True)
        self.assertIn('CPAMC_TIMEZONE_V1',out['instructions'])
        self.assertEqual(out['input'],req['input'])

    def test_chat_messages(self):
        req={'model':'gpt-5.5','messages':[{'role':'user','content':'hello'}]}
        out=invoke(req)
        self.assertEqual(out['messages'][0]['role'],'developer')
        self.assertIn('Current timezone',out['messages'][0]['content'])
        self.assertEqual(out['messages'][1],req['messages'][0])

    def test_null_instructions(self):
        out=invoke({'instructions':None,'input':[]})
        self.assertIn('Current date:',out['instructions'])

    def test_empty_messages(self):
        self.assertEqual(len(invoke({'messages':[]})['messages']),1)

    def test_non_codex_noop(self):
        self.assertIsNone(invoke({'instructions':'hello','input':[]},fmt='claude'))

    def test_fail_open_malformed(self):
        payload={'ToFormat':'codex','Body':'NOT-BASE64'}
        self.assertEqual(call('request.intercept_after',payload)['result'],{})

    def test_idempotence(self):
        req={'model':'gpt-5.5','instructions':'Test.','input':[]}
        first=invoke(req)
        second=invoke(first)
        self.assertIsNone(second)

    def test_missing_state_fail_open(self):
        backup=self.path.read_text()
        self.path.unlink()
        try:
            self.assertIsNone(invoke({'input':[]}))
        finally:
            self.path.write_text(backup)

    def test_state_update_without_restart(self):
        req={'model':'gpt-5.5','input':[]}
        self.path.write_text('America/New_York|2026-10-12|EDT|-04:00\n2.59.182.244\n')
        self.assertIn('America/New_York', invoke(req)['instructions'])
        self.path.write_text('America/Los_Angeles|2026-10-11|PDT|-07:00\n66.92.223.4\n')


    def test_replace_existing_environment_and_date(self):
        local_date=datetime.now(timezone(timedelta(hours=-7))).strftime('%Y-%m-%d')
        req={'input':[{'role':'developer','content':
              '<environment_context>\n<current_date>2001-01-01</current_date>\n<timezone>Asia/Shanghai</timezone>\n</environment_context>'}],
             'tools':[{'type':'function','name':'clock','parameters':{'timezone':'Asia/Shanghai'}}]}
        out=invoke(req)
        self.assertNotIn('instructions',out)
        self.assertIn('<timezone>America/Los_Angeles</timezone>',out['input'][0]['content'])
        self.assertIn('<current_date>'+local_date+'</current_date>',out['input'][0]['content'])
        self.assertEqual(out['tools'],req['tools'])
        self.assertIsNone(invoke(out))

    def test_multiple_environment_blocks_and_idempotence(self):
        req={'instructions':'Header\n<environment_context><timezone>Asia/Taipei</timezone></environment_context>\n'
            '<environment_context><timezone>Europe/Paris</timezone><current_date>1999-01-01</current_date></environment_context>',
             'input':[{'role':'user','content':'plain user words'}]}
        out=invoke(req)
        self.assertEqual(out['instructions'].count('<timezone>America/Los_Angeles</timezone>'),2)
        self.assertNotIn('Asia/Taipei',out['instructions'])
        self.assertNotIn('Europe/Paris',out['instructions'])
        self.assertIsNone(invoke(out))

    def test_inject_missing_environment_elements(self):
        req={'messages':[{'role':'system','content':'<environment_context><cwd>/tmp</cwd></environment_context>'}]}
        out=invoke(req)
        text=out['messages'][0]['content']
        self.assertIn('<cwd>/tmp</cwd>',text)
        self.assertIn('<timezone>America/Los_Angeles</timezone>',text)
        self.assertIn('<current_date>',text)
        self.assertEqual(len(out['messages']),1)
        self.assertIsNone(invoke(out))

    def test_unicode_json_escaped_context(self):
        source={'input':[{'role':'developer','content':
                '<environment_context><timezone>Asia/Shanghai</timezone></environment_context>'}]}
        raw=json.dumps(source,ensure_ascii=True).replace('<',r'\u003c').replace('>',r'\u003e')
        outer={'ToFormat':'codex','Body':base64.b64encode(raw.encode()).decode()}
        response=call('request.intercept_after',outer)
        output=json.loads(base64.b64decode(response['result']['Body']))
        self.assertIn('<timezone>America/Los_Angeles</timezone>',output['input'][0]['content'])
        self.assertIsNone(invoke(output))

    def test_does_not_modify_assistant_or_tool_output(self):
        fixture='<environment_context><timezone>Asia/Taipei</timezone></environment_context>'
        req={'input':[{'type':'function_call_output','output':fixture},
                      {'role':'assistant','content':fixture},
                      {'role':'developer','content':'<environment_context><timezone>Europe/London</timezone></environment_context>'}]}
        out=invoke(req)
        self.assertEqual(out['input'][0]['output'],fixture)
        self.assertEqual(out['input'][1]['content'],fixture)
        self.assertIn('America/Los_Angeles',out['input'][2]['content'])

    def test_state_date_is_not_stale_after_midnight(self):
        original=self.path.read_bytes()
        try:
            self.path.write_text('America/Los_Angeles|2000-01-01|PDT|-07:00\n66.92.223.4\n')
            expected=datetime.now(timezone(timedelta(hours=-7))).strftime('%Y-%m-%d')
            out=invoke({'input':[]})
            self.assertIn('Current date: '+expected,out['instructions'])
        finally:
            self.path.write_bytes(original)


    def _monitor_request(self, request_id, requested, routed=None):
        payload={'model':routed or requested, 'input':[]}
        wrapped={'ToFormat':'codex','Body':base64.b64encode(json.dumps(payload).encode()).decode(),
                 'RequestID':request_id,'RequestedModel':requested,'Model':routed or requested}
        self.assertTrue(call('request.intercept_after',wrapped)['ok'])

    def _monitor_response(self, method,request_id,body,**extras):
        key='Payload' if method=='websocket.response_event' else 'Body'
        arg={'RequestID':request_id,key:base64.b64encode(body.encode()).decode(),**extras}
        data=call(method,arg)
        self.assertEqual(data['result'],{})

    def _model_rows(self):
        code,data=self._management('models')
        self.assertEqual(code,200)
        return data

    def test_model_match_response_observer(self):
        self._monitor_request('model-match-001','gpt-6-astra')
        self._monitor_response('response.intercept_after','model-match-001',
            '{"type":"response.completed","response":{"model":"gpt-6-astra"}}')
        result=self._model_rows()
        self.assertEqual(result['records'][0]['status'],'matched')
        self.assertEqual(result['records'][0]['upstream_model'],'gpt-6-astra')
        self.assertEqual(result['records'][0]['channel'],'response')
        self.assertGreater(result['matched'],0)

    def test_model_mismatch_from_sse_and_header_init(self):
        self._monitor_request('model-sse-002','gpt-6-astra')
        self._monitor_response('response.intercept_stream_chunk','model-sse-002','',ChunkIndex=-1)
        self.assertEqual(self._model_rows()['records'][0]['status'],'unknown')
        self._monitor_response('response.intercept_stream_chunk','model-sse-002',
            'data: {"type":"response.created","response":{"model":"gpt-5.6-sol"}}\n\n',ChunkIndex=0)
        result=self._model_rows()['records'][0]
        self.assertEqual(result['status'],'mismatched')
        self.assertEqual(result['requested_model'],'gpt-6-astra')
        self.assertEqual(result['upstream_model'],'gpt-5.6-sol')
        self.assertEqual(result['channel'],'sse')

    def test_model_websocket_response_observer(self):
        self._monitor_request('model-ws-003','gpt-5.6-sol')
        self._monitor_response('websocket.response_event','model-ws-003',
            '{"type":"response.created","response":{"model":"gpt-5.6-sol"}}')
        result=self._model_rows()['records'][0]
        self.assertEqual((result['status'],result['channel']),('matched','websocket'))

    def test_model_monitor_unknown_and_not_fake_success(self):
        self._monitor_request('model-unknown-004','gpt-6-astra')
        self._monitor_response('response.intercept_after','model-unknown-004','{"output":"hi"}')
        row=self._model_rows()['records'][0]
        self.assertEqual(row['status'],'unknown')
        self.assertEqual(row['upstream_model'],'')
        self.assertGreater(self._model_rows()['unknown'],0)

    def test_model_monitor_never_reads_fake_model_from_tool_output(self):
        self._monitor_request('model-spoof-005','gpt-6-astra')
        self._monitor_response('response.intercept_after','model-spoof-005',
          '{"object":"response","output":[{"content":[{"text":"model: gpt-5.6-sol"}]}]}')
        self.assertEqual(self._model_rows()['records'][0]['status'],'unknown')

    def test_model_monitor_separate_from_timezone_sync(self):
        self.assertEqual(self._management('settings','PUT',{'enabled':False})[0],200)
        try:
            self._monitor_request('model-disabled-006','gpt-6-astra')
            self._monitor_response('response.intercept_after','model-disabled-006',
                '{"object":"response","model":"gpt-6-astra"}')
            self.assertEqual(self._model_rows()['records'][0]['status'],'matched')
        finally:
            self.assertEqual(self._management('settings','PUT',{'enabled':True})[0],200)

    def test_model_monitor_readonly_no_cross_request_contamination(self):
        self._monitor_request('model-other-007','gpt-6-astra')
        self._monitor_response('response.intercept_after','unexpected-other',
            '{"object":"response","model":"gpt-5.6-sol"}')
        self.assertEqual(self._model_rows()['records'][0]['status'],'unknown')
        self._monitor_request('model-other-008','gpt-5.6-sol')
        self._monitor_response('response.intercept_after','model-other-008',
            '{"object":"response","model":"gpt-5.6-sol"}')
        self.assertEqual(self._model_rows()['records'][0]['status'],'matched')
        self.assertEqual(self._model_rows()['records'][1]['status'],'unknown')


    def test_alias_requested_model_preserved(self):
        self._monitor_request('model-alias-009','gpt-6-astra','gpt-5.6-sol')
        self._monitor_response('response.intercept_after','model-alias-009',
            '{"object":"response","model":"gpt-5.6-sol"}')
        result=self._model_rows()['records'][0]
        self.assertEqual(result['requested_model'],'gpt-6-astra')
        self.assertEqual(result['routed_model'],'gpt-5.6-sol')
        self.assertEqual(result['upstream_model'],'gpt-5.6-sol')
        self.assertEqual(result['status'],'mismatched')

    def test_original_timezone_in_audit_log(self):
        self.config.unlink(missing_ok=True)
        self.logfile.unlink(missing_ok=True)
        request={'instructions':'<environment_context><timezone>Asia/Taipei</timezone></environment_context>','input':[]}
        out=invoke(request)
        self.assertIn('America/Los_Angeles',out['instructions'])
        status,body=self._management('logs')
        self.assertEqual(status,200)
        self.assertEqual(body['events'][0]['timezone_before'],'Asia/Taipei')
        self.assertEqual(body['events'][0]['timezone'],'America/Los_Angeles')
        self.assertNotIn('<environment_context>',self.logfile.read_text())

    def test_original_timezone_in_model_monitor(self):
        request={'model':'gpt-6-astra','instructions':'<environment_context><timezone>Asia/Shanghai</timezone></environment_context>','input':[]}
        envelope={'ToFormat':'codex','RequestID':'zone-model-unique-04','RequestedModel':'gpt-6-astra','Body':base64.b64encode(json.dumps(request).encode()).decode()}
        call('request.intercept_after',envelope)
        rows=self._model_rows()['records']
        row=next(x for x in rows if x['request_id']=='zone-model-unique-04')
        self.assertEqual(row['timezone_before'],'Asia/Shanghai')
        self.assertEqual(row['timezone_after'],'America/Los_Angeles')

    def test_original_timezone_unknown_without_environment(self):
        self.logfile.unlink(missing_ok=True)
        invoke({'input':[]})
        self.assertEqual(self._management('logs')[1]['events'][0]['timezone_before'],'unknown')

if __name__ == '__main__':
    unittest.main(verbosity=2)
