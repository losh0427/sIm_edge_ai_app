import threading
from dataclasses import dataclass, field
from typing import Optional
import time

@dataclass
class DetectionResult:
    edge_id: str = ""
    timestamp_ms: int = 0
    boxes: list = field(default_factory=list)  # list of dicts
    thumbnail_jpeg: bytes = b""
    latency_ms: float = 0.0
    frame_number: int = 0
    hal_backend: str = ""
    orig_width: int = 0
    orig_height: int = 0

class Store:
    def __init__(self):
        self._lock = threading.Lock()
        self._latest: dict[str, DetectionResult] = {}  # edge_id -> latest result
        self._stats: dict[str, dict] = {}
        self._frame_count = 0

    def update_detection(self, result: DetectionResult):
        with self._lock:
            self._latest[result.edge_id] = result
            self._frame_count += 1

    def get_latest(self) -> dict[str, DetectionResult]:
        with self._lock:
            return dict(self._latest)

    def get_frame_count(self) -> int:
        with self._lock:
            return self._frame_count

store = Store()
