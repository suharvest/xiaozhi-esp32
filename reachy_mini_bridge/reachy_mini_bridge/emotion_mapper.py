"""Map LLM emotions to Reachy Mini robot movements.

Translates emotion strings from the xiaozhi server into physical
robot expressions using head poses, antenna movements, and recorded moves.
"""

import asyncio
import logging
import random
import time
from dataclasses import dataclass
from queue import Queue, Empty
from typing import Optional

logger = logging.getLogger(__name__)


@dataclass
class HeadPose:
    """Target head pose in degrees and mm."""
    z: float = 0.0       # yaw (left/right rotation)
    pitch: float = 0.0   # up/down tilt (via x or pitch)
    roll: float = 0.0    # side tilt
    duration: float = 0.8


@dataclass
class AntennaMotion:
    """Antenna target positions."""
    left: float = 0.0    # degrees
    right: float = 0.0   # degrees
    duration: float = 0.5


@dataclass
class RobotExpression:
    """A complete robot expression combining head and antenna movement."""
    head: Optional[HeadPose] = None
    antenna: Optional[AntennaMotion] = None
    recorded_move: Optional[str] = None
    description: str = ""


# Emotion -> Expression mapping
# These map the emotion strings that xiaozhi LLM sends
EMOTION_MAP: dict[str, list[RobotExpression]] = {
    # Positive emotions
    "happy": [
        RobotExpression(
            head=HeadPose(z=0, pitch=5, roll=0, duration=0.6),
            antenna=AntennaMotion(left=30, right=30, duration=0.4),
            description="Happy nod with antenna raise",
        ),
        RobotExpression(
            head=HeadPose(z=10, pitch=5, roll=5, duration=0.5),
            antenna=AntennaMotion(left=40, right=20, duration=0.3),
            description="Happy tilt",
        ),
    ],
    "laugh": [
        RobotExpression(
            head=HeadPose(z=0, pitch=10, roll=0, duration=0.3),
            antenna=AntennaMotion(left=45, right=45, duration=0.2),
            description="Laughing - quick nod with high antennas",
        ),
    ],
    "excited": [
        RobotExpression(
            head=HeadPose(z=0, pitch=8, roll=0, duration=0.4),
            antenna=AntennaMotion(left=50, right=50, duration=0.3),
            description="Excited - head up, antennas high",
        ),
    ],

    # Curious / thinking
    "thinking": [
        RobotExpression(
            head=HeadPose(z=15, pitch=-5, roll=10, duration=1.0),
            antenna=AntennaMotion(left=10, right=-10, duration=0.8),
            description="Thinking - head tilted, asymmetric antennas",
        ),
    ],
    "confused": [
        RobotExpression(
            head=HeadPose(z=-10, pitch=0, roll=-15, duration=0.8),
            antenna=AntennaMotion(left=-20, right=20, duration=0.6),
            description="Confused - head tilt with opposite antennas",
        ),
    ],
    "curious": [
        RobotExpression(
            head=HeadPose(z=10, pitch=-8, roll=5, duration=0.7),
            antenna=AntennaMotion(left=25, right=25, duration=0.5),
            description="Curious - slight lean forward and tilt",
        ),
    ],

    # Negative emotions
    "sad": [
        RobotExpression(
            head=HeadPose(z=0, pitch=-15, roll=0, duration=1.2),
            antenna=AntennaMotion(left=-30, right=-30, duration=1.0),
            description="Sad - head down, antennas drooping",
        ),
    ],
    "angry": [
        RobotExpression(
            head=HeadPose(z=0, pitch=-5, roll=0, duration=0.3),
            antenna=AntennaMotion(left=-10, right=-10, duration=0.2),
            description="Angry - slight head forward, antennas down",
        ),
    ],
    "surprised": [
        RobotExpression(
            head=HeadPose(z=0, pitch=15, roll=0, duration=0.3),
            antenna=AntennaMotion(left=50, right=50, duration=0.2),
            description="Surprised - head back, antennas way up",
        ),
    ],
    "fear": [
        RobotExpression(
            head=HeadPose(z=-5, pitch=-10, roll=-5, duration=0.4),
            antenna=AntennaMotion(left=-20, right=-20, duration=0.3),
            description="Fear - slight retreat",
        ),
    ],

    # Conversational
    "neutral": [
        RobotExpression(
            head=HeadPose(z=0, pitch=0, roll=0, duration=0.8),
            antenna=AntennaMotion(left=0, right=0, duration=0.6),
            description="Neutral - return to center",
        ),
    ],
    "listening": [
        RobotExpression(
            head=HeadPose(z=5, pitch=-3, roll=3, duration=0.6),
            antenna=AntennaMotion(left=15, right=15, duration=0.5),
            description="Listening - slight lean with attentive antennas",
        ),
    ],
    "agreeing": [
        RobotExpression(
            head=HeadPose(z=0, pitch=8, roll=0, duration=0.4),
            description="Agreeing - nod",
        ),
    ],
    "disagreeing": [
        RobotExpression(
            head=HeadPose(z=15, pitch=0, roll=0, duration=0.3),
            description="Disagreeing - head shake",
        ),
    ],
}

# Chinese emotion names mapping
EMOTION_ALIASES = {
    "开心": "happy",
    "高兴": "happy",
    "大笑": "laugh",
    "笑": "laugh",
    "兴奋": "excited",
    "思考": "thinking",
    "困惑": "confused",
    "好奇": "curious",
    "伤心": "sad",
    "难过": "sad",
    "生气": "angry",
    "愤怒": "angry",
    "惊讶": "surprised",
    "害怕": "fear",
    "中性": "neutral",
    "平静": "neutral",
    "倾听": "listening",
    "同意": "agreeing",
    "点头": "agreeing",
    "摇头": "disagreeing",
    "不同意": "disagreeing",
}


class EmotionMapper:
    """Maps emotion strings to robot movements via a queue."""

    def __init__(self, intensity: float = 0.7):
        self._intensity = max(0.0, min(1.0, intensity))
        self._move_queue: Queue[RobotExpression] = Queue(maxsize=20)
        self._last_emotion = "neutral"
        self._last_expression_time = 0.0

    @property
    def move_queue(self) -> Queue:
        return self._move_queue

    def map_emotion(self, emotion: str) -> Optional[RobotExpression]:
        """Convert an emotion string to a RobotExpression."""
        # Normalize: try alias mapping first
        normalized = EMOTION_ALIASES.get(emotion, emotion).lower()

        expressions = EMOTION_MAP.get(normalized)
        if not expressions:
            logger.debug(f"No mapping for emotion: {emotion} (normalized: {normalized})")
            return None

        # Pick a random variant if multiple available
        expr = random.choice(expressions)

        # Apply intensity scaling
        if expr.head:
            expr.head.z *= self._intensity
            expr.head.pitch *= self._intensity
            expr.head.roll *= self._intensity

        if expr.antenna:
            expr.antenna.left *= self._intensity
            expr.antenna.right *= self._intensity

        return expr

    def queue_emotion(self, emotion: str):
        """Map an emotion and add it to the motion queue."""
        # Debounce: don't repeat the same emotion too quickly
        now = time.monotonic()
        if emotion == self._last_emotion and (now - self._last_expression_time) < 2.0:
            return

        expr = self.map_emotion(emotion)
        if expr:
            try:
                self._move_queue.put_nowait(expr)
                self._last_emotion = emotion
                self._last_expression_time = now
                logger.info(f"Queued expression: {expr.description}")
            except Exception:
                logger.warning("Motion queue full, dropping expression")

    def get_next_expression(self) -> Optional[RobotExpression]:
        """Get the next queued expression, or None."""
        try:
            return self._move_queue.get_nowait()
        except Empty:
            return None

    def get_idle_expression(self) -> RobotExpression:
        """Generate a subtle idle animation."""
        z = random.uniform(-5, 5)
        pitch = random.uniform(-3, 3)
        roll = random.uniform(-3, 3)
        antenna = random.uniform(-10, 10)
        return RobotExpression(
            head=HeadPose(z=z, pitch=pitch, roll=roll, duration=2.0),
            antenna=AntennaMotion(left=antenna, right=antenna, duration=1.5),
            description="Idle breathing",
        )
