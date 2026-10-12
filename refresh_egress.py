#!/usr/bin/env python3
"""Refresh timezone state through CPAMC's *same* Mihomo proxy.
Never queries OpenAI or reads OAuth credentials. Atomic writes, last-good-state semantics.
"""
import datetime as dt
import ipaddress
import json
import os
import pathlib
import re
import socketserver
import stat
import subprocess
import sys
import tempfile
import threading
import urllib.request
from zoneinfo import ZoneInfo

def resolve_proxy():
    override = os.getenv('CODEX_TZ_PROXY')
    if override:
        return override
    result = subprocess.run(
        ['docker', 'inspect', '-f',
         '{{(index .NetworkSettings.Networks "cpa-proxy-net").IPAddress}}',
         'mihomo'], capture_output=True, text=True, timeout=8, check=True)
    ip = str(ipaddress.ip_address(result.stdout.strip()))
    return f'http://{ip}:7890'

STATE = pathlib.Path(os.getenv('CODEX_TZ_STATE', '/home/admin/CLIProxyAPI-mc/data/codex-timezone-sync.state'))
URL = 'https://ipinfo.io/json'

def refresh():
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({'https': resolve_proxy()}))
    with opener.open(urllib.request.Request(URL, headers={'User-Agent': 'cpamc-tz-sync/0.1'}), timeout=12) as resp:
        data = json.load(resp)
    ip = str(ipaddress.ip_address(data['ip']))
    zone = str(data['timezone'])
    if not re.fullmatch(r'[A-Za-z0-9_+/-]{1,79}', zone):
        raise ValueError('invalid IANA timezone')
    now = dt.datetime.now(ZoneInfo(zone))
    abbr = now.tzname() or 'UTC'
    offset = now.strftime('%z')
    offset = offset[:3] + ':' + offset[3:] if len(offset) == 5 else '+00:00'
    payload = f'{zone}|{now:%Y-%m-%d}|{abbr}|{offset}\n{ip}\n'
    STATE.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix='.tz-tmp-', dir=STATE.parent)
    try:
        with os.fdopen(fd, 'w', encoding='ascii') as f:
            f.write(payload)
            f.flush()
            os.fsync(f.fileno())
        os.chmod(tmp, 0o644)
        os.replace(tmp, STATE)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)
    print(f'{zone} {now:%Y-%m-%d} {abbr} UTC{offset} {ip}')

_lock = threading.Lock()

class RefreshHandler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(16)
        try:
            command = self.request.recv(32)
            if command != b'REFRESH\n':
                self.request.sendall(b'ERR\n')
                return
            with _lock:
                refresh()
            self.request.sendall(b'OK\n')
        except (OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
            print(f'refresh helper error: {type(exc).__name__}: {exc}', file=sys.stderr)
            try:
                self.request.sendall(b'ERR\n')
            except OSError:
                pass

def serve():
    socket_path = pathlib.Path(os.getenv('CODEX_TZ_REFRESH_SOCKET',
        '/home/admin/CLIProxyAPI-mc/data/codex-timezone-sync-refresh.sock'))
    socket_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        if stat.S_ISSOCK(socket_path.lstat().st_mode):
            socket_path.unlink()
        else:
            raise RuntimeError('refresh socket path exists and is not a socket')
    except FileNotFoundError:
        pass
    old_umask = os.umask(0o077)
    try:
        with socketserver.ThreadingUnixStreamServer(str(socket_path), RefreshHandler) as server:
            os.chmod(socket_path, 0o600)
            print(f'Codex TZ refresh helper listening on {socket_path}', flush=True)
            server.serve_forever(poll_interval=.5)
    finally:
        os.umask(old_umask)
        if socket_path.exists() and stat.S_ISSOCK(socket_path.lstat().st_mode):
            socket_path.unlink()

if __name__ == '__main__':
    try:
        if sys.argv[1:] == ['--serve']:
            serve()
        elif not sys.argv[1:]:
            refresh()
        else:
            raise ValueError('usage: refresh_egress.py [--serve]')
    except Exception as exc:
        print(f'timezone refresh failed: {type(exc).__name__}: {exc}', file=sys.stderr)
        sys.exit(1)
