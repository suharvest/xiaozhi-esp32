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
import traceback
from pathlib import Path
from typing import Optional, Set

from aiohttp import web, WSMsgType

from config import Config
from audio_player import AudioPlayer

# MCP process management
import subprocess
import shutil
import tempfile

# Optional mDNS support
try:
    from zeroconf import ServiceInfo, Zeroconf, ServiceBrowser, ServiceStateChange
    from zeroconf.asyncio import AsyncZeroconf
    MDNS_AVAILABLE = True
except ImportError:
    MDNS_AVAILABLE = False
    logger = logging.getLogger(__name__)
    logger.warning("zeroconf not installed, mDNS service discovery disabled. Install with: pip install zeroconf")

import aiohttp

# Setup logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

# Message types (binary protocol from ESP32)
MSG_TYPE_UI_STATE = 0x10
MSG_TYPE_AUDIO_FRAME = 0x02  # Opus audio (primary)
MSG_TYPE_AUDIO_PCM = 0x03    # PCM audio (legacy)
MSG_TYPE_HEARTBEAT = 0x04

# Narrate config fields (persisted to disk)
NARRATE_FIELDS = ("name", "xiaozhiWsUrl", "mcpEnabled", "autoConnect", "defaultBgUrl", "triggers")

class MCPManager:
    """Manages the MCP subprocess for narrate mode"""

    def __init__(self, server: 'RemoteDisplayServer'):
        self.server = server
        self.process: Optional[subprocess.Popen] = None
        self.running = False
        self._read_task: Optional[asyncio.Task] = None

    async def start(self, xiaozhi_ws_url: str) -> bool:
        """Start the MCP pipe subprocess (bridges stdio MCP server to WebSocket)"""
        if self.running:
            logger.warning("MCP already running")
            return True

        base_dir = Path(__file__).parent
        pipe_script = base_dir / "mcp_pipe.py"
        mcp_script = base_dir / "narrate_mcp.py"

        if not pipe_script.exists():
            logger.error(f"MCP pipe script not found: {pipe_script}")
            return False
        if not mcp_script.exists():
            logger.error(f"MCP server script not found: {mcp_script}")
            return False

        # Find uv or python
        uv_path = shutil.which("uv")
        python_path = shutil.which("python3") or shutil.which("python")

        try:
            env = os.environ.copy()
            env["NARRATE_SERVER_URL"] = f"http://localhost:{self.server.config.PORT}"
            env["MCP_ENDPOINT"] = xiaozhi_ws_url

            if uv_path:
                # Use uv run for dependency management
                self.process = subprocess.Popen(
                    [uv_path, "run", "python", str(pipe_script), str(mcp_script)],
                    cwd=str(base_dir),
                    env=env,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True
                )
            elif python_path:
                self.process = subprocess.Popen(
                    [python_path, str(pipe_script), str(mcp_script)],
                    cwd=str(base_dir),
                    env=env,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True
                )
            else:
                logger.error("Neither uv nor python found")
                return False

            self.running = True
            self._read_task = asyncio.create_task(self._read_output())
            logger.info(f"MCP process started (PID: {self.process.pid})")

            # Broadcast status
            await self.server.broadcast_mcp_status(True)
            return True

        except Exception as e:
            logger.error(f"Failed to start MCP process: {e}")
            return False

    async def stop(self):
        """Stop the MCP subprocess"""
        if not self.running:
            return

        self.running = False

        if self._read_task:
            self._read_task.cancel()
            try:
                await self._read_task
            except asyncio.CancelledError:
                pass
            self._read_task = None

        if self.process:
            try:
                self.process.terminate()
                self.process.wait(timeout=5)
            except Exception as e:
                logger.warning(f"Error stopping MCP process: {e}")
                self.process.kill()
            self.process = None

        logger.info("MCP process stopped")
        await self.server.broadcast_mcp_status(False)

    async def _read_output(self):
        """Read and log MCP process output"""
        if not self.process or not self.process.stdout:
            return

        try:
            loop = asyncio.get_event_loop()
            while self.running and self.process.poll() is None:
                line = await loop.run_in_executor(None, self.process.stdout.readline)
                if line:
                    logger.info(f"[MCP] {line.rstrip()}")
        except Exception as e:
            if self.running:
                logger.error(f"Error reading MCP output: {e}")

        # Process ended
        if self.running:
            self.running = False
            logger.warning("MCP process exited unexpectedly")
            await self.server.broadcast_mcp_status(False)


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

        # UDP beacon discovery
        self._beacon_devices: dict = {}  # keyed by IP: {name, ip, port, board, version, last_seen}

        # Narrate mode
        self.narrate_config: dict = {
            "name": "",
            "xiaozhiWsUrl": "",
            "mcpEnabled": False,
            "autoConnect": False,
            "defaultBgUrl": "",
            "triggers": []
        }
        self.mcp_manager = MCPManager(self)
        self.current_narrate_state: Optional[dict] = None

        # Uploads directory for local images
        self.uploads_dir = self.base_dir / "uploads"
        self.uploads_dir.mkdir(exist_ok=True)

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
            logger.info(f"Local IP for mDNS: {local_ip}")

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

    async def start_beacon_listener(self):
        """Listen for UDP beacon broadcasts from ESP32 devices on port 12321.
        Uses asyncio.DatagramProtocol for compatibility with Python 3.9+."""
        BEACON_PORT = 12321
        server_ref = self

        class BeaconProtocol(asyncio.DatagramProtocol):
            def datagram_received(self, data: bytes, addr: tuple):
                try:
                    text = data.decode('utf-8', errors='ignore')
                    if not text.startswith('XZWATCH|'):
                        return
                    parts = text.split('|')
                    if len(parts) < 6:
                        return
                    # XZWATCH|name|ip|port|board|version
                    _, name, ip, port_str, board, version = parts[:6]
                    source_ip = addr[0]  # Use source IP as authoritative
                    server_ref._beacon_devices[source_ip] = {
                        "name": name,
                        "ip": source_ip,
                        "port": int(port_str),
                        "board": board,
                        "version": version,
                        "last_seen": asyncio.get_event_loop().time(),
                    }
                except Exception as e:
                    logger.debug(f"Failed to parse beacon: {e}")

            def error_received(self, exc):
                logger.debug(f"Beacon listener error: {exc}")

        try:
            loop = asyncio.get_event_loop()
            kwargs = {"local_addr": ('0.0.0.0', BEACON_PORT), "allow_broadcast": True}
            if hasattr(socket, 'SO_REUSEPORT'):
                kwargs["reuse_port"] = True
            transport, _ = await loop.create_datagram_endpoint(
                BeaconProtocol, **kwargs
            )
            self._beacon_transport = transport
            logger.info(f"UDP beacon listener started on port {BEACON_PORT}")
        except Exception as e:
            logger.error(f"Failed to start beacon listener: {e}")
            self._beacon_transport = None

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
                # Notify browsers that device disconnected
                await self._broadcast_to_browsers({
                    "type": "device_status",
                    "connected": False,
                })
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

        # Send current device connection status to new client
        try:
            device_connected = self.device_ws is not None and not self.device_ws.closed
            await ws.send_json({
                "type": "device_status",
                "connected": device_connected,
            })
        except Exception as e:
            logger.error(f"Failed to send device status: {e}")

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

                # Notify browsers that a device connected
                await self._broadcast_to_browsers({
                    "type": "device_status",
                    "connected": True,
                    "address": client_addr
                })

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

        if msg_type == MSG_TYPE_AUDIO_FRAME:
            await self._handle_opus_audio(data)
        elif msg_type == MSG_TYPE_AUDIO_PCM:
            # Legacy PCM format (deprecated)
            await self._handle_pcm_audio(data)
        elif msg_type == MSG_TYPE_HEARTBEAT:
            pass  # Heartbeat, no action needed
        else:
            logger.debug(f"Unknown binary message type: 0x{msg_type:02x}, size={len(data)}")

    async def _handle_opus_audio(self, data: bytes):
        """Handle Opus audio frame from ESP32, decode to PCM for browser/local playback"""
        if len(data) < 4:  # type(1) + sample_rate(2) + frame_duration(1)
            return

        # Parse Opus header (little-endian)
        # type(1B) + sample_rate(2B LE) + frame_duration(1B) + opus_data
        sample_rate, frame_duration = struct.unpack("<HB", data[1:4])
        opus_data = data[4:]

        if len(opus_data) == 0:
            return

        self.audio_count += 1

        # Decode Opus to PCM (for both browser and local playback)
        pcm_data = self._decode_opus(opus_data, sample_rate, frame_duration)
        if pcm_data is None:
            return

        # Play locally (if enabled, default is browser-only)
        if self.audio_player:
            self.audio_player.play_pcm(pcm_data, sample_rate)

        # Forward PCM to browser clients (default audio path)
        if self.browser_clients:
            await self._broadcast_audio_to_browsers(pcm_data, sample_rate)

        # Log periodically
        if self.audio_count == 1:
            logger.info(f"First Opus audio: sr={sample_rate}, dur={frame_duration}ms, opus={len(opus_data)}B, pcm={len(pcm_data)}B, browsers={len(self.browser_clients)}")
        elif self.audio_count % 100 == 0:
            logger.info(f"Audio packets: {self.audio_count}")

    def _init_opus_decoder(self, sample_rate: int, frame_duration: int):
        """Initialize or reinitialize Opus decoder"""
        try:
            import opuslib
            self._opus_decoder = opuslib.Decoder(sample_rate, 1)
            self._opus_decoder_sr = sample_rate
            self._opus_frame_duration = frame_duration
            self._opus_frame_size = sample_rate * frame_duration // 1000
            logger.info(f"Opus decoder initialized: {sample_rate}Hz, {frame_duration}ms")
        except Exception as e:
            logger.error(f"Failed to create Opus decoder: {e}")
            self._opus_decoder = None

    def _decode_opus(self, opus_data: bytes, sample_rate: int, frame_duration: int) -> bytes:
        """Decode Opus to PCM bytes"""
        # Lazy init or reinit on sample rate / duration change
        if (not hasattr(self, '_opus_decoder') or self._opus_decoder is None
                or sample_rate != self._opus_decoder_sr
                or frame_duration != self._opus_frame_duration):
            self._init_opus_decoder(sample_rate, frame_duration)

        if self._opus_decoder is None:
            return None

        try:
            return self._opus_decoder.decode(opus_data, self._opus_frame_size)
        except Exception as e:
            logger.error(f"Opus decode error: {e}")
            return None

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

    async def _handle_pcm_audio(self, data: bytes):
        """Handle legacy PCM audio frame (deprecated, for backward compatibility)"""
        if len(data) < 9:  # type(1) + sample_rate(4) + samples(4)
            return

        sample_rate, samples = struct.unpack("<II", data[1:9])
        pcm_data = data[9:]

        expected_size = samples * 2
        if len(pcm_data) != expected_size:
            return

        self.audio_count += 1
        if self.audio_player:
            self.audio_player.play_pcm(pcm_data, sample_rate)
        await self._broadcast_audio_to_browsers(pcm_data, sample_rate)

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

    # ========== Narrate Mode APIs ==========

    async def handle_get_config(self, request: web.Request) -> web.Response:
        """GET /api/config - Get narrate mode config"""
        return web.json_response({
            **self.narrate_config,
            "mcpConnected": self.mcp_manager.running
        })

    async def handle_post_config(self, request: web.Request) -> web.Response:
        """POST /api/config - Update narrate mode config"""
        try:
            data = await request.json()

            # Update config fields
            for field in ["name", "xiaozhiWsUrl", "mcpEnabled", "autoConnect", "defaultBgUrl", "triggers"]:
                if field in data:
                    self.narrate_config[field] = data[field]

            return web.json_response({"success": True})

        except Exception as e:
            logger.error(f"Failed to update config: {e}")
            return web.json_response({"success": False, "error": str(e)}, status=400)

    async def handle_mcp_control(self, request: web.Request) -> web.Response:
        """POST /api/mcp - Control MCP server (start/stop/restart)"""
        try:
            data = await request.json()
            action = data.get("action")

            if action == "start":
                ws_url = self.narrate_config.get("xiaozhiWsUrl", "")
                if not ws_url:
                    return web.json_response({"success": False, "error": "WebSocket URL not configured"}, status=400)
                if self.mcp_manager.running:
                    return web.json_response({"success": True, "message": "MCP already running"})
                success = await self.mcp_manager.start(ws_url)
                return web.json_response({"success": success})

            elif action == "stop":
                await self.mcp_manager.stop()
                return web.json_response({"success": True})

            elif action == "restart":
                ws_url = self.narrate_config.get("xiaozhiWsUrl", "")
                if not ws_url:
                    return web.json_response({"success": False, "error": "WebSocket URL not configured"}, status=400)
                await self.mcp_manager.stop()
                success = await self.mcp_manager.start(ws_url)
                return web.json_response({"success": success})

            else:
                return web.json_response({"success": False, "error": "Invalid action"}, status=400)

        except Exception as e:
            logger.error(f"Failed to control MCP: {e}")
            return web.json_response({"success": False, "error": str(e)}, status=400)

    async def handle_upload_image(self, request: web.Request) -> web.Response:
        """POST /api/upload - Upload local image"""
        try:
            import uuid
            import time

            reader = await request.multipart()
            field = await reader.next()

            if field is None or field.name != 'file':
                return web.json_response({"success": False, "error": "No file provided"}, status=400)

            # Get filename and extension
            filename = field.filename or "image"
            ext = Path(filename).suffix.lower()
            if ext not in ['.jpg', '.jpeg', '.png', '.gif', '.webp']:
                return web.json_response({"success": False, "error": "Invalid image format"}, status=400)

            # Generate unique filename
            unique_name = f"{int(time.time())}_{uuid.uuid4().hex[:8]}{ext}"
            file_path = self.uploads_dir / unique_name

            # Save file
            size = 0
            with open(file_path, 'wb') as f:
                while True:
                    chunk = await field.read_chunk()
                    if not chunk:
                        break
                    size += len(chunk)
                    f.write(chunk)

            # Return URL
            url = f"/uploads/{unique_name}"
            logger.info(f"Image uploaded: {unique_name} ({size} bytes)")

            return web.json_response({
                "success": True,
                "url": url,
                "filename": unique_name,
                "size": size
            })

        except Exception as e:
            logger.error(f"Failed to upload image: {e}")
            return web.json_response({"success": False, "error": str(e)}, status=400)

    async def handle_list_images(self, request: web.Request) -> web.Response:
        """GET /api/images - List uploaded images"""
        try:
            images = []
            for f in self.uploads_dir.iterdir():
                if f.is_file() and f.suffix.lower() in ['.jpg', '.jpeg', '.png', '.gif', '.webp']:
                    images.append({
                        "filename": f.name,
                        "url": f"/uploads/{f.name}",
                        "size": f.stat().st_size,
                        "modified": f.stat().st_mtime
                    })
            # Sort by modified time, newest first
            images.sort(key=lambda x: x["modified"], reverse=True)
            return web.json_response({"success": True, "images": images})

        except Exception as e:
            logger.error(f"Failed to list images: {e}")
            return web.json_response({"success": False, "error": str(e)}, status=400)

    async def handle_delete_image(self, request: web.Request) -> web.Response:
        """DELETE /api/images/{filename} - Delete uploaded image"""
        try:
            filename = request.match_info.get('filename')
            if not filename:
                return web.json_response({"success": False, "error": "Filename required"}, status=400)

            file_path = self.uploads_dir / filename
            if not file_path.exists():
                return web.json_response({"success": False, "error": "File not found"}, status=404)

            # Security check: ensure file is within uploads_dir
            if not str(file_path.resolve()).startswith(str(self.uploads_dir.resolve())):
                return web.json_response({"success": False, "error": "Invalid path"}, status=400)

            file_path.unlink()
            logger.info(f"Image deleted: {filename}")
            return web.json_response({"success": True})

        except Exception as e:
            logger.error(f"Failed to delete image: {e}")
            return web.json_response({"success": False, "error": str(e)}, status=400)

    async def handle_narrate(self, request: web.Request) -> web.Response:
        """POST /api/narrate - Control narrate mode display"""
        try:
            data = await request.json()
            action = data.get("action")

            if action == "show":
                image_url = data.get("imageUrl")
                caption = data.get("caption", "")

                if not image_url:
                    return web.json_response({"success": False, "error": "imageUrl required"}, status=400)

                # Update state
                self.current_narrate_state = {
                    "type": "narrate_state",
                    "action": "show",
                    "imageUrl": image_url,
                    "caption": caption
                }

                # Broadcast to browsers
                await self._broadcast_to_browsers(self.current_narrate_state)
                logger.info(f"Narrate: show image {image_url}")

                return web.json_response({"success": True})

            elif action == "clear":
                # Clear state
                self.current_narrate_state = {
                    "type": "narrate_state",
                    "action": "clear"
                }

                # Broadcast to browsers
                await self._broadcast_to_browsers(self.current_narrate_state)
                logger.info("Narrate: clear background")

                return web.json_response({"success": True})

            else:
                return web.json_response({"success": False, "error": "Invalid action"}, status=400)

        except Exception as e:
            logger.error(f"Failed to handle narrate: {e}")
            return web.json_response({"success": False, "error": str(e)}, status=400)

    async def broadcast_mcp_status(self, connected: bool):
        """Broadcast MCP connection status to browsers"""
        msg = {
            "type": "mcp_status",
            "connected": connected
        }
        await self._broadcast_to_browsers(msg)

    # ========== Cast Control APIs ==========

    async def handle_get_devices(self, request: web.Request) -> web.Response:
        """GET /api/devices - Discover ESP32 devices via UDP beacon (with mDNS fallback)"""
        devices = []

        # First: collect from beacon cache (prune entries older than 10s)
        now = asyncio.get_event_loop().time()
        stale_keys = [ip for ip, d in self._beacon_devices.items()
                      if now - d["last_seen"] > 10]
        for k in stale_keys:
            del self._beacon_devices[k]

        for d in self._beacon_devices.values():
            devices.append({
                "name": d["name"],
                "ip": d["ip"],
                "port": d["port"],
                "board": d.get("board", ""),
                "version": d.get("version", ""),
            })

        # Fallback: if no beacon devices found, try mDNS
        if not devices and MDNS_AVAILABLE:
            try:
                loop = asyncio.get_event_loop()
                devices = await loop.run_in_executor(None, self._discover_devices_sync)
            except Exception as e:
                logger.error(f"mDNS discovery failed: {e}")

        logger.info(f"Discovered {len(devices)} device(s)")
        return web.json_response({"success": True, "devices": devices})

    def _discover_devices_sync(self) -> list:
        """Synchronous mDNS browse (runs in thread executor)"""
        import time
        import threading

        devices = []
        event = threading.Event()

        def on_service_state_change(zeroconf: Zeroconf, service_type: str,
                                    name: str, state_change: ServiceStateChange):
            if state_change != ServiceStateChange.Added:
                return
            info = zeroconf.get_service_info(service_type, name)
            if info is None:
                return
            addresses = info.parsed_addresses()
            if not addresses:
                return
            ip = addresses[0]
            port = info.port
            props = {k.decode(): v.decode() if isinstance(v, bytes) else v
                     for k, v in info.properties.items()}
            instance_name = name.replace(f".{service_type}", "")
            devices.append({
                "name": instance_name,
                "ip": ip,
                "port": port,
                "board": props.get("board", ""),
                "version": props.get("version", ""),
            })
            event.set()

        zc = Zeroconf()
        browser = ServiceBrowser(zc, "_xiaozhi-watcher._tcp.local.",
                                 handlers=[on_service_state_change])
        # Wait: up to 3s total, but return early if at least one device found
        event.wait(timeout=3)
        if devices:
            # Give a short extra window for more devices
            time.sleep(0.5)
        browser.cancel()
        zc.close()
        return devices

    async def handle_cast_start(self, request: web.Request) -> web.Response:
        """POST /api/cast/start - Tell ESP32 to connect back to our WS"""
        try:
            data = await request.json()
            ip = data.get("ip")
            port = data.get("port", 80)

            if not ip:
                return web.json_response({"success": False, "error": "Missing ip"}, status=400)

            # Build our WS URL that ESP32 should connect to
            local_ip = self._get_local_ip()
            ws_url = f"ws://{local_ip}:{self.config.PORT}"

            # POST to ESP32's HTTP server
            esp32_url = f"http://{ip}:{port}/api/start_cast"
            async with aiohttp.ClientSession() as session:
                async with session.post(esp32_url,
                                        json={"ws_url": ws_url},
                                        timeout=aiohttp.ClientTimeout(total=5)) as resp:
                    result = await resp.json()
                    return web.json_response(result)

        except aiohttp.ClientError as e:
            logger.error(f"Failed to reach ESP32: {e}")
            return web.json_response({"success": False, "error": f"Cannot reach device: {e}"}, status=502)
        except Exception as e:
            logger.error(f"Cast start failed: {e}")
            return web.json_response({"success": False, "error": str(e)}, status=500)

    async def handle_cast_stop(self, request: web.Request) -> web.Response:
        """POST /api/cast/stop - Tell ESP32 to stop casting"""
        try:
            data = await request.json()
            ip = data.get("ip")
            port = data.get("port", 80)

            if not ip:
                return web.json_response({"success": False, "error": "Missing ip"}, status=400)

            esp32_url = f"http://{ip}:{port}/api/stop_cast"
            async with aiohttp.ClientSession() as session:
                async with session.post(esp32_url,
                                        timeout=aiohttp.ClientTimeout(total=5)) as resp:
                    result = await resp.json()
                    return web.json_response(result)

        except aiohttp.ClientError as e:
            logger.error(f"Failed to reach ESP32: {e}")
            return web.json_response({"success": False, "error": f"Cannot reach device: {e}"}, status=502)
        except Exception as e:
            logger.error(f"Cast stop failed: {e}")
            return web.json_response({"success": False, "error": str(e)}, status=500)

    async def cleanup(self):
        """Cleanup resources"""
        if hasattr(self, '_beacon_transport') and self._beacon_transport:
            self._beacon_transport.close()
            self._beacon_transport = None
            logger.info("UDP beacon listener stopped")
        await self.stop_mdns()
        await self.mcp_manager.stop()
        if self.audio_player:
            self.audio_player.close()
        logger.info(f"Server stopped. Total UI states: {self.ui_state_count}, audio packets: {self.audio_count}")


def _kill_previous_instance(port: int):
    """Kill any previous server process listening on the same port"""
    import signal
    my_pid = os.getpid()
    try:
        # lsof works on macOS and Linux
        result = subprocess.run(
            ["lsof", "-ti", f":{port}"],
            capture_output=True, text=True, timeout=5
        )
        for line in result.stdout.strip().splitlines():
            pid = int(line.strip())
            if pid != my_pid:
                logger.info(f"Killing previous server process (PID {pid}) on port {port}")
                os.kill(pid, signal.SIGTERM)
        # Wait for port release (up to 3s)
        if result.stdout.strip():
            import time
            for _ in range(6):
                time.sleep(0.5)
                check = subprocess.run(
                    ["lsof", "-ti", f":{port}"],
                    capture_output=True, text=True, timeout=5
                )
                remaining = [int(p) for p in check.stdout.strip().splitlines() if int(p) != my_pid]
                if not remaining:
                    break
            else:
                # Force kill if still alive
                for pid in remaining:
                    logger.warning(f"Force killing PID {pid}")
                    os.kill(pid, signal.SIGKILL)
                time.sleep(0.5)
    except FileNotFoundError:
        # lsof not available (e.g. minimal Docker image), try ss
        try:
            result = subprocess.run(
                ["ss", "-tlnp", f"sport = :{port}"],
                capture_output=True, text=True, timeout=5
            )
            import re
            for match in re.finditer(r'pid=(\d+)', result.stdout):
                pid = int(match.group(1))
                if pid != my_pid:
                    logger.info(f"Killing previous server process (PID {pid}) on port {port}")
                    os.kill(pid, signal.SIGTERM)
            if 'pid=' in result.stdout:
                import time
                time.sleep(0.5)
        except Exception:
            pass
    except Exception as e:
        logger.warning(f"Failed to check for previous instance: {e}")


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

    # Kill any previous instance on the same port
    _kill_previous_instance(server.config.PORT)

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

    # Narrate mode APIs
    app.router.add_get("/api/config", server.handle_get_config)
    app.router.add_post("/api/config", server.handle_post_config)
    app.router.add_post("/api/narrate", server.handle_narrate)
    app.router.add_post("/api/mcp", server.handle_mcp_control)

    # Cast control APIs (RPi discovers ESP32, sends HTTP to start/stop casting)
    app.router.add_get("/api/devices", server.handle_get_devices)
    app.router.add_post("/api/cast/start", server.handle_cast_start)
    app.router.add_post("/api/cast/stop", server.handle_cast_stop)

    # Image upload APIs
    app.router.add_post("/api/upload", server.handle_upload_image)
    app.router.add_get("/api/images", server.handle_list_images)
    app.router.add_delete("/api/images/{filename}", server.handle_delete_image)

    # Static files
    if server.web_dir.exists():
        app.router.add_static("/web/", server.web_dir)
    if server.assets_dir.exists():
        app.router.add_static("/assets/", server.assets_dir)
    if server.uploads_dir.exists():
        app.router.add_static("/uploads/", server.uploads_dir)

    # Setup cleanup
    async def on_shutdown(app):
        await server.cleanup()

    app.on_shutdown.append(on_shutdown)

    # Run server
    runner = web.AppRunner(app)
    await runner.setup()

    site = web.TCPSite(runner, server.config.HOST, server.config.PORT)
    await site.start()

    # Start UDP beacon listener for ESP32 discovery
    await server.start_beacon_listener()

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
