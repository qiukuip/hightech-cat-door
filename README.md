# 高科技猫门

一个自动猫门系统。两块 ESP32-S3-CAM（室外、室内各一块），每块接一个 VL53L1X 测距雷达和一个 OV2640 摄像头。室外端把 JPEG 帧通过私有 TCP 协议流式发给本地 N100 迷你主机上的 Go 服务端；服务端通过 Python 子进程跑 YOLOv8n（单类别 `my-cat`），由它决定是否开门。室内端驱动门机（两个舵机），同时也接了摄像头——当室内雷达触发时，室内端拍一张 JPEG 并直接请求服务端开门。

## 架构

```
   ┌─────────────┐   雷达 + JPEG     ┌──────────────────┐    YOLOv8n   ┌──────────┐
   │ esp32out    │ ────────────────►  │ Go TCP 服务端    │ ──────────► │ detect.py│
   │ (室外)      │    TCP :1234       │ (状态机)         │ ◄────────── │ (Python) │
   └─────────────┘                    └──────────────────┘             └──────────┘
                                              ▲  │
                                              │  │ 开关门指令
                                              ▼  ▼
                                       ┌─────────────┐
                                       │ esp32in     │  雷达 + JPEG + 2× 舵机
                                       │ (室内)      │
                                       └─────────────┘
```

门的状态流是对称的：室外 `0x1` 检测 → 服务端开门 → 室内上报通过 → 关；室内雷达 `0x9` → 服务端开门 → 室外上报通过 → 关。状态转移由 `expectedPassage` 把关，只有正确一侧的通过信号才会关门。

## 目录结构

| 路径 | 用途 |
|---|---|
| `src/esp32out.ino` | 室外 ESP32-S3-CAM 固件（雷达 → 摄像头 → JPEG → 服务端） |
| `src/esp32in.ino` | 室内 ESP32-S3-CAM 固件（舵机控制 + 雷达触发的 JPEG） |
| `src/server/` | Go TCP 服务端（协议、状态机、存档、数据库） |
| `src/client1/`、`src/client2/` | 早期遗留的 ASCII 字节测试客户端 |
| `model-training/` | 单类别 `my-cat` 的 YOLOv8n 训练与 ONNX 导出 |
| `docs/` | 设计文档（系统方案、协议、接线）——中文 |
| `scripts/` | 服务端启动脚本：`start-stub.sh`、`start-always.sh`、`start-python.sh` |

关于接线、通信协议、门状态机、服务端参数、模型训练命令和踩过的坑，见 [`AGENTS.md`](./AGENTS.md)。

## 快速开始

```bash
# 1. 用 stub 检测器跑服务端（永远不会开门）
./scripts/start-stub.sh

# 2. 用 always-open 检测器跑服务端（连通性冒烟测试）
./scripts/start-always.sh

# 3. 用真实的 YOLOv8n 跑服务端
./scripts/start-python.sh
# 等价于：
# go run ./src/server --detector=python \
#     --model=model-training/best_int8.onnx \
#     --script=model-training/detect.py \
#     --save-dir=/var/cat-door/snapshots

# 4. 烧录 ESP32 固件（先看 docs/硬件连线设计.md）
#    先编辑每个 .ino 文件顶部的 WIFI_SSID / WIFI_PASS / SERVER_HOST，
#    然后把 src/esp32out.ino 和 src/esp32in.ino 分别烧到两块板子上。
```

## 备注

- 两块 ESP32 在连上服务端时各发一次 `0x8` 注册帧声明身份（`{0x00}` 室外 / `{0x01}` 室内），服务端据此识别 peer。
- 两块 ESP32 都连同一个 `SERVER_PORT`（默认 `1234`）。
- Go 服务端没有和 `src/client{1,2}` 组成 workspace——每个包各自声明 `module main`，请在各自的目录里运行。
- 没有测试、没有 CI、没有 lint 配置。

## 许可

代码按原样提供。数据集（Roboflow `cat-link` v1）为 CC BY 4.0——见 `model-training/README.md`。