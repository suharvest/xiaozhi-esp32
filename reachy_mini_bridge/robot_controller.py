"""Reachy Mini robot controller.

Wraps the Reachy Mini SDK to provide high-level control for
audio capture/playback, head movement, and antenna expressions.
"""

import asyncio
import logging
import threading
import time
from typing import Optional

import numpy as np

from .emotion_mapper import RobotExpression, HeadPose, AntennaMotion

logger = logging.getLogger(__name__)

# Try to import reachy_mini SDK
try:
    from reachy_mini import ReachyMini
    from reachy_mini.utils import create_head_pose
    HAS_REACHY = True
except ImportError:
    HAS_REACHY = False
    logger.warning(
        "reachy_mini SDK not found. Running in simulation mode. "
        "Install with: pip install reachy-mini"
    )


class RobotController:
    """High-level controller for Reachy Mini robot."""

    def __init__(self, media_backend: str = "default_no_video"):
        self._media_backend = media_backend
        self._mini: Optional[object] = None
        self._running = False
        self._recording = False
        self._playing = False

    def connect(self) -> bool:
        """Connect to the Reachy Mini robot."""
        if not HAS_REACHY:
            logger.info("Simulation mode: no real robot connected")
            return True

        try:
            self._mini = ReachyMini(
                log_level="INFO",
                media_backend=self._media_backend,
            )
            self._mini.__enter__()
            logger.info("Connected to Reachy Mini")

            # Wake up the robot
            self._mini.wake_up()
            time.sleep(1.0)
            return True
        except Exception as e:
            logger.error(f"Failed to connect to Reachy Mini: {e}")
            return False

    def disconnect(self):
        """Disconnect from the robot."""
        if self._mini:
            try:
                self._mini.goto_sleep()
                time.sleep(1.0)
                self._mini.__exit__(None, None, None)
            except Exception as e:
                logger.warning(f"Error during disconnect: {e}")
            self._mini = None

    def get_audio_sample_rate(self) -> int:
        """Get the microphone input sample rate."""
        if not HAS_REACHY or not self._mini:
            return 16000
        try:
            return self._mini.media.get_input_audio_samplerate()
        except Exception:
            return 16000

    def get_output_sample_rate(self) -> int:
        """Get the speaker output sample rate."""
        if not HAS_REACHY or not self._mini:
            return 24000
        try:
            return self._mini.media.get_output_audio_samplerate()
        except Exception:
            return 24000

    def start_recording(self):
        """Start capturing audio from the microphone."""
        if not self._recording and self._mini and HAS_REACHY:
            try:
                self._mini.media.start_recording()
                self._recording = True
                logger.info("Started recording")
            except Exception as e:
                logger.error(f"Failed to start recording: {e}")

    def stop_recording(self):
        """Stop capturing audio."""
        if self._recording and self._mini and HAS_REACHY:
            try:
                self._mini.media.stop_recording()
                self._recording = False
                logger.info("Stopped recording")
            except Exception as e:
                logger.error(f"Failed to stop recording: {e}")

    def get_audio_sample(self) -> Optional[np.ndarray]:
        """Get a chunk of captured audio samples."""
        if not HAS_REACHY or not self._mini or not self._recording:
            return None
        try:
            return self._mini.media.get_audio_sample()
        except Exception:
            return None

    def start_playing(self):
        """Start audio playback to the speaker."""
        if not self._playing and self._mini and HAS_REACHY:
            try:
                self._mini.media.start_playing()
                self._playing = True
                logger.info("Started playing")
            except Exception as e:
                logger.error(f"Failed to start playing: {e}")

    def stop_playing(self):
        """Stop audio playback."""
        if self._playing and self._mini and HAS_REACHY:
            try:
                self._mini.media.stop_playing()
                self._playing = False
                logger.info("Stopped playing")
            except Exception as e:
                logger.error(f"Failed to stop playing: {e}")

    def push_audio(self, samples: np.ndarray):
        """Push PCM float32 samples to the speaker."""
        if not HAS_REACHY or not self._mini or not self._playing:
            return
        try:
            self._mini.media.push_audio_sample(samples)
        except Exception as e:
            logger.error(f"Failed to push audio: {e}")

    def execute_expression(self, expr: RobotExpression):
        """Execute a robot expression (head + antenna movement)."""
        if not HAS_REACHY or not self._mini:
            logger.info(f"[SIM] Expression: {expr.description}")
            return

        try:
            kwargs = {}

            if expr.head:
                head_pose = create_head_pose(
                    z=expr.head.z,
                    roll=expr.head.roll,
                    degrees=True,
                    mm=True,
                )
                kwargs["head"] = head_pose
                kwargs["duration"] = expr.head.duration

            if expr.antenna:
                kwargs["left_antenna"] = expr.antenna.left
                kwargs["right_antenna"] = expr.antenna.right
                if "duration" not in kwargs:
                    kwargs["duration"] = expr.antenna.duration

            if expr.recorded_move:
                self._mini.goto_target(**kwargs)
            elif kwargs:
                self._mini.goto_target(**kwargs)

            logger.debug(f"Executed: {expr.description}")
        except Exception as e:
            logger.error(f"Failed to execute expression: {e}")

    def look_neutral(self):
        """Return to neutral position."""
        self.execute_expression(
            RobotExpression(
                head=HeadPose(z=0, pitch=0, roll=0, duration=1.0),
                antenna=AntennaMotion(left=0, right=0, duration=0.8),
                description="Return to neutral",
            )
        )
