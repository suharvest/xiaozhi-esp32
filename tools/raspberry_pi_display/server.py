"""
Remote Display Server - Web-based UI rendering with aiohttp
Receives UI state from ESP32 device and broadcasts to browser clients
"""

import os
import sys
import platform

# macOS: Set library path for Opus (Homebrew installation)
if platform.system() == "Darwin":
    brew_prefix = os.popen("brew --prefix opus 2>/dev/null").read().strip()
    if brew_prefix:
        lib_path = os.path.join(brew_prefix, "lib")
        current_path = os.environ.get("DYLD_LIBRARY_PATH", "")
        if lib_path not in current_path:
            os.environ["DYLD_LIBRARY_PATH"] = f"{lib_path}:{current_path}" if current_path else lib_path

import asyncio
import json
import struct
import signal
import logging
import socket
from pathlib import Path
from typing import Optional, Set

from aiohttp import web, WSMsgType

from config import Config
from audio_player import AudioPlayer

# Optional mDNS support
try:
    from zeroconf import ServiceInfo
    from zeroconf.asyncio import AsyncZeroconf
    MDNS_AVAILABLE = True
except ImportError:
    MDNS_AVAILABLE = False
    logger = logging.getLogger(__name__)
    logger.warning("zeroconf not installed, mDNS service discovery disabled. Install with: pip install zeroconf")

# Setup logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

# Message types (binary protocol from ESP32)
MSG_TYPE_UI_STATE = 0x10
MSG_TYPE_AUDIO_FRAME = 0x02  # Deprecated (Opus)
MSG_TYPE_AUDIO_PCM = 0x03    # PCM audio
MSG_TYPE_HEARTBEAT = 0x04


class RemoteDisplayServer:
    def __init__(self):
        self.config = Config()
        self.audio_player: Optional[AudioPlayer] = None

        # WebSocket clients
        self.device_ws: Optional[web.WebSocketResponse] = None  # ESP32 device
        self.browser_clients: Set[web.WebSocketResponse] = set()  # Browser clients

        # Stats
        self.ui_state_count = 0
        self.audio_count = 0

        # Current UI state (for new browser connections)
        self.current_ui_state: Optional[dict] = None

        # Paths
        self.base_dir = Path(__file__).parent
        self.web_dir = self.base_dir / "web"
        self.assets_dir = self.base_dir / "assets"

        # mDNS service
        self.async_zeroconf: Optional['AsyncZeroconf'] = None
        self.service_info: Optional['ServiceInfo'] = None

    def _get_local_ip(self) -> str:
        """Get local IP address for mDNS registration"""
        # Allow manual override via environment variable
        manual_ip = os.getenv("RD_LOCAL_IP")
        if manual_ip:
            return manual_ip

        try:
            # Get all network interfaces
            import subprocess
            result = subprocess.run(
                ["ifconfig"] if platform.system() != "Windows" else ["ipconfig"],
                capture_output=True, text=True
            )
            # Look for 192.168.x.x or 10.x.x.x addresses (typical LAN)
            import re
            for pattern in [r'192\.168\.\d+\.\d+', r'10\.\d+\.\d+\.\d+', r'172\.(1[6-9]|2\d|3[01])\.\d+\.\d+']:
                matches = re.findall(pattern, result.stdout)
                if matches:
                    return matches[0]
        except Exception:
            pass

        # Fallback: socket-based detection
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("8.8.8.8", 80))
            ip = s.getsockname()[0]
            s.close()
            return ip
        except Exception:
            return "127.0.0.1"

    async def start_mdns(self):
        """Start mDNS service broadcast"""
        if not MDNS_AVAILABLE:
            logger.warning("mDNS not available, skipping service registration")
            return

        try:
            self.async_zeroconf = AsyncZeroconf()

            # Get local IP
            local_ip = self._get_local_ip()

            # Get device name from config (sanitize for mDNS - replace spaces with dashes)
            device_name = self.config.DEVICE_NAME
            mdns_name = device_name.replace(" ", "-")

            self.service_info = ServiceInfo(
                "_xiaozhi-display._tcp.local.",  # Service type
                f"{mdns_name}._xiaozhi-display._tcp.local.",  # Instance name
                addresses=[socket.inet_aton(local_ip)],
                port=self.config.PORT,
                properties={
                    "version": "1.0",
                    "device": device_name
                }
            )
            await self.async_zeroconf.async_register_service(self.service_info)
            logger.info(f"mDNS service registered: {device_name} at {local_ip}:{self.config.PORT}")
        except Exception as e:
            import traceback
            logger.error(f"Failed to start mDNS service: {type(e).__name__}: {e}")
            if logger.isEnabledFor(logging.DEBUG):
                logger.debug(traceback.format_exc())

    async def stop_mdns(self):
        """Stop mDNS service"""
        if self.async_zeroconf and self.service_info:
            try:
                await self.async_zeroconf.async_unregister_service(self.service_info)
                await self.async_zeroconf.async_close()
                logger.info("mDNS service unregistered")
            except Exception as e:
                logger.error(f"Failed to stop mDNS service: {e}")
            finally:
                self.async_zeroconf = None
                self.service_info = None

    async def handle_root(self, request: web.Request) -> web.Response:
        """Handle root path - WebSocket for device, HTML for browser"""
        logger.debug(f"Root request from {request.remote}, headers: {dict(request.headers)}")
        # Check if this is a WebSocket upgrade request
        if request.headers.get("Upgrade", "").lower() == "websocket":
            logger.info(f"WebSocket upgrade request from {request.remote}")
            return await self.handle_device_ws(request)

        # Otherwise serve index.html
        index_path = self.web_dir / "index.html"
        if index_path.exists():
            return web.FileResponse(index_path)
        return web.Response(text="index.html not found", status=404)

    async def handle_device_ws(self, request: web.Request) -> web.WebSocketResponse:
        """Handle ESP32 device WebSocket connection (path: /)"""
        ws = web.WebSocketResponse()
        await ws.prepare(request)

        client_addr = request.remote
        logger.info(f"New WebSocket connection from {client_addr}")

        # Check if this is a device connection (will be confirmed by hello message)
        # For now, treat root path connections as potential device connections

        try:
            async for msg in ws:
                if msg.type == WSMsgType.TEXT:
                    await self._handle_text_message(ws, msg.data, client_addr)
                elif msg.type == WSMsgType.BINARY:
                    await self._handle_binary_message(msg.data)
                elif msg.type == WSMsgType.ERROR:
                    logger.error(f"WebSocket error: {ws.exception()}")
        except Exception as e:
            logger.error(f"Error handling WebSocket: {e}")
        finally:
            # Clean up
            if self.device_ws == ws:
                self.device_ws = None
                logger.info(f"Device disconnected: {client_addr}")
            elif ws in self.browser_clients:
                self.browser_clients.discard(ws)
                logger.info(f"Browser client disconnected: {client_addr}")

        return ws

    async def handle_browser_ws(self, request: web.Request) -> web.WebSocketResponse:
        """Handle browser WebSocket connection (path: /ws)"""
        ws = web.WebSocketResponse()
        await ws.prepare(request)

        client_addr = request.remote
        logger.info(f"Browser client connected: {client_addr}")

        self.browser_clients.add(ws)

        # Send current UI state to new client
        if self.current_ui_state:
            try:
                await ws.send_json(self.current_ui_state)
            except Exception as e:
                logger.error(f"Failed to send initial state: {e}")

        try:
            async for msg in ws:
                if msg.type == WSMsgType.TEXT:
                    # Browser clients don't send meaningful messages, just keep alive
                    pass
                elif msg.type == WSMsgType.ERROR:
                    logger.error(f"Browser WebSocket error: {ws.exception()}")
        except Exception as e:
            logger.error(f"Error handling browser WebSocket: {e}")
        finally:
            self.browser_clients.discard(ws)
            logger.info(f"Browser client disconnected: {client_addr}")

        return ws

    async def _handle_text_message(self, ws: web.WebSocketResponse, data: str, client_addr: str):
        """Handle text (JSON) message"""
        try:
            msg = json.loads(data)
            msg_type = msg.get("type")

            if msg_type == "hello":
                # Device hello message
                logger.info(f"Device hello: {msg}")
                self.device_ws = ws

                # Send acknowledgment
                response = {
                    "type": "hello_ack",
                    "status": "ok",
                    "mode": "ui_state"
                }
                await ws.send_json(response)
                logger.info("Sent hello_ack to device")

            elif msg_type == "ui_state":
                # UI state update from device
                self.current_ui_state = msg
                self.ui_state_count += 1

                if self.ui_state_count == 1:
                    logger.info("Received first UI state update")
                elif self.ui_state_count % 100 == 0:
                    logger.info(f"Received {self.ui_state_count} UI state updates")

                # Broadcast to all browser clients
                await self._broadcast_to_browsers(msg)

            elif msg_type == "preview_image":
                # Preview image from camera - forward to browsers
                image_size = msg.get("size", 0)
                logger.info(f"Received preview image: {image_size} bytes")

                # Broadcast to all browser clients
                await self._broadcast_to_browsers(msg)

        except json.JSONDecodeError as e:
            logger.error(f"Failed to parse JSON: {e}")
        except Exception as e:
            logger.error(f"Error handling text message: {e}")

    async def _handle_binary_message(self, data: bytes):
        """Handle binary message (audio frame)"""
        if len(data) < 1:
            logger.warning("Message too short")
            return

        msg_type = data[0]

        # Debug log for binary messages
        if self.audio_count == 0 and msg_type in (MSG_TYPE_AUDIO_PCM, MSG_TYPE_AUDIO_FRAME):
            logger.info(f"Received first binary audio message: type=0x{msg_type:02x}, size={len(data)}")

        if msg_type == MSG_TYPE_AUDIO_PCM:
            await self._handle_pcm_audio(data)
        elif msg_type == MSG_TYPE_AUDIO_FRAME:
            # Legacy Opus format (deprecated)
            await self._handle_audio_frame(data[4:])
        elif msg_type == MSG_TYPE_HEARTBEAT:
            pass  # Heartbeat, no action needed
        else:
            logger.debug(f"Unknown binary message type: 0x{msg_type:02x}, size={len(data)}")

    async def _handle_pcm_audio(self, data: bytes):
        """Handle PCM audio frame"""
        if len(data) < 9:  # type(1) + sample_rate(4) + samples(4)
            return

        # Parse PCM header (little-endian, ESP32 native)
        # type(1B) + sample_rate(4B LE) + samples(4B LE) + pcm_data
        sample_rate, samples = struct.unpack("<II", data[1:9])
        pcm_data = data[9:]

        expected_size = samples * 2  # 16-bit samples
        if len(pcm_data) != expected_size:
            logger.warning(f"PCM data size mismatch: expected {expected_size}, got {len(pcm_data)}")
            return

        # Count audio packets
        self.audio_count += 1

        # Play audio locally (if enabled)
        if self.audio_player:
            self.audio_player.play_pcm(pcm_data, sample_rate)

        # Forward to browser clients
        await self._broadcast_audio_to_browsers(pcm_data, sample_rate)

        # Log audio count periodically
        if self.audio_count == 1:
            logger.info(f"First audio packet: sample_rate={sample_rate}, samples={samples}, forwarded to {len(self.browser_clients)} browser(s)")
        elif self.audio_count % 100 == 0:
            logger.info(f"Audio packets forwarded: {self.audio_count}")

    async def _broadcast_audio_to_browsers(self, pcm_data: bytes, sample_rate: int):
        """Broadcast PCM audio to all browser clients"""
        if not self.browser_clients:
            return

        import base64
        audio_msg = {
            "type": "audio",
            "data": base64.b64encode(pcm_data).decode('ascii'),
            "sample_rate": sample_rate
        }

        disconnected = set()
        for client in self.browser_clients:
            try:
                await client.send_json(audio_msg)
            except Exception:
                disconnected.add(client)

        self.browser_clients -= disconnected

    async def _handle_audio_frame(self, payload: bytes):
        """Handle legacy Opus audio frame (deprecated)"""
        if len(payload) < 4:
            return

        # Parse audio frame header: sample_rate(2) + frame_duration(1) + reserved(1)
        sample_rate, frame_duration, _ = struct.unpack(">HBB", payload[:4])
        opus_data = payload[4:]

        # Play audio (Opus decode)
        if self.audio_player:
            self.audio_player.play_opus(opus_data, sample_rate, frame_duration)
            self.audio_count += 1

    async def _broadcast_to_browsers(self, msg: dict):
        """Broadcast message to all connected browser clients"""
        if not self.browser_clients:
            return

        # Send to all clients, remove disconnected ones
        disconnected = set()
        for client in self.browser_clients:
            try:
                await client.send_json(msg)
            except Exception as e:
                logger.debug(f"Failed to send to browser client: {e}")
                disconnected.add(client)

        # Remove disconnected clients
        self.browser_clients -= disconnected

    async def cleanup(self):
        """Cleanup resources"""
        await self.stop_mdns()
        if self.audio_player:
            self.audio_player.close()
        logger.info(f"Server stopped. Total UI states: {self.ui_state_count}, audio packets: {self.audio_count}")


async def main():
    """Main entry point"""
    print("""
+-----------------------------------------------------------+
|         Xiaozhi ESP32 Remote Display Server               |
|              (Web UI Mode)                                |
|                                                           |
|  Open http://localhost:8765 in your browser               |
|  Waiting for connection from SenseCAP Watcher...          |
+-----------------------------------------------------------+
    """)

    server = RemoteDisplayServer()

    # Initialize audio player (skip by default, enable with RD_LOCAL_AUDIO=1)
    # Audio is played via browser by default for consistent behavior
    if os.getenv("RD_LOCAL_AUDIO", "0") == "1":
        try:
            logger.info("Initializing local audio player (RD_LOCAL_AUDIO=1)...")
            server.audio_player = AudioPlayer(
                sample_rate=server.config.AUDIO_SAMPLE_RATE,
                channels=server.config.AUDIO_CHANNELS,
                frame_duration=server.config.AUDIO_FRAME_DURATION
            )
            logger.info("Local audio player initialized successfully")
        except Exception as e:
            logger.warning(f"Failed to initialize local audio player: {e}")
            server.audio_player = None
    else:
        logger.info("Local audio disabled, audio plays via browser only")

    # Create aiohttp app
    app = web.Application()

    # Routes
    app.router.add_route("*", "/", server.handle_root)  # Handle both HTTP and WebSocket
    app.router.add_get("/ws", server.handle_browser_ws)
    app.router.add_get("/device", server.handle_device_ws)  # Alternative device endpoint

    # Static files
    if server.web_dir.exists():
        app.router.add_static("/web/", server.web_dir)
    if server.assets_dir.exists():
        app.router.add_static("/assets/", server.assets_dir)

    # Setup cleanup
    async def on_shutdown(app):
        await server.cleanup()

    app.on_shutdown.append(on_shutdown)

    # Run server
    runner = web.AppRunner(app)
    await runner.setup()

    site = web.TCPSite(runner, server.config.HOST, server.config.PORT)
    await site.start()

    # Start mDNS service broadcast
    await server.start_mdns()

    logger.info(f"Server running on http://{server.config.HOST}:{server.config.PORT}")
    logger.info(f"Browser UI: http://localhost:{server.config.PORT}")
    logger.info(f"Device WebSocket: ws://localhost:{server.config.PORT}/device")
    logger.info(f"mDNS service: _xiaozhi-display._tcp.local. ({server.config.DEVICE_NAME})")

    # Wait forever
    try:
        await asyncio.Event().wait()
    except asyncio.CancelledError:
        pass
    finally:
        await runner.cleanup()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        logger.info("Server stopped by user")
