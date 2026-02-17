"""Xiaozhi MQTT+UDP protocol client.

Implements the xiaozhi-esp32 MQTT+UDP protocol in Python:
- MQTT (TLS) for JSON control messages (hello, goodbye, tts, stt, llm, mcp)
- UDP (AES-128-CTR encrypted) for low-latency OPUS audio streaming

This is the PRIMARY protocol used by the official xiaozhi server.
MQTT handles signaling, UDP handles real-time audio.
"""

import asyncio
import hashlib
import json
import logging
import socket
import struct
import time
from enum import Enum
from typing import Callable, Optional

from .config import MqttServerConfig, AudioConfig

logger = logging.getLogger(__name__)

# Try to import dependencies
try:
    import aiomqtt
    HAS_MQTT = True
except ImportError:
    HAS_MQTT = False
    logger.warning("aiomqtt not installed. Install with: pip install aiomqtt")

try:
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    HAS_CRYPTO = True
except ImportError:
    HAS_CRYPTO = False
    logger.warning("cryptography not installed. Install with: pip install cryptography")


class DeviceState(str, Enum):
    IDLE = "idle"
    CONNECTING = "connecting"
    LISTENING = "listening"
    SPEAKING = "speaking"
    ERROR = "error"


class ListeningMode(str, Enum):
    AUTO_STOP = "auto"
    MANUAL_STOP = "manual"
    REALTIME = "realtime"


class AbortReason(int, Enum):
    NONE = 0
    WAKE_WORD_DETECTED = 1


class AesUdpCodec:
    """AES-128-CTR encryption/decryption for UDP audio packets.

    UDP Packet Format (encrypted):
    |type 1u|flags 1u|payload_len 2u|ssrc 4u|timestamp 4u|sequence 4u|
    |encrypted_payload payload_len|

    The first 16 bytes serve as the AES nonce/IV.
    """

    def __init__(self, key_hex: str, nonce_hex: str):
        if not HAS_CRYPTO:
            raise ImportError("cryptography package required for AES encryption")
        self._key = bytes.fromhex(key_hex)
        self._base_nonce = bytes.fromhex(nonce_hex)

    def encrypt(self, opus_data: bytes, timestamp: int, sequence: int) -> bytes:
        """Encrypt an OPUS frame for UDP transmission."""
        payload_len = len(opus_data)

        # Build nonce (16 bytes): type(1) + flags(1) + payload_len(2) + ssrc(4) + timestamp(4) + sequence(4)
        nonce = bytearray(self._base_nonce)
        struct.pack_into("!H", nonce, 2, payload_len)
        struct.pack_into("!I", nonce, 8, timestamp)
        struct.pack_into("!I", nonce, 12, sequence)

        # AES-128-CTR encrypt
        cipher = Cipher(algorithms.AES(self._key), modes.CTR(bytes(nonce)))
        encryptor = cipher.encryptor()
        encrypted = encryptor.update(opus_data) + encryptor.finalize()

        # Packet = nonce + encrypted payload
        return bytes(nonce) + encrypted

    def decrypt(self, packet: bytes) -> tuple[bytes, int, int]:
        """Decrypt a UDP packet, return (opus_data, timestamp, sequence)."""
        if len(packet) < 16:
            raise ValueError(f"Packet too short: {len(packet)}")

        # Parse nonce header
        nonce = packet[:16]
        pkt_type = nonce[0]
        if pkt_type != 0x01:
            raise ValueError(f"Invalid packet type: {pkt_type:#x}")

        payload_len = struct.unpack_from("!H", nonce, 2)[0]
        timestamp = struct.unpack_from("!I", nonce, 8)[0]
        sequence = struct.unpack_from("!I", nonce, 12)[0]

        encrypted = packet[16:]
        if len(encrypted) < payload_len:
            raise ValueError(f"Payload truncated: {len(encrypted)} < {payload_len}")

        # AES-128-CTR decrypt
        cipher = Cipher(algorithms.AES(self._key), modes.CTR(nonce))
        decryptor = cipher.decryptor()
        opus_data = decryptor.update(encrypted) + decryptor.finalize()

        return opus_data, timestamp, sequence


class MqttProtocolClient:
    """Async MQTT+UDP client implementing the Xiaozhi protocol.

    - MQTT channel: JSON control messages (hello/goodbye, tts, stt, llm, mcp)
    - UDP channel: AES-128 encrypted OPUS audio packets
    """

    RECONNECT_INTERVAL = 60.0
    KEEPALIVE_INTERVAL = 240

    def __init__(
        self,
        mqtt_config: "MqttServerConfig",
        audio_config: AudioConfig,
    ):
        self._mqtt_config = mqtt_config
        self._audio_config = audio_config
        self._state = DeviceState.IDLE
        self._connected = False
        self._session_id = ""
        self._server_sample_rate = 24000
        self._server_frame_duration = 60
        self._aborted = False
        self._listening_mode = ListeningMode.AUTO_STOP
        self._last_incoming_time = time.monotonic()
        self._timeout_seconds = 120.0

        # UDP state
        self._udp_socket: Optional[socket.socket] = None
        self._udp_server = ""
        self._udp_port = 0
        self._aes_codec: Optional[AesUdpCodec] = None
        self._local_sequence = 0
        self._remote_sequence = 0

        # MQTT client
        self._mqtt_client = None

        # Audio channel events
        self._hello_event = asyncio.Event()
        self._audio_channel_open = False

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

    # -- Properties --

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
        return self._connected

    # -- Callback registration --

    def on_audio(self, cb): self._on_audio = cb
    def on_tts_start(self, cb): self._on_tts_start = cb
    def on_tts_stop(self, cb): self._on_tts_stop = cb
    def on_tts_sentence(self, cb): self._on_tts_sentence = cb
    def on_stt_text(self, cb): self._on_stt_text = cb
    def on_emotion(self, cb): self._on_emotion = cb
    def on_state_change(self, cb): self._on_state_change = cb
    def on_mcp(self, cb): self._on_mcp = cb
    def on_system_command(self, cb): self._on_system_command = cb
    def on_alert(self, cb): self._on_alert = cb

    # -- State management --

    def _set_state(self, state: DeviceState):
        if self._state != state:
            old = self._state
            self._state = state
            logger.info(f"State: {old.value} -> {state.value}")
            if self._on_state_change:
                self._on_state_change(state)

    # -- MQTT connection --

    async def connect(self) -> bool:
        """Connect to MQTT broker."""
        if not HAS_MQTT:
            logger.error("aiomqtt not installed")
            return False

        self._set_state(DeviceState.CONNECTING)
        cfg = self._mqtt_config

        try:
            # Parse endpoint
            host = cfg.endpoint
            port = 8883
            if ":" in cfg.endpoint:
                host, port_str = cfg.endpoint.rsplit(":", 1)
                port = int(port_str)

            self._mqtt_client = aiomqtt.Client(
                hostname=host,
                port=port,
                username=cfg.username or None,
                password=cfg.password or None,
                identifier=cfg.client_id,
                keepalive=cfg.keepalive or self.KEEPALIVE_INTERVAL,
                tls_params=aiomqtt.TLSParameters() if port == 8883 else None,
            )
            await self._mqtt_client.__aenter__()

            # Subscribe to the device's topic
            if cfg.subscribe_topic:
                await self._mqtt_client.subscribe(cfg.subscribe_topic)
                logger.info(f"Subscribed to {cfg.subscribe_topic}")

            self._connected = True
            logger.info(f"Connected to MQTT broker {host}:{port}")
            return True

        except Exception as e:
            logger.error(f"MQTT connection failed: {e}")
            self._set_state(DeviceState.ERROR)
            return False

    async def disconnect(self):
        """Disconnect MQTT and close UDP."""
        self._close_audio_channel()
        if self._mqtt_client:
            try:
                await self._mqtt_client.__aexit__(None, None, None)
            except Exception:
                pass
            self._mqtt_client = None
        self._connected = False
        self._set_state(DeviceState.IDLE)

    # -- Audio channel (UDP) --

    async def open_audio_channel(self) -> bool:
        """Open audio channel: send hello via MQTT, wait for server hello with UDP info."""
        self._hello_event.clear()
        self._session_id = ""

        hello = self._build_hello_message()
        await self._send_mqtt(hello)

        # Wait for server hello
        try:
            await asyncio.wait_for(self._hello_event.wait(), timeout=10.0)
        except asyncio.TimeoutError:
            logger.error("Timeout waiting for server hello")
            return False

        # Open UDP socket
        try:
            self._udp_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self._udp_socket.setblocking(False)
            self._udp_socket.connect((self._udp_server, self._udp_port))
            self._local_sequence = 0
            self._remote_sequence = 0
            self._audio_channel_open = True
            logger.info(f"UDP channel open: {self._udp_server}:{self._udp_port}")
            self._set_state(DeviceState.LISTENING)
            return True
        except Exception as e:
            logger.error(f"Failed to open UDP socket: {e}")
            return False

    def _close_audio_channel(self):
        """Close UDP audio channel and send goodbye."""
        if self._udp_socket:
            self._udp_socket.close()
            self._udp_socket = None
        self._audio_channel_open = False
        self._aes_codec = None

        if self._session_id and self._connected:
            goodbye = json.dumps({
                "session_id": self._session_id,
                "type": "goodbye",
            })
            # Fire and forget
            asyncio.ensure_future(self._send_mqtt(goodbye))

    # -- Send methods --

    async def send_audio(self, opus_data: bytes) -> bool:
        """Send OPUS audio via encrypted UDP."""
        if not self._udp_socket or not self._aes_codec:
            return False

        try:
            self._local_sequence += 1
            timestamp = int(time.monotonic() * 1000) & 0xFFFFFFFF
            packet = self._aes_codec.encrypt(opus_data, timestamp, self._local_sequence)
            self._udp_socket.send(packet)
            return True
        except Exception as e:
            logger.error(f"UDP send error: {e}")
            return False

    async def send_start_listening(self, mode: ListeningMode = ListeningMode.AUTO_STOP):
        msg = json.dumps({"type": "listen", "state": "start", "mode": mode.value})
        await self._send_mqtt(msg)

    async def send_stop_listening(self):
        msg = json.dumps({"type": "listen", "state": "stop"})
        await self._send_mqtt(msg)

    async def send_wake_word_detected(self, wake_word: str = ""):
        msg = json.dumps({"type": "listen", "state": "detect", "text": wake_word})
        await self._send_mqtt(msg)

    async def send_abort_speaking(self, reason: AbortReason = AbortReason.NONE):
        logger.info(f"Abort speaking (reason={reason.name})")
        self._aborted = True
        msg = json.dumps({"type": "abort", "reason": reason.value})
        await self._send_mqtt(msg)

    async def send_mcp_message(self, payload: str):
        msg = json.dumps({"type": "mcp", "payload": json.loads(payload)})
        await self._send_mqtt(msg)

    def set_listening_mode(self, mode: ListeningMode):
        self._listening_mode = mode
        self._set_state(DeviceState.LISTENING)

    def is_timeout(self) -> bool:
        return (time.monotonic() - self._last_incoming_time) > self._timeout_seconds

    # -- Receive loops --

    async def receive_loop(self):
        """Main receive loop - listen for MQTT messages and UDP audio."""
        if not self._mqtt_client:
            return

        # Run MQTT and UDP receive concurrently
        await asyncio.gather(
            self._mqtt_receive_loop(),
            self._udp_receive_loop(),
        )

    async def _mqtt_receive_loop(self):
        """Process incoming MQTT messages."""
        try:
            async for message in self._mqtt_client.messages:
                self._last_incoming_time = time.monotonic()
                payload = message.payload.decode("utf-8")
                try:
                    data = json.loads(payload)
                except json.JSONDecodeError:
                    logger.error(f"Invalid MQTT JSON: {payload[:100]}")
                    continue

                msg_type = data.get("type", "")
                if msg_type == "hello":
                    self._parse_server_hello(data)
                elif msg_type == "goodbye":
                    sid = data.get("session_id", "")
                    if not sid or sid == self._session_id:
                        self._close_audio_channel()
                else:
                    self._handle_json_message(data)

        except asyncio.CancelledError:
            pass
        except Exception as e:
            logger.error(f"MQTT receive error: {e}")
        finally:
            self._connected = False
            self._set_state(DeviceState.IDLE)

    async def _udp_receive_loop(self):
        """Process incoming UDP audio packets."""
        loop = asyncio.get_event_loop()
        while self._connected:
            if not self._udp_socket or not self._aes_codec:
                await asyncio.sleep(0.1)
                continue
            try:
                data = await asyncio.wait_for(
                    loop.sock_recv(self._udp_socket, 4096),
                    timeout=0.1,
                )
                if data:
                    self._last_incoming_time = time.monotonic()
                    try:
                        opus_data, timestamp, sequence = self._aes_codec.decrypt(data)
                    except ValueError as e:
                        logger.warning(f"UDP decrypt error: {e}")
                        continue

                    # Sequence checking
                    if sequence < self._remote_sequence:
                        logger.warning(f"Old sequence: {sequence}, expected: {self._remote_sequence}")
                        continue
                    if sequence != self._remote_sequence + 1:
                        logger.warning(f"Sequence gap: got {sequence}, expected {self._remote_sequence + 1}")

                    self._remote_sequence = sequence
                    if self._on_audio:
                        self._on_audio(opus_data)

            except asyncio.TimeoutError:
                continue
            except OSError:
                # Socket closed
                break
            except Exception as e:
                logger.error(f"UDP receive error: {e}")
                await asyncio.sleep(0.1)

    # -- Internal helpers --

    def _build_hello_message(self) -> str:
        msg = {
            "type": "hello",
            "version": 3,
            "transport": "udp",
            "features": {"mcp": True},
            "audio_params": {
                "format": "opus",
                "sample_rate": self._audio_config.input_sample_rate,
                "channels": self._audio_config.channels,
                "frame_duration": self._audio_config.opus_frame_duration_ms,
            },
        }
        return json.dumps(msg)

    def _parse_server_hello(self, data: dict):
        transport = data.get("transport")
        if transport != "udp":
            logger.error(f"Unsupported transport: {transport}")
            return

        self._session_id = data.get("session_id", "")
        logger.info(f"Session ID: {self._session_id}")

        audio_params = data.get("audio_params", {})
        if "sample_rate" in audio_params:
            self._server_sample_rate = audio_params["sample_rate"]
        if "frame_duration" in audio_params:
            self._server_frame_duration = audio_params["frame_duration"]

        udp_info = data.get("udp", {})
        if not udp_info:
            logger.error("No UDP info in server hello")
            return

        self._udp_server = udp_info["server"]
        self._udp_port = udp_info["port"]
        key_hex = udp_info["key"]
        nonce_hex = udp_info["nonce"]

        self._aes_codec = AesUdpCodec(key_hex, nonce_hex)
        logger.info(
            f"Server audio: rate={self._server_sample_rate}, "
            f"frame={self._server_frame_duration}ms, "
            f"UDP={self._udp_server}:{self._udp_port}"
        )
        self._hello_event.set()

    def _handle_json_message(self, data: dict):
        """Handle incoming JSON messages (same logic as WebSocket version)."""
        msg_type = data.get("type", "")

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
                if self._on_mcp:
                    self._on_mcp(payload)

        elif msg_type == "system":
            command = data.get("command", "")
            logger.info(f"System command: {command}")
            if self._on_system_command:
                self._on_system_command(command)

        elif msg_type == "alert":
            status = data.get("status", "")
            message = data.get("message", "")
            emotion = data.get("emotion", "")
            logger.info(f"Alert: {status} - {message}")
            if self._on_alert:
                self._on_alert(status, message, emotion)

        elif msg_type == "custom":
            logger.info(f"Custom message: {json.dumps(data.get('payload', {}))}")

        else:
            logger.warning(f"Unknown message type: {msg_type}")

    async def _send_mqtt(self, text: str) -> bool:
        """Publish a message to the MQTT publish topic."""
        if not self._mqtt_client or not self._mqtt_config.publish_topic:
            return False
        try:
            await self._mqtt_client.publish(
                self._mqtt_config.publish_topic, text
            )
            logger.debug(f"MQTT sent: {text[:200]}")
            return True
        except Exception as e:
            logger.error(f"MQTT publish error: {e}")
            return False
