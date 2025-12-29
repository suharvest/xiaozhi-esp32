"""
Audio Player - Opus decode and PyAudio playback
"""

import logging
from queue import Queue, Empty
from threading import Thread

logger = logging.getLogger(__name__)

# Try to import optional dependencies
try:
    import pyaudio
    PYAUDIO_AVAILABLE = True
except ImportError:
    PYAUDIO_AVAILABLE = False
    logger.warning("PyAudio not available, audio playback disabled")

try:
    import opuslib
    OPUSLIB_AVAILABLE = True
except ImportError:
    OPUSLIB_AVAILABLE = False
    logger.warning("opuslib not available, audio playback disabled")


class AudioPlayer:
    def __init__(self, sample_rate: int = 24000, channels: int = 1, frame_duration: int = 60):
        self.sample_rate = sample_rate
        self.channels = channels
        self.frame_duration = frame_duration
        self.frame_size = sample_rate * frame_duration // 1000

        self.running = True
        self.audio_queue: Queue = Queue(maxsize=10)

        if not PYAUDIO_AVAILABLE or not OPUSLIB_AVAILABLE:
            logger.warning("Audio playback not available")
            self.enabled = False
            return

        self.enabled = True

        # Initialize Opus decoder
        self.decoder = opuslib.Decoder(sample_rate, channels)

        # Initialize PyAudio
        self.pa = pyaudio.PyAudio()
        self.stream = self.pa.open(
            format=pyaudio.paInt16,
            channels=channels,
            rate=sample_rate,
            output=True,
            frames_per_buffer=self.frame_size
        )

        # Start playback thread
        self.play_thread = Thread(target=self._play_loop, daemon=True)
        self.play_thread.start()

        logger.info(f"Audio player started: {sample_rate}Hz, {frame_duration}ms frames")

    def play(self, opus_data: bytes, sample_rate: int, frame_duration: int):
        """Play Opus data (non-blocking)"""
        if not self.enabled:
            return

        # Reinit decoder if sample rate changed
        if sample_rate != self.sample_rate:
            self._reinit(sample_rate, frame_duration)

        try:
            # Decode Opus to PCM
            pcm = self.decoder.decode(opus_data, self.frame_size)

            # Put in queue (drop old if full)
            if self.audio_queue.full():
                try:
                    self.audio_queue.get_nowait()
                except Empty:
                    pass
            self.audio_queue.put_nowait(pcm)
        except Exception as e:
            logger.error(f"Failed to decode audio: {e}")

    def _reinit(self, sample_rate: int, frame_duration: int):
        """Reinitialize decoder with new sample rate"""
        self.sample_rate = sample_rate
        self.frame_duration = frame_duration
        self.frame_size = sample_rate * frame_duration // 1000

        self.decoder = opuslib.Decoder(sample_rate, self.channels)

        # Reopen audio stream
        self.stream.close()
        self.stream = self.pa.open(
            format=pyaudio.paInt16,
            channels=self.channels,
            rate=sample_rate,
            output=True,
            frames_per_buffer=self.frame_size
        )
        logger.info(f"Audio reinitialized: {sample_rate}Hz")

    def _play_loop(self):
        """Playback loop"""
        while self.running:
            try:
                pcm = self.audio_queue.get(timeout=0.1)
                self.stream.write(pcm)
            except Empty:
                pass
            except Exception as e:
                logger.error(f"Playback error: {e}")

    def close(self):
        """Close player"""
        self.running = False
        if hasattr(self, 'play_thread') and self.play_thread.is_alive():
            self.play_thread.join(timeout=1.0)
        if hasattr(self, 'stream'):
            self.stream.close()
        if hasattr(self, 'pa'):
            self.pa.terminate()
        logger.info("Audio player stopped")
