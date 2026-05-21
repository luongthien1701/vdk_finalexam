#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <coap-simple.h>
#include <ESP32Servo.h>
#include <Stepper.h>

// ================= CONFIG =================

const char* WIFI_SSID = "ba";
const char* WIFI_PASS = "123456789";

IPAddress SERVER_IP(192, 168, 175, 183);
const int COAP_PORT = 5683;

#define TRIG_PIN   18
#define ECHO_PIN   19
#define METAL_PIN  2
#define RAIN_PIN   0
#define SERVO_PIN  3

#define IN1 4
#define IN2 5
#define IN3 6
#define IN4 7

const int TRASH_DISTANCE_CM = 5;
const int RAIN_WET_THRESHOLD = 2500;
const int DISTANCE_SAMPLES = 7;

const bool METAL_ACTIVE_LOW = true;

const unsigned long DETECT_WAIT_MS = 3000;
const unsigned long DROP_WAIT_MS = 1200;
const unsigned long SERVO_CLOSE_MS = 700;
const unsigned long TELEMETRY_MS = 3000;
const unsigned long COMMAND_MS = 1000;
const unsigned long COAP_GAP_MS = 800;

const int SERVO_CLOSE = 180;
const int SERVO_OPEN = 0;

const int STEPS_PER_REV = 2048;
const int WET_STEPS = STEPS_PER_REV / 3;
const int METAL_STEPS = (STEPS_PER_REV * 2) / 3;

// ================= OBJECTS =================

WiFiUDP udp;
Coap coap(udp);
Servo lidServo;
Stepper stepperMotor(STEPS_PER_REV, IN1, IN3, IN2, IN4);

// ================= STATE =================

enum State {
  INIT,
  IDLE,
  DETECTED,
  CLASSIFY,
  ROTATE,
  OPEN,
  WAIT_DROP,
  CLOSE,
  HOME,
  WAIT_CLEAR
};

enum Trash {
  NONE,
  DRY,
  WET,
  METAL
};

enum Mode {
  AUTO,
  MANUAL
};

State state = INIT;
Trash trash = NONE;
Mode mode = AUTO;

unsigned long stateTime = 0;
unsigned long lastTelemetry = 0;
unsigned long lastCommand = 0;
unsigned long lastCoap = 0;

int currentSteps = 0;
int targetSteps = 0;

long distanceCm = 999;
int rainValue = 0;
bool metalDetected = false;
bool trashDetected = false;

// ================= NAME =================

const char* stateText(State s) {
  switch (s) {
    case INIT: return "INIT";
    case IDLE: return "IDLE";
    case DETECTED: return "DETECTED";
    case CLASSIFY: return "CLASSIFY";
    case ROTATE: return "ROTATE_TO_BIN";
    case OPEN: return "OPEN_LID";
    case WAIT_DROP: return "WAIT_DROP";
    case CLOSE: return "CLOSE_LID";
    case HOME: return "RETURN_HOME";
    case WAIT_CLEAR: return "WAIT_CLEAR";
    default: return "UNKNOWN";
  }
}

const char* trashText(Trash t) {
  switch (t) {
    case DRY: return "dry";
    case WET: return "wet";
    case METAL: return "metal";
    default: return "none";
  }
}

bool isAuto() {
  return mode == AUTO;
}

// ================= MOTOR =================

void servoOpen() {
  lidServo.write(SERVO_OPEN);
}

void servoClose() {
  lidServo.write(SERVO_CLOSE);
}

void stepperTo(int steps) {
  int delta = steps - currentSteps;

  if (delta != 0) {
    stepperMotor.step(delta);
  }

  currentSteps = steps;
}

void resetHome() {
  servoClose();
  stepperTo(0);
  trash = NONE;
}

int stepsOf(Trash t) {
  if (t == WET) return WET_STEPS;
  if (t == METAL) return METAL_STEPS;
  return 0;
}

int angleOf(Trash t) {
  if (t == WET) return 120;
  if (t == METAL) return 240;
  return 0;
}

// ================= COAP =================

bool coapReady() {
  return millis() - lastCoap >= COAP_GAP_MS;
}

bool sendPut(const char* path, JsonDocument& doc) {
  if (WiFi.status() != WL_CONNECTED) return false;

  char payload[192];
  size_t len = serializeJson(doc, payload, sizeof(payload));

  if (len == 0 || len >= sizeof(payload) - 1) {
    return false;
  }

  coap.put(SERVER_IP, COAP_PORT, path, payload);
  lastCoap = millis();

  return true;
}

void addStatus(JsonDocument& doc) {
  doc["s"] = stateText(state);
  doc["d"] = distanceCm;
  doc["r"] = rainValue;
  doc["m"] = metalDetected ? 1 : 0;
  doc["t"] = trashDetected ? 1 : 0;
  doc["a"] = isAuto() ? 1 : 0;
}

void sendState(const char* msg) {
  StaticJsonDocument<160> doc;

  addStatus(doc);
  doc["tt"] = trashText(trash);
  doc["msg"] = msg;

  sendPut("bin/state", doc);
}

void sendTelemetry() {
  StaticJsonDocument<128> doc;

  addStatus(doc);

  sendPut("bin/telemetry", doc);
}

void sendClassify() {
  StaticJsonDocument<160> doc;

  doc["ty"] = trashText(trash);
  doc["s"] = stateText(state);
  doc["d"] = distanceCm;
  doc["r"] = rainValue;
  doc["m"] = metalDetected ? 1 : 0;
  doc["ang"] = angleOf(trash);
  doc["sv"] = "oc";

  sendPut("bin/classify", doc);
}

void sendAck(const String& id, const String& cmd, const char* status, const char* msg) {
  StaticJsonDocument<160> doc;

  doc["cid"] = id;
  doc["cmd"] = cmd;
  doc["st"] = status;
  doc["msg"] = msg;

  sendPut("bin/command_ack", doc);
}

void getCommand() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!coapReady()) return;

  coap.get(SERVER_IP, COAP_PORT, "bin/command");
  lastCoap = millis();
}

// ================= SENSOR =================

long readDistanceOnce() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);

  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);

  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) return -1;

  return duration * 0.034 / 2;
}

void sortLong(long arr[], int n) {
  for (int i = 0; i < n - 1; i++) {
    for (int j = i + 1; j < n; j++) {
      if (arr[i] > arr[j]) {
        long tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
      }
    }
  }
}

long readDistance() {
  long samples[DISTANCE_SAMPLES];

  for (int i = 0; i < DISTANCE_SAMPLES; i++) {
    samples[i] = readDistanceOnce();

    if (samples[i] <= 0) {
      samples[i] = 999;
    }

    delay(30);
  }

  sortLong(samples, DISTANCE_SAMPLES);

  distanceCm = samples[DISTANCE_SAMPLES / 2];
  return distanceCm;
}

bool hasTrash() {
  readDistance();

  trashDetected = distanceCm > 0 && distanceCm <= TRASH_DISTANCE_CM;
  return trashDetected;
}

bool readMetal() {
  int value = digitalRead(METAL_PIN);

  metalDetected = METAL_ACTIVE_LOW ? value == LOW : value == HIGH;
  return metalDetected;
}

bool readWet() {
  rainValue = analogRead(RAIN_PIN);
  return rainValue < RAIN_WET_THRESHOLD;
}

Trash classifyTrash() {
  if (readMetal()) return METAL;
  if (readWet()) return WET;
  return DRY;
}

// ================= MAIN STATE MACHINE =================

void go(State next, const char* msg = "State changed") {
  state = next;
  stateTime = millis();

  sendState(msg);
}

void runStateMachine() {
  switch (state) {
    case INIT:
      resetHome();
      rainValue = analogRead(RAIN_PIN);
      readMetal();
      readDistance();
      go(IDLE);
      break;

    case IDLE:
      trash = NONE;

      if (isAuto() && hasTrash()) {
        go(DETECTED, "Trash detected");
      }
      break;

    case DETECTED:
      if (millis() - stateTime >= DETECT_WAIT_MS) {
        go(CLASSIFY, "Start classify");
      }
      break;

    case CLASSIFY:
      trash = classifyTrash();
      targetSteps = stepsOf(trash);

      sendState("Trash classified");
      sendClassify();

      go(ROTATE);
      break;

    case ROTATE:
      stepperTo(targetSteps);
      go(OPEN);
      break;

    case OPEN:
      servoOpen();
      go(WAIT_DROP);
      break;

    case WAIT_DROP:
      if (millis() - stateTime >= DROP_WAIT_MS) {
        go(CLOSE);
      }
      break;

    case CLOSE:
      servoClose();
      go(HOME);
      break;

    case HOME:
      if (millis() - stateTime >= SERVO_CLOSE_MS) {
        stepperTo(0);
        go(WAIT_CLEAR);
      }
      break;

    case WAIT_CLEAR:
      if (!hasTrash()) {
        trashDetected = false;
        go(IDLE, "Trash cleared");
      }
      break;
  }
}

// ================= COMMAND FSM =================

enum CommandType {
  CMD_NONE,
  CMD_AUTO,
  CMD_MANUAL,
  CMD_OPEN_LID,
  CMD_CLOSE_LID,
  CMD_RESET_HOME,
  CMD_CLASSIFY_NOW,
  CMD_ROTATE_DRY,
  CMD_ROTATE_WET,
  CMD_ROTATE_METAL,
  CMD_UNKNOWN
};

enum CommandState {
  CMD_RECEIVED,
  CMD_VALIDATE,
  CMD_EXECUTE,
  CMD_ACK,
  CMD_DONE
};

struct CommandContext {
  String id;
  String raw;
  CommandType type = CMD_NONE;
  CommandState state = CMD_RECEIVED;

  const char* ackStatus = "done";
  const char* ackMsg = "";
};

CommandType parseCommand(const String& cmd) {
  if (cmd == "auto_on" || cmd == "mode_auto") return CMD_AUTO;
  if (cmd == "auto_off" || cmd == "manual_on" || cmd == "mode_manual") return CMD_MANUAL;
  if (cmd == "open_lid") return CMD_OPEN_LID;
  if (cmd == "close_lid") return CMD_CLOSE_LID;
  if (cmd == "reset_home") return CMD_RESET_HOME;
  if (cmd == "classify_now") return CMD_CLASSIFY_NOW;
  if (cmd == "rotate_dry") return CMD_ROTATE_DRY;
  if (cmd == "rotate_wet") return CMD_ROTATE_WET;
  if (cmd == "rotate_metal") return CMD_ROTATE_METAL;

  return CMD_UNKNOWN;
}

bool isRotateCommand(CommandType type) {
  return type == CMD_ROTATE_DRY ||
         type == CMD_ROTATE_WET ||
         type == CMD_ROTATE_METAL;
}

Trash trashFromCommand(CommandType type) {
  if (type == CMD_ROTATE_WET) return WET;
  if (type == CMD_ROTATE_METAL) return METAL;
  return DRY;
}

void setCommandResult(CommandContext& ctx, const char* status, const char* msg) {
  ctx.ackStatus = status;
  ctx.ackMsg = msg;
}

void validateCommand(CommandContext& ctx) {
  if (ctx.type == CMD_UNKNOWN) {
    setCommandResult(ctx, "unknown", "Unknown command");
    ctx.state = CMD_ACK;
    return;
  }

  if (isRotateCommand(ctx.type) && isAuto()) {
    setCommandResult(ctx, "ignored", "Switch to Manual first");
    ctx.state = CMD_ACK;
    return;
  }

  if (ctx.type == CMD_CLASSIFY_NOW) {
    if (!isAuto()) {
      setCommandResult(ctx, "ignored", "Only in AUTO mode");
      ctx.state = CMD_ACK;
      return;
    }

    if (!(state == IDLE || state == WAIT_CLEAR)) {
      setCommandResult(ctx, "ignored", "System busy");
      ctx.state = CMD_ACK;
      return;
    }
  }

  ctx.state = CMD_EXECUTE;
}

void executeCommand(CommandContext& ctx) {
  switch (ctx.type) {
    case CMD_AUTO:
      mode = AUTO;
      resetHome();
      state = IDLE;
      sendState("Mode changed to AUTO");
      setCommandResult(ctx, "done", "Auto mode enabled");
      break;

    case CMD_MANUAL:
      mode = MANUAL;
      resetHome();
      state = IDLE;
      sendState("Mode changed to MANUAL");
      setCommandResult(ctx, "done", "Manual mode enabled");
      break;

    case CMD_OPEN_LID:
      servoOpen();
      sendState("Remote open lid");
      setCommandResult(ctx, "done", "Servo opened");
      break;

    case CMD_CLOSE_LID:
      servoClose();
      sendState("Remote close lid");
      setCommandResult(ctx, "done", "Servo closed");
      break;

    case CMD_RESET_HOME:
      resetHome();
      sendState("Remote reset home");
      setCommandResult(ctx, "done", "Returned home");
      break;

    case CMD_CLASSIFY_NOW:
      go(CLASSIFY, "Force classify");
      setCommandResult(ctx, "accepted", "Classify accepted");
      break;

    case CMD_ROTATE_DRY:
    case CMD_ROTATE_WET:
    case CMD_ROTATE_METAL:
      trash = trashFromCommand(ctx.type);
      stepperTo(stepsOf(trash));
      sendState("Manual rotate");
      setCommandResult(ctx, "done", trashText(trash));
      break;

    default:
      setCommandResult(ctx, "unknown", "Unknown command");
      break;
  }

  ctx.state = CMD_ACK;
}

void runCommandFSM(CommandContext& ctx) {
  while (ctx.state != CMD_DONE) {
    switch (ctx.state) {
      case CMD_RECEIVED:
        ctx.type = parseCommand(ctx.raw);
        ctx.state = CMD_VALIDATE;
        break;

      case CMD_VALIDATE:
        validateCommand(ctx);
        break;

      case CMD_EXECUTE:
        executeCommand(ctx);
        break;

      case CMD_ACK:
        sendAck(ctx.id, ctx.raw, ctx.ackStatus, ctx.ackMsg);
        ctx.state = CMD_DONE;
        break;

      case CMD_DONE:
        break;
    }
  }
}

void handleCommand(const String& id, const String& cmd) {
  CommandContext ctx;

  ctx.id = id;
  ctx.raw = cmd;
  ctx.state = CMD_RECEIVED;

  runCommandFSM(ctx);
}

// ================= COAP RESPONSE =================

void onCoapResponse(CoapPacket& packet, IPAddress ip, int port) {
  String payload;

  for (int i = 0; i < packet.payloadlen; i++) {
    payload += (char)packet.payload[i];
  }

  if (payload.length() == 0) return;

  StaticJsonDocument<384> doc;

  if (deserializeJson(doc, payload)) {
    return;
  }

  bool hasCommand = doc["has_command"] | false;
  if (!hasCommand) return;

  String id = doc["id"] | "";
  String cmd = doc["cmd"] | "";

  if (cmd.length() > 0) {
    handleCommand(id, cmd);
  }
}

// ================= WIFI =================

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
  }
}

// ================= PERIODIC =================

void runPeriodicTasks() {
  unsigned long now = millis();

  if (now - lastTelemetry >= TELEMETRY_MS) {
    lastTelemetry = now;
    sendTelemetry();
    return;
  }

  if (now - lastCommand >= COMMAND_MS) {
    lastCommand = now;
    getCommand();
    return;
  }
}

// ================= SETUP + LOOP =================

void setup() {
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(METAL_PIN, INPUT_PULLUP);

  lidServo.attach(SERVO_PIN);
  servoClose();

  stepperMotor.setSpeed(10);

  connectWiFi();

  coap.response(onCoapResponse);
  coap.start();

  go(INIT);
}

void loop() {
  coap.loop();

  runPeriodicTasks();
  runStateMachine();
}