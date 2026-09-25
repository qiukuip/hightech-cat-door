"""Long-running YOLOv8 detection helper for the Go server.

Reads (len, jpeg) pairs from stdin, writes one byte per request to stdout:
    0x00  no `my-cat` detected
    0x01  `my-cat` detected

This script is invoked once by the Go server (`-detector python`) and kept
alive so the ONNX model stays loaded. The wire format mirrors the framing
helper in `src/server/protocol.go`.
"""

import argparse
import struct
import sys
import time


def log(msg: str) -> None:
    print(f"[detect.py] {msg}", file=sys.stderr, flush=True)


def load_model(model_path: str):
    from ultralytics import YOLO
    log(f"loading {model_path}")
    return YOLO(model_path)


def run(model) -> None:
    stdin = sys.stdin.buffer
    stdout = sys.stdout.buffer
    while True:
        hdr = stdin.read(4)
        if len(hdr) < 4:
            log("stdin closed; exiting")
            return
        (length,) = struct.unpack("<I", hdr)
        if length == 0:
            stdout.write(b"\x00")
            stdout.flush()
            continue
        jpeg = b""
        while len(jpeg) < length:
            chunk = stdin.read(length - len(jpeg))
            if not chunk:
                log("unexpected eof reading jpeg")
                return
            jpeg += chunk

        try:
            results = model.predict(jpeg, verbose=False)
        except Exception as e:
            log(f"predict error: {e}; responding not-cat")
            stdout.write(b"\x00")
            stdout.flush()
            continue

        detected = any(len(r.boxes) > 0 for r in results)
        stdout.write(b"\x01" if detected else b"\x00")
        stdout.flush()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True, help="ONNX model path")
    parser.add_argument("--warmup", action="store_true", help="run a dummy inference before serving")
    args = parser.parse_args()

    model = load_model(args.model)

    if args.warmup:
        log("warmup inference")
        t0 = time.time()
        model.predict(b"\xff\xd8\xff\xe0", verbose=False)
        log(f"warmup done in {time.time() - t0:.2f}s")

    log("ready")
    try:
        run(model)
    except BrokenPipeError:
        log("stdout closed; exiting")
    return 0


if __name__ == "__main__":
    sys.exit(main())
