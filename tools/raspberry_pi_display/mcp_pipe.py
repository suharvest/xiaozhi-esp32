"""
MCP stdio <-> WebSocket pipe.

Bridges a local MCP server (stdio) to a remote WebSocket endpoint.
The MCP server runs as a subprocess, and this pipe forwards messages
between the WebSocket connection and the subprocess's stdin/stdout.

Usage:
    export MCP_ENDPOINT=wss://...
    python mcp_pipe.py narrate_mcp.py

Or set NARRATE_SERVER_URL to configure the display server address:
    export NARRATE_SERVER_URL=http://localhost:8765
    export MCP_ENDPOINT=wss://...
    python mcp_pipe.py narrate_mcp.py
"""

import asyncio
import websockets
import subprocess
import logging
import os
import signal
import sys

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger('MCP_PIPE')

# Reconnection settings
INITIAL_BACKOFF = 1
MAX_BACKOFF = 60


async def connect_with_retry(uri, script_path):
    """Connect to WebSocket server with exponential backoff retry."""
    reconnect_attempt = 0
    backoff = INITIAL_BACKOFF
    while True:
        try:
            if reconnect_attempt > 0:
                logger.info(f"Waiting {backoff}s before reconnection attempt {reconnect_attempt}...")
                await asyncio.sleep(backoff)

            await connect_to_server(uri, script_path)

        except asyncio.CancelledError:
            logger.info("Connection cancelled")
            break
        except Exception as e:
            reconnect_attempt += 1
            logger.warning(f"Connection closed (attempt {reconnect_attempt}): {e}")
            backoff = min(backoff * 2, MAX_BACKOFF)


async def connect_to_server(uri, script_path):
    """Connect to WebSocket and pipe to/from MCP subprocess."""
    process = None
    try:
        logger.info(f"Connecting to WebSocket: {uri[:80]}...")
        async with websockets.connect(uri) as websocket:
            logger.info("Connected to WebSocket endpoint")

            # Build command to start MCP server
            cmd, env = build_command(script_path)
            process = subprocess.Popen(
                cmd,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                encoding='utf-8',
                text=True,
                env=env
            )
            logger.info(f"Started MCP server: {' '.join(cmd)}")

            # Bidirectional piping
            await asyncio.gather(
                pipe_ws_to_process(websocket, process),
                pipe_process_to_ws(process, websocket),
                pipe_stderr(process)
            )
    except websockets.exceptions.ConnectionClosed as e:
        logger.error(f"WebSocket connection closed: {e}")
        raise
    except Exception as e:
        logger.error(f"Connection error: {e}")
        raise
    finally:
        if process:
            logger.info("Terminating MCP server process")
            try:
                process.terminate()
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
            logger.info("MCP server process terminated")


async def pipe_ws_to_process(websocket, process):
    """Forward WebSocket messages to subprocess stdin."""
    try:
        while True:
            message = await websocket.recv()
            logger.debug(f"<< {str(message)[:120]}...")
            if isinstance(message, bytes):
                message = message.decode('utf-8')
            process.stdin.write(message + '\n')
            process.stdin.flush()
    except Exception as e:
        logger.error(f"Error in WS->process pipe: {e}")
        raise
    finally:
        if not process.stdin.closed:
            process.stdin.close()


async def pipe_process_to_ws(process, websocket):
    """Forward subprocess stdout to WebSocket."""
    try:
        while True:
            data = await asyncio.to_thread(process.stdout.readline)
            if not data:
                logger.info("MCP server process ended output")
                break
            logger.debug(f">> {data[:120]}...")
            await websocket.send(data)
    except Exception as e:
        logger.error(f"Error in process->WS pipe: {e}")
        raise


async def pipe_stderr(process):
    """Forward subprocess stderr to terminal for debugging."""
    try:
        while True:
            data = await asyncio.to_thread(process.stderr.readline)
            if not data:
                break
            sys.stderr.write(data)
            sys.stderr.flush()
    except Exception as e:
        logger.error(f"Error reading stderr: {e}")
        raise


def build_command(script_path):
    """Build subprocess command and environment."""
    env = os.environ.copy()

    # Find uv or python
    import shutil
    uv_path = shutil.which("uv")

    if uv_path:
        cmd = [uv_path, "run", "python", script_path]
    else:
        cmd = [sys.executable, script_path]

    return cmd, env


def signal_handler(sig, frame):
    logger.info("Received interrupt signal, shutting down...")
    sys.exit(0)


if __name__ == "__main__":
    signal.signal(signal.SIGINT, signal_handler)

    endpoint_url = os.environ.get('MCP_ENDPOINT')
    if not endpoint_url:
        logger.error("Please set the MCP_ENDPOINT environment variable")
        sys.exit(1)

    if len(sys.argv) < 2:
        logger.error("Usage: python mcp_pipe.py <mcp_server_script.py>")
        sys.exit(1)

    script = sys.argv[1]
    if not os.path.exists(script):
        # Try relative to this file's directory
        script = os.path.join(os.path.dirname(__file__), script)
        if not os.path.exists(script):
            logger.error(f"Script not found: {sys.argv[1]}")
            sys.exit(1)

    try:
        asyncio.run(connect_with_retry(endpoint_url, script))
    except KeyboardInterrupt:
        logger.info("Program interrupted by user")
    except Exception as e:
        logger.error(f"Program error: {e}")
