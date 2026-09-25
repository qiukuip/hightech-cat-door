# 高科技猫门

一个自动猫门系统。两块 ESP32-S3-CAM（室外、室内各一块），每块接一个 VL53L1X 测距雷达和一个 OV2640 摄像头。室外端把 JPEG 帧通过私有 TCP 协议流式发给本地 N100 迷你主机上的 Go 服务端；服务端通过 Python 子进程跑 YOLOv8n（单类别 `my-cat`），由它决定是否开门。室内端驱动门机（两个舵机），同时也接了摄像头——当室内雷达触发时，室内端拍一张 JPEG 并直接请求服务端开门。设计初衷、组件选型和历史讨论见 `docs/猫自动门系统方案可行性评估报告.md`。

## 系统组件

### 硬件

| 设备 | 型号 | 作用 |
|---|---|---|
| 室外端 | ESP32-S3-CAM（含 OV2640） | 测距 + 拍照 + Wi-Fi 上传 |
| 室内端 | ESP32-S3-CAM（含 OV2640） | 测距 + 拍照 + 控制舵机 |
| 室外雷达 | VL53L1X（I2C 地址 `0x29`） | 检测门外是否有猫靠近（10 cm 阈值） |
| 室内雷达 | VL53L1X（I2C 地址 `0x29`） | 检测门内是否有猫经过（10 cm 阈值） |
| 光照传感器 | 光敏电阻 → GPIO3 (ADC1_CH2) | 判断白天/夜晚，决定是否点亮 IR 补光灯 |
| IR 补光灯 | 红外 LED | 夜间照明，配合 OV2640 夜间成像 |
| 门机 | 2× 舵机（压下把手 + 推拉） | 物理开关门 |
| 主控 | N100 迷你主机 | 跑 Go 服务端 + Python YOLOv8n |

### 软件

- **`src/esp32out.ino`**：室外固件。轮询室外雷达；触发后初始化 OV2640、拍一帧 JPEG、打包成 `0x1` 帧发给服务端。在轮询期间持续检测，若室外雷达持续报告猫存在则每帧重发 `0x1`。
- **`src/esp32in.ino`**：室内固件。监听室内雷达；触发后初始化 OV2640、拍一帧 JPEG、打包成 `0x9` 帧发给服务端（直接请求开门，跳过 YOLO）。同时根据服务端 `0x5` / `0x6` 指令驱动两个舵机，并把自身的状态变化通过 `0x7` 上报。
- **`src/server/`**：Go TCP 服务端，单端口 `:1234`，按 `0x8` 注册帧区分室外/室内 peer。包含协议解析（`protocol.go`）、状态机（`server.go`）、检测器（`detector.go`：stub / always / python 三种后端）、JPEG 存档（`saver.go`）、PostgreSQL 事件日志（`db.go`）。
- **`model-training/detect.py`**：常驻 Python 进程，按 `[4 字节 LE 长度][JPEG 字节]` 读 stdin，输出 1 字节（`0x00` 非猫 / `0x01` 是我的猫）。
- **`scripts/`**：`start-stub.sh` / `start-always.sh` / `start-python.sh` 三个启动脚本。

## 工作流程

### 进猫流程（室外 → 室内）

```
[室外 idle]
   │  室外雷达 ≤10 cm
   ▼
室外初始化 OV2640 → 拍 JPEG → 发 0x1
   │
   ▼
服务端启动 YOLO：
   ├─ not-cat → 回 0x2(0x00)，不入库；保持 idle
   └─ cat     → 写盘 <save-dir>/outdoor/，回 0x2(0x01)
                    │
                    ▼
              requestOpen(roleOutdoor)
              expectedPassage = indoor
              发 0x5 给室内 → 状态 = opening
                    │
                    ▼  室内回 0x7(0x02)
              状态 = open，启动 30 s 保持定时器
                    │
       ┌────────────┴────────────┐
       ▼                         ▼
  室内回 0x7(0x04)            30 s 到
  passage（仅当              发 0x6 给室内
  expectedPassage=indoor）    状态 = closing
       │                         │
       ▼                         ▼  室内回 0x7(0x00)
  requestClose                状态 = idle
  状态 = closing                  expectedPassage = reset
       │
       ▼  室内回 0x7(0x00)
  状态 = idle
  expectedPassage = reset
```

### 出猫流程（室内 → 室外）

```
[室内 idle]
   │  室内雷达 ≤10 cm
   ▼
室内初始化 OV2640 → 拍 JPEG → 发 0x9（含 JPEG）
   │
   ▼
服务端：写盘 <save-dir>/indoor/（不跑 YOLO，因为猫已确认在室内）
   │
   ▼
requestOpen(roleIndoor)
expectedPassage = outdoor
发 0x5 给室内 → 状态 = opening
   │
   ▼  室内回 0x7(0x02)
状态 = open，启动 30 s 保持定时器
   │
   ├──────────────────────┐
   ▼                      ▼
室外发 0xA             30 s 到
passage（仅当          发 0x6 给室内
expectedPassage=outdoor） 状态 = closing
   │                      │
   ▼                      ▼  室内回 0x7(0x00)
requestClose             状态 = idle
状态 = closing              expectedPassage = reset
   │
   ▼  室内回 0x7(0x00)
状态 = idle
expectedPassage = reset
```

### 关键设计点

- **去抖**：`0xA` / `0x7` passage 事件可能高频刷，由 `state + expectedPassage` 双重把关，只接受正确一侧的通过信号。
- **室内掉线兜底**：若室内连接在非 idle 状态断开，控制机直接重置为 idle，避免悬挂的开关指令。
- **JPEG 帧大小**：受 65531 字节 body 上限约束；ESP32 端会丢弃更大的帧。
- **角色自声明**：每个 ESP32 连上后 5 秒内必须发一次 `0x8` 注册帧（`{0x00}` 室外 / `{0x01}` 室内），否则服务端关闭连接。

## 通信协议

完整权威定义见 `docs/通信协议设计.md`。这里只列骨架：

### 帧格式

```
┌──────────┬──────────┬────────────────────┬──────────────────────┐
│ magic    │ type     │ total-length (LE)  │ body                 │
│ 1B=0xAA  │ 1B       │ 2B                 │ 0..65531 B           │
└──────────┴──────────┴────────────────────┴──────────────────────┘
```

定长头 4 字节 + 变长 body。total-length 含头字节。

### 消息类型

| type | 方向 | body | 含义 |
|---|---|---|---|
| `0x0` | 双向 | 空 | 心跳 |
| `0x1` | 室外 → 服务端 | JPEG | 上传一帧并请求 YOLO 检测 |
| `0x2` | 服务端 → 室外 | 1B | 检测结果（`0x00` 非猫 / `0x01` 是我的猫） |
| `0x3` | 服务端 → 室内 | 空 | 请求室内拍一张 |
| `0x4` | 室内 → 服务端 | JPEG | 上传一帧并存储 |
| `0x5` | 服务端 → 室内 | 空 | 开门 |
| `0x6` | 服务端 → 室内 | 空 | 关门 |
| `0x7` | 室内 → 服务端 | 1B | 门状态上报（`0x00` closed / `0x01` opening / `0x02` open / `0x03` closing / `0x04` passage） |
| `0x8` | ESP32 → 服务端（连上后一次） | 1B | 身份注册（`0x00` 室外 / `0x01` 室内） |
| `0x9` | 室内 → 服务端 | JPEG | 雷达触发的室内开门请求（单帧 JPEG） |
| `0xA` | 室外 → 服务端 | 空 | 室外检测到猫正在通过 |
| `0xF` | 双向 | 空 | 断开连接 |

## 目录结构

| 路径 | 用途 |
|---|---|
| `src/esp32out.ino` | 室外 ESP32-S3-CAM 固件 |
| `src/esp32in.ino` | 室内 ESP32-S3-CAM 固件 |
| `src/server/` | Go TCP 服务端（`protocol.go`、`detector.go`、`server.go`、`saver.go`、`db.go`） |
| `src/client1/`、`src/client2/` | 早期遗留的 ASCII 字节测试客户端（各自独立 `module main`） |
| `model-training/` | YOLOv8n 训练与 ONNX 导出（`train.py`、`detect.py`、`export_onnx_int8.py` 等） |
| `docs/` | 设计文档（系统方案、协议、接线、工作流程） |
| `scripts/` | 服务端启动脚本：`start-stub.sh`、`start-always.sh`、`start-python.sh` |
| `AGENTS.md` | 给 AI 协作者看的项目指南（含服务端参数、踩过的坑） |

## 快速开始

```bash
# 1. 用 stub 检测器跑服务端（永远不会开门，仅验证连通）
./scripts/start-stub.sh

# 2. 用 always-open 检测器跑服务端（连通性 + 开关门冒烟测试）
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

### 服务端参数（完整列表见 `src/server/server.go`）

- `--listen=:1234` 监听端口
- `--detector=stub|always|python` 检测器后端（默认 stub）
- `--model` / `--script` 仅 python 后端用
- `--save-dir=""` 非空时，把所有触发猫事件的 JPEG 写到 `<save-dir>/outdoor/`（进门，YOLO 说是猫）或 `<save-dir>/indoor/`（出门，室内雷达触发）
- `--db-dsn=""` 非空时启用 PostgreSQL 事件日志（`door_events`、`detections` 两张表，异步写入，缓冲 256 条）

### 模型训练

```bash
pip install ultralytics
# 手动从 Ultralytics release 页面下载 yolov8n.pt（被 gitignore）
python model-training/train.py
python model-training/export_onnx_int8.py    # 输出 runs/detect/train/weights/best_int8.onnx
python model-training/validate_onnx_int8.py  # 用 model-val-1.jpeg 跑一遍
python model-training/detect.py --model runs/detect/train/weights/best_int8.onnx --warmup
```

`train.py` 默认 `device='cpu'`；如有 GPU 改为 `'mps'`（Apple Silicon）或 `'0'`。

## 阈值

- **室外触发**：`OPEN_CAMERA_DISTANCE_CM = 10`（`src/esp32out.ino`）—— 同时把控进门触发（拍照 + `0x1`）和 `0xA` 通过事件。
- **室内触发**：`INDOOR_OPEN_DISTANCE_CM = 10`（`src/esp32in.ino`）—— 同时把控出门触发（`0x9`）和 `0x7` 通过事件。

雷达距离判断是整数除法：`(distanceMM / 10) <= THRESHOLD`，即 0–10 cm 范围都触发。

## 踩过的坑（节选，完整见 `AGENTS.md`）

- ESP32-S3-CAM 配 OV2640；每侧只用一块 VL53L1X（默认地址 `0x29`），GPIO39（`PIN_XSHUT2`）悬空。以后加第二块雷达再做 XSHUT 分配。
- 两个 .ino 文件顶部硬编码了 `WIFI_SSID` / `WIFI_PASS` / `SERVER_HOST`，烧录前要改。
- 光照传感器在 GPIO3 (ADC1_CH2)。早期误写成 GPIO2（I2C SCL），会把总线短路——别回滚。
- 室内固件每猫时段只拍一帧（仅上升沿），拍完立刻 deinit 摄像头；雷达恢复安静后才允许下一次 `0x9`。
- 室外固件每轮询 tick 都会重发 `0xA`；服务端靠 `state + expectedPassage` 去抖。
- `--save-dir` 写入 `<save-dir>/outdoor/` 和 `<save-dir>/indoor/`，目录按需创建，轮转/清理由调用方负责。
- 没有测试、没有 CI、没有 lint 配置。

## 许可

代码按原样提供。数据集（Roboflow `cat-link` v1）为 CC BY 4.0——见 `model-training/README.md`。