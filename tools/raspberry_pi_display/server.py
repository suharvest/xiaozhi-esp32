#!/usr/bin/env python3
"""
Xiaozhi ESP32 Remote Display Server

Receives screen frames and audio from SenseCAP Watcher via WebSocket
and displays/plays them on Raspberry Pi.

Usage:
    python3 server.py

Environment variables:
    RD_HOST   - Server host (default: 0.0.0.0)
    RD_PORT   - Server port (default: 8765)
    RD_SCALE  - Display scale (default: 1.5)
    RD_DEBUG  - Enable debug logging (default: 0)
"""

import asyncio
import struct
import json
import logging
import signal
import sys
from typing import Optional

try:
    import websockets
    from websockets.server import WebSocketServerProtocol
except ImportError:
    print("Error: websockets not installed. Run: pip install websockets")
    sys.exit(1)

from config import Config
from screen_renderer import ScreenRenderer
from audio_player import AudioPlayer

# Message types (must match ESP32 side)
MSG_TYPE_SCREEN_FRAME = 0x01
MSG_TYPE_AUDIO_FRAME = 0x02
MSG_TYPE_HEARTBEAT = 0x04

# Setup logging
logging.basicConfig(
    level=logging.DEBUG if Config.DEBUG else logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)


class RemoteDisplayServer:
    def __init__(self):
        self.config = Config
        self.screen_renderer: Optional[ScreenRenderer] = None
        self.audio_player: Optional[AudioPlayer] = None
        self.connected_client: Optional[WebSocketServerProtocol] = None
        self.running = True
        self.frame_count = 0
        self.audio_count = 0


    def cleanup(self):
        """Cleanup resources"""
        if self.screen_renderer:
            self.screen_renderer.close()
        if self.audio_player:
            self.audio_player.close()
        logger.info(f"Server stopped. Total frames: {self.frame_count}, audio packets: {self.audio_count}")

    async def handle_client(self, websocket: WebSocketServerProtocol):
        """Handle client connection"""
        client_addr = websocket.remote_address
        logger.info(f"Client connected: {client_addr}")

        if self.connected_client is not None:
            logger.warning("Rejecting new client, already have a connection")
            await websocket.close(1013, "Only one client allowed")
            return

        self.connected_client = websocket

        try:
            async for message in websocket:
                if isinstance(message, bytes):
                    await self.handle_binary_message(message)
                else:
                    await self.handle_text_message(message, websocket)
        except websockets.exceptions.ConnectionClosed as e:
            logger.info(f"Client disconnected: {e}")
        except Exception as e:
            logger.error(f"Error handling client: {e}")
        finally:
            self.connected_client = None
            logger.info(f"Client connection closed: {client_addr}")

    async def handle_binary_message(self, data: bytes):
        """Handle binary message"""
        if len(data) < 4:
            logger.warning("Message too short")
            return

        # Parse header: type(1) + flags(1) + payload_size(2)
        msg_type, flags, payload_size = struct.unpack(">BBH", data[:4])
        payload = data[4:]

        if msg_type == MSG_TYPE_SCREEN_FRAME:
            await self.handle_screen_frame(payload)
        elif msg_type == MSG_TYPE_AUDIO_FRAME:
            await self.handle_audio_frame(payload)
        elif msg_type == MSG_TYPE_HEARTBEAT:
            pass  # Heartbeat, no action needed

    async def handle_screen_frame(self, payload: bytes):
        """Handle screen frame"""
        if len(payload) < 8:
            return

        # Parse screen frame header: width(2) + height(2) + timestamp(4)
        width, height, timestamp = struct.unpack(">HHI", payload[:8])
        jpeg_data = payload[8:]

        # Update display
        if self.screen_renderer:
            self.screen_renderer.update(jpeg_data)
            self.frame_count += 1

            if self.frame_count % 50 == 0:
                logger.debug(f"Received {self.frame_count} frames, size: {len(jpeg_data)} bytes")

    async def handle_audio_frame(self, payload: bytes):
        """Handle audio frame"""
        if len(payload) < 4:
            return

        # Parse audio frame header: sample_rate(2) + frame_duration(1) + reserved(1)
        sample_rate, frame_duration, _ = struct.unpack(">HBB", payload[:4])
        opus_data = payload[4:]

        # Play audio
        if self.audio_player:
            self.audio_player.play(opus_data, sample_rate, frame_duration)
            self.audio_count += 1

    async def handle_text_message(self, message: str, websocket: WebSocketServerProtocol):
        """Handle text message (JSON)"""
        try:
            data = json.loads(message)
            msg_type = data.get("type")

            if msg_type == "hello":
                logger.info(f"Client hello: {data}")
                # Send acknowledgment
                response = {
                    "type": "hello_ack",
                    "status": "ok",
                    "config": {
                        "display_scale": self.config.DISPLAY_SCALE
                    }
                }
                await websocket.send(json.dumps(response))
        except json.JSONDecodeError:
            logger.warning(f"Invalid JSON: {message}")


def signal_handler(sig, frame):
    """Handle Ctrl+C"""
    logger.info("Shutting down...")
    sys.exit(0)


def main():
    """Main function - runs OpenCV in main thread, WebSocket in background"""
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    print("""
╔═══════════════════════════════════════════════════════════╗
║         Xiaozhi ESP32 Remote Display Server               ║
║                                                           ║
║  Waiting for connection from SenseCAP Watcher...          ║
║  Press Ctrl+C to stop, or 'q' in the display window       ║
╚═══════════════════════════════════════════════════════════╝
    """)

    server = RemoteDisplayServer()

    # Create event loop for background WebSocket server
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)

    # Initialize components
    server.screen_renderer = ScreenRenderer(
        width=server.config.SCREEN_WIDTH,
        height=server.config.SCREEN_HEIGHT,
        scale=server.config.DISPLAY_SCALE
    )
    server.audio_player = AudioPlayer(
        sample_rate=server.config.AUDIO_SAMPLE_RATE,
        channels=server.config.AUDIO_CHANNELS,
        frame_duration=server.config.AUDIO_FRAME_DURATION
    )

    async def run_websocket_server():
        """Run WebSocket server as async task"""
        logger.info(f"Starting server on ws://{server.config.HOST}:{server.config.PORT}")
        async with websockets.serve(
            server.handle_client,
            server.config.HOST,
            server.config.PORT,
            max_size=1024 * 1024,
            ping_interval=30,
            ping_timeout=10
        ):
            logger.info("Server started, waiting for connection...")
            while server.running and server.screen_renderer.running:
                await asyncio.sleep(0.1)

    # Start WebSocket server in background thread
    import threading

    def run_loop():
        loop.run_until_complete(run_websocket_server())

    ws_thread = threading.Thread(target=run_loop, daemon=True)
    ws_thread.start()

    # Main thread: handle OpenCV rendering
    try:
        while server.running and server.screen_renderer.running:
            if not server.screen_renderer.render_once():
                break
    except KeyboardInterrupt:
        pass
    finally:
        server.running = False
        server.cleanup()


if __name__ == "__main__":
    main()
