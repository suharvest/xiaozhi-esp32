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
    # Jitter buffer settings
    BUFFER_MAX_SIZE = 20       # Maximum packets in buffer
    BUFFER_START_THRESHOLD = 3  # Start playback after this many packets buffered
    BUFFER_LOW_THRESHOLD = 1    # Re-buffer if drops below this

    def __init__(self, sample_rate: int = 24000, channels: int = 1, frame_duration: int = 60):
        self.sample_rate = sample_rate
        self.channels = channels
        self.frame_duration = frame_duration
        self.frame_size = sample_rate * frame_duration // 1000

        self.running = True
        self.audio_queue: Queue = Queue(maxsize=self.BUFFER_MAX_SIZE)
        self.decoder = None
        self.buffering = True  # Start in buffering mode

        if not PYAUDIO_AVAILABLE:
            logger.warning("PyAudio not available, audio playback disabled")
            self.enabled = False
            return

        self.enabled = True

        # Initialize Opus decoder (optional, only needed for legacy Opus format)
        if OPUSLIB_AVAILABLE:
            self.decoder = opuslib.Decoder(sample_rate, channels)
        else:
            logger.info("opuslib not available, Opus decoding disabled (PCM playback OK)")

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

        logger.info(f"Audio player started: {sample_rate}Hz, {frame_duration}ms frames, "
                    f"jitter buffer: {self.BUFFER_START_THRESHOLD}-{self.BUFFER_MAX_SIZE} packets")

    def play(self, opus_data: bytes, sample_rate: int, frame_duration: int):
        """Play Opus data (non-blocking) - Legacy method, requires opuslib"""
        if not self.enabled or not self.decoder:
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

    def play_pcm(self, pcm_data: bytes, sample_rate: int):
        """Play raw PCM data directly (non-blocking)"""
        if not PYAUDIO_AVAILABLE:
            return

        # Reinit stream if sample rate changed
        if sample_rate != self.sample_rate:
            self._reinit_stream(sample_rate)
            self.buffering = True  # Re-buffer on sample rate change

        try:
            # Put in queue, drop if full (overflow protection)
            if not self.audio_queue.full():
                self.audio_queue.put_nowait(pcm_data)
            else:
                # Queue full - drop oldest packet to make room
                try:
                    self.audio_queue.get_nowait()
                    self.audio_queue.put_nowait(pcm_data)
                except Empty:
                    pass
        except Exception as e:
            logger.error(f"Failed to queue PCM audio: {e}")

    def _reinit(self, sample_rate: int, frame_duration: int):
        """Reinitialize decoder with new sample rate (for Opus)"""
        self.sample_rate = sample_rate
        self.frame_duration = frame_duration
        self.frame_size = sample_rate * frame_duration // 1000

        if OPUSLIB_AVAILABLE:
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

    def _reinit_stream(self, sample_rate: int):
        """Reinitialize audio stream only (for PCM playback)"""
        self.sample_rate = sample_rate
        # Use a reasonable buffer size for PCM
        self.frame_size = sample_rate * 60 // 1000  # 60ms buffer

        # Clear existing queue
        while not self.audio_queue.empty():
            try:
                self.audio_queue.get_nowait()
            except Empty:
                break

        # Reopen audio stream
        self.stream.close()
        self.stream = self.pa.open(
            format=pyaudio.paInt16,
            channels=self.channels,
            rate=sample_rate,
            output=True,
            frames_per_buffer=self.frame_size
        )
        self.buffering = True  # Re-buffer after stream change
        logger.info(f"Audio stream reinitialized: {sample_rate}Hz (PCM mode)")

    def _play_loop(self):
        """Playback loop with jitter buffer"""
        while self.running:
            try:
                # Jitter buffer logic: wait for enough packets before playing
                if self.buffering:
                    if self.audio_queue.qsize() >= self.BUFFER_START_THRESHOLD:
                        self.buffering = False
                        logger.debug(f"Jitter buffer filled, starting playback ({self.audio_queue.qsize()} packets)")
                    else:
                        # Still buffering, wait a bit
                        try:
                            self.audio_queue.get(timeout=0.01)  # Quick check
                        except Empty:
                            pass
                        continue

                # Check if buffer is getting low
                if self.audio_queue.qsize() <= self.BUFFER_LOW_THRESHOLD and self.audio_queue.qsize() > 0:
                    logger.debug(f"Jitter buffer low ({self.audio_queue.qsize()} packets)")

                pcm = self.audio_queue.get(timeout=0.1)
                self.stream.write(pcm)

            except Empty:
                # Buffer underrun - switch back to buffering mode
                if not self.buffering:
                    self.buffering = True
                    logger.debug("Buffer underrun, re-buffering...")
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
