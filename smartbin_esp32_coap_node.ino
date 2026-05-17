#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <coap-simple.h>

#include <ESP32Servo.h>
#include <Stepper.h>

// ======================================================
// WIFI + COAP CONFIG
// ======================================================

const char* WIFI_SSID = "banh";
const char* WIFI_PASS = "123456789";

// Đổi IP này thành IP máy đang chạy Node.js server
IPAddress SERVER_IP(192, 168, 133, 183);
const int COAP_PORT = 5683;

const char* DEVICE_ID = "smartbin-01";

WiFiUDP udp;
Coap coap(udp);

// ======================================================
// PIN CONFIG
// ======================================================

// HC-SR04
#define TRIG_PIN 18
#define ECHO_PIN 19

// Cảm biến kim loại
#define METAL_PIN 2

// Cảm biến mưa analog
#define RAIN_PIN 0

// Servo
#define SERVO_PIN 3

// ULN2003
#define IN1 4
#define IN2 5
#define IN3 6
#define IN4 7

// ======================================================
// SENSOR CONFIG
// ======================================================

const int TRASH_DISTANCE_CM = 5;
const int RAIN_WET_THRESHOLD = 2500;

const int DISTANCE_SAMPLE_COUNT = 7;

// Khi phát hiện rác thì chờ 3 giây rồi phân loại
const unsigned long DETECT_WAIT_TIME = 3000;

// Nếu cảm biến kim loại phát hiện kim loại trả LOW thì để true
const bool METAL_ACTIVE_LOW = true;

// Gửi telemetry định kỳ để web biết ESP32 còn online
const unsigned long TELEMETRY_INTERVAL = 3000;

// ESP32 hỏi server có lệnh điều khiển mới không
const unsigned long COMMAND_CHECK_INTERVAL = 5000;

// Khoảng cách tối thiểu giữa 2 request CoAP
const unsigned long COAP_GAP_TIME = 800;

// ======================================================
// SERVO CONFIG
// ======================================================

Servo lidServo;

const int SERVO_CLOSE_ANGLE = 180;
const int SERVO_OPEN_ANGLE = 0;

const unsigned long DROP_WAIT_TIME = 1200;
const unsigned long SERVO_CLOSE_TIME = 700;

// ======================================================
// STEPPER CONFIG
// ======================================================

const int STEPS_PER_REVOLUTION = 2048;

Stepper stepperMotor(STEPS_PER_REVOLUTION, IN1, IN3, IN2, IN4);

const int WET_STEPS = STEPS_PER_REVOLUTION / 3;
const int METAL_STEPS = (STEPS_PER_REVOLUTION * 2) / 3;

// ======================================================
// STATE MACHINE
// ======================================================

enum SystemState {
  INIT,
  IDLE,
  DETECTED,
  CLASSIFY,
  ROTATE_TO_BIN,
  OPEN_LID,
  WAIT_DROP,
  CLOSE_LID,
  RETURN_HOME,
  WAIT_CLEAR
};

enum TrashType {
  TRASH_NONE,
  TRASH_DRY,
  TRASH_WET,
  TRASH_METAL
};

SystemState currentState = INIT;
TrashType currentTrash = TRASH_NONE;

unsigned long stateStartTime = 0;
unsigned long lastTelemetryTime = 0;
unsigned long lastCommandCheckTime = 0;
unsigned long lastCoapSendTime = 0;

int targetSteps = 0;
bool autoMode = true;

// ======================================================
// LAST SENSOR VALUES
// ======================================================

long lastDistance = 999;
int lastRainValue = 0;
bool lastMetalDetected = false;
bool lastTrashDetected = false;

// ======================================================
// HELPER
// ======================================================

String getStateName(SystemState state) {
  switch (state) {
    case INIT: return "INIT";
    case IDLE: return "IDLE";
    case DETECTED: return "DETECTED";
    case CLASSIFY: return "CLASSIFY";
    case ROTATE_TO_BIN: return "ROTATE_TO_BIN";
    case OPEN_LID: return "OPEN_LID";
    case WAIT_DROP: return "WAIT_DROP";
    case CLOSE_LID: return "CLOSE_LID";
    case RETURN_HOME: return "RETURN_HOME";
    case WAIT_CLEAR: return "WAIT_CLEAR";
    default: return "UNKNOWN";
  }
}

String getTrashName(TrashType type) {
  switch (type) {
    case TRASH_DRY: return "dry";
    case TRASH_WET: return "wet";
    case TRASH_METAL: return "metal";
    default: return "none";
  }
}

int getStepperAngle(TrashType type) {
  switch (type) {
    case TRASH_WET:
      return 120;
    case TRASH_METAL:
      return 240;
    case TRASH_DRY:
    default:
      return 0;
  }
}

// ======================================================
// COAP SEND FUNCTIONS
// ======================================================

bool canSendCoap() {
  return millis() - lastCoapSendTime >= COAP_GAP_TIME;
}

void markCoapSent() {
  lastCoapSendTime = millis();
}

bool sendJsonPut(const char* path, JsonDocument& doc) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[CoAP] WiFi not connected, skip PUT");
    return false;
  }

  char payload[192];
  size_t len = serializeJson(doc, payload, sizeof(payload));

  if (len == 0) {
    Serial.println("[CoAP PUT] Serialize failed");
    return false;
  }

  if (len >= sizeof(payload) - 1) {
    Serial.println("[CoAP PUT] Payload too long, skip");
    return false;
  }

  Serial.print("[CoAP PUT] ");
  Serial.print(path);
  Serial.print(" len=");
  Serial.print(len);
  Serial.print(" ");
  Serial.println(payload);

  // KHÔNG thêm "/" đầu path vì coap-simple có thể tự thêm
  coap.put(SERVER_IP, COAP_PORT, path, payload);

  markCoapSent();
  return true;
}

void sendStateToServer(const char* message) {
  StaticJsonDocument<160> doc;

  // key ngắn:
  // s=state, tt=trash_type, d=distance, r=rain, m=metal, t=trash_detected, a=auto, msg=message
  doc["s"] = getStateName(currentState);
  doc["tt"] = getTrashName(currentTrash);
  doc["d"] = lastDistance;
  doc["r"] = lastRainValue;
  doc["m"] = lastMetalDetected ? 1 : 0;
  doc["t"] = lastTrashDetected ? 1 : 0;
  doc["a"] = autoMode ? 1 : 0;
  doc["msg"] = message;

  sendJsonPut("bin/state", doc);
}

void sendTelemetryToServer() {
  StaticJsonDocument<128> doc;

  // key ngắn:
  // s=state, d=distance, r=rain, m=metal, t=trash_detected, a=auto
  doc["s"] = getStateName(currentState);
  doc["d"] = lastDistance;
  doc["r"] = lastRainValue;
  doc["m"] = lastMetalDetected ? 1 : 0;
  doc["t"] = lastTrashDetected ? 1 : 0;
  doc["a"] = autoMode ? 1 : 0;

  sendJsonPut("bin/telemetry", doc);
}

void sendClassifyToServer() {
  StaticJsonDocument<160> doc;

  // key ngắn:
  // ty=type, s=state, d=distance, r=rain, m=metal, ang=stepper_angle, sv=servo
  doc["ty"] = getTrashName(currentTrash);
  doc["s"] = getStateName(currentState);
  doc["d"] = lastDistance;
  doc["r"] = lastRainValue;
  doc["m"] = lastMetalDetected ? 1 : 0;
  doc["ang"] = getStepperAngle(currentTrash);
  doc["sv"] = "oc"; // open→closed

  sendJsonPut("bin/classify", doc);
}

void sendCommandAck(const String& id, const String& cmd, const String& status, const String& msg = "") {
  StaticJsonDocument<160> doc;

  // cid=command id, cmd=command, st=status, msg=message
  doc["cid"] = id;
  doc["cmd"] = cmd;
  doc["st"] = status;
  doc["msg"] = msg;

  sendJsonPut("bin/command_ack", doc);
}

bool checkCommandFromServer() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[CoAP GET] WiFi not connected, skip");
    return false;
  }

  if (!canSendCoap()) {
    Serial.println("[CoAP GET] Skip busy");
    return false;
  }

  Serial.println("[CoAP GET] bin/command");

  // KHÔNG thêm "/" đầu path vì coap-simple có thể tự thêm
  coap.get(SERVER_IP, COAP_PORT, "bin/command");

  markCoapSent();
  return true;
}

// ======================================================
// STATE CHANGE
// ======================================================

void changeState(SystemState newState) {
  currentState = newState;
  stateStartTime = millis();

  Serial.print("State changed to: ");
  Serial.println(getStateName(newState));

  sendStateToServer("State changed");
}

// ======================================================
// SENSOR FUNCTIONS
// ======================================================

long readDistanceCM() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);

  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);

  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000);

  if (duration == 0) {
    return -1;
  }

  long distance = duration * 0.034 / 2;
  return distance;
}

void sortArray(long arr[], int size) {
  for (int i = 0; i < size - 1; i++) {
    for (int j = i + 1; j < size; j++) {
      if (arr[i] > arr[j]) {
        long temp = arr[i];
        arr[i] = arr[j];
        arr[j] = temp;
      }
    }
  }
}

long readDistanceMedianCM() {
  long samples[DISTANCE_SAMPLE_COUNT];

  for (int i = 0; i < DISTANCE_SAMPLE_COUNT; i++) {
    samples[i] = readDistanceCM();

    if (samples[i] <= 0) {
      samples[i] = 999;
    }

    delay(30);
  }

  sortArray(samples, DISTANCE_SAMPLE_COUNT);

  long median = samples[DISTANCE_SAMPLE_COUNT / 2];

  lastDistance = median;
  return median;
}

bool hasTrashMedian() {
  long distance = readDistanceMedianCM();

  lastTrashDetected = distance > 0 && distance <= TRASH_DISTANCE_CM;
  return lastTrashDetected;
}

bool isMetalTrash() {
  int value = digitalRead(METAL_PIN);

  Serial.print("Metal value: ");
  Serial.println(value);

  if (METAL_ACTIVE_LOW) {
    lastMetalDetected = value == LOW;
  } else {
    lastMetalDetected = value == HIGH;
  }

  return lastMetalDetected;
}

bool isWetTrash() {
  int rainValue = analogRead(RAIN_PIN);

  lastRainValue = rainValue;

  Serial.print("Rain value: ");
  Serial.println(rainValue);

  return rainValue < RAIN_WET_THRESHOLD;
}

// ======================================================
// CLASSIFY
// ======================================================

TrashType classifyTrash() {
  bool metal = isMetalTrash();
  bool wet = isWetTrash();

  if (metal) {
    return TRASH_METAL;
  }

  if (wet) {
    return TRASH_WET;
  }

  return TRASH_DRY;
}

int getTargetSteps(TrashType type) {
  switch (type) {
    case TRASH_DRY:
      return 0;

    case TRASH_WET:
      return WET_STEPS;

    case TRASH_METAL:
      return METAL_STEPS;

    default:
      return 0;
  }
}

// ======================================================
// REMOTE COMMAND
// ======================================================

void handleRemoteCommand(const String& id, const String& cmd) {
  Serial.print("[CMD] ");
  Serial.println(cmd);

  if (cmd == "open_lid") {
    lidServo.write(SERVO_OPEN_ANGLE);
    sendCommandAck(id, cmd, "done", "Servo opened");
    sendStateToServer("Remote command: open_lid");
    return;
  }

  if (cmd == "close_lid") {
    lidServo.write(SERVO_CLOSE_ANGLE);
    sendCommandAck(id, cmd, "done", "Servo closed");
    sendStateToServer("Remote command: close_lid");
    return;
  }

  if (cmd == "auto_on") {
    autoMode = true;
    sendCommandAck(id, cmd, "done", "Auto mode enabled");
    sendStateToServer("Remote command: auto_on");
    return;
  }

  if (cmd == "auto_off") {
    autoMode = false;
    sendCommandAck(id, cmd, "done", "Auto mode disabled");
    sendStateToServer("Remote command: auto_off");
    return;
  }

  if (cmd == "reset_home") {
    if (targetSteps != 0) {
      stepperMotor.step(-targetSteps);
      targetSteps = 0;
    }

    currentTrash = TRASH_NONE;
    sendCommandAck(id, cmd, "done", "Stepper returned home");
    sendStateToServer("Remote command: reset_home");
    return;
  }

  if (cmd == "classify_now") {
    if (currentState == IDLE || currentState == WAIT_CLEAR) {
      changeState(CLASSIFY);
      sendCommandAck(id, cmd, "accepted", "Force classify accepted");
    } else {
      sendCommandAck(id, cmd, "ignored", "System is busy");
    }
    return;
  }

  sendCommandAck(id, cmd, "unknown", "Unknown command");
}

void onCoapResponse(CoapPacket& packet, IPAddress ip, int port) {
  String payload = "";

  for (int i = 0; i < packet.payloadlen; i++) {
    payload += (char)packet.payload[i];
  }

  if (payload.length() == 0) {
    return;
  }

  Serial.print("[CoAP RESPONSE FROM] ");
  Serial.print(ip);
  Serial.print(":");
  Serial.println(port);

  Serial.print("[CoAP RESPONSE] ");
  Serial.println(payload);

  StaticJsonDocument<384> doc;
  DeserializationError error = deserializeJson(doc, payload);

  if (error) {
    Serial.println("[CoAP RESPONSE] Invalid JSON");
    return;
  }

  bool hasCommand = doc["has_command"] | false;

  if (!hasCommand) {
    return;
  }

  String id = doc["id"] | "";
  String cmd = doc["cmd"] | "";

  if (cmd.length() > 0) {
    handleRemoteCommand(id, cmd);
  }
}

// ======================================================
// WIFI
// ======================================================

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("Connecting WiFi");

  unsigned long startAttempt = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 15000) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected. IP: ");
    Serial.println(WiFi.localIP());

    Serial.print("Server IP: ");
    Serial.println(SERVER_IP);

    Serial.print("Gateway IP: ");
    Serial.println(WiFi.gatewayIP());

    Serial.print("Subnet mask: ");
    Serial.println(WiFi.subnetMask());
  } else {
    Serial.println("WiFi connect failed. System still runs offline.");
  }
}

// ======================================================
// SETUP
// ======================================================

void setup() {
  Serial.begin(115200);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  pinMode(METAL_PIN, INPUT_PULLUP);

  lidServo.attach(SERVO_PIN);
  lidServo.write(SERVO_CLOSE_ANGLE);

  stepperMotor.setSpeed(10);

  connectWiFi();

  coap.response(onCoapResponse);
  coap.start();

  changeState(INIT);

  Serial.println("Smart Trash System Ready - CoAP State Machine Mode");
}

// ======================================================
// LOOP STATE MACHINE
// ======================================================

void loop() {
  coap.loop();

  unsigned long now = millis();
  bool sentCoapThisLoop = false;

  // Ưu tiên telemetry trước
  if (now - lastTelemetryTime >= TELEMETRY_INTERVAL) {
    lastTelemetryTime = now;
    sendTelemetryToServer();
    sentCoapThisLoop = true;
  }

  // GET command chỉ chạy khi không vừa gửi PUT, tránh đè request
  if (!sentCoapThisLoop && now - lastCommandCheckTime >= COMMAND_CHECK_INTERVAL) {
    lastCommandCheckTime = now;
    checkCommandFromServer();
    sentCoapThisLoop = true;
  }

  switch (currentState) {
    case INIT:
      currentTrash = TRASH_NONE;
      targetSteps = 0;
      lidServo.write(SERVO_CLOSE_ANGLE);
      lastRainValue = analogRead(RAIN_PIN);
      isMetalTrash();
      readDistanceMedianCM();

      Serial.println("Init done.");
      changeState(IDLE);
      break;

    case IDLE:
      currentTrash = TRASH_NONE;
      targetSteps = 0;

      if (autoMode && hasTrashMedian()) {
        Serial.println("Detected trash. Wait 3 seconds before classify...");
        changeState(DETECTED);
      }
      break;

    case DETECTED:
      if (millis() - stateStartTime >= DETECT_WAIT_TIME) {
        Serial.println("Wait done. Start classify.");
        changeState(CLASSIFY);
      }
      break;

    case CLASSIFY:
      currentTrash = classifyTrash();
      targetSteps = getTargetSteps(currentTrash);

      Serial.print("Trash type: ");
      Serial.println(getTrashName(currentTrash));

      sendStateToServer("Trash classified");

      // Gửi kết quả phân loại ngay để web cập nhật thống kê
      sendClassifyToServer();

      changeState(ROTATE_TO_BIN);
      break;

    case ROTATE_TO_BIN:
      if (targetSteps != 0) {
        Serial.print("Rotate to bin, steps: ");
        Serial.println(targetSteps);

        stepperMotor.step(targetSteps);
      } else {
        Serial.println("Dry trash: stepper stay home");
      }

      changeState(OPEN_LID);
      break;

    case OPEN_LID:
      Serial.println("Open lid");
      lidServo.write(SERVO_OPEN_ANGLE);
      changeState(WAIT_DROP);
      break;

    case WAIT_DROP:
      if (millis() - stateStartTime >= DROP_WAIT_TIME) {
        changeState(CLOSE_LID);
      }
      break;

    case CLOSE_LID:
      Serial.println("Close lid");
      lidServo.write(SERVO_CLOSE_ANGLE);

      // Không gọi sendClassifyToServer ở đây nữa để tránh cộng 2 lần
      changeState(RETURN_HOME);
      break;

    case RETURN_HOME:
      if (millis() - stateStartTime >= SERVO_CLOSE_TIME) {
        if (targetSteps != 0) {
          Serial.print("Return home, steps: ");
          Serial.println(-targetSteps);

          stepperMotor.step(-targetSteps);
        }

        changeState(WAIT_CLEAR);
      }
      break;

    case WAIT_CLEAR:
      if (!hasTrashMedian()) {
        Serial.println("Trash cleared, back to IDLE");
        lastTrashDetected = false;
        changeState(IDLE);
      }
      break;
  }
}