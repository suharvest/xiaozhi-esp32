"""XVF3800 DOA (Direction of Arrival) reader and head tracking.

Reads DOA from the Seeed ReSpeaker XVF3800 4-mic array via USB
control transfers, and converts the azimuth angle into Reachy Mini
head yaw commands so the robot looks toward the speaker.

The XVF3800 does all audio processing on-chip (AEC, beamforming,
noise suppression). We only read the DOA metadata here.
"""

import asyncio
import logging
import math
import struct
import time
from typing import Optional

logger = logging.getLogger(__name__)

# Try to import pyusb
try:
    import usb.core
    import usb.util
    HAS_USB = True
except ImportError:
    HAS_USB = False
    logger.debug("pyusb not installed. DOA tracking unavailable.")


# XVF3800 USB identifiers (Seeed ReSpeaker)
XVF3800_VID = 0x2886
XVF3800_PID = 0x001A

# USB control transfer parameters
# (resource_id, command_id, response_length, type)
_PARAMS = {
    "AEC_AZIMUTH_VALUES": (33, 75, 17, "radians"),  # 4 floats + 1 status byte
    "DOA_VALUE":          (20, 18, 5,  "uint16"),    # 1 uint16 + padding + status
}

_USB_TIMEOUT_MS = 100


class Xvf3800DoaReader:
    """Read DOA (Direction of Arrival) from XVF3800 via USB vendor control transfers.

    The XVF3800 computes azimuth for 3 beams:
      - beam 0, 1: focused tracking beams
      - beam 2: free-running scanning beam
      - beam 3 (auto-selected): best estimate of current speaker direction

    We use beam 3 (auto-selected) as our DOA source.
    """

    def __init__(self):
        self._dev = None

    def open(self) -> bool:
        """Find and open the XVF3800 USB device."""
        if not HAS_USB:
            logger.warning("pyusb not installed, DOA unavailable")
            return False

        try:
            self._dev = usb.core.find(idVendor=XVF3800_VID, idProduct=XVF3800_PID)
            if self._dev is None:
                logger.warning(
                    f"XVF3800 not found (VID={XVF3800_VID:#06x} PID={XVF3800_PID:#06x}). "
                    "Is the ReSpeaker connected?"
                )
                return False
            logger.info("XVF3800 DOA reader opened")
            return True
        except Exception as e:
            logger.error(f"Failed to open XVF3800: {e}")
            return False

    def close(self):
        """Release the USB device."""
        self._dev = None

    def read_azimuth(self) -> Optional[float]:
        """Read the auto-selected beam azimuth in radians.

        Returns None if no speech detected or on error.
        The azimuth is relative to the mic array's forward direction:
          0 = straight ahead
          positive = left (counter-clockwise)
          negative = right (clockwise)
        """
        if not self._dev:
            return None

        resid, cmdid, length, dtype = _PARAMS["AEC_AZIMUTH_VALUES"]
        try:
            response = self._dev.ctrl_transfer(
                usb.util.CTRL_IN | usb.util.CTRL_TYPE_VENDOR | usb.util.CTRL_RECIPIENT_DEVICE,
                0,             # bRequest
                0x80 | cmdid,  # wValue (read bit set)
                resid,         # wIndex
                length,        # wLength
                _USB_TIMEOUT_MS,
            )
            raw = response.tobytes()
            # 1 status byte + 4 floats (little-endian)
            if len(raw) < 17:
                return None
            azimuths = struct.unpack("<ffff", raw[1:17])
            # beam 3 = auto-selected (best DOA estimate)
            az = azimuths[3]
            # NaN means no speech detected
            if math.isnan(az):
                return None
            return az
        except Exception as e:
            logger.debug(f"DOA read error: {e}")
            return None

    def read_azimuth_degrees(self) -> Optional[float]:
        """Read the auto-selected beam azimuth in degrees."""
        az = self.read_azimuth()
        if az is not None:
            return math.degrees(az)
        return None


class HeadTracker:
    """Tracks speaker DOA and generates head yaw commands.

    Reads DOA from XVF3800 at a configurable rate and smoothly
    updates the target head yaw. Integrates with the emotion/motion
    system by yielding head poses that the motion loop can apply.

    Design:
    - Only controls yaw (left/right). Pitch and roll are left to
      emotion expressions and idle animations.
    - Uses exponential smoothing to avoid jittery head movements.
    - Has a "no speech" timeout: returns to neutral after silence.
    - Respects a yaw range limit to stay within natural head motion.
    """

    def __init__(
        self,
        poll_interval: float = 0.1,
        smoothing: float = 0.3,
        max_yaw: float = 30.0,
        no_speech_timeout: float = 3.0,
        min_angle_change: float = 3.0,
    ):
        """
        Args:
            poll_interval: Seconds between DOA reads.
            smoothing: Exponential smoothing factor (0=no smoothing, 1=instant).
            max_yaw: Maximum head yaw in degrees (clamped).
            no_speech_timeout: Seconds of silence before returning to neutral.
            min_angle_change: Minimum angle change (degrees) to trigger a move.
        """
        self._poll_interval = poll_interval
        self._smoothing = smoothing
        self._max_yaw = max_yaw
        self._no_speech_timeout = no_speech_timeout
        self._min_angle_change = min_angle_change

        self._doa_reader = Xvf3800DoaReader()
        self._enabled = False
        self._paused = False
        self._current_yaw = 0.0
        self._target_yaw = 0.0
        self._last_speech_time = 0.0
        self._last_applied_yaw = 0.0

    @property
    def enabled(self) -> bool:
        return self._enabled

    def start(self) -> bool:
        """Initialize DOA reader and enable tracking."""
        if not self._doa_reader.open():
            logger.warning("Head tracking disabled: XVF3800 DOA unavailable")
            return False
        self._enabled = True
        self._paused = False
        self._current_yaw = 0.0
        self._target_yaw = 0.0
        self._last_speech_time = time.monotonic()
        logger.info("Head tracking enabled")
        return True

    def stop(self):
        """Stop tracking and release DOA reader."""
        self._enabled = False
        self._doa_reader.close()
        logger.info("Head tracking stopped")

    def pause(self):
        """Pause tracking (e.g. during TTS playback).

        Freezes the no-speech timer so that TTS duration doesn't
        count as silence.  The head stays at its last position.
        """
        self._paused = True

    def resume(self):
        """Resume tracking after a pause.

        Resets the no-speech timer so the head won't immediately
        snap back to neutral.
        """
        self._paused = False
        self._last_speech_time = time.monotonic()

    def update(self) -> Optional[float]:
        """Read DOA and return smoothed target yaw in degrees, or None if no update needed.

        Call this periodically (e.g. every poll_interval seconds).
        Returns the yaw angle the head should turn to, or None if
        no movement is needed.
        """
        if not self._enabled or self._paused:
            return None

        now = time.monotonic()
        doa_deg = self._doa_reader.read_azimuth_degrees()

        if doa_deg is not None:
            # Speech detected - update target
            self._last_speech_time = now
            # Clamp to max range
            clamped = max(-self._max_yaw, min(self._max_yaw, doa_deg))
            self._target_yaw = clamped
        elif (now - self._last_speech_time) > self._no_speech_timeout:
            # No speech for a while - drift back to neutral
            self._target_yaw = 0.0

        # Exponential smoothing
        self._current_yaw += self._smoothing * (self._target_yaw - self._current_yaw)

        # Only report if the change is significant
        delta = abs(self._current_yaw - self._last_applied_yaw)
        if delta >= self._min_angle_change:
            self._last_applied_yaw = self._current_yaw
            return self._current_yaw

        return None
