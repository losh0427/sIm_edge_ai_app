import grpc
from concurrent import futures
import edge_ai_pb2
import edge_ai_pb2_grpc
from app.state import store, DetectionResult

class EdgeServicer(edge_ai_pb2_grpc.EdgeServiceServicer):
    def ReportDetection(self, request, context):
        boxes = []
        for b in request.boxes:
            boxes.append({
                "x_min": b.x_min, "y_min": b.y_min,
                "x_max": b.x_max, "y_max": b.y_max,
                "class_id": b.class_id, "label": b.label,
                "confidence": b.confidence,
            })

        result = DetectionResult(
            edge_id=request.edge_id,
            timestamp_ms=request.timestamp_ms,
            boxes=boxes,
            thumbnail_jpeg=request.thumbnail_jpeg,
            latency_ms=request.inference_latency_ms,
            frame_number=request.frame_number,
            hal_backend=request.hal_backend,
            orig_width=request.original_width,
            orig_height=request.original_height,
        )
        store.update_detection(result)
        return edge_ai_pb2.ServerResponse(ok=True, model_version=1)

    def ReportStats(self, request, context):
        return edge_ai_pb2.ServerResponse(ok=True, model_version=1)

def start_grpc_server(port=50051):
    server = grpc.server(futures.ThreadPoolExecutor(max_workers=4))
    edge_ai_pb2_grpc.add_EdgeServiceServicer_to_server(EdgeServicer(), server)
    server.add_insecure_port(f"0.0.0.0:{port}")
    server.start()
    print(f"gRPC server started on :{port}")
    return server
