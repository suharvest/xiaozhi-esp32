#!/usr/bin/env python3
"""
Narrate Mode MCP Service

Provides tools for controlling the remote display in narrate mode:
- show_background_image: Display a background image
- clear_background: Clear the background and exit narrate mode
- get_display_status: Get current display status
- get_triggers: Get trigger rules for automatic image switching

Transport: stdio (bridged to WebSocket via mcp_pipe)
"""

import os
import sys
import logging
import httpx
from fastmcp import FastMCP

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger('NarrateMCP')

# Fix Windows console encoding
if sys.platform == 'win32':
    sys.stderr.reconfigure(encoding='utf-8')
    sys.stdout.reconfigure(encoding='utf-8')

# Configuration from environment
NARRATE_SERVER_URL = os.environ.get("NARRATE_SERVER_URL", "http://localhost:8765")

# Create MCP server
mcp = FastMCP("Xiaozhi Display Narrate")


def api_post(endpoint: str, data: dict) -> dict:
    """Send POST request to narrate server"""
    try:
        response = httpx.post(
            f"{NARRATE_SERVER_URL}{endpoint}",
            json=data,
            timeout=10
        )
        return response.json()
    except httpx.ConnectError:
        return {
            "success": False,
            "error": "Cannot connect to display server",
            "message": f"Please ensure the display server is running at {NARRATE_SERVER_URL}"
        }
    except Exception as e:
        return {
            "success": False,
            "error": str(e),
            "message": f"API request failed: {str(e)}"
        }


def api_get(endpoint: str) -> dict:
    """Send GET request to narrate server"""
    try:
        response = httpx.get(
            f"{NARRATE_SERVER_URL}{endpoint}",
            timeout=10
        )
        return response.json()
    except httpx.ConnectError:
        return {
            "success": False,
            "error": "Cannot connect to display server",
            "message": f"Please ensure the display server is running at {NARRATE_SERVER_URL}"
        }
    except Exception as e:
        return {
            "success": False,
            "error": str(e),
            "message": f"API request failed: {str(e)}"
        }


@mcp.tool()
def show_background_image(image_url: str, caption: str = "") -> dict:
    """
    Display a background image on the remote display in narrate mode.

    This will switch the display to narrate mode (if not already) and show
    the specified image as a full-screen background. The original display
    content (emoji, chat, etc.) will be moved to a small window in the
    bottom-right corner.

    Args:
        image_url: URL of the image to display (HTTP/HTTPS or local /uploads/ path)
        caption: Optional caption text to show below the image

    Returns:
        Dict with success status and message
    """
    if not image_url:
        return {
            "success": False,
            "error": "image_url is required",
            "message": "Please provide an image URL"
        }

    result = api_post("/api/narrate", {
        "action": "show",
        "imageUrl": image_url,
        "caption": caption
    })

    if result.get("success"):
        logger.info(f"Displayed image: {image_url}")
        return {
            "success": True,
            "message": f"Background image displayed: {caption if caption else image_url}"
        }
    else:
        logger.warning(f"Failed to display image: {result.get('error')}")
        return result


@mcp.tool()
def clear_background() -> dict:
    """
    Clear the background image and exit narrate mode.

    This will remove the background image and return the display to normal
    mode, with the original content (emoji, chat, etc.) in full view.

    Returns:
        Dict with success status and message
    """
    result = api_post("/api/narrate", {
        "action": "clear"
    })

    if result.get("success"):
        logger.info("Background cleared")
        return {
            "success": True,
            "message": "Background cleared, returned to normal display mode"
        }
    else:
        logger.warning(f"Failed to clear background: {result.get('error')}")
        return result


@mcp.tool()
def get_display_status() -> dict:
    """
    Get the current display status.

    Returns information about the current display state, including whether
    narrate mode is active and the current configuration.

    Returns:
        Dict with display status information
    """
    result = api_get("/api/config")

    if "error" not in result:
        return {
            "success": True,
            "status": {
                "mcpConnected": result.get("mcpConnected", False),
                "mcpEnabled": result.get("mcpEnabled", False),
                "triggersCount": len(result.get("triggers", []))
            },
            "message": "Display status retrieved successfully"
        }
    else:
        return result


@mcp.tool()
def get_triggers() -> dict:
    """
    Get all configured trigger rules for automatic image switching.

    Each trigger has a 'prompt' describing the scenario and an 'imageUrl'
    for the image to display. You should call this tool when the conversation
    starts, then monitor the conversation context. When the context semantically
    matches a trigger's prompt, call show_background_image with the
    corresponding imageUrl.

    Returns:
        Dict with trigger list. Each trigger has:
        - prompt: Natural language description of when to show the image
        - imageUrl: The image URL to pass to show_background_image
    """
    result = api_get("/api/config")

    if "error" not in result:
        triggers = result.get("triggers", [])
        return {
            "success": True,
            "triggers": triggers,
            "message": f"Found {len(triggers)} trigger(s). Monitor the conversation and call show_background_image when context matches a trigger's prompt."
        }
    else:
        return result


# Start server
if __name__ == "__main__":
    logger.info(f"Starting Narrate MCP service (server: {NARRATE_SERVER_URL})")
    mcp.run(transport="stdio")
