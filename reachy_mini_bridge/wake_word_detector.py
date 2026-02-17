"""Local wake word detection using sherpa-onnx keyword spotting.

Processes streaming PCM audio and fires a callback when the
configured keyword (e.g. "你好小智") is detected.

sherpa-onnx provides low-latency, on-device Chinese keyword spotting
with pre-trained models.  It runs entirely on CPU and doesn't need
a network connection.
"""

import logging
import time
from typing import Callable, Optional

import numpy as np

logger = logging.getLogger(__name__)

try:
    import sherpa_onnx
    HAS_SHERPA = True
except ImportError:
    HAS_SHERPA = False
    logger.debug("sherpa-onnx not installed. Wake word detection unavailable.")


class WakeWordDetector:
    """Streaming wake word detector backed by sherpa-onnx keyword spotter.

    Usage::

        detector = WakeWordDetector(
            keyword="你好小智",
            model_dir="/path/to/sherpa-onnx-kws-zipformer-wenetspeech",
            sample_rate=16000,
        )
        detector.on_detected = my_callback
        if detector.start():
            # In audio capture loop:
            detector.feed_audio(pcm_float32_samples)
    """

    def __init__(
        self,
        keyword: str = "你好小智",
        model_dir: str = "",
        sample_rate: int = 16000,
        sensitivity: float = 0.5,
        cooldown: float = 2.0,
    ):
        """
        Args:
            keyword: The keyword phrase to detect.
            model_dir: Path to sherpa-onnx keyword spotting model directory.
                       If empty, uses the default bundled model.
            sample_rate: Audio sample rate (must match the audio being fed).
            sensitivity: Detection sensitivity (0.0 - 1.0, higher = more sensitive).
            cooldown: Minimum seconds between detections to avoid repeat triggers.
        """
        self._keyword = keyword
        self._model_dir = model_dir
        self._sample_rate = sample_rate
        self._sensitivity = sensitivity
        self._cooldown = cooldown

        self._spotter = None
        self._stream = None
        self._running = False
        self._last_detect_time = 0.0
        self.on_detected: Optional[Callable[[str], None]] = None

    def start(self) -> bool:
        """Initialize the keyword spotter model."""
        if not HAS_SHERPA:
            logger.warning(
                "sherpa-onnx not installed. Wake word detection disabled. "
                "Install with: pip install sherpa-onnx"
            )
            return False

        try:
            self._spotter = self._create_spotter()
            self._stream = self._spotter.create_stream()
            self._running = True
            logger.info(f"Wake word detector started (keyword='{self._keyword}')")
            return True
        except Exception as e:
            logger.error(f"Failed to start wake word detector: {e}")
            return False

    def _create_spotter(self):
        """Create the sherpa-onnx keyword spotter.

        Users should provide model files in model_dir. Expected structure:

            model_dir/
              encoder-epoch-XX-avg-X-chunk-16-left-128.onnx
              decoder-epoch-XX-avg-X-chunk-16-left-128.onnx
              joiner-epoch-XX-avg-X-chunk-16-left-128.onnx
              tokens.txt
              keywords.txt   (one keyword per line, with tokens)

        If keyword is not in the existing keywords.txt, append it.
        See: https://k2-fsa.github.io/sherpa/onnx/kws/
        """
        import os

        model_dir = self._model_dir
        if not model_dir:
            raise FileNotFoundError(
                "model_dir is required for wake word detection. "
                "Download a model from https://k2-fsa.github.io/sherpa/onnx/kws/ "
                "and set wake_word.model_dir in config."
            )

        # Locate model files
        encoder = self._find_file(model_dir, "encoder")
        decoder = self._find_file(model_dir, "decoder")
        joiner = self._find_file(model_dir, "joiner")
        tokens = os.path.join(model_dir, "tokens.txt")

        if not all(os.path.exists(f) for f in [encoder, decoder, joiner, tokens]):
            raise FileNotFoundError(
                f"Missing model files in {model_dir}. "
                f"Need encoder, decoder, joiner .onnx files and tokens.txt"
            )

        # Keywords file
        keywords_file = os.path.join(model_dir, "keywords.txt")
        if not os.path.exists(keywords_file):
            raise FileNotFoundError(
                f"keywords.txt not found in {model_dir}. "
                "Create it with your keyword tokens."
            )

        config = sherpa_onnx.KeywordSpotterConfig(
            feat_config=sherpa_onnx.FeatureExtractorConfig(
                sample_rate=self._sample_rate,
            ),
            model_config=sherpa_onnx.OnlineModelConfig(
                transducer=sherpa_onnx.OnlineTransducerModelConfig(
                    encoder=encoder,
                    decoder=decoder,
                    joiner=joiner,
                ),
                tokens=tokens,
                num_threads=2,
                provider="cpu",
            ),
            keywords_file=keywords_file,
            keywords_threshold=1.0 - self._sensitivity,
        )
        return sherpa_onnx.KeywordSpotter(config)

    @staticmethod
    def _find_file(model_dir: str, prefix: str) -> str:
        """Find a .onnx file starting with the given prefix in model_dir."""
        import os
        for f in os.listdir(model_dir):
            if f.startswith(prefix) and f.endswith(".onnx"):
                return os.path.join(model_dir, f)
        return os.path.join(model_dir, f"{prefix}.onnx")

    def stop(self):
        """Stop the detector and release resources."""
        self._running = False
        self._stream = None
        self._spotter = None
        logger.info("Wake word detector stopped")

    def feed_audio(self, samples: np.ndarray):
        """Feed PCM float32 audio samples for wake word detection.

        Args:
            samples: float32 audio in [-1, 1] range at the configured sample_rate.
        """
        if not self._running or not self._stream:
            return

        # sherpa-onnx expects float32 in [-1, 1]
        if samples.dtype != np.float32:
            samples = samples.astype(np.float32)

        self._stream.accept_waveform(self._sample_rate, samples)

        while self._spotter.is_ready(self._stream):
            self._spotter.decode(self._stream)

        result = self._spotter.get_result(self._stream)
        if result:
            now = time.monotonic()
            if (now - self._last_detect_time) >= self._cooldown:
                self._last_detect_time = now
                keyword = result.strip()
                logger.info(f"Wake word detected: '{keyword}'")
                if self.on_detected:
                    self.on_detected(keyword or self._keyword)
