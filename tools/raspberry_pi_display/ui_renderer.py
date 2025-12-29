"""
UI Renderer - Pygame-based UI rendering for remote display
Renders emotion, status, and chat messages based on UI state
"""

import pygame
import os
import json
import logging
import threading
from pathlib import Path
from typing import Optional, Dict, Any

logger = logging.getLogger(__name__)

# Colors
COLORS = {
    "light": {
        "background": (255, 255, 255),
        "text": (0, 0, 0),
        "status": (100, 100, 100),
        "user_bubble": (220, 248, 198),
        "assistant_bubble": (240, 240, 240),
    },
    "dark": {
        "background": (30, 30, 30),
        "text": (255, 255, 255),
        "status": (180, 180, 180),
        "user_bubble": (38, 87, 52),
        "assistant_bubble": (60, 60, 60),
    }
}

# Simple emoji faces (programmatic drawing as fallback)
EMOJI_COLORS = {
    "happy": (255, 200, 50),      # Yellow
    "neutral": (255, 200, 50),
    "sad": (100, 150, 255),       # Blue
    "angry": (255, 100, 100),     # Red
    "sleepy": (200, 180, 255),    # Light purple
    "surprised": (255, 180, 100), # Orange
}


class UIRenderer:
    def __init__(self, width: int = 412, height: int = 412, scale: float = 1.5, fullscreen: bool = False):
        self.base_width = width
        self.base_height = height
        self.width = int(width * scale)
        self.height = int(height * scale)
        self.scale = scale
        self.fullscreen = fullscreen
        self.running = True
        # Content area offset for centering in fullscreen
        self.content_offset_x = 0
        self.content_offset_y = 0
        self.content_width = self.width
        self.content_height = self.height

        # UI State
        self.emotion = "neutral"
        self.status = ""
        self.chat_role = ""
        self.chat_text = ""
        self.theme = "light"
        self.background_image = None
        self.volume = 70
        self.muted = False

        # Assets
        self.assets_dir = Path(__file__).parent / "assets"
        self.emoji_images: Dict[str, pygame.Surface] = {}
        self.background_surfaces: Dict[str, pygame.Surface] = {}
        self.font: Optional[pygame.font.Font] = None
        self.font_large: Optional[pygame.font.Font] = None
        self.font_small: Optional[pygame.font.Font] = None

        # Animation
        self.animation_frame = 0
        self.last_animation_time = 0

        # Thread safety for state updates
        self.state_lock = threading.Lock()

        # Initialize Pygame
        pygame.init()
        pygame.display.set_caption("Xiaozhi Remote Display")

        if self.fullscreen:
            # Get display info before switching to fullscreen
            display_info = pygame.display.Info()
            screen_w, screen_h = display_info.current_w, display_info.current_h

            self.screen = pygame.display.set_mode((screen_w, screen_h), pygame.FULLSCREEN)

            # Get actual screen size after fullscreen switch
            screen_w, screen_h = self.screen.get_size()

            # Recalculate scale to fit screen while maintaining aspect ratio
            scale_w = screen_w / self.base_width
            scale_h = screen_h / self.base_height
            self.scale = min(scale_w, scale_h)
            self.width = screen_w
            self.height = screen_h
            # Calculate content area (centered)
            self.content_width = int(self.base_width * self.scale)
            self.content_height = int(self.base_height * self.scale)
            self.content_offset_x = (screen_w - self.content_width) // 2
            self.content_offset_y = (screen_h - self.content_height) // 2
            logger.info(f"Fullscreen: {screen_w}x{screen_h}, content: {self.content_width}x{self.content_height}, offset: ({self.content_offset_x}, {self.content_offset_y})")
        else:
            self.screen = pygame.display.set_mode((self.width, self.height))
            self.content_width = self.width
            self.content_height = self.height
            self.content_offset_x = 0
            self.content_offset_y = 0

        self.clock = pygame.time.Clock()

        # Load fonts
        self._load_fonts()

        # Load assets
        self._load_assets()

        logger.info(f"UI Renderer initialized: {self.width}x{self.height}")

    def _load_fonts(self):
        """Load fonts for text rendering"""
        import platform

        # Common font paths for Chinese support
        font_paths = []

        if platform.system() == "Darwin":  # macOS
            font_paths = [
                "/System/Library/Fonts/PingFang.ttc",
                "/System/Library/Fonts/STHeiti Light.ttc",
                "/System/Library/Fonts/Hiragino Sans GB.ttc",
                "/Library/Fonts/Arial Unicode.ttf",
            ]
        elif platform.system() == "Linux":  # Linux / Raspberry Pi
            font_paths = [
                "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
                "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
                "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
                "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
            ]
        else:  # Windows
            font_paths = [
                "C:/Windows/Fonts/msyh.ttc",
                "C:/Windows/Fonts/simsun.ttc",
            ]

        # Try to load font from file
        for font_path in font_paths:
            try:
                if os.path.exists(font_path):
                    self.font = pygame.font.Font(font_path, int(24 * self.scale))
                    self.font_large = pygame.font.Font(font_path, int(32 * self.scale))
                    self.font_small = pygame.font.Font(font_path, int(18 * self.scale))
                    logger.info(f"Using font file: {font_path}")
                    return
            except Exception as e:
                logger.debug(f"Failed to load font {font_path}: {e}")
                continue

        # Try system font names (Pygame format)
        font_names = ["pingfangsc", "hiraginosansgb", "microsoftyahei",
                     "simhei", "arialunicodems", "notosanscjksc",
                     "wqyzenhei", "wqymicrohei", "droidsansfallback"]

        for name in font_names:
            try:
                test_font = pygame.font.SysFont(name, int(24 * self.scale))
                # Test if font can render Chinese
                test_surface = test_font.render("测试", True, (0, 0, 0))
                if test_surface.get_width() > 10:  # Font rendered something
                    self.font = test_font
                    self.font_large = pygame.font.SysFont(name, int(32 * self.scale))
                    self.font_small = pygame.font.SysFont(name, int(18 * self.scale))
                    logger.info(f"Using system font: {name}")
                    return
            except Exception as e:
                logger.debug(f"Failed to use font {name}: {e}")
                continue

        # Fallback to default font
        self.font = pygame.font.Font(None, int(24 * self.scale))
        self.font_large = pygame.font.Font(None, int(32 * self.scale))
        self.font_small = pygame.font.Font(None, int(18 * self.scale))
        logger.warning("Using default font (Chinese may not display correctly)")

    def _load_assets(self):
        """Load emoji and background images from assets directory"""
        if not self.assets_dir.exists():
            logger.warning(f"Assets directory not found: {self.assets_dir}")
            return

        # Load index.json if exists
        index_path = self.assets_dir / "index.json"
        if index_path.exists():
            try:
                with open(index_path, 'r') as f:
                    index = json.load(f)
                logger.info(f"Loaded asset index: {index_path}")
            except Exception as e:
                logger.error(f"Failed to load index.json: {e}")

        # Load emoji images
        emojis_dir = self.assets_dir / "emojis"
        if emojis_dir.exists():
            for img_file in emojis_dir.glob("*.png"):
                try:
                    name = img_file.stem
                    img = pygame.image.load(str(img_file))
                    # Scale to fit
                    emoji_size = int(150 * self.scale)
                    img = pygame.transform.smoothscale(img, (emoji_size, emoji_size))
                    self.emoji_images[name] = img
                    logger.debug(f"Loaded emoji: {name}")
                except Exception as e:
                    logger.error(f"Failed to load emoji {img_file}: {e}")

        # Load background images
        bg_dir = self.assets_dir / "backgrounds"
        if bg_dir.exists():
            for img_file in bg_dir.glob("*.png"):
                try:
                    name = img_file.stem
                    img = pygame.image.load(str(img_file))
                    img = pygame.transform.smoothscale(img, (self.width, self.height))
                    self.background_surfaces[name] = img
                    logger.debug(f"Loaded background: {name}")
                except Exception as e:
                    logger.error(f"Failed to load background {img_file}: {e}")

    def update_state(self, state: Dict[str, Any]):
        """Update UI state from received message (thread-safe)"""
        changed = []
        with self.state_lock:
            if "emotion" in state and state["emotion"] != self.emotion:
                self.emotion = state["emotion"]
                changed.append(f"emotion={self.emotion}")
            if "status" in state and state["status"] != self.status:
                self.status = state["status"]
                changed.append(f"status={self.status[:20]}..." if len(self.status) > 20 else f"status={self.status}")
            if "chat" in state:
                chat = state["chat"]
                self.chat_role = chat.get("role", "")
                self.chat_text = chat.get("text", "")
                if self.chat_text:
                    changed.append(f"chat={self.chat_role}:{self.chat_text[:20]}...")
            if "theme" in state:
                self.theme = state["theme"] if state["theme"] in COLORS else "light"
            if "background" in state:
                self.background_image = state["background"]
            if "volume" in state:
                self.volume = state["volume"]
            if "muted" in state:
                self.muted = state["muted"]

        if changed:
            logger.debug(f"UI state updated: {', '.join(changed)}")

    def _draw_emoji_fallback(self, emotion: str, center_x: int, center_y: int, radius: int):
        """Draw a simple emoji face when no image is available"""
        color = EMOJI_COLORS.get(emotion, EMOJI_COLORS["neutral"])

        # Draw face circle
        pygame.draw.circle(self.screen, color, (center_x, center_y), radius)
        pygame.draw.circle(self.screen, (0, 0, 0), (center_x, center_y), radius, 3)

        # Draw eyes
        eye_y = center_y - radius // 4
        eye_offset = radius // 3
        eye_radius = radius // 8

        # Eye style based on emotion
        if emotion == "happy":
            # Happy eyes (curved lines)
            pygame.draw.arc(self.screen, (0, 0, 0),
                          (center_x - eye_offset - eye_radius, eye_y - eye_radius,
                           eye_radius * 2, eye_radius * 2),
                          3.14, 6.28, 3)
            pygame.draw.arc(self.screen, (0, 0, 0),
                          (center_x + eye_offset - eye_radius, eye_y - eye_radius,
                           eye_radius * 2, eye_radius * 2),
                          3.14, 6.28, 3)
        elif emotion == "sleepy":
            # Sleepy eyes (horizontal lines)
            pygame.draw.line(self.screen, (0, 0, 0),
                           (center_x - eye_offset - eye_radius, eye_y),
                           (center_x - eye_offset + eye_radius, eye_y), 3)
            pygame.draw.line(self.screen, (0, 0, 0),
                           (center_x + eye_offset - eye_radius, eye_y),
                           (center_x + eye_offset + eye_radius, eye_y), 3)
        else:
            # Normal eyes (circles)
            pygame.draw.circle(self.screen, (0, 0, 0),
                             (center_x - eye_offset, eye_y), eye_radius)
            pygame.draw.circle(self.screen, (0, 0, 0),
                             (center_x + eye_offset, eye_y), eye_radius)

        # Draw mouth based on emotion
        mouth_y = center_y + radius // 3
        mouth_width = radius // 2

        if emotion == "happy":
            # Smile
            pygame.draw.arc(self.screen, (0, 0, 0),
                          (center_x - mouth_width, mouth_y - mouth_width // 2,
                           mouth_width * 2, mouth_width),
                          3.14, 6.28, 3)
        elif emotion == "sad":
            # Frown
            pygame.draw.arc(self.screen, (0, 0, 0),
                          (center_x - mouth_width, mouth_y,
                           mouth_width * 2, mouth_width),
                          0, 3.14, 3)
        else:
            # Neutral line
            pygame.draw.line(self.screen, (0, 0, 0),
                           (center_x - mouth_width // 2, mouth_y),
                           (center_x + mouth_width // 2, mouth_y), 3)

    def _get_text_height(self, text: str, font: pygame.font.Font, max_width: int) -> int:
        """Calculate the total height of wrapped text without rendering"""
        if not text:
            return 0

        words = list(text)
        lines = []
        current_line = ""

        for char in words:
            test_line = current_line + char
            if font.size(test_line)[0] <= max_width:
                current_line = test_line
            else:
                if current_line:
                    lines.append(current_line)
                current_line = char

        if current_line:
            lines.append(current_line)

        line_height = font.get_linesize()
        return len(lines) * (line_height + 2) - 2 if lines else 0

    def _render_text_wrapped(self, text: str, font: pygame.font.Font,
                            color: tuple, max_width: int, x: int, y: int,
                            align: str = "left") -> int:
        """Render text with word wrapping, returns total height"""
        if not text:
            return 0

        words = list(text)  # Split by character for Chinese
        lines = []
        current_line = ""

        for char in words:
            test_line = current_line + char
            if font.size(test_line)[0] <= max_width:
                current_line = test_line
            else:
                if current_line:
                    lines.append(current_line)
                current_line = char

        if current_line:
            lines.append(current_line)

        total_height = 0
        for line in lines:
            text_surface = font.render(line, True, color)
            text_rect = text_surface.get_rect()

            if align == "center":
                text_rect.centerx = x
            elif align == "right":
                text_rect.right = x
            else:
                text_rect.left = x

            text_rect.top = y + total_height
            self.screen.blit(text_surface, text_rect)
            total_height += text_rect.height + 2

        return total_height

    def render(self):
        """Render the current UI state (thread-safe read of state)"""
        # Copy state under lock to avoid race conditions
        with self.state_lock:
            theme = self.theme
            background_image = self.background_image
            status = self.status
            emotion = self.emotion
            chat_text = self.chat_text
            chat_role = self.chat_role

        colors = COLORS[theme]

        # Fill entire screen with background color first (for fullscreen black bars)
        self.screen.fill((0, 0, 0) if self.fullscreen else colors["background"])

        # Content area offset for centering
        ox, oy = self.content_offset_x, self.content_offset_y
        cw, ch = self.content_width, self.content_height

        # Draw content background
        if background_image and background_image in self.background_surfaces:
            self.screen.blit(self.background_surfaces[background_image], (ox, oy))
        else:
            content_rect = pygame.Rect(ox, oy, cw, ch)
            pygame.draw.rect(self.screen, colors["background"], content_rect)

        # Layout calculations
        padding = int(20 * self.scale)
        status_height = int(50 * self.scale)
        emoji_size = int(150 * self.scale)
        chat_height = int(120 * self.scale)

        # Draw status bar at top
        status_y = oy + padding
        if status:
            self._render_text_wrapped(
                status, self.font, colors["status"],
                cw - padding * 2,
                ox + cw // 2, status_y,
                align="center"
            )

        # Draw emoji in center
        emoji_center_x = ox + cw // 2
        emoji_center_y = oy + ch // 2 - int(30 * self.scale)

        if emotion in self.emoji_images:
            # Use loaded image
            img = self.emoji_images[emotion]
            img_rect = img.get_rect(center=(emoji_center_x, emoji_center_y))
            self.screen.blit(img, img_rect)
        else:
            # Draw fallback emoji
            self._draw_emoji_fallback(
                emotion, emoji_center_x, emoji_center_y,
                emoji_size // 2
            )

        # Draw chat message at bottom
        chat_y = oy + ch - chat_height - padding
        if chat_text:
            # Draw chat bubble
            bubble_padding = int(15 * self.scale)
            bubble_color = (colors["user_bubble"] if chat_role == "user"
                          else colors["assistant_bubble"])

            # Calculate text size
            text_width = cw - padding * 4

            # Draw bubble background
            bubble_rect = pygame.Rect(
                ox + padding, chat_y,
                cw - padding * 2, chat_height
            )
            pygame.draw.rect(self.screen, bubble_color, bubble_rect,
                           border_radius=int(15 * self.scale))

            # Calculate text height for vertical centering
            text_total_height = self._get_text_height(chat_text, self.font, text_width)
            text_y = chat_y + (chat_height - text_total_height) // 2

            # Draw chat text (centered vertically)
            self._render_text_wrapped(
                chat_text, self.font, colors["text"],
                text_width, ox + padding + bubble_padding,
                text_y
            )

        # Update display
        pygame.display.flip()

    def toggle_fullscreen(self):
        """Toggle between fullscreen and windowed mode"""
        self.fullscreen = not self.fullscreen

        if self.fullscreen:
            # Get display info before switching to fullscreen
            display_info = pygame.display.Info()
            screen_w, screen_h = display_info.current_w, display_info.current_h

            self.screen = pygame.display.set_mode((screen_w, screen_h), pygame.FULLSCREEN)

            # Get actual screen size after fullscreen switch
            screen_w, screen_h = self.screen.get_size()

            scale_w = screen_w / self.base_width
            scale_h = screen_h / self.base_height
            self.scale = min(scale_w, scale_h)
            self.width = screen_w
            self.height = screen_h
            # Calculate content area (centered)
            self.content_width = int(self.base_width * self.scale)
            self.content_height = int(self.base_height * self.scale)
            self.content_offset_x = (screen_w - self.content_width) // 2
            self.content_offset_y = (screen_h - self.content_height) // 2
        else:
            self.scale = 1.5  # Reset to default scale
            self.width = int(self.base_width * self.scale)
            self.height = int(self.base_height * self.scale)
            self.screen = pygame.display.set_mode((self.width, self.height))
            self.content_width = self.width
            self.content_height = self.height
            self.content_offset_x = 0
            self.content_offset_y = 0

        # Reload fonts with new scale
        self._load_fonts()
        # Reload assets with new scale
        self._load_assets()

        logger.info(f"Toggled fullscreen: {self.fullscreen}, screen: {self.width}x{self.height}, content: {self.content_width}x{self.content_height}, offset: ({self.content_offset_x}, {self.content_offset_y})")

    def handle_events(self) -> bool:
        """Handle Pygame events, returns False if should quit"""
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                logger.info("Received QUIT event")
                self.running = False
                return False
            elif event.type == pygame.KEYDOWN:
                logger.debug(f"Key pressed: {event.key}")
                if event.key == pygame.K_q or event.key == pygame.K_ESCAPE:
                    logger.info(f"Exit key pressed: {event.key}")
                    self.running = False
                    return False
                elif event.key == pygame.K_f or event.key == pygame.K_F11:
                    self.toggle_fullscreen()
        return True

    def tick(self, fps: int = 30):
        """Limit frame rate"""
        self.clock.tick(fps)

    def close(self):
        """Clean up Pygame"""
        self.running = False
        pygame.quit()
        logger.info("UI Renderer closed")
