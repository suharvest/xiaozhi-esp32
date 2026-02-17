"""Main entry point for the Reachy Mini <-> Xiaozhi bridge.

Orchestrates:
1. Connect to Reachy Mini robot (audio + motion)
2. Connect to Xiaozhi server via WebSocket
3. Stream mic audio -> OPUS encode -> server
4. Receive server OPUS audio -> decode -> play on speaker
5. Receive emotion/control messages -> robot expressions
6. Handle MCP tool calls from LLM
7. Handle abort speaking (user interrupts TTS)
"""

import argparse
import asyncio
import logging
import signal
import sys
import time
from typing import Optional

import numpy as np
import scipy.signal

from .config import BridgeConfig
from .xiaozhi_protocol import (
    XiaozhiProtocolClient, DeviceState, ListeningMode, AbortReason,
)
from .audio_bridge import AudioBridge
from .emotion_mapper import EmotionMapper
from .mcp_server import McpServer
from .robot_controller import RobotController

logger = logging.getLogger(__name__)


class ReachyXiaozhiBridge:
    """Main bridge application connecting Reachy Mini to Xiaozhi server."""

    def __init__(self, config: BridgeConfig):
        self._config = config
        self._running = False

        # Components
        self._protocol = XiaozhiProtocolClient(config.server, config.audio)
        self._audio = AudioBridge(config.audio)
        self._emotions = EmotionMapper(intensity=config.motion.emotion_intensity)
        self._robot = RobotController(media_backend=config.audio.media_backend)
        self._mcp = McpServer()

        # Wire up protocol callbacks
        self._protocol.on_audio(self._on_server_audio)
        self._protocol.on_tts_start(self._on_tts_start)
        self._protocol.on_tts_stop(self._on_tts_stop)
        self._protocol.on_tts_sentence(self._on_tts_sentence)
        self._protocol.on_stt_text(self._on_stt_text)
        self._protocol.on_emotion(self._on_emotion)
        self._protocol.on_state_change(self._on_state_change)
        self._protocol.on_mcp(self._on_mcp_message)
        self._protocol.on_system_command(self._on_system_command)
        self._protocol.on_alert(self._on_alert)

        # MCP send callback (needs async bridge)
        self._mcp_send_queue: asyncio.Queue[str] = asyncio.Queue()
        self._mcp.set_send_callback(self._queue_mcp_response)

        # VAD state for antenna-press abort
        self._is_speaking = False

    # -- MCP setup --

    def _setup_mcp_tools(self):
        """Register Reachy Mini tools with the MCP server."""
        self._mcp.add_reachy_mini_tools(self._robot, self._audio)

    def _queue_mcp_response(self, payload: str):
        """Queue an MCP response for async sending."""
        try:
            self._mcp_send_queue.put_nowait(payload)
        except asyncio.QueueFull:
            logger.warning("MCP send queue full, dropping response")

    # -- Callbacks from protocol --

    def _on_server_audio(self, opus_data: bytes):
        """Called when OPUS audio arrives from the server (TTS)."""
        self._audio.on_server_audio(opus_data)

    def _on_tts_start(self):
        """Server starts sending TTS audio."""
        logger.info("TTS started - begin playback")
        self._is_speaking = True
        self._robot.start_playing()

    def _on_tts_stop(self):
        """Server finished TTS."""
        logger.info("TTS stopped")
        self._is_speaking = False
        asyncio.get_event_loop().call_soon(self._flush_playback)

    def _on_tts_sentence(self, text: str):
        """A sentence of TTS text is being spoken."""
        logger.info(f"Speaking: {text}")

    def _on_stt_text(self, text: str):
        """User's speech was recognized."""
        logger.info(f"User said: {text}")

    def _on_emotion(self, emotion: str):
        """LLM emitted an emotion."""
        if self._config.motion.enable_emotions:
            self._emotions.queue_emotion(emotion)

    def _on_state_change(self, state: DeviceState):
        """Protocol state changed."""
        logger.debug(f"Device state: {state.value}")

    def _on_mcp_message(self, payload: dict):
        """Handle incoming MCP JSON-RPC message from server."""
        self._mcp.parse_message(payload)

    def _on_system_command(self, command: str):
        """Handle system command from server."""
        if command == "reboot":
            logger.warning("Server requested reboot - ignoring (not an embedded device)")
        else:
            logger.warning(f"Unknown system command: {command}")

    def _on_alert(self, status: str, message: str, emotion: str):
        """Handle alert from server."""
        logger.info(f"ALERT [{status}]: {message}")
        # Express the emotion physically
        if emotion and self._config.motion.enable_emotions:
            self._emotions.queue_emotion(emotion)

    def _flush_playback(self):
        """Play remaining audio and stop."""
        remaining = self._audio.drain_playback()
        if len(remaining) > 0:
            self._robot.push_audio(remaining)
        time.sleep(0.3)
        self._robot.stop_playing()

    # -- Main async loops --

    async def _audio_capture_loop(self):
        """Continuously capture mic audio and feed to audio bridge."""
        logger.info("Audio capture loop started")
        self._robot.start_recording()

        while self._running:
            sample = self._robot.get_audio_sample()
            if sample is not None:
                # Resample if needed (robot mic rate -> config input rate)
                robot_rate = self._robot.get_audio_sample_rate()
                if robot_rate != self._config.audio.input_sample_rate:
                    sample = scipy.signal.resample(
                        sample,
                        int(len(sample) * self._config.audio.input_sample_rate / robot_rate),
                    )
                self._audio.feed_captured_audio(sample)
            else:
                await asyncio.sleep(0.02)

        self._robot.stop_recording()
        logger.info("Audio capture loop stopped")

    async def _audio_send_loop(self):
        """Send encoded OPUS frames from audio bridge to the server."""
        logger.info("Audio send loop started")
        while self._running:
            try:
                opus_data = await asyncio.wait_for(
                    self._audio.send_queue.get(), timeout=0.1
                )
                await self._protocol.send_audio(opus_data)
            except asyncio.TimeoutError:
                continue
            except Exception as e:
                logger.error(f"Send loop error: {e}")
                await asyncio.sleep(0.1)
        logger.info("Audio send loop stopped")

    async def _playback_loop(self):
        """Push decoded audio from the audio bridge to the robot speaker."""
        logger.info("Playback loop started")
        while self._running:
            samples = self._audio.get_playback_samples()
            if samples is not None:
                # Resample if needed (server rate -> robot output rate)
                robot_out_rate = self._robot.get_output_sample_rate()
                if self._config.audio.output_sample_rate != robot_out_rate:
                    samples = scipy.signal.resample(
                        samples,
                        int(len(samples) * robot_out_rate / self._config.audio.output_sample_rate),
                    )
                self._robot.push_audio(samples)
            else:
                await asyncio.sleep(0.02)
        logger.info("Playback loop stopped")

    async def _motion_loop(self):
        """Process queued expressions and idle animations."""
        logger.info("Motion loop started")
        last_idle = time.monotonic()

        while self._running:
            expr = self._emotions.get_next_expression()
            if expr:
                self._robot.execute_expression(expr)
                await asyncio.sleep(expr.head.duration if expr.head else 0.5)
            elif (
                self._config.motion.idle_animation
                and self._protocol.state in (DeviceState.IDLE, DeviceState.LISTENING)
                and time.monotonic() - last_idle > self._config.motion.idle_animation_interval
            ):
                idle_expr = self._emotions.get_idle_expression()
                self._robot.execute_expression(idle_expr)
                last_idle = time.monotonic()
                await asyncio.sleep(idle_expr.head.duration if idle_expr.head else 1.0)
            else:
                await asyncio.sleep(0.1)
        logger.info("Motion loop stopped")

    async def _mcp_send_loop(self):
        """Send queued MCP responses to the server."""
        logger.info("MCP send loop started")
        while self._running:
            try:
                payload = await asyncio.wait_for(
                    self._mcp_send_queue.get(), timeout=0.5
                )
                await self._protocol.send_mcp_message(payload)
            except asyncio.TimeoutError:
                continue
            except Exception as e:
                logger.error(f"MCP send error: {e}")
                await asyncio.sleep(0.1)
        logger.info("MCP send loop stopped")

    async def _timeout_watchdog_loop(self):
        """Monitor connection timeout and handle reconnection."""
        logger.info("Timeout watchdog started")
        while self._running:
            if self._protocol.is_connected and self._protocol.is_timeout():
                logger.warning("Connection timeout detected")
                await self._protocol.disconnect()
                # Try to reconnect
                logger.info("Attempting reconnection...")
                if await self._protocol.connect():
                    mode = self._get_listening_mode()
                    await self._protocol.send_start_listening(mode)
                    logger.info("Reconnected successfully")
                else:
                    logger.error("Reconnection failed, will retry in 5s")
                    await asyncio.sleep(5.0)
            await asyncio.sleep(5.0)
        logger.info("Timeout watchdog stopped")

    def _get_listening_mode(self) -> ListeningMode:
        """Determine listening mode from config."""
        if self._config.wake_word.mode == "always_on":
            return ListeningMode.REALTIME
        elif self._config.wake_word.mode == "auto_stop":
            return ListeningMode.AUTO_STOP
        else:
            return ListeningMode.AUTO_STOP

    async def _abort_speaking(self):
        """Abort current TTS playback."""
        if self._is_speaking:
            logger.info("Aborting current speech")
            self._is_speaking = False
            # Clear playback queue
            self._audio.drain_playback()
            self._robot.stop_playing()
            # Tell server
            await self._protocol.send_abort_speaking(AbortReason.NONE)

    async def run(self):
        """Main run loop."""
        self._running = True

        # 1. Connect to robot
        logger.info("Connecting to Reachy Mini...")
        if not self._robot.connect():
            logger.error("Failed to connect to Reachy Mini")
            return

        # 2. Setup MCP tools
        self._setup_mcp_tools()

        # 3. Connect to xiaozhi server
        logger.info("Connecting to Xiaozhi server...")
        if not await self._protocol.connect():
            logger.error("Failed to connect to Xiaozhi server")
            self._robot.disconnect()
            return

        # 4. Start audio bridge
        self._audio.start()

        # 5. Tell server we're listening
        mode = self._get_listening_mode()
        await self._protocol.send_start_listening(mode)
        self._protocol.set_listening_mode(mode)

        # 6. Start playback on the robot
        self._robot.start_playing()

        logger.info("Bridge is running! Speak to interact...")

        # 7. Run all loops concurrently
        try:
            await asyncio.gather(
                self._protocol.receive_loop(),
                self._audio_capture_loop(),
                self._audio_send_loop(),
                self._playback_loop(),
                self._motion_loop(),
                self._mcp_send_loop(),
                self._timeout_watchdog_loop(),
            )
        except asyncio.CancelledError:
            logger.info("Bridge tasks cancelled")
        except Exception as e:
            logger.error(f"Bridge error: {e}")
        finally:
            await self.shutdown()

    async def shutdown(self):
        """Clean shutdown."""
        logger.info("Shutting down...")
        self._running = False
        self._audio.stop()
        self._robot.look_neutral()
        await self._protocol.disconnect()
        self._robot.disconnect()
        logger.info("Shutdown complete")


def main():
    """CLI entry point."""
    parser = argparse.ArgumentParser(
        description="Reachy Mini <-> Xiaozhi LLM Conversation Bridge"
    )
    parser.add_argument(
        "-c", "--config",
        type=str,
        default="bridge_config.yaml",
        help="Path to YAML config file (default: bridge_config.yaml)",
    )
    parser.add_argument(
        "-u", "--url",
        type=str,
        default=None,
        help="Xiaozhi WebSocket server URL (overrides config)",
    )
    parser.add_argument(
        "-t", "--token",
        type=str,
        default=None,
        help="Authorization token (overrides config)",
    )
    parser.add_argument(
        "--backend",
        type=str,
        choices=["default_no_video", "gstreamer_no_video", "webrtc"],
        default=None,
        help="Reachy Mini media backend (overrides config)",
    )
    parser.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="Enable debug logging",
    )
    parser.add_argument(
        "--generate-config",
        action="store_true",
        help="Generate a default config file and exit",
    )

    args = parser.parse_args()

    # Load or generate config
    config = BridgeConfig.from_yaml(args.config)

    if args.generate_config:
        config.to_yaml(args.config)
        print(f"Generated default config: {args.config}")
        return

    # Apply CLI overrides
    if args.url:
        config.server.websocket_url = args.url
    if args.token:
        config.server.token = args.token
    if args.backend:
        config.audio.media_backend = args.backend
    if args.verbose:
        config.log_level = "DEBUG"

    # Setup logging
    logging.basicConfig(
        level=getattr(logging, config.log_level.upper(), logging.INFO),
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    )

    # Create and run bridge
    bridge = ReachyXiaozhiBridge(config)

    # Handle Ctrl+C
    loop = asyncio.new_event_loop()

    def signal_handler():
        logger.info("Received stop signal")
        for task in asyncio.all_tasks(loop):
            task.cancel()

    loop.add_signal_handler(signal.SIGINT, signal_handler)
    loop.add_signal_handler(signal.SIGTERM, signal_handler)

    try:
        loop.run_until_complete(bridge.run())
    except KeyboardInterrupt:
        loop.run_until_complete(bridge.shutdown())
    finally:
        loop.close()


if __name__ == "__main__":
    main()
