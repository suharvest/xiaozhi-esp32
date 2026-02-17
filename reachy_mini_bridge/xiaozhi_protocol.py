"""Xiaozhi WebSocket protocol client.

Implements the xiaozhi-esp32 WebSocket protocol in Python, handling:
- Hello handshake (client hello -> server hello)
- OPUS audio streaming (send mic audio, receive TTS audio)
- JSON control messages (tts, stt, llm emotions, mcp)
- Listening mode control
"""

import asyncio
import json
import logging
import struct
import time
import uuid
from enum import Enum, IntEnum
from typing import Callable, Optional

import websockets
from websockets.asyncio.client import ClientConnection

from .config import XiaozhiServerConfig, AudioConfig

logger = logging.getLogger(__name__)


class ListeningMode(str, Enum):
    AUTO_STOP = "auto"
    MANUAL_STOP = "manual"
    REALTIME = "realtime"


class AbortReason(IntEnum):
    NONE = 0
    WAKE_WORD_DETECTED = 1


class DeviceState(str, Enum):
    IDLE = "idle"
    CONNECTING = "connecting"
    LISTENING = "listening"
    SPEAKING = "speaking"
    ERROR = "error"


class XiaozhiProtocolClient:
    """Async WebSocket client implementing the Xiaozhi protocol."""

    def __init__(
        self,
        server_config: XiaozhiServerConfig,
        audio_config: AudioConfig,
    ):
        self._server_config = server_config
        self._audio_config = audio_config
        self._ws: Optional[ClientConnection] = None
        self._session_id: Optional[str] = None
        self._server_sample_rate: int = 24000
        self._server_frame_duration: int = 60
        self._state = DeviceState.IDLE
        self._connected = False

        # Abort tracking
        self._aborted = False
        self._listening_mode = ListeningMode.AUTO_STOP
        # Timeout tracking
        self._last_incoming_time = time.monotonic()
        self._timeout_seconds = 120.0

        # Callbacks
        self._on_audio: Optional[Callable[[bytes], None]] = None
        self._on_tts_start: Optional[Callable[[], None]] = None
        self._on_tts_stop: Optional[Callable[[], None]] = None
        self._on_tts_sentence: Optional[Callable[[str], None]] = None
        self._on_stt_text: Optional[Callable[[str], None]] = None
        self._on_emotion: Optional[Callable[[str], None]] = None
        self._on_state_change: Optional[Callable[[DeviceState], None]] = None
        self._on_mcp: Optional[Callable[[dict], None]] = None
        self._on_system_command: Optional[Callable[[str], None]] = None
        self._on_alert: Optional[Callable[[str, str, str], None]] = None

        # Connection event for hello handshake
        self._hello_event = asyncio.Event()

    @property
    def state(self) -> DeviceState:
        return self._state

    @property
    def server_sample_rate(self) -> int:
        return self._server_sample_rate

    @property
    def server_frame_duration(self) -> int:
        return self._server_frame_duration

    @property
    def is_connected(self) -> bool:
        return self._connected and self._ws is not None

    def on_audio(self, callback: Callable[[bytes], None]):
        self._on_audio = callback

    def on_tts_start(self, callback: Callable[[], None]):
        self._on_tts_start = callback

    def on_tts_stop(self, callback: Callable[[], None]):
        self._on_tts_stop = callback

    def on_tts_sentence(self, callback: Callable[[str], None]):
        self._on_tts_sentence = callback

    def on_stt_text(self, callback: Callable[[str], None]):
        self._on_stt_text = callback

    def on_emotion(self, callback: Callable[[str], None]):
        self._on_emotion = callback

    def on_state_change(self, callback: Callable[[DeviceState], None]):
        self._on_state_change = callback

    def on_mcp(self, callback: Callable[[dict], None]):
        """Called when MCP JSON-RPC message arrives."""
        self._on_mcp = callback

    def on_system_command(self, callback: Callable[[str], None]):
        """Called when a system command arrives (e.g. 'reboot')."""
        self._on_system_command = callback

    def on_alert(self, callback: Callable[[str, str, str], None]):
        """Called when an alert arrives (status, message, emotion)."""
        self._on_alert = callback

    def _set_state(self, state: DeviceState):
        if self._state != state:
            old = self._state
            self._state = state
            logger.info(f"State: {old.value} -> {state.value}")
            if self._on_state_change:
                self._on_state_change(state)

    def _build_hello_message(self) -> str:
        """Build the client hello message matching xiaozhi protocol."""
        msg = {
            "type": "hello",
            "version": self._server_config.protocol_version,
            "features": {
                "mcp": True,
            },
            "transport": "websocket",
            "audio_params": {
                "format": "opus",
                "sample_rate": self._audio_config.input_sample_rate,
                "channels": self._audio_config.channels,
                "frame_duration": self._audio_config.opus_frame_duration_ms,
            },
        }
        return json.dumps(msg)

    def _parse_server_hello(self, data: dict):
        """Parse server hello response."""
        transport = data.get("transport")
        if transport != "websocket":
            logger.error(f"Unsupported transport: {transport}")
            return

        self._session_id = data.get("session_id", "")
        logger.info(f"Session ID: {self._session_id}")

        audio_params = data.get("audio_params", {})
        if "sample_rate" in audio_params:
            self._server_sample_rate = audio_params["sample_rate"]
        if "frame_duration" in audio_params:
            self._server_frame_duration = audio_params["frame_duration"]

        logger.info(
            f"Server audio: sample_rate={self._server_sample_rate}, "
            f"frame_duration={self._server_frame_duration}ms"
        )
        self._hello_event.set()

    def _handle_json_message(self, data: dict):
        """Handle incoming JSON messages from the server."""
        msg_type = data.get("type", "")

        if msg_type == "hello":
            self._parse_server_hello(data)
            return

        if msg_type == "tts":
            state = data.get("state", "")
            if state == "start":
                self._aborted = False
                if self._state in (DeviceState.IDLE, DeviceState.LISTENING):
                    self._set_state(DeviceState.SPEAKING)
                if self._on_tts_start:
                    self._on_tts_start()
            elif state == "stop":
                if self._state == DeviceState.SPEAKING:
                    if self._listening_mode == ListeningMode.MANUAL_STOP:
                        self._set_state(DeviceState.IDLE)
                    else:
                        self._set_state(DeviceState.LISTENING)
                if self._on_tts_stop:
                    self._on_tts_stop()
            elif state == "sentence_start":
                text = data.get("text", "")
                logger.info(f"<< {text}")
                if self._on_tts_sentence:
                    self._on_tts_sentence(text)

        elif msg_type == "stt":
            text = data.get("text", "")
            logger.info(f">> {text}")
            if self._on_stt_text:
                self._on_stt_text(text)

        elif msg_type == "llm":
            emotion = data.get("emotion", "")
            if emotion:
                logger.info(f"Emotion: {emotion}")
                if self._on_emotion:
                    self._on_emotion(emotion)

        elif msg_type == "mcp":
            payload = data.get("payload")
            if payload and isinstance(payload, dict):
                logger.debug(f"MCP message: {json.dumps(payload)}")
                if self._on_mcp:
                    self._on_mcp(payload)
            else:
                logger.warning("MCP message with invalid payload")

        elif msg_type == "system":
            command = data.get("command", "")
            logger.info(f"System command: {command}")
            if self._on_system_command:
                self._on_system_command(command)

        elif msg_type == "alert":
            status = data.get("status", "")
            message = data.get("message", "")
            emotion = data.get("emotion", "")
            logger.info(f"Alert: status={status}, message={message}, emotion={emotion}")
            if self._on_alert:
                self._on_alert(status, message, emotion)

        elif msg_type == "custom":
            payload = data.get("payload", {})
            logger.info(f"Custom message: {json.dumps(payload)}")

        else:
            logger.warning(f"Unknown message type: {msg_type}")

    async def connect(self) -> bool:
        """Connect to the xiaozhi server and perform hello handshake."""
        self._set_state(DeviceState.CONNECTING)
        self._hello_event.clear()

        url = self._server_config.websocket_url
        headers = {
            "Protocol-Version": str(self._server_config.protocol_version),
            "Device-Id": self._server_config.device_id,
            "Client-Id": self._server_config.client_id,
        }
        if self._server_config.token:
            token = self._server_config.token
            if " " not in token:
                token = f"Bearer {token}"
            headers["Authorization"] = token

        try:
            self._ws = await websockets.connect(url, additional_headers=headers)
            logger.info(f"Connected to {url}")
        except Exception as e:
            logger.error(f"Failed to connect: {e}")
            self._set_state(DeviceState.ERROR)
            return False

        # Send client hello
        hello = self._build_hello_message()
        await self._ws.send(hello)
        logger.debug(f"Sent hello: {hello}")

        # Wait for server hello (timeout 10s)
        try:
            await asyncio.wait_for(self._hello_event.wait(), timeout=10.0)
        except asyncio.TimeoutError:
            logger.error("Timeout waiting for server hello")
            self._set_state(DeviceState.ERROR)
            return False

        self._connected = True
        self._set_state(DeviceState.LISTENING)
        return True

    async def disconnect(self):
        """Disconnect from the server."""
        self._connected = False
        if self._ws:
            await self._ws.close()
            self._ws = None
        self._set_state(DeviceState.IDLE)

    async def send_audio(self, opus_data: bytes) -> bool:
        """Send OPUS-encoded audio frame to the server."""
        if not self.is_connected:
            return False

        version = self._server_config.protocol_version
        try:
            if version == 2:
                # BinaryProtocol2: version(2) + type(2) + reserved(4) + timestamp(4) + payload_size(4) + payload
                timestamp = int(time.monotonic() * 1000) & 0xFFFFFFFF
                header = struct.pack(
                    "!HHIII",
                    version,       # version
                    0,             # type: 0 = OPUS
                    0,             # reserved
                    timestamp,     # timestamp ms
                    len(opus_data) # payload_size
                )
                await self._ws.send(header + opus_data)
            elif version == 3:
                # BinaryProtocol3: type(1) + reserved(1) + payload_size(2) + payload
                header = struct.pack(
                    "!BBH",
                    0,             # type: 0 = OPUS
                    0,             # reserved
                    len(opus_data) # payload_size
                )
                await self._ws.send(header + opus_data)
            else:
                # v1: raw OPUS data
                await self._ws.send(opus_data)
            return True
        except Exception as e:
            logger.error(f"Failed to send audio: {e}")
            return False

    async def send_start_listening(self, mode: ListeningMode = ListeningMode.AUTO_STOP):
        """Tell server we're starting to listen."""
        msg = {"type": "listen", "state": "start", "mode": mode.value}
        await self._send_json(msg)

    async def send_stop_listening(self):
        """Tell server we've stopped listening."""
        msg = {"type": "listen", "state": "stop"}
        await self._send_json(msg)

    async def send_wake_word_detected(self, wake_word: str = ""):
        """Tell server a wake word was detected."""
        msg = {"type": "listen", "state": "detect", "text": wake_word}
        await self._send_json(msg)

    async def send_abort_speaking(self, reason: AbortReason = AbortReason.NONE):
        """Tell server to abort current TTS."""
        logger.info(f"Abort speaking (reason={reason.name})")
        self._aborted = True
        msg = {"type": "abort", "reason": reason.value}
        await self._send_json(msg)

    async def send_mcp_message(self, payload: str):
        """Send an MCP JSON-RPC response to the server."""
        msg = {"type": "mcp", "payload": json.loads(payload)}
        await self._send_json(msg)

    def set_listening_mode(self, mode: ListeningMode):
        """Set the current listening mode."""
        self._listening_mode = mode
        self._set_state(DeviceState.LISTENING)

    def is_timeout(self) -> bool:
        """Check if the connection has timed out."""
        return (time.monotonic() - self._last_incoming_time) > self._timeout_seconds

    async def _send_json(self, data: dict) -> bool:
        """Send a JSON message to the server."""
        if not self.is_connected:
            return False
        try:
            text = json.dumps(data)
            await self._ws.send(text)
            logger.debug(f"Sent: {text}")
            return True
        except Exception as e:
            logger.error(f"Failed to send JSON: {e}")
            return False

    async def receive_loop(self):
        """Main receive loop - process incoming messages from server."""
        if not self._ws:
            return

        try:
            async for message in self._ws:
                self._last_incoming_time = time.monotonic()
                if isinstance(message, bytes):
                    # Binary = OPUS audio from server (TTS)
                    opus_data = self._parse_binary(message)
                    if opus_data and self._on_audio:
                        self._on_audio(opus_data)
                else:
                    # Text = JSON control message
                    try:
                        data = json.loads(message)
                        self._handle_json_message(data)
                    except json.JSONDecodeError:
                        logger.error(f"Invalid JSON: {message[:100]}")
        except websockets.exceptions.ConnectionClosed as e:
            logger.warning(f"Connection closed: {e}")
        except Exception as e:
            logger.error(f"Receive loop error: {e}")
        finally:
            self._connected = False
            self._set_state(DeviceState.IDLE)

    def _parse_binary(self, data: bytes) -> Optional[bytes]:
        """Parse binary protocol message and extract OPUS payload."""
        version = self._server_config.protocol_version
        if version == 2:
            if len(data) < 16:
                return None
            # BinaryProtocol2 header: 16 bytes
            _ver, _type, _reserved, _ts, payload_size = struct.unpack_from("!HHIII", data)
            return data[16:16 + payload_size]
        elif version == 3:
            if len(data) < 4:
                return None
            # BinaryProtocol3 header: 4 bytes
            _type, _reserved, payload_size = struct.unpack_from("!BBH", data)
            return data[4:4 + payload_size]
        else:
            # v1: raw OPUS
            return data
