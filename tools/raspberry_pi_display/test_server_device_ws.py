import os
import sys
import tempfile
import types
import unittest
from unittest.mock import AsyncMock, patch

# These tests exercise only WebSocket session ownership. Avoid requiring the
# host's native Opus library merely to import the server module.
audio_player_stub = types.ModuleType("audio_player")
audio_player_stub.AudioPlayer = object
sys.modules["audio_player"] = audio_player_stub

from server import RemoteDisplayServer


class FakeWebSocket:
    def __init__(self, *, closed=False):
        self.closed = closed
        self.messages = []
        self.close_code = None

    async def send_json(self, message):
        self.messages.append(message)

    async def close(self, *, code, message):
        self.closed = True
        self.close_code = code


class DeviceHelloTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.data_dir = tempfile.TemporaryDirectory()
        self.env = patch.dict(os.environ, {"RD_DATA_DIR": self.data_dir.name})
        self.env.start()
        self.server = RemoteDisplayServer()
        self.server._broadcast_to_browsers = AsyncMock()

    def tearDown(self):
        self.env.stop()
        self.data_dir.cleanup()

    async def test_first_hello_becomes_active_device(self):
        ws = FakeWebSocket()

        await self.server._handle_text_message(ws, '{"type":"hello"}', "192.0.2.1")

        self.assertIs(self.server.device_ws, ws)
        self.assertEqual(ws.messages[-1]["status"], "ok")
        self.server._broadcast_to_browsers.assert_awaited_once()

    async def test_second_hello_cannot_replace_active_device(self):
        active = FakeWebSocket()
        newcomer = FakeWebSocket()
        self.server.device_ws = active
        self.server.device_ip = "192.0.2.1"
        self.server.current_ui_state = {"type": "ui_state", "status": "working"}

        await self.server._handle_text_message(newcomer, '{"type":"hello"}', "192.0.2.2")

        self.assertIs(self.server.device_ws, active)
        self.assertFalse(active.closed)
        self.assertTrue(newcomer.closed)
        self.assertEqual(newcomer.close_code, 1008)
        self.assertEqual(newcomer.messages[-1]["status"], "busy")
        self.assertEqual(self.server.current_ui_state["status"], "working")
        self.server._broadcast_to_browsers.assert_not_awaited()

    async def test_closed_device_can_be_replaced(self):
        closed = FakeWebSocket(closed=True)
        newcomer = FakeWebSocket()
        self.server.device_ws = closed

        await self.server._handle_text_message(newcomer, '{"type":"hello"}', "192.0.2.2")

        self.assertIs(self.server.device_ws, newcomer)
        self.assertEqual(newcomer.messages[-1]["status"], "ok")
        self.server._broadcast_to_browsers.assert_awaited_once()


if __name__ == "__main__":
    unittest.main(testRunner=unittest.TextTestRunner(stream=sys.stdout, verbosity=2))
