"""
Screen Renderer - Display JPEG frames using OpenCV
Note: OpenCV GUI must run in main thread on macOS
"""

import cv2
import numpy as np
from queue import Queue, Empty
import logging

logger = logging.getLogger(__name__)


class ScreenRenderer:
    def __init__(self, width: int = 412, height: int = 412, scale: float = 1.5):
        self.width = width
        self.height = height
        self.scale = scale
        self.display_width = int(width * scale)
        self.display_height = int(height * scale)

        self.frame_queue: Queue = Queue(maxsize=2)
        self.running = True
        self.window_created = False
        self.window_name = "Xiaozhi Remote Display"
        self.latest_frame = None

        logger.info(f"Screen renderer initialized: {self.display_width}x{self.display_height}")

    def update(self, jpeg_data: bytes):
        """Update frame (non-blocking)"""
        try:
            # Decode JPEG
            np_arr = np.frombuffer(jpeg_data, np.uint8)
            frame = cv2.imdecode(np_arr, cv2.IMREAD_COLOR)

            if frame is not None:
                # Scale if needed
                if self.scale != 1.0:
                    frame = cv2.resize(
                        frame,
                        (self.display_width, self.display_height),
                        interpolation=cv2.INTER_LINEAR
                    )
                self.latest_frame = frame
        except Exception as e:
            logger.error(f"Failed to decode frame: {e}")

    def render_once(self):
        """Render one frame - must be called from main thread"""
        if not self.window_created:
            cv2.namedWindow(self.window_name, cv2.WINDOW_NORMAL)
            cv2.resizeWindow(self.window_name, self.display_width, self.display_height)
            self.window_created = True
            logger.info("Display window created")

        if self.latest_frame is not None:
            cv2.imshow(self.window_name, self.latest_frame)

        # Check for window close or 'q' key
        key = cv2.waitKey(1) & 0xFF
        if key == ord('q'):
            self.running = False
            return False
        try:
            if cv2.getWindowProperty(self.window_name, cv2.WND_PROP_VISIBLE) < 1:
                self.running = False
                return False
        except cv2.error:
            self.running = False
            return False

        return True

    def close(self):
        """Close renderer"""
        self.running = False
        if self.window_created:
            cv2.destroyAllWindows()
        logger.info("Screen renderer stopped")
