#!/bin/bash
# Xiaozhi Remote Display Server launcher

# Set library path for macOS (homebrew)
export DYLD_LIBRARY_PATH="/opt/homebrew/lib:$DYLD_LIBRARY_PATH"

# For Linux (Raspberry Pi)
export LD_LIBRARY_PATH="/usr/lib:/usr/local/lib:$LD_LIBRARY_PATH"

# Run server with uv
cd "$(dirname "$0")"
uv run server.py "$@"
