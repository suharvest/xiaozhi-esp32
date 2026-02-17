"""MCP (Model Context Protocol) server for Reachy Mini.

Implements the MCP 2024-11-05 specification, exposing robot capabilities
as tools that can be invoked by the LLM through the Xiaozhi server.

Tools include:
- Device status (volume, battery, network info)
- Audio volume control
- Camera photo capture
- Head movement control
- Antenna control
- Emotion/dance playback
"""

import json
import logging
import platform
import time
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Callable, Optional, Union

logger = logging.getLogger(__name__)

# Protocol version
MCP_PROTOCOL_VERSION = "2024-11-05"
SERVER_NAME = "reachy-mini-bridge"
SERVER_VERSION = "0.1.0"
MAX_PAYLOAD_SIZE = 8000


class PropertyType(str, Enum):
    BOOLEAN = "boolean"
    INTEGER = "integer"
    STRING = "string"


@dataclass
class Property:
    """A tool input property."""
    name: str
    type: PropertyType
    default: Optional[Any] = None
    has_default: bool = False
    min_value: Optional[int] = None
    max_value: Optional[int] = None
    value: Any = None

    def to_schema(self) -> dict:
        schema: dict[str, Any] = {"type": self.type.value}
        if self.has_default:
            schema["default"] = self.default
        if self.min_value is not None:
            schema["minimum"] = self.min_value
        if self.max_value is not None:
            schema["maximum"] = self.max_value
        return schema


@dataclass
class McpTool:
    """An MCP tool definition with callback."""
    name: str
    description: str
    properties: list[Property] = field(default_factory=list)
    callback: Optional[Callable] = None
    user_only: bool = False

    def to_json(self) -> dict:
        props = {}
        required = []
        for p in self.properties:
            props[p.name] = p.to_schema()
            if not p.has_default:
                required.append(p.name)

        result: dict[str, Any] = {
            "name": self.name,
            "description": self.description,
            "inputSchema": {
                "type": "object",
                "properties": props,
            },
        }
        if required:
            result["inputSchema"]["required"] = required
        if self.user_only:
            result["annotations"] = {"audience": ["user"]}
        return result

    def call(self, arguments: dict) -> dict:
        """Execute the tool with the given arguments, return MCP result."""
        # Validate and fill defaults
        params = {}
        for p in self.properties:
            if p.name in arguments:
                val = arguments[p.name]
                # Type validation
                if p.type == PropertyType.INTEGER and isinstance(val, (int, float)):
                    val = int(val)
                    if p.min_value is not None and val < p.min_value:
                        raise ValueError(f"{p.name}: value {val} below minimum {p.min_value}")
                    if p.max_value is not None and val > p.max_value:
                        raise ValueError(f"{p.name}: value {val} above maximum {p.max_value}")
                params[p.name] = val
            elif p.has_default:
                params[p.name] = p.default
            else:
                raise ValueError(f"Missing required argument: {p.name}")

        try:
            result = self.callback(params) if self.callback else "OK"
            return self._format_result(result)
        except Exception as e:
            return {
                "content": [{"type": "text", "text": str(e)}],
                "isError": True,
            }

    @staticmethod
    def _format_result(value: Any) -> dict:
        if isinstance(value, dict) and "content" in value:
            # Already formatted
            return value
        if isinstance(value, bool):
            text = "true" if value else "false"
        elif isinstance(value, (dict, list)):
            text = json.dumps(value)
        else:
            text = str(value)
        return {
            "content": [{"type": "text", "text": text}],
            "isError": False,
        }


class McpServer:
    """MCP server handling JSON-RPC 2.0 messages from the Xiaozhi server."""

    def __init__(self):
        self._tools: list[McpTool] = []
        self._send_callback: Optional[Callable[[str], None]] = None

    def set_send_callback(self, callback: Callable[[str], None]):
        """Set the callback to send MCP response messages to the server."""
        self._send_callback = callback

    def add_tool(self, tool: McpTool):
        """Register an MCP tool."""
        # Prevent duplicates
        for existing in self._tools:
            if existing.name == tool.name:
                logger.warning(f"Tool {tool.name} already registered, skipping")
                return
        logger.info(f"Registered MCP tool: {tool.name}{'  [user-only]' if tool.user_only else ''}")
        self._tools.append(tool)

    def add_reachy_mini_tools(self, robot_controller, audio_bridge=None):
        """Register Reachy Mini specific tools.

        These are the equivalent of the ESP32 board tools, adapted for Reachy Mini.
        """
        # --- Common tools (visible to AI) ---

        # Device status
        self.add_tool(McpTool(
            name="self.get_device_status",
            description=(
                "Provides the real-time information of the device, including "
                "the current status of the audio, battery, network, robot, etc.\n"
                "Use this tool for:\n"
                "1. Answering questions about current condition\n"
                "2. As the first step to control the device"
            ),
            callback=lambda params: self._get_device_status(robot_controller),
        ))

        # Volume control
        self.add_tool(McpTool(
            name="self.audio_speaker.set_volume",
            description=(
                "Set the volume of the audio speaker. If the current volume "
                "is unknown, call `self.get_device_status` first."
            ),
            properties=[
                Property("volume", PropertyType.INTEGER, min_value=0, max_value=100),
            ],
            callback=lambda params: self._set_volume(params["volume"]),
        ))

        # Head movement
        self.add_tool(McpTool(
            name="self.head.move",
            description=(
                "Move the robot's head to a specific position.\n"
                "Args:\n"
                "  yaw: Left/right rotation in degrees (-30 to 30)\n"
                "  pitch: Up/down tilt in degrees (-20 to 20)\n"
                "  roll: Side tilt in degrees (-15 to 15)\n"
                "  duration: Movement duration in seconds (0.3 to 3.0)"
            ),
            properties=[
                Property("yaw", PropertyType.INTEGER, default=0, has_default=True, min_value=-30, max_value=30),
                Property("pitch", PropertyType.INTEGER, default=0, has_default=True, min_value=-20, max_value=20),
                Property("roll", PropertyType.INTEGER, default=0, has_default=True, min_value=-15, max_value=15),
                Property("duration", PropertyType.INTEGER, default=1, has_default=True, min_value=1, max_value=3),
            ],
            callback=lambda params: self._move_head(robot_controller, params),
        ))

        # Antenna control
        self.add_tool(McpTool(
            name="self.antenna.move",
            description=(
                "Move the robot's antennas to express emotions or signals.\n"
                "Args:\n"
                "  left: Left antenna angle in degrees (-50 to 50)\n"
                "  right: Right antenna angle in degrees (-50 to 50)\n"
                "  duration: Movement duration in seconds"
            ),
            properties=[
                Property("left", PropertyType.INTEGER, default=0, has_default=True, min_value=-50, max_value=50),
                Property("right", PropertyType.INTEGER, default=0, has_default=True, min_value=-50, max_value=50),
                Property("duration", PropertyType.INTEGER, default=1, has_default=True, min_value=1, max_value=3),
            ],
            callback=lambda params: self._move_antenna(robot_controller, params),
        ))

        # Camera (if available on Reachy Mini)
        self.add_tool(McpTool(
            name="self.camera.take_photo",
            description=(
                "Take a photo with the robot's camera and describe what is seen.\n"
                "Args:\n"
                "  question: The question about what you see in the photo."
            ),
            properties=[
                Property("question", PropertyType.STRING),
            ],
            callback=lambda params: self._take_photo(robot_controller, params["question"]),
        ))

        # --- User-only tools (invisible to AI) ---

        self.add_tool(McpTool(
            name="self.get_system_info",
            description="Get the system information of the robot",
            callback=lambda params: self._get_system_info(),
            user_only=True,
        ))

        self.add_tool(McpTool(
            name="self.robot.sleep",
            description="Put the robot to sleep (disable motors)",
            callback=lambda params: self._robot_sleep(robot_controller),
            user_only=True,
        ))

        self.add_tool(McpTool(
            name="self.robot.wake_up",
            description="Wake up the robot (enable motors)",
            callback=lambda params: self._robot_wake_up(robot_controller),
            user_only=True,
        ))

    # --- Tool implementations ---

    @staticmethod
    def _get_device_status(robot_controller) -> dict:
        status = {
            "device_type": "reachy_mini",
            "robot_connected": robot_controller._mini is not None,
        }
        try:
            if robot_controller._mini:
                joints = robot_controller._mini.get_current_joint_positions()
                status["joint_positions"] = {k: round(v, 2) for k, v in joints.items()} if joints else {}
        except Exception:
            pass
        return status

    @staticmethod
    def _set_volume(volume: int) -> bool:
        # Volume control depends on the OS audio system
        # On Raspberry Pi, we can use amixer
        import subprocess
        try:
            subprocess.run(
                ["amixer", "sset", "Master", f"{volume}%"],
                capture_output=True, timeout=5,
            )
            return True
        except Exception as e:
            logger.warning(f"Failed to set volume: {e}")
            return False

    @staticmethod
    def _move_head(robot_controller, params: dict) -> str:
        from .emotion_mapper import RobotExpression, HeadPose
        expr = RobotExpression(
            head=HeadPose(
                z=float(params.get("yaw", 0)),
                pitch=float(params.get("pitch", 0)),
                roll=float(params.get("roll", 0)),
                duration=float(params.get("duration", 1)),
            ),
            description=f"MCP head move: yaw={params.get('yaw', 0)}, pitch={params.get('pitch', 0)}",
        )
        robot_controller.execute_expression(expr)
        return "Head movement queued"

    @staticmethod
    def _move_antenna(robot_controller, params: dict) -> str:
        from .emotion_mapper import RobotExpression, AntennaMotion
        expr = RobotExpression(
            antenna=AntennaMotion(
                left=float(params.get("left", 0)),
                right=float(params.get("right", 0)),
                duration=float(params.get("duration", 1)),
            ),
            description=f"MCP antenna move: left={params.get('left', 0)}, right={params.get('right', 0)}",
        )
        robot_controller.execute_expression(expr)
        return "Antenna movement queued"

    @staticmethod
    def _take_photo(robot_controller, question: str) -> str:
        if not robot_controller._mini:
            return "Robot not connected, cannot take photo"
        try:
            # Capture frame from camera
            frame = robot_controller._mini.media.camera.get_frame()
            if frame is None:
                return "Failed to capture photo"
            return f"Photo captured. (Vision processing requires server-side support. Question: {question})"
        except Exception as e:
            return f"Camera error: {e}"

    @staticmethod
    def _get_system_info() -> dict:
        return {
            "server": SERVER_NAME,
            "version": SERVER_VERSION,
            "platform": platform.platform(),
            "python": platform.python_version(),
        }

    @staticmethod
    def _robot_sleep(robot_controller) -> bool:
        try:
            if robot_controller._mini:
                robot_controller._mini.goto_sleep()
                return True
        except Exception as e:
            logger.error(f"Failed to sleep: {e}")
        return False

    @staticmethod
    def _robot_wake_up(robot_controller) -> bool:
        try:
            if robot_controller._mini:
                robot_controller._mini.wake_up()
                return True
        except Exception as e:
            logger.error(f"Failed to wake up: {e}")
        return False

    # --- JSON-RPC message handling ---

    def parse_message(self, payload: dict):
        """Parse an MCP JSON-RPC 2.0 message from the server."""
        version = payload.get("jsonrpc")
        if version != "2.0":
            logger.error(f"Invalid JSONRPC version: {version}")
            return

        method = payload.get("method", "")
        params = payload.get("params", {})
        msg_id = payload.get("id")

        if method.startswith("notifications"):
            return

        if msg_id is None:
            logger.error(f"Missing id for method: {method}")
            return

        if method == "initialize":
            self._handle_initialize(msg_id, params)
        elif method == "tools/list":
            self._handle_tools_list(msg_id, params)
        elif method == "tools/call":
            self._handle_tools_call(msg_id, params)
        else:
            logger.error(f"Method not implemented: {method}")
            self._reply_error(msg_id, f"Method not implemented: {method}")

    def _handle_initialize(self, msg_id: int, params: dict):
        """Handle MCP initialize request."""
        result = {
            "protocolVersion": MCP_PROTOCOL_VERSION,
            "capabilities": {"tools": {}},
            "serverInfo": {
                "name": SERVER_NAME,
                "version": SERVER_VERSION,
            },
        }
        self._reply_result(msg_id, result)

    def _handle_tools_list(self, msg_id: int, params: dict):
        """Handle tools/list request with pagination support."""
        cursor = params.get("cursor", "")
        list_user_only = params.get("withUserTools", False)

        tools_json = []
        found_cursor = not bool(cursor)
        next_cursor = ""
        current_size = len('{"tools":[]}')

        for tool in self._tools:
            if not found_cursor:
                if tool.name == cursor:
                    found_cursor = True
                else:
                    continue

            if not list_user_only and tool.user_only:
                continue

            tool_json = tool.to_json()
            tool_str = json.dumps(tool_json)
            if current_size + len(tool_str) + 50 > MAX_PAYLOAD_SIZE:
                next_cursor = tool.name
                break

            tools_json.append(tool_json)
            current_size += len(tool_str) + 1

        result: dict[str, Any] = {"tools": tools_json}
        if next_cursor:
            result["nextCursor"] = next_cursor

        self._reply_result(msg_id, result)

    def _handle_tools_call(self, msg_id: int, params: dict):
        """Handle tools/call request."""
        tool_name = params.get("name", "")
        arguments = params.get("arguments", {})

        tool = None
        for t in self._tools:
            if t.name == tool_name:
                tool = t
                break

        if not tool:
            logger.error(f"Unknown tool: {tool_name}")
            self._reply_error(msg_id, f"Unknown tool: {tool_name}")
            return

        try:
            result = tool.call(arguments)
            self._reply_result(msg_id, result)
        except Exception as e:
            logger.error(f"Tool call error: {e}")
            self._reply_error(msg_id, str(e))

    def _reply_result(self, msg_id: int, result: Any):
        """Send a JSON-RPC result response."""
        response = {
            "jsonrpc": "2.0",
            "id": msg_id,
            "result": result,
        }
        self._send(response)

    def _reply_error(self, msg_id: int, message: str):
        """Send a JSON-RPC error response."""
        response = {
            "jsonrpc": "2.0",
            "id": msg_id,
            "error": {"message": message},
        }
        self._send(response)

    def _send(self, payload: dict):
        """Send an MCP message back to the server."""
        if self._send_callback:
            self._send_callback(json.dumps(payload))
        else:
            logger.warning(f"No send callback, dropping MCP response: {payload}")
