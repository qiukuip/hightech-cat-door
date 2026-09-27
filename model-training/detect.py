"""Long-running YOLOv8 detection helper for the Go server.

Reads (len, jpeg) pairs from stdin, writes one byte per request to stdout:
    0x00  no `my-cat` detected
    0x01  `my-cat` detected

This script is invoked once by the Go server (`-detector python`) and kept
alive so the ONNX model stays loaded. The wire format mirrors the framing
helper in `src/server/protocol.go`.
"""

import argparse
import contextlib
import io
import os
import struct
import sys
import time

CAT_CONF_THRESHOLD = 0.8


def log(msg: str) -> None:
    print(f"[detect.py] {msg}", file=sys.stderr, flush=True)


@contextlib.contextmanager
def silenced_stdout():
    """Redirect fd 1 to /dev/null for the duration of the block.

    Required because Ultralytics writes its own loading/inference status
    directly to stdout (often caching sys.stdout at import time, so a
    Python-level redirect_stdout swap misses it). Going through os.dup2
    catches every write to fd 1, including old cached references.
    """
    saved_fd = os.dup(1)
    devnull = os.open(os.devnull, os.O_WRONLY)
    try:
        sys.stdout.flush()
        os.dup2(devnull, 1)
        yield
        sys.stdout.flush()
    finally:
        os.dup2(saved_fd, 1)
        os.close(saved_fd)
        os.close(devnull)


def load_model(model_path: str):
    log(f"loading {model_path}")
    with silenced_stdout():
        from ultralytics import YOLO
        return YOLO(model_path)


def decode_jpeg(jpeg: bytes):
    from PIL import Image
    return Image.open(io.BytesIO(jpeg)).convert("RGB")


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
            img = decode_jpeg(jpeg)
        except Exception as e:
            log(f"jpeg decode failed: {e}; responding not-cat")
            stdout.write(b"\x00")
            stdout.flush()
            continue

        try:
            with silenced_stdout():
                results = model.predict(img, verbose=False)
        except Exception as e:
            log(f"predict error: {e}; responding not-cat")
            stdout.write(b"\x00")
            stdout.flush()
            continue

        max_conf = 0.0
        for r in results:
            if len(r.boxes) > 0:
                c = float(r.boxes.conf.max())
                if c > max_conf:
                    max_conf = c
        detected = max_conf >= CAT_CONF_THRESHOLD
        verdict = "cat" if detected else "not-cat"
        log(f"conf={max_conf:.3f} threshold={CAT_CONF_THRESHOLD:.2f} -> {verdict}")
        stdout.write(b"\x01" if detected else b"\x00")
        stdout.flush()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True, help="ONNX model path")
    parser.add_argument("--warmup", action="store_true", help="run a dummy inference before serving")
    args = parser.parse_args()

    model = load_model(args.model)

    if args.warmup:
        import numpy as np
        log("warmup inference")
        t0 = time.time()
        dummy = np.zeros((480, 640, 3), dtype=np.uint8)
        with silenced_stdout():
            model.predict(dummy, verbose=False)
        log(f"warmup done in {time.time() - t0:.2f}s")

    log("ready")
    try:
        run(model)
    except BrokenPipeError:
        log("stdout closed; exiting")
    return 0


if __name__ == "__main__":
    sys.exit(main())
