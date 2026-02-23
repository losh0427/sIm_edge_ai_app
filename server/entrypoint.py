#!/usr/bin/env python3
"""Start gRPC server first, then launch Streamlit in the same process."""
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from app.grpc_server import start_grpc_server

# Start gRPC server in background thread (same process as Streamlit)
_grpc_server = start_grpc_server(port=50051)

# Run Streamlit in the same process so it shares the same `store` object
from streamlit.web.cli import main as st_main
sys.argv = ["streamlit", "run", "app/main.py",
            "--server.port=8501", "--server.address=0.0.0.0", "--server.headless=true"]
st_main()
