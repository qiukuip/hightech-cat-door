# Hightech Cat Door

An automatic cat door. Two ESP32-S3-CAM boards (one outdoor, one indoor) each with a VL53L1X time-of-flight radar and an OV2640 camera. The outdoor unit streams JPEG frames over a private TCP protocol to a Go server running on a local N100 mini-PC; the server runs YOLOv8n (single class `my-cat`) via a Python subprocess to decide whether to open. The indoor unit drives the door mechanism (two servos) and also has the camera wired — when its radar triggers, it sends one JPEG and asks the server to open directly.

## Architecture

```
   ┌─────────────┐   radar + JPEG     ┌──────────────────┐    YOLOv8n   ┌──────────┐
   │ esp32out    │ ────────────────►  │ Go TCP server    │ ──────────► │ detect.py│
   │ (outdoor)   │    TCP :1234       │ (state machine)  │ ◄────────── │ (Python) │
   └─────────────┘                    └──────────────────┘             └──────────┘
                                              ▲  │
                                              │  │ open/close commands
                                              ▼  ▼
                                       ┌─────────────┐
                                       │ esp32in     │  radar + JPEG + 2× servos
                                       │ (indoor)    │
                                       └─────────────┘
```

Door flow is symmetric: outdoor `0x1` detection → server opens → indoor reports passage → close. Indoor radar `0x9` → server opens → outdoor reports passage → close. State transitions are gated by `expectedPassage` so only the correct side's passage signal closes the door.

## Repository layout

| Path | Purpose |
|---|---|
| `src/esp32out.ino` | Outdoor ESP32-S3-CAM firmware (radar → camera → JPEG → server) |
| `src/esp32in.ino` | Indoor ESP32-S3-CAM firmware (servo control + radar-driven JPEG) |
| `src/server/` | Go TCP server (protocol, state machine, saver, db) |
| `src/client1/`, `src/client2/` | Legacy ASCII byte test clients |
| `model-training/` | YOLOv8n training and ONNX export for the `my-cat` class |
| `docs/` | Design notes (system spec, protocol, pinout) — in Chinese |
| `scripts/` | Server launchers: `start-stub.sh`, `start-always.sh`, `start-python.sh` |

For pinout, wire protocol, door-state machine, server flags, model-training commands, and known gotchas, see [`AGENTS.md`](./AGENTS.md).

## Quick start

```bash
# 1. Server with stub detector (never opens the door)
./scripts/start-stub.sh

# 2. Server with always-open detector (smoke-test the wire)
./scripts/start-always.sh

# 3. Server with real YOLOv8n
./scripts/start-python.sh
# or: go run ./src/server --detector=python \
#     --model=model-training/best_int8.onnx \
#     --script=model-training/detect.py \
#     --save-dir=/var/cat-door/snapshots

# 4. Flash ESP32 firmware (read docs/硬件连线设计.md first)
#    Edit WIFI_SSID / WIFI_PASS / SERVER_HOST at the top of each .ino,
#    then flash src/esp32out.ino and src/esp32in.ino to the two boards.
```

## Notes

- Each ESP32 announces its role to the server with a one-time `0x8` register frame on connect (`{0x00}` outdoor / `{0x01}` indoor). The server reads this to classify the peer.
- Both ESP32s connect to the same `SERVER_PORT` (default `1234`).
- The Go server is intentionally not a workspace with `src/client{1,2}` — each package declares its own `module main`. Run them from inside their directories.
- No tests, no CI, no linter config.

## License

Code is provided as-is. Dataset (Roboflow `cat-link` v1) is CC BY 4.0 — see `model-training/README.md`.