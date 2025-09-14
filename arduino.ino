#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <ArduinoWebsockets.h>

#include "pin_config.h"
#include "Arduino_DriveBus_Library.h"
#include "Arduino_GFX_Library.h"
#include "HWCDC.h"

using namespace websockets;

// ===================== NETWORK CONFIG =====================
const char* ssid     = "EE-A2563P";
const char* password = "mthj19051986-";
String serverIp      = "";   // entered on-screen (e.g. "192.168.0.83:81")

// ===================== GLOBAL OBJECTS =====================
HWCDC USBSerial;
WebsocketsClient client;

enum AppState { STATE_INPUT_IP, STATE_CONNECTING_WIFI, STATE_CONNECTING_WS, STATE_RUNNING };
AppState currentState = STATE_INPUT_IP;

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3
);
Arduino_GFX *gfx = new Arduino_CO5300(
    bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 22, 0, 0, 0
);

uint8_t* frameBuffer = nullptr;

// retry timers
unsigned long wifiRetryStart = 0;
unsigned long wsRetryStart   = 0;
const unsigned long WIFI_RETRY_INTERVAL = 4000;
const unsigned long WS_RETRY_INTERVAL   = 4000;

// ===================== TOUCH SETUP =====================
#include <memory>
std::shared_ptr<Arduino_IIC_DriveBus> IIC_Bus =
  std::make_shared<Arduino_HWIIC>(IIC_SDA, IIC_SCL, &Wire);

void TouchIRQ(void);
std::unique_ptr<Arduino_IIC> Touch(new Arduino_FT3x68(
  IIC_Bus, FT3168_DEVICE_ADDRESS,
  DRIVEBUS_DEFAULT_VALUE, TP_INT, TouchIRQ));

void TouchIRQ() { Touch->IIC_Interrupt_Flag = true; }

enum TouchState { IDLE, TOUCH, HOLD };
TouchState touchState = IDLE;
unsigned long touchStart = 0;
const unsigned long holdTime = 500;
const unsigned long tapMax   = 200;

// recovery
unsigned long lastTouchInitAttempt = 0;
const unsigned long TOUCH_RETRY_INTERVAL = 3000;

// ===================== KEYBOARD CONFIG =====================
struct Key {
  const char *label;
  int16_t x, y, w, h;
};
Key keys[] = {
  {"1",  10, 140, 60, 60}, {"2",  80, 140, 60, 60}, {"3", 150, 140, 60, 60},
  {"4",  10, 210, 60, 60}, {"5",  80, 210, 60, 60}, {"6", 150, 210, 60, 60},
  {"7",  10, 280, 60, 60}, {"8",  80, 280, 60, 60}, {"9", 150, 280, 60, 60},
  {".",  10, 350, 60, 60}, {"0",  80, 350, 60, 60}, {":", 150, 350, 60, 60},
  {"<-",220, 140, 80, 130}, {"OK",220, 280, 80, 130}
};
String inputBuffer = "";
const unsigned long KEY_DEBOUNCE = 200;
unsigned long lastKeyPress = 0;

// ===================== PROTOTYPES =====================
void displayMessage(String msg);
void connectWiFiNonBlocking();
void connectWebsocketNonBlocking();
void onWebsocketMessage(WebsocketsMessage message);
void onWebsocketEvent(WebsocketsEvent event, String data);
void processTouch();
void sendTouchJSON(const char *type, int32_t x, int32_t y);
void drawKeyboard();
void handleKeyboardTouch();
bool touchInKey(const Key &k, int32_t x, int32_t y);
void recoverTouchIfNeeded();

// ===================== DUAL CORE DRAWING =====================
// Heavy frame drawing will run on Core 1
QueueHandle_t frameQueue;
TaskHandle_t  frameDrawTaskHandle;

void frameDrawTask(void *pv) {
    for (;;) {
        uint8_t *buf = nullptr;
        if (xQueueReceive(frameQueue, &buf, portMAX_DELAY) == pdTRUE) {
            if (buf) {
                gfx->draw16bitRGBBitmap(0, 0, (uint16_t*)buf, 410, 502);
                free(buf);
            }
        }
    }
}

// ===================== SETUP =====================
void setup() {
    USBSerial.begin(115200);
    Wire.begin(IIC_SDA, IIC_SCL);

    if (!gfx->begin()) {
        USBSerial.println("gfx->begin() failed!");
    }

    frameBuffer = (uint8_t*)ps_malloc(410 * 502 * 2);
    if (!frameBuffer) {
        USBSerial.println("Failed to allocate PSRAM for frame buffer!");
        while (1);
    }

    recoverTouchIfNeeded();

    gfx->fillScreen(BLACK);
    drawKeyboard();

    // Create queue and task for drawing frames on Core 1
    frameQueue = xQueueCreate(2, sizeof(uint8_t*));
    xTaskCreatePinnedToCore(frameDrawTask,
                            "FrameDraw",
                            8192,
                            NULL,
                            1,
                            &frameDrawTaskHandle,
                            1); // <-- Core 1
}

// ===================== LOOP =====================
void loop() {
    if (currentState == STATE_INPUT_IP) {
        handleKeyboardTouch();
    } else if (currentState == STATE_CONNECTING_WIFI) {
        connectWiFiNonBlocking();
    } else if (currentState == STATE_CONNECTING_WS) {
        connectWebsocketNonBlocking();
    } else if (currentState == STATE_RUNNING) {
        client.poll();
        processTouch();
    }

    recoverTouchIfNeeded();
    delay(1);
}

// ===================== TOUCH RECOVERY =====================
void recoverTouchIfNeeded() {
    static bool touchReady = false;

    if (!touchReady && millis() - lastTouchInitAttempt > TOUCH_RETRY_INTERVAL) {
        lastTouchInitAttempt = millis();
        USBSerial.println("Attempting touch init...");
        Wire.end();
        delay(50);
        Wire.begin(IIC_SDA, IIC_SCL);
        if (Touch->begin()) {
            Touch->IIC_Write_Device_State(
              Touch->Arduino_IIC_Touch::Device::TOUCH_POWER_MODE,
              Touch->Arduino_IIC_Touch::Device_Mode::TOUCH_POWER_MONITOR);
            USBSerial.println("Touch ready");
            touchReady = true;
        } else {
            USBSerial.println("Touch init fail (will retry)");
        }
    }

    if (touchReady) {
        Wire.beginTransmission(FT3168_DEVICE_ADDRESS);
        if (Wire.endTransmission() != 0) {
            USBSerial.println("I2C NACK detected -> forcing reinit");
            touchReady = false;
        }
    }
}

// ===================== VIRTUAL KEYBOARD =====================
void drawKeyboard() {
    gfx->fillScreen(BLACK);
    gfx->setTextColor(WHITE);
    gfx->setTextSize(2);
    gfx->setCursor(10, 20);
    gfx->print("Enter IP:Port");
    gfx->drawRect(10, 60, 290, 50, WHITE);
    gfx->setCursor(15, 75);
    gfx->print(inputBuffer);
    for (auto &k : keys) {
        gfx->drawRect(k.x, k.y, k.w, k.h, WHITE);
        gfx->setCursor(k.x + 15, k.y + 20);
        gfx->setTextSize(3);
        gfx->print(k.label);
    }
}

bool touchInKey(const Key &k, int32_t x, int32_t y) {
    return (x >= k.x && x <= k.x + k.w && y >= k.y && y <= k.y + k.h);
}

void handleKeyboardTouch() {
    if (!Touch->IIC_Interrupt_Flag) return;
    Touch->IIC_Interrupt_Flag = false;

    int32_t x = Touch->IIC_Read_Device_Value(
        Touch->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
    int32_t y = Touch->IIC_Read_Device_Value(
        Touch->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);

    if (millis() - lastKeyPress < KEY_DEBOUNCE) return;
    lastKeyPress = millis();

    for (auto &k : keys) {
        if (touchInKey(k, x, y)) {
            String lbl = k.label;
            if (lbl == "<-") {
                if (inputBuffer.length() > 0) inputBuffer.remove(inputBuffer.length()-1);
            } else if (lbl == "OK") {
                if (inputBuffer.length() > 0) {
                    serverIp = inputBuffer;
                    USBSerial.print("Server IP/Port set to: ");
                    USBSerial.println(serverIp);
                    gfx->fillScreen(BLACK);
                    currentState = STATE_CONNECTING_WIFI;
                }
            } else {
                if (inputBuffer.length() < 40) inputBuffer += lbl;
            }
            drawKeyboard();
            break;
        }
    }
}

// ===================== WEBSOCKET =====================
void onWebsocketMessage(WebsocketsMessage message) {
    if (message.type() == MessageType::Binary &&
        message.length() == 410*502*2) {
        // Make a copy and send to Core 1 for drawing
        uint8_t *copy = (uint8_t*)ps_malloc(410 * 502 * 2);
        if (!copy) return;
        memcpy(copy, message.c_str(), 410*502*2);
        xQueueSend(frameQueue, &copy, 0);
    }
}

void onWebsocketEvent(WebsocketsEvent event, String data) {
    if (event == WebsocketsEvent::ConnectionClosed) {
        USBSerial.println("Connection closed. Reconnecting...");
        displayMessage("Server\nDisconnected.\nReconnecting...");
        currentState = STATE_CONNECTING_WS;
        wsRetryStart = millis();
    } else if (event == WebsocketsEvent::ConnectionOpened) {
        USBSerial.println("Connection opened. Waiting for image...");
        displayMessage("Connected!\nWaiting for\nimage...");
    }
}

// ===================== TOUCH PROCESS =====================
void processTouch() {
    if (Touch->IIC_Interrupt_Flag) {
        Touch->IIC_Interrupt_Flag = false;
        int32_t x = Touch->IIC_Read_Device_Value(
            Touch->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
        int32_t y = Touch->IIC_Read_Device_Value(
            Touch->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);

        if (touchState == IDLE) {
            touchState = TOUCH;
            touchStart = millis();
            USBSerial.printf("TOUCH @ %d,%d\n", x, y);
            sendTouchJSON("touch", x, y);
        } else if (touchState == TOUCH) {
            if (millis() - touchStart > holdTime) {
                touchState = HOLD;
                USBSerial.printf("HOLD @ %d,%d\n", x, y);
                sendTouchJSON("hold", x, y);
            }
        }
    } else {
        if (touchState == TOUCH || touchState == HOLD) {
            unsigned long duration = millis() - touchStart;
            if (duration < tapMax) {
                USBSerial.println("TAP");
                sendTouchJSON("tap", 0, 0);
            } else {
                USBSerial.println("RELEASE");
                sendTouchJSON("release", 0, 0);
            }
            touchState = IDLE;
        }
    }
}

// ===================== JSON SENDER =====================
void sendTouchJSON(const char *type, int32_t x, int32_t y) {
    if (currentState != STATE_RUNNING) return;
    String json = "{\"event\":\"";
    json += type;
    json += "\",\"x\":";
    json += x;
    json += ",\"y\":";
    json += y;
    json += "}";
    if (json.length() < 128) {
        client.send(json);
    }
}

// ===================== UTILS =====================
void displayMessage(String msg) {
    gfx->fillScreen(BLACK);
    gfx->setTextColor(WHITE);
    gfx->setTextSize(3);
    gfx->setCursor(20, 150);
    gfx->println(msg);
    USBSerial.println(msg);
}

void connectWiFiNonBlocking() {
    static bool started = false;
    if (!started) {
        displayMessage("Connecting to\nWiFi...");
        WiFi.begin(ssid, password);
        wifiRetryStart = millis();
        started = true;
    }
    if (WiFi.status() == WL_CONNECTED) {
        USBSerial.println("\nWiFi connected.");
        USBSerial.print("IP address: ");
        USBSerial.println(WiFi.localIP());
        currentState = STATE_CONNECTING_WS;
        started = false;
    } else if (millis() - wifiRetryStart >= WIFI_RETRY_INTERVAL) {
        USBSerial.print(".");
        wifiRetryStart = millis();
    }
}

void connectWebsocketNonBlocking() {
    static bool started = false;
    if (!started) {
        displayMessage("Connecting to\nServer...");
        client.onMessage(onWebsocketMessage);
        client.onEvent(onWebsocketEvent);
        wsRetryStart = millis();
        started = true;
    }
    if (!client.available()) {
        if (millis() - wsRetryStart >= WS_RETRY_INTERVAL) {
            USBSerial.println("Attempting WS connection...");
            int colon = serverIp.indexOf(':');
            String ip = serverIp;
            int port = 81;
            if (colon > 0) {
                ip = serverIp.substring(0, colon);
                port = serverIp.substring(colon + 1).toInt();
            }
            bool connected = client.connect(ip, port, "/");
            if (connected) {
                currentState = STATE_RUNNING;
            }
            wsRetryStart = millis();
        }
    }
}
