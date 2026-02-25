#!/usr/bin/env python3
"""Start gRPC server, then launch NiceGUI dashboard."""
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from app.grpc_server import start_grpc_server

# 1. Start gRPC server in background thread
_grpc_server = start_grpc_server(port=50051)

# 2. Import registers @ui.page and @app.get routes
import app.main  # noqa: F401

# 3. Launch NiceGUI (built-in uvicorn)
from nicegui import ui
ui.run(host="0.0.0.0", port=8501, reload=False, show=False,
       title="Edge AI Runtime")
