# AGENTS.md

## Project
Automatic cat door. Outdoor ESP32-S3-CAM (`src/esp32out.ino`) polls one VL53L1X radar + a light sensor + an IR light + OV2640 and streams JPEGs over a private TCP protocol to a Go server on a local N100 mini-PC. The server runs YOLOv8n via a Python subprocess and decides open/close. Indoor ESP32-S3-CAM (`src/esp32in.ino`) drives two servos (handle-down, push-pull), monitors passage with one VL53L1X, and **also** has the OV2640 wired — when its radar triggers it captures a single JPEG and asks the server to open the door directly (no YOLO, since the cat is indoors). Design history and component decisions live in `docs/猫自动门系统方案可行性评估报告.md`; flow in `docs/系统工作流程.md`; wire-format spec in `docs/通信协议设计.md`; full pinout in `docs/硬件连线设计.md`.

## Layout
- `src/esp32out.ino` — outdoor ESP32-S3-CAM firmware (radar → camera → JPEG → server).
- `src/esp32in.ino` — indoor ESP32-S3-CAM firmware (servo control + radar-driven JPEG trigger on `0x9` + passage event on `0x7`).
- `src/server/` — Go TCP server, split into `protocol.go`, `detector.go`, `server.go`, `saver.go`, `db.go`. Listens on a single port (default `:1234`); each ESP32 announces its role via a one-time `0x8` register frame on connect.
- `src/client1/`, `src/client2/` — legacy ASCII byte test clients. Each is its own `module main` (`go 1.26.2`). `src/client2/client1.go` is a byte-for-byte copy of `src/client1/client1.go` — the second file was not renamed. Treat as a single client unless told otherwise.
- `model-training/` — YOLOv8n training and ONNX export for the single class `my-cat` (`data.yaml`). `detect.py` is the long-running helper the Go server spawns when `--detector=python`.
- `docs/` — design notes in Chinese. `通信协议设计.md` is the authoritative framing spec; `硬件连线设计.md` is the pinout (read before flashing).

## Wire protocol (authoritative: `docs/通信协议设计.md`)
Fixed header + variable body: `[1B magic=0xAA][1B type][2B total-length (LE)][body…]`.
Message types: `0x0` heartbeat, `0x1` upload-and-detect, `0x2` detect result, `0x3` request capture, `0x4` capture-and-store, `0x5` manual open, `0x6` manual close, `0x7` door-state report, `0x8` register/identity, `0x9` indoor-trigger-open (with JPEG body), `0xA` outdoor-passage-detected (empty body), `0xF` disconnect.
Body conventions used by the implementation:
- `0x2` body = 1 byte: `0x00` not-cat, `0x01` my-cat.
- `0x7` body = 1 byte: `0x00` closed, `0x01` opening, `0x02` open, `0x03` closing, `0x04` passage-detected.
- `0x8` body = 1 byte: `0x00` outdoor, `0x01` indoor. Sent **once** on connect; server ignores re-sends.
- `0x9` body = JPEG bytes (single frame, captured by indoor ESP32 on radar-triggered rising edge).
- All other types have empty body.
Implemented end-to-end in `src/server/protocol.go` and the matching C helpers in both `.ino` files. Max body is 65531 bytes; `esp32out.ino` and `esp32in.ino` drop frames larger than that.

## Commands
Each Go package is a separate module — run from inside the dir, or use the wrappers in `scripts/` (accept extra flags via `"$@"`, e.g. `./scripts/start-stub.sh --listen=:9000`):
```bash
./scripts/start-stub.sh                       # listens :1234, never opens the door (--detector=stub)
./scripts/start-always.sh                     # always opens the door, smoke-test the wire (--detector=always)
./scripts/start-python.sh                     # real YOLOv8n; reads SAVE_DIR (default /var/cat-door/snapshots) and DB_DSN env vars
# raw form, equivalent:
go run ./src/server --detector=stub
go run ./src/server --detector=always
go run ./src/server --detector=python --model=model-training/best_int8.onnx --script=model-training/detect.py --save-dir=/var/cat-door/snapshots
go run ./src/client1                           # sends bytes 'a'..'j' (legacy ASCII, hits the old single-port server shape)
go run ./src/client2                           # identical to client1
```
Do not try `go build ./...` from the repo root — the three sibling dirs each declare `module main` and are not a workspace.

Server flags (defaults shown in `src/server/server.go`):
- `--listen=:1234`
- `--detector=stub|always|python` (default `stub`)
- `--model`, `--script` (python detector only)
- `--save-dir=""` — when non-empty, every JPEG that **triggered a cat event** is written under `<save-dir>/outdoor/` (entry, detector said cat) or `<save-dir>/indoor/` (exit, indoor radar triggered). Empty disables saving.
- `--db-dsn=""` — PostgreSQL DSN for door-event and detection logging; empty disables DB logging (waiting for configuration). When set, the server connects on startup, creates `door_events` and `detections` tables if missing, and asynchronously inserts rows via a background worker (buffered chan size 256; full → drop with warning).

Model training (CPU is the default in `train.py`; switch `device=` to `'mps'` or `'0'` for GPU):
```bash
pip install ultralytics
# yolov8n.pt must be downloaded manually from the Ultralytics release page (gitignored)
python model-training/train.py
python model-training/export_onnx_int8.py     # writes runs/detect/train/weights/best_int8.onnx
python model-training/validate_onnx_int8.py   # infers on model-val-1.jpeg
python model-training/detect.py --model runs/detect/train/weights/best_int8.onnx --warmup   # standalone smoke test
```

## Door flow (server state machine)
Two symmetric flows, gated by `expectedPassage` on the `doorController` (set by the side that **triggered** the open):

### Entry flow (outdoor → indoor)
- `idle` → outdoor `0x1` returns `0x01` (YOLO says cat) → JPEG saved to `<save-dir>/outdoor/` → `requestOpen(roleOutdoor)` → `expectedPassage=indoor` → send `0x5` to indoor → `opening`.
- `opening` → indoor `0x7` with `0x02` (open) → `open`, start 30 s hold timer.
- `open` → indoor `0x7` with `0x04` (passage) closes — **only if** `expectedPassage==indoor`; `0xA` from outdoor is ignored in this flow.
- `open` → 30 s timer fires → send `0x6` to indoor → `closing`.
- `closing` → indoor `0x7` with `0x00` (closed) → `idle`, `expectedPassage` reset.

### Exit flow (indoor → outdoor)
- `idle` → indoor `0x9` (JPEG body) → JPEG saved to `<save-dir>/indoor/` → `requestOpen(roleIndoor)` → `expectedPassage=outdoor` → send `0x5` to indoor → `opening`.
- `opening` → indoor `0x7` with `0x02` (open) → `open`, start 30 s hold timer.
- `open` → outdoor `0xA` closes — **only if** `expectedPassage==outdoor`; indoor `0x7` passage is ignored in this flow.
- `open` → 30 s timer fires → send `0x6` to indoor → `closing`.
- `closing` → indoor `0x7` with `0x00` (closed) → `idle`, `expectedPassage` reset.

## PostgreSQL logging (`src/server/db.go`)
When `--db-dsn` is non-empty, the server uses `pgxpool` to log two kinds of events asynchronously through a buffered channel (size 256, drop-on-full with warning):

- `door_events` — one row per state transition the controller performs:
  - `event='open_request'` from `requestOpen` (source = side that triggered it, reason `'request'`)
  - `event='open_confirmed'` from indoor `0x7` `0x02` (source `'indoor'`)
  - `event='close_request'` from `requestClose` (source = side whose passage was expected, reason = `'timeout'` | `'passage'` | `'passage-outdoor'`)
  - `event='closed'` from indoor `0x7` `0x00` (source `'indoor'`)
- `detections` — one row per JPEG that **was actually persisted to disk** (i.e. `--save-dir` is non-empty). For outdoor `0x1` detections that returned not-cat, no row is written (no image exists).
  - `direction='outdoor' | 'indoor'`, `image_path` = absolute path the saver wrote, `result='cat'`

Schema bootstrap runs `CREATE TABLE IF NOT EXISTS …` plus a `created_at DESC` index on each table on startup. Both tables also have `id BIGSERIAL PRIMARY KEY` and `created_at TIMESTAMPTZ NOT NULL DEFAULT now()`.

### Robustness
- If the indoor connection drops while the door is not idle, the controller resets to `idle` (no orphaned commands).
- Both ESP32s may spam `0xA` / `0x7` Passage; the server filters by state + `expectedPassage` so only the *correct* side's signal closes the door.

## Thresholds
- Outdoor: `OPEN_CAMERA_DISTANCE_CM = 10` (`src/esp32out.ino`) — gates both the entry trigger (camera + `0x1`) and the `0xA` passage event.
- Indoor: `INDOOR_OPEN_DISTANCE_CM = 10` (`src/esp32in.ino`) — gates both the exit trigger (`0x9`) and the `0x7` passage event.
- Both default to 10 cm; change the `#define` and re-flash to adjust.
- Debounce: `RADAR_DEBOUNCE_N = 3` in both `.ino` files. Each radar requires N consecutive identical readings (200 ms/poll × N) before the stable state flips; N=3 → 600 ms confirmation. Raise to slow the door, lower to react faster.

## Gotchas (read before touching the code)
- `src/esp32out.ino` and `src/esp32in.ino` are written for ESP32-S3-CAM. Each side uses **one VL53L1X** at the default I2C address `0x29`; GPIO39 (`PIN_XSHUT2`) is left floating. If a second radar is added later, restore the XSHUT dance and assign `0x29` / `0x30` — see `initRadars()` history and `docs/硬件连线设计.md`.
- Both sketches hard-code `WIFI_SSID`, `WIFI_PASS`, `SERVER_HOST` at the top. Change them before flashing.
- Light sensor is on **GPIO 3 (ADC1_CH2)** in `esp32out.ino`. The original draft claimed GPIO 2 — that pin is I2C SCL and would short the bus. Don't revert.
- Radar threshold is in cm: `(distanceMM / 10) <= THRESHOLD` (10 cm by default). This is integer division — values 0-10 cm trigger.
- Both radars debounce by requiring `RADAR_DEBOUNCE_N` consecutive identical readings before the stable state flips — protects outdoor camera `init`/`deinit` and indoor `catSessionActive` reset from radar jitter.
- Both ESP32s connect to the **same** `SERVER_PORT` (default `1234`). On connect each sends a one-time `0x8` register frame (`body = {0x00}` outdoor / `{0x01}` indoor) within 5s; the server reads it to classify the peer. If the frame doesn't arrive in time, the connection is closed.
- `src/esp32in.ino` captures **one** JPEG per cat session (rising-edge only) and immediately deinits the camera. The cat session flag resets when the **debounced** radar has been clear for `RADAR_DEBOUNCE_N` polls (600 ms by default) — re-approaching the door re-arms a fresh `0x9`.
- `esp32out.ino` sends `0xA` every poll tick while the **debounced** radar state is "cat present"; server de-bounces via state + `expectedPassage` so the same indoor cat won't trigger an outdoor close during entry flow.
- `src/server/server.go` no longer panics on per-connection errors — it logs and exits the read loop. The listener keeps accepting new peers; a stale peer is closed when a new one with the same role connects.
- `model-training/detect.py` keeps the ONNX model loaded across calls. Wire format on its stdin: `[4-byte LE length][jpeg bytes]`. Wire format on its stdout: 1 byte (`0x00` / `0x01`). The Go side speaks this in `src/server/detector.go` `PythonDetector`.
- `model-training/export_onnx_int8.py` and `validate_onnx_int8.py` load from `runs/detect/train/weights/…`, but the README inside `model-training/` still references the old path `runs/detect/train-2/weights/best.pt` — trust the `.py` files.
- `.gitignore` excludes `*.pt`, dataset dirs, `runs/`, and most `*.onnx`. `best_int8.onnx` is already committed (the `*.onnx` line is commented out). Confirm with `git status` before adding more model artifacts.
- `--save-dir` writes to `<save-dir>/outdoor/` and `<save-dir>/indoor/`; the directory tree is created on demand. Rotation/cleanup is the caller's responsibility.
- No tests, no CI, no linter, no formatter config. Don't grep for them.
</content>
</invoke>