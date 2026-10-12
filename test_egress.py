#!/usr/bin/env python3
"""Offline host-helper contract tests: verify that the probe updates state."""
import importlib.util
import io
import os
import pathlib
import socket
import socketserver
import tempfile
import threading
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("refresh_egress", ROOT / "refresh_egress.py")
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)

class FakeOpener:
    def __init__(self, text):
        self.text=text
    def open(self, request, timeout):
        assert timeout==12
        assert request.full_url=="https://ipinfo.io/json"
        return io.BytesIO(self.text.encode("utf-8"))

class TestHostEgress(unittest.TestCase):
    def test_refresh_updates_iana_zone_over_proxy(self):
        with tempfile.TemporaryDirectory() as path:
            with mock.patch.object(helper, "STATE", pathlib.Path(path)/"state"), \
                 mock.patch.object(helper, "resolve_proxy", return_value="http://proxy:7890"), \
                 mock.patch.object(helper.urllib.request, "build_opener", return_value=FakeOpener('{"ip":"203.0.113.22","timezone":"America/Los_Angeles"}')) as opener:
                helper.refresh()
                observed=helper.STATE.read_text()
                self.assertIn("America/Los_Angeles|",observed)
                self.assertIn("203.0.113.22",observed)
                opener.assert_called_once()

    def test_unix_socket_helper_refresh_command(self):
        with tempfile.TemporaryDirectory() as path:
            sock=str(pathlib.Path(path)/"test.sock")
            with mock.patch.object(helper,"refresh") as refresh:
                with socketserver.ThreadingUnixStreamServer(sock,helper.RefreshHandler) as server:
                    thread=threading.Thread(target=server.serve_forever,daemon=True)
                    thread.start()
                    try:
                        with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as client:
                            client.connect(sock)
                            client.sendall(b"REFRESH\n")
                            self.assertEqual(client.recv(8),b"OK\n")
                    finally:
                        server.shutdown()
                        thread.join(timeout=3)
                refresh.assert_called_once()

if __name__ == "__main__":
    unittest.main()
