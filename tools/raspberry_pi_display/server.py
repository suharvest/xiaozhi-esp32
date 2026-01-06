"""
Remote Display Server - WebSocket server for receiving UI state and audio
Uses Pygame for UI rendering (UI state sync mode)
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
import sys
import logging
import threading
from typing import Optional

import websockets
from websockets.server import WebSocketServerProtocol

from config import Config
from ui_renderer import UIRenderer
from audio_player import AudioPlayer

# Setup logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

# Message types
MSG_TYPE_UI_STATE = 0x10
MSG_TYPE_AUDIO_FRAME = 0x02
MSG_TYPE_HEARTBEAT = 0x04


class RemoteDisplayServer:
    def __init__(self):
        self.config = Config()
        self.ui_renderer: Optional[UIRenderer] = None
        self.audio_player: Optional[AudioPlayer] = None
        self.connected_client: Optional[WebSocketServerProtocol] = None
        self.running = True
        self.ui_state_count = 0
        self.audio_count = 0

    def cleanup(self):
        """Cleanup resources"""
        if self.ui_renderer:
            self.ui_renderer.close()
        if self.audio_player:
            self.audio_player.close()
        logger.info(f"Server stopped. Total UI states: {self.ui_state_count}, audio packets: {self.audio_count}")

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
            logger.info(f"Client disconnected (ConnectionClosed): code={e.code}, reason={e.reason}")
        except websockets.exceptions.ConnectionClosedError as e:
            logger.info(f"Client disconnected (ConnectionClosedError): code={e.code}, reason={e.reason}")
        except websockets.exceptions.ConnectionClosedOK as e:
            logger.info(f"Client disconnected (ConnectionClosedOK): code={e.code}, reason={e.reason}")
        except Exception as e:
            logger.error(f"Error handling client: {e}")
            import traceback
            traceback.print_exc()
        finally:
            self.connected_client = None
            logger.info(f"Client connection handler finished: {client_addr}")

    async def handle_binary_message(self, data: bytes):
        """Handle binary message (audio frame)"""
        if len(data) < 4:
            logger.warning("Message too short")
            return

        # Parse header: type(1) + flags(1) + payload_size(2)
        msg_type, flags, payload_size = struct.unpack(">BBH", data[:4])
        payload = data[4:]

        if msg_type == MSG_TYPE_AUDIO_FRAME:
            await self.handle_audio_frame(payload)
        elif msg_type == MSG_TYPE_HEARTBEAT:
            pass  # Heartbeat, no action needed

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
                    "mode": "ui_state"
                }
                await websocket.send(json.dumps(response))
                logger.info("Sent hello_ack to client")

            elif msg_type == "ui_state":
                # Update UI state
                if self.ui_renderer:
                    try:
                        self.ui_renderer.update_state(data)
                        self.ui_state_count += 1

                        if self.ui_state_count == 1:
                            logger.info("Received first UI state update")
                        elif self.ui_state_count % 100 == 0:
                            logger.info(f"Received {self.ui_state_count} UI state updates")
                    except Exception as e:
                        logger.error(f"Error updating UI state: {e}")
                        import traceback
                        traceback.print_exc()

        except json.JSONDecodeError as e:
            logger.error(f"Failed to parse JSON: {e}")
        except Exception as e:
            logger.error(f"Error handling text message: {e}")
            import traceback
            traceback.print_exc()


# Global server instance for signal handler
server_instance: Optional[RemoteDisplayServer] = None


def signal_handler(signum, frame):
    """Handle shutdown signals"""
    global server_instance
    logger.info("Shutting down...")
    if server_instance:
        server_instance.running = False
    sys.exit(0)


def main():
    """Main function - runs Pygame in main thread, WebSocket in background"""
    global server_instance

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    print("""
+-----------------------------------------------------------+
|         Xiaozhi ESP32 Remote Display Server               |
|              (UI State Sync Mode)                         |
|                                                           |
|  Waiting for connection from SenseCAP Watcher...          |
|  Press 'F' or F11 to toggle fullscreen                    |
|  Press 'q' or ESC to quit                                 |
+-----------------------------------------------------------+
    """)

    server = RemoteDisplayServer()
    server_instance = server

    # Initialize components with error handling
    try:
        logger.info("Initializing UI Renderer...")
        server.ui_renderer = UIRenderer(
            width=server.config.SCREEN_WIDTH,
            height=server.config.SCREEN_HEIGHT,
            scale=server.config.DISPLAY_SCALE,
            fullscreen=server.config.FULLSCREEN
        )
        logger.info("UI Renderer initialized successfully")
    except Exception as e:
        logger.error(f"Failed to initialize UI Renderer: {e}")
        import traceback
        traceback.print_exc()
        return

    try:
        logger.info("Initializing Audio Player...")
        server.audio_player = AudioPlayer(
            sample_rate=server.config.AUDIO_SAMPLE_RATE,
            channels=server.config.AUDIO_CHANNELS,
            frame_duration=server.config.AUDIO_FRAME_DURATION
        )
        logger.info("Audio Player initialized successfully")
    except Exception as e:
        logger.warning(f"Failed to initialize Audio Player: {e}")
        server.audio_player = None

    # Create event loop for background WebSocket server
    loop = asyncio.new_event_loop()

    async def run_websocket_server():
        """Run WebSocket server as async task"""
        logger.info(f"Starting server on ws://{server.config.HOST}:{server.config.PORT}")
        try:
            async with websockets.serve(
                server.handle_client,
                server.config.HOST,
                server.config.PORT,
                max_size=1024 * 1024,
                ping_interval=30,
                ping_timeout=10
            ):
                logger.info("Server started, waiting for connection...")
                while server.running and server.ui_renderer.running:
                    await asyncio.sleep(0.1)
                logger.info(f"WebSocket server loop ended (running={server.running}, ui_running={server.ui_renderer.running})")
        except Exception as e:
            logger.error(f"WebSocket server error: {e}")
            import traceback
            traceback.print_exc()

    # Start WebSocket server in background thread
    def run_loop():
        asyncio.set_event_loop(loop)
        loop.run_until_complete(run_websocket_server())
        logger.info("WebSocket thread finished")

    ws_thread = threading.Thread(target=run_loop, daemon=True)
    ws_thread.start()
    logger.info("WebSocket thread started")

    # Main thread: handle Pygame rendering
    try:
        logger.info("Starting main render loop...")
        frame_count = 0
        while server.running and server.ui_renderer.running:
            # Handle Pygame events
            if not server.ui_renderer.handle_events():
                logger.info("UI renderer requested exit")
                break

            # Render UI
            try:
                server.ui_renderer.render()
            except Exception as e:
                logger.error(f"Render error: {e}")
                import traceback
                traceback.print_exc()

            # Limit frame rate
            server.ui_renderer.tick(30)

            frame_count += 1
            if frame_count == 1:
                logger.info("First frame rendered successfully")

    except KeyboardInterrupt:
        logger.info("Keyboard interrupt received")
    except Exception as e:
        logger.error(f"Main loop error: {e}")
        import traceback
        traceback.print_exc()
    finally:
        logger.info("Cleaning up...")
        server.running = False
        server.cleanup()


if __name__ == "__main__":
    main()
