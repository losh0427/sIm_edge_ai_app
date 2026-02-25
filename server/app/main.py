"""NiceGUI dashboard — replaces Streamlit polling with server-push updates."""

import os
import time
from collections import deque

from fastapi import Response
from fastapi.responses import StreamingResponse
from nicegui import app, ui

from app.state import store
from app.draw import load_labels, get_label, render_annotated_jpeg

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------
LABELS_PATH = os.environ.get("LABELS_PATH", "/app/models/coco_labels.txt")
LABELS = load_labels(LABELS_PATH)
OFFLINE_TIMEOUT_S = 10
CHART_MAX_POINTS = 60

# ---------------------------------------------------------------------------
# Per-device UI state (survives across ticks, rebuilt only on new device)
# ---------------------------------------------------------------------------
_device_cards: dict[str, dict] = {}
# edge_id -> { "card", "status_icon", "image", "latency_lbl", "det_lbl",
#               "frame_lbl", "hal_lbl", "det_list", "container" }

_chart_data: dict[str, dict] = {}
# edge_id -> { "fps_times": deque, "fps_vals": deque,
#               "lat_times": deque, "lat_vals": deque,
#               "prev_frame": int, "prev_ts": float }

_fps_chart = None
_latency_chart = None
_total_frames_label = None
_device_container = None
_header_label = None

# ---------------------------------------------------------------------------
# FastAPI endpoint: annotated JPEG per device
# ---------------------------------------------------------------------------
@app.get("/api/frame/{edge_id}")
def frame_jpeg(edge_id: str):
    latest = store.get_latest()
    result = latest.get(edge_id)
    if result is None or not result.thumbnail_jpeg:
        return Response(status_code=404)
    jpeg = render_annotated_jpeg(result.thumbnail_jpeg, result.boxes, LABELS)
    return Response(content=jpeg, media_type="image/jpeg")


# ---------------------------------------------------------------------------
# MJPEG streaming endpoint: smooth video feed per device
# ---------------------------------------------------------------------------
def _mjpeg_generate(edge_id: str):
    """Yield MJPEG frames as multipart chunks. Blocks on new frame arrival."""
    last_frame = -1
    while True:
        store.wait_new_frame(edge_id, timeout=1.0)
        latest = store.get_latest()
        result = latest.get(edge_id)
        if result is None or not result.thumbnail_jpeg:
            continue
        if result.frame_number == last_frame:
            continue
        last_frame = result.frame_number
        jpeg = render_annotated_jpeg(result.thumbnail_jpeg, result.boxes, LABELS)
        yield (
            b"--frame\r\n"
            b"Content-Type: image/jpeg\r\n\r\n" + jpeg + b"\r\n"
        )


@app.get("/api/stream/{edge_id}")
def mjpeg_stream(edge_id: str):
    return StreamingResponse(
        _mjpeg_generate(edge_id),
        media_type="multipart/x-mixed-replace; boundary=frame",
    )


# ---------------------------------------------------------------------------
# Dashboard page
# ---------------------------------------------------------------------------
@ui.page("/")
def dashboard():
    global _fps_chart, _latency_chart, _total_frames_label
    global _device_container, _header_label

    ui.dark_mode(True)

    # --- Header ---
    with ui.row().classes("w-full items-center justify-between px-4 py-2"):
        _header_label = ui.label("Edge AI Runtime — Detection Dashboard").classes(
            "text-2xl font-bold"
        )
        _total_frames_label = ui.label("Total frames: 0").classes("text-lg")

    ui.separator()

    with ui.row().classes("w-full px-4 gap-4"):
        # --- Left: device cards ---
        with ui.column().classes("flex-[2]"):
            _device_container = ui.column().classes("w-full gap-4")

        # --- Right: charts ---
        with ui.column().classes("flex-1 gap-4"):
            ui.label("FPS (per device)").classes("text-lg font-semibold")
            _fps_chart = ui.echart({
                "tooltip": {"trigger": "axis"},
                "xAxis": {"type": "category", "data": []},
                "yAxis": {"type": "value", "name": "FPS"},
                "series": [],
                "legend": {"data": []},
            }).classes("w-full h-64")

            ui.label("Inference Latency (ms)").classes("text-lg font-semibold")
            _latency_chart = ui.echart({
                "tooltip": {"trigger": "axis"},
                "xAxis": {"type": "category", "data": []},
                "yAxis": {"type": "value", "name": "ms"},
                "series": [],
                "legend": {"data": []},
            }).classes("w-full h-64")

    # --- Periodic tick ---
    ui.timer(0.5, _tick)


# ---------------------------------------------------------------------------
# Tick — called every 0.5 s, updates DOM in-place
# ---------------------------------------------------------------------------
def _tick():
    latest = store.get_latest()
    total = store.get_frame_count()
    now = time.time()

    if _total_frames_label:
        _total_frames_label.set_text(f"Total frames: {total}")

    if not latest and _device_container:
        # Show waiting message if no device cards exist yet
        if not _device_cards:
            _device_container.clear()
            with _device_container:
                ui.label("Waiting for edge devices to connect...").classes(
                    "text-gray-400 text-lg"
                )
        return

    for edge_id, result in latest.items():
        _ensure_device_card(edge_id)
        dc = _device_cards[edge_id]

        # Online / offline status
        online = (now - result.timestamp_ms / 1000.0) < OFFLINE_TIMEOUT_S
        color_cls = "text-green-500" if online else "text-red-500"
        dc["status_icon"]._props["name"] = "circle"
        dc["status_icon"].classes(remove="text-green-500 text-red-500")
        dc["status_icon"].classes(add=color_cls)
        dc["status_icon"].update()

        # Metrics
        dc["latency_lbl"].set_text(f"Latency: {result.latency_ms:.1f} ms")
        dc["det_lbl"].set_text(f"Detections: {len(result.boxes)}")
        dc["frame_lbl"].set_text(f"Frame: #{result.frame_number}")
        dc["hal_lbl"].set_text(f"HAL: {result.hal_backend}")

        # Detection list
        dc["det_list"].clear()
        with dc["det_list"]:
            for box in result.boxes:
                lbl = get_label(LABELS, box.get("class_id", -1))
                ui.label(f"  {lbl}: {box['confidence']:.0%}").classes("text-sm")

        # Chart data
        _update_chart_data(edge_id, result, now)

    _refresh_charts()


# ---------------------------------------------------------------------------
# Build a device card (called once per new edge_id)
# ---------------------------------------------------------------------------
def _ensure_device_card(edge_id: str):
    if edge_id in _device_cards:
        return

    with _device_container:
        # Remove "waiting" message if present
        if not _device_cards:
            _device_container.clear()

        with ui.card().classes("w-full p-4") as card:
            with ui.row().classes("items-center gap-2"):
                status_icon = ui.icon("circle").classes("text-green-500")
                ui.label(edge_id).classes("text-xl font-bold")

            image = ui.html(
                f'<img src="/api/stream/{edge_id}" style="width:100%">'
            )

            with ui.row().classes("gap-6 flex-wrap"):
                latency_lbl = ui.label("Latency: — ms")
                det_lbl = ui.label("Detections: 0")
                frame_lbl = ui.label("Frame: #0")
                hal_lbl = ui.label("HAL: —")

            det_list = ui.column().classes("pl-2")

    _device_cards[edge_id] = {
        "card": card,
        "status_icon": status_icon,
        "image": image,
        "latency_lbl": latency_lbl,
        "det_lbl": det_lbl,
        "frame_lbl": frame_lbl,
        "hal_lbl": hal_lbl,
        "det_list": det_list,
    }

    _chart_data[edge_id] = {
        "fps_times": deque(maxlen=CHART_MAX_POINTS),
        "fps_vals": deque(maxlen=CHART_MAX_POINTS),
        "lat_times": deque(maxlen=CHART_MAX_POINTS),
        "lat_vals": deque(maxlen=CHART_MAX_POINTS),
        "prev_frame": 0,
        "prev_ts": 0.0,
    }


# ---------------------------------------------------------------------------
# Chart helpers
# ---------------------------------------------------------------------------
def _update_chart_data(edge_id: str, result, now: float):
    cd = _chart_data[edge_id]
    frame = result.frame_number

    if cd["prev_ts"] > 0 and frame > cd["prev_frame"]:
        dt = now - cd["prev_ts"]
        if dt > 0:
            fps = (frame - cd["prev_frame"]) / dt
            cd["fps_times"].append(frame)
            cd["fps_vals"].append(round(fps, 1))

    cd["lat_times"].append(frame)
    cd["lat_vals"].append(round(result.latency_ms, 1))

    cd["prev_frame"] = frame
    cd["prev_ts"] = now


def _refresh_charts():
    if not _fps_chart or not _latency_chart:
        return

    # Build x-axis from union of all frame numbers
    all_frames = sorted(set(
        f for cd in _chart_data.values() for f in cd["fps_times"]
    ))
    fps_series = []
    for eid, cd in _chart_data.items():
        mapping = dict(zip(cd["fps_times"], cd["fps_vals"]))
        fps_series.append({
            "name": eid,
            "type": "line",
            "smooth": True,
            "data": [mapping.get(f) for f in all_frames],
        })

    _fps_chart.options["xAxis"]["data"] = [str(f) for f in all_frames]
    _fps_chart.options["series"] = fps_series
    _fps_chart.options["legend"]["data"] = list(_chart_data.keys())
    _fps_chart.update()

    all_lat_frames = sorted(set(
        f for cd in _chart_data.values() for f in cd["lat_times"]
    ))
    lat_series = []
    for eid, cd in _chart_data.items():
        mapping = dict(zip(cd["lat_times"], cd["lat_vals"]))
        lat_series.append({
            "name": eid,
            "type": "line",
            "smooth": True,
            "data": [mapping.get(f) for f in all_lat_frames],
        })

    _latency_chart.options["xAxis"]["data"] = [str(f) for f in all_lat_frames]
    _latency_chart.options["series"] = lat_series
    _latency_chart.options["legend"]["data"] = list(_chart_data.keys())
    _latency_chart.update()
