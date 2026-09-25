// hightech-cat-door — indoor node
// 流：服务端 0x5/0x6 → 舵机 1（把手下拉）+ 舵机 2（推拉门）；雷达监测通过并上报；
// 雷达首次触发时拍单帧 JPEG 通过 0x9 上报（出门触发）。
//
// 系统流程：
//   1. 连接服务端（:1234），建立心跳。
//   2. 接收 0x5 开门：舵机 1 → 60° 下拉把手；舵机 2 → 90° 推门至 15cm 缝。
//   3. 上报 0x7 DoorStateOpen。
//   4. 持续监测雷达；雷达在 INDOOR_OPEN_DISTANCE_CM 内（连续 RADAR_DEBOUNCE_N 次相同读数才视为有效，防抖动）：
//        a) 本次会话首次触发 → 初始化摄像头，拍单帧 JPEG，通过 0x9 上报（服务端跳过识别直接开门）。
//        b) 上报 0x7 DoorStatePassage（仅一次，passageReported 去重）。
//   5. 接收 0x6 关门：舵机 1 → 0°；舵机 2 → 0°。
//   6. 上报 0x7 DoorStateClosed。
//   7. 雷达离开范围 → 重置 catSessionActive 与 passageReported。

#include <WiFi.h>
#include <Wire.h>
#include <VL53L1X.h>
#include "esp_camera.h"

// ============= 用户配置 =============
#define WIFI_SSID       "YOUR_SSID"
#define WIFI_PASS       "YOUR_PASS"
#define SERVER_HOST     "192.168.1.100"
#define SERVER_PORT     1234   // 与 esp32out 共用同一端口；身份由 0x8 register 帧声明
// ====================================

// ============= GPIO =============
#define PIN_I2C_SDA  1
#define PIN_I2C_SCL  2
#define PIN_XSHUT1   38
#define PIN_SERVO1   14   // 门把手
#define PIN_SERVO2   21   // 推拉门

// 相机 pinout（与 esp32out 一致；GOOUUU ESP32-S3-CAM 兼容）
#define CAM_PIN_PWDN    -1
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    15
#define CAM_PIN_SIOD    4
#define CAM_PIN_SIOC    5
#define CAM_PIN_D0      11
#define CAM_PIN_D1      9
#define CAM_PIN_D2      8
#define CAM_PIN_D3      10
#define CAM_PIN_D4      12
#define CAM_PIN_D5      18
#define CAM_PIN_D6      17
#define CAM_PIN_D7      16
#define CAM_PIN_VSYNC   6
#define CAM_PIN_HREF    7
#define CAM_PIN_PCLK    13
// =================================

// ============= 协议常量 =============
#define MAGIC                  0xAA
#define TYPE_HEARTBEAT         0x0
#define TYPE_UPLOAD_AND_DETECT 0x1
#define TYPE_DETECT_RESULT     0x2
#define TYPE_REQUEST_CAPTURE   0x3
#define TYPE_CAPTURE_AND_STORE 0x4
#define TYPE_MANUAL_OPEN       0x5
#define TYPE_MANUAL_CLOSE      0x6
#define TYPE_DOOR_STATE_REPORT 0x7
#define TYPE_REGISTER          0x8
#define TYPE_INDOOR_TRIGGER    0x9
#define TYPE_OUTDOOR_PASSAGE   0xA
#define TYPE_DISCONNECT        0xF

#define ROLE_OUTDOOR           0x0
#define ROLE_INDOOR            0x1

#define DOOR_STATE_CLOSED   0x0
#define DOOR_STATE_OPENING  0x1
#define DOOR_STATE_OPEN     0x2
#define DOOR_STATE_CLOSING  0x3
#define DOOR_STATE_PASSAGE  0x4
// ===================================

// ============= 调参 =============
#define RADAR_ADDR_1            0x29
#define INDOOR_OPEN_DISTANCE_CM 10   // 雷达 < 10cm 视为有猫（出门触发+进门通过检测共用）
#define RADAR_POLL_MS           200
#define RADAR_DEBOUNCE_N        3     // 防抖动：连续 N 次相同读数才翻转为稳定状态（200ms × N）
#define HEARTBEAT_MS            30000
#define WIFI_RECONNECT_MAX_MS   10000
#define PROTOCOL_MAX_BODY       65531 // = 0xFFFF - 4

// 舵机：50Hz，500-2500µs 对应 0-180°
#define SERVO_FREQ              50
#define SERVO_RES               16
#define SERVO_PULSE_MIN_US      500
#define SERVO_PULSE_MAX_US      2500
#define SERVO_PERIOD_US         20000

#define SERVO1_CH               0
#define SERVO2_CH               1

#define SERVO1_HANDLE_CLOSED    0     // 释放把手
#define SERVO1_HANDLE_OPEN      60    // 下拉把手至 60°
#define SERVO2_DOOR_CLOSED      0     // 关门
#define SERVO2_DOOR_OPEN        90    // 推门至 15cm 缝（按现场机械标定）
// =================================

VL53L1X radar1;

static camera_config_t camera_config = {
    .pin_pwdn  = CAM_PIN_PWDN,
    .pin_reset = CAM_PIN_RESET,
    .pin_xclk = CAM_PIN_XCLK,
    .pin_sccb_sda = CAM_PIN_SIOD,
    .pin_sccb_scl = CAM_PIN_SIOC,
    .pin_d7 = CAM_PIN_D7, .pin_d6 = CAM_PIN_D6,
    .pin_d5 = CAM_PIN_D5, .pin_d4 = CAM_PIN_D4,
    .pin_d3 = CAM_PIN_D3, .pin_d2 = CAM_PIN_D2,
    .pin_d1 = CAM_PIN_D1, .pin_d0 = CAM_PIN_D0,
    .pin_vsync = CAM_PIN_VSYNC,
    .pin_href = CAM_PIN_HREF,
    .pin_pclk = CAM_PIN_PCLK,
    .xclk_freq_hz = 20000000,
    .pixel_format = PIXFORMAT_JPEG,
    .frame_size = FRAMESIZE_VGA,
    .jpeg_quality = 15,
    .fb_count = 2,
    .fb_location = CAMERA_FB_IN_PSRAM,
    .grab_mode = CAMERA_GRAB_LATEST,
};

WiFiClient tcp;
uint32_t lastHeartbeat = 0;
uint32_t lastPoll      = 0;
bool     passageReported   = false;
bool     catSessionActive  = false;

bool     radarRawLast       = false;
uint8_t  radarStreak        = 0;
bool     radarStable        = false;

void initRadars() {
    pinMode(PIN_XSHUT1, OUTPUT);
    digitalWrite(PIN_XSHUT1, LOW);
    delay(10);
    digitalWrite(PIN_XSHUT1, HIGH);
    delay(10);

    if (radar1.init()) {
        Serial.println("radar init failed");
        while (1) delay(1000);
    }
    radar1.setDistanceMode(VL53L1X::Short);
    radar1.setMeasurementTimingBudget(50000);
    radar1.startContinuous(200);

    Serial.printf("radar ok (0x%02X)\n", RADAR_ADDR_1);
}

uint32_t angleToDuty(int angle) {
    if (angle < 0) angle = 0;
    if (angle > 180) angle = 180;
    uint32_t pulseUs = SERVO_PULSE_MIN_US +
        (uint32_t)(SERVO_PULSE_MAX_US - SERVO_PULSE_MIN_US) * angle / 180;
    return (uint32_t)((uint64_t)pulseUs * 65535ULL / SERVO_PERIOD_US);
}

void initServos() {
    ledcSetup(SERVO1_CH, SERVO_FREQ, SERVO_RES);
    ledcAttachPin(PIN_SERVO1, SERVO1_CH);
    ledcSetup(SERVO2_CH, SERVO_FREQ, SERVO_RES);
    ledcAttachPin(PIN_SERVO2, SERVO2_CH);

    ledcWrite(SERVO1_CH, angleToDuty(SERVO1_HANDLE_CLOSED));
    ledcWrite(SERVO2_CH, angleToDuty(SERVO2_DOOR_CLOSED));
    Serial.println("servos at closed position");
}

void openDoor() {
    Serial.println("opening door");
    ledcWrite(SERVO1_CH, angleToDuty(SERVO1_HANDLE_OPEN));
    delay(500);
    ledcWrite(SERVO2_CH, angleToDuty(SERVO2_DOOR_OPEN));
    delay(800);
}

void closeDoor() {
    Serial.println("closing door");
    ledcWrite(SERVO2_CH, angleToDuty(SERVO2_DOOR_CLOSED));
    delay(800);
    ledcWrite(SERVO1_CH, angleToDuty(SERVO1_HANDLE_CLOSED));
    delay(300);
}

bool passageDetectedRaw() {
    uint16_t d = radar1.read();
    bool t = radar1.timeoutOccurred();
    if (t) return false;
    return (d / 10) <= INDOOR_OPEN_DISTANCE_CM;
}

bool passageStable() {
    bool raw = passageDetectedRaw();
    if (raw == radarRawLast) {
        if (radarStreak < 255) radarStreak++;
    } else {
        radarRawLast = raw;
        radarStreak  = 1;
    }
    if (radarStreak >= RADAR_DEBOUNCE_N) {
        radarStable = raw;
    }
    return radarStable;
}

esp_err_t cameraInit() {
    if (CAM_PIN_PWDN != -1) {
        gpio_config_t conf = { .pin_bit_mask = 1ULL << CAM_PIN_PWDN, .mode = GPIO_MODE_OUTPUT };
        gpio_config(&conf);
        gpio_set_level((gpio_num_t)CAM_PIN_PWDN, 0);
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
        ESP_LOGE("CAM", "Camera Init Failed: 0x%x", err);
        return err;
    }
    sensor_t* s = esp_camera_sensor_get();
    s->set_vflip(s, 1);
    s->set_hmirror(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
    return ESP_OK;
}

void captureAndSendIndoorTrigger() {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("camera capture failed");
        return;
    }
    if (fb->len > PROTOCOL_MAX_BODY) {
        Serial.printf("frame too large (%u), dropping\n", fb->len);
        esp_camera_fb_return(fb);
        return;
    }
    sendFrame(TYPE_INDOOR_TRIGGER, fb->buf, fb->len);
    esp_camera_fb_return(fb);
}

size_t writeAll(const uint8_t* buf, size_t len) {
    size_t written = 0;
    uint32_t start = millis();
    while (written < len) {
        size_t n = tcp.write(buf + written, len - written);
        if (n == 0) {
            if (millis() - start > 5000) return written;
            delay(10);
            continue;
        }
        written += n;
        start = millis();
    }
    return written;
}

void sendFrame(uint8_t type, const uint8_t* body, size_t bodyLen) {
    uint8_t hdr[4];
    uint16_t total = 4 + bodyLen;
    hdr[0] = MAGIC;
    hdr[1] = type;
    hdr[2] = total & 0xFF;
    hdr[3] = (total >> 8) & 0xFF;
    if (writeAll(hdr, 4) != 4) return;
    if (bodyLen > 0) writeAll(body, bodyLen);
}

void reportDoorState(uint8_t state) {
    sendFrame(TYPE_DOOR_STATE_REPORT, &state, 1);
    Serial.printf("door state -> 0x%02X\n", state);
}

void sendRegister(uint8_t role) {
    sendFrame(TYPE_REGISTER, &role, 1);
    Serial.printf("register sent: role=0x%02X\n", role);
}

bool readFrame(uint8_t& type, uint8_t* body, size_t bodyCap, size_t& bodyLen) {
    uint8_t hdr[4];
    if (tcp.readBytes(hdr, 4) != 4) return false;
    if (hdr[0] != MAGIC) return false;
    uint16_t total = hdr[2] | (hdr[3] << 8);
    if (total < 4) return false;
    bodyLen = total - 4;
    if (bodyLen > bodyCap) return false;
    if (bodyLen > 0 && tcp.readBytes(body, bodyLen) != (int)bodyLen) return false;
    type = hdr[1];
    return true;
}

void ensureConnection() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi lost, reconnecting");
        WiFi.reconnect();
        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_RECONNECT_MAX_MS) {
            delay(500);
        }
        if (WiFi.status() != WL_CONNECTED) return;
    }
    if (!tcp.connected()) {
        Serial.printf("connecting to %s:%d\n", SERVER_HOST, SERVER_PORT);
        if (!tcp.connect(SERVER_HOST, SERVER_PORT)) {
            Serial.println("TCP connect failed");
            return;
        }
        Serial.println("TCP connected");
        sendRegister(ROLE_INDOOR);
        lastHeartbeat = millis();
        reportDoorState(DOOR_STATE_CLOSED);
    }
}

void handleServerFrame(uint8_t type, const uint8_t* body, size_t bodyLen) {
    switch (type) {
        case TYPE_HEARTBEAT:
            return;
        case TYPE_MANUAL_OPEN:
            Serial.println("server: manual open");
            passageReported = false;
            reportDoorState(DOOR_STATE_OPENING);
            openDoor();
            reportDoorState(DOOR_STATE_OPEN);
            return;
        case TYPE_MANUAL_CLOSE:
            Serial.println("server: manual close");
            closeDoor();
            reportDoorState(DOOR_STATE_CLOSED);
            return;
        case TYPE_DISCONNECT:
            Serial.println("server: disconnect");
            tcp.stop();
            return;
        default:
            Serial.printf("server: unhandled frame 0x%02X\n", type);
            return;
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("WiFi");
    uint32_t wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED) {
        Serial.print(".");
        if (millis() - wifiStart > 30000) {
            Serial.println(" timeout");
            ESP.restart();
        }
        delay(500);
    }
    Serial.printf(" ok (%s)\n", WiFi.localIP().toString().c_str());

    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    initRadars();
    initServos();

    ensureConnection();
    Serial.println("setup done");
}

void loop() {
    ensureConnection();
    if (!tcp.connected()) {
        delay(1000);
        return;
    }

    uint32_t now = millis();

    if (now - lastHeartbeat >= HEARTBEAT_MS) {
        sendFrame(TYPE_HEARTBEAT, nullptr, 0);
        lastHeartbeat = now;
    }

    int avail = tcp.available();
    if (avail >= 4) {
        uint8_t type;
        uint8_t body[64];
        size_t  bodyLen = 0;
        if (!readFrame(type, body, sizeof(body), bodyLen)) {
            Serial.println("read frame failed; closing");
            tcp.stop();
            return;
        }
        handleServerFrame(type, body, bodyLen);
    }

    bool cat = passageStable();
    if (cat) {
        if (!catSessionActive) {
            catSessionActive = true;
            Serial.println("indoor radar triggered: sending 0x9");
            if (cameraInit() == ESP_OK) {
                captureAndSendIndoorTrigger();
                esp_camera_deinit();
            } else {
                Serial.println("camera init failed, skipping 0x9 frame");
            }
        }
        if (!passageReported) {
            passageReported = true;
            reportDoorState(DOOR_STATE_PASSAGE);
        }
    } else {
        catSessionActive = false;
        passageReported = false;
    }

    delay(RADAR_POLL_MS);
}
