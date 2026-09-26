// hightech-cat-door — outdoor node
// 流：激光雷达 / 光照传感器 / 红外补光灯 / 摄像头 → 私有 TCP 协议 → 服务端
//
// 系统流程：
//   1. 雷达每 200ms 检测猫是否在 10cm 内；连续 RADAR_DEBOUNCE_N 次相同读数才翻转为稳定状态（防抖动）。
//   2. 检测到猫 → 读取光照传感器；若黑暗则开启红外补光灯。
//   3. 初始化摄像头；按 4FPS 抓拍 JPEG，通过 TCP 0x1 帧上报服务端。
//   4. 读取 0x2 响应（仅日志，服务端已直接命令门内侧开门）。
//   5. 同步每 200ms 上报一次 0xA 门外通过事件，由服务端按门状态过滤。
//   6. 雷达不再触发 → 关闭摄像头、红外补光灯。

#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_VL53L1X.h>
#include "esp_camera.h"

// ============= 用户配置 =============
#define WIFI_SSID       "YOUR_SSID"
#define WIFI_PASS       "YOUR_PASS"
#define SERVER_HOST     "192.168.1.100"
#define SERVER_PORT     1234   // 与 esp32in 共用同一端口；身份由 0x8 register 帧声明
// ====================================

// ============= GPIO =============
#define PIN_I2C_SDA      1
#define PIN_I2C_SCL      2
#define PIN_XSHUT1       38
#define PIN_LIGHT_SENSOR 3
#define PIN_IR_LIGHT     47

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

#define RADAR_ADDR_1    0x29
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
// ===================================

// ============= 调参 =============
#define BRIGHT_BOUND            2500
#define OPEN_CAMERA_DISTANCE_CM 10
#define LIGHT_SAMPLES           8
#define RADAR_POLL_MS           200
#define RADAR_DEBOUNCE_N        3     // 防抖动：连续 N 次相同读数才翻转为稳定状态（200ms × N）
#define CAMERA_FRAME_MS         250   // 4 FPS
#define HEARTBEAT_MS            30000
#define WIFI_RECONNECT_MAX_MS   10000
#define PROTOCOL_MAX_BODY       65531 // = 0xFFFF - 4
// =================================

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
    .frame_size = FRAMESIZE_VGA,    // 640x480; 协议 body 上限 ~65KB
    .jpeg_quality = 15,             // 平衡画质与体积
    .fb_count = 2,
    .fb_location = CAMERA_FB_IN_PSRAM,
    .grab_mode = CAMERA_GRAB_LATEST,
};

Adafruit_VL53L1X radar1;

WiFiClient tcp;
uint32_t lastHeartbeat = 0;
uint32_t lastFrame     = 0;
uint32_t lastPoll      = 0;
bool     capturing     = false;

bool     radarRawLast  = false;
uint8_t  radarStreak   = 0;
bool     radarStable   = false;

void initRadars() {
    pinMode(PIN_XSHUT1, OUTPUT);
    digitalWrite(PIN_XSHUT1, LOW);
    delay(10);
    digitalWrite(PIN_XSHUT1, HIGH);
    delay(10);

    if (!radar1.begin(RADAR_ADDR_1)) {
        Serial.printf("radar init failed (status=%d)\n", (int)radar1.vl_status);
        while (1) delay(1000);
    }
    radar1.VL53L1X_SetDistanceMode(1);
    radar1.setTimingBudget(50);
    radar1.VL53L1X_SetInterMeasurementInMs(200);
    radar1.startRanging();

    Serial.printf("radar ok (0x%02X)\n", RADAR_ADDR_1);
}

bool catPresentRaw() {
    int16_t d = radar1.distance();
    if (d < 0) return false;
    return (d / 10) <= OPEN_CAMERA_DISTANCE_CM;
}

bool catPresentStable() {
    bool raw = catPresentRaw();
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

bool isBright() {
    long sum = 0;
    for (int i = 0; i < LIGHT_SAMPLES; i++) {
        sum += analogRead(PIN_LIGHT_SENSOR);
        delay(5);
    }
    return (sum / LIGHT_SAMPLES) >= BRIGHT_BOUND;
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

size_t writeAll(const uint8_t* buf, size_t len) {
    size_t written = 0;
    uint32_t start = millis();
    while (written < len) {
        size_t n = tcp.write(buf + written, len - written);
        if (n == 0) {
            if (millis() - start > 5000) return written; // 5s 超时
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

void sendHeartbeat() { sendFrame(TYPE_HEARTBEAT, nullptr, 0); }

void sendOutdoorPassage() { sendFrame(TYPE_OUTDOOR_PASSAGE, nullptr, 0); }

void sendRegister(uint8_t role) {
    sendFrame(TYPE_REGISTER, &role, 1);
    Serial.printf("register sent: role=0x%02X\n", role);
}

void captureAndSend() {
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

    sendFrame(TYPE_UPLOAD_AND_DETECT, fb->buf, fb->len);
    esp_camera_fb_return(fb);

    // 等待 0x2 响应（5s 超时）；结果仅日志，服务端已独立驱动门内侧
    uint32_t deadline = millis() + 5000;
    while (tcp.connected() && millis() < deadline) {
        uint8_t respType;
        uint8_t respBody[16];
        size_t  respLen = 0;
        int avail = tcp.available();
        if (avail < 4) {
            delay(20);
            continue;
        }
        if (!readFrame(respType, respBody, sizeof(respBody), respLen)) {
            Serial.println("read response failed");
            return;
        }
        if (respType == TYPE_DETECT_RESULT && respLen >= 1) {
            Serial.printf("detect result: %s\n", respBody[0] == 0x01 ? "my cat" : "not cat");
            return;
        }
        if (respType == TYPE_HEARTBEAT) continue;
        Serial.printf("unexpected server frame type=0x%02X len=%u\n", respType, respLen);
        return;
    }
    Serial.println("detect response timeout");
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
        sendRegister(ROLE_OUTDOOR);
        lastHeartbeat = millis();
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

    pinMode(PIN_IR_LIGHT, OUTPUT);
    digitalWrite(PIN_IR_LIGHT, LOW);

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
        sendHeartbeat();
        lastHeartbeat = now;
    }

    bool cat = catPresentStable();
    if (cat) {
        sendOutdoorPassage();
        if (!capturing) {
            if (!isBright()) {
                digitalWrite(PIN_IR_LIGHT, HIGH);
            }
            if (cameraInit() != ESP_OK) {
                Serial.println("camera init failed, aborting capture session");
                digitalWrite(PIN_IR_LIGHT, LOW);
                return;
            }
            capturing = true;
            lastFrame = 0;
        }
        if (now - lastFrame >= CAMERA_FRAME_MS) {
            captureAndSend();
            lastFrame = now;
        }
    } else if (capturing) {
        Serial.println("cat left radar range, stopping capture");
        capturing = false;
        digitalWrite(PIN_IR_LIGHT, LOW);
        esp_camera_deinit();
    }

    delay(RADAR_POLL_MS);
}
