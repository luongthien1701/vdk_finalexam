/**
 * Smart Bin — Node.js CoAP + WebSocket + Firebase Backend
 *
 * Install:
 *   npm install coap ws firebase-admin
 *
 * Run:
 *   node server.js
 *
 * Ports:
 *   CoAP UDP : 5683
 *   WS TCP   : 8765
 *   HTTP TCP : 8080
 */

const coap = require("coap");
const WebSocket = require("ws");
const http = require("http");
const fs = require("fs");
const path = require("path");
const admin = require("firebase-admin");

// ======================
// CONFIG
// ======================

const DEVICE_ID_DEFAULT = "smartbin-01";
const COAP_PORT = 5683;
const WS_PORT = 8765;
const HTTP_PORT = 8080;

const DASHBOARD_FILE = path.join(__dirname, "index.html");
const SERVICE_ACCOUNT_FILE = path.join(__dirname, "serviceAccountKey.json");

// ======================
// FIREBASE INIT
// ======================

let db = null;
let firebaseEnabled = false;

function initFirebase() {
  try {
    if (fs.existsSync(SERVICE_ACCOUNT_FILE)) {
      const serviceAccount = require(SERVICE_ACCOUNT_FILE);

      admin.initializeApp({
        credential: admin.credential.cert(serviceAccount),
      });

      db = admin.firestore();
      firebaseEnabled = true;
      console.log("[Firebase] Connected using serviceAccountKey.json");
      return;
    }

    if (process.env.GOOGLE_APPLICATION_CREDENTIALS) {
      admin.initializeApp({
        credential: admin.credential.applicationDefault(),
      });

      db = admin.firestore();
      firebaseEnabled = true;
      console.log("[Firebase] Connected using GOOGLE_APPLICATION_CREDENTIALS");
      return;
    }

    console.warn("[Firebase] serviceAccountKey.json not found. Server still runs without database.");
  } catch (err) {
    console.error("[Firebase] Init failed:", err.message);
  }
}

initFirebase();

// ======================
// IN-MEMORY STATE
// ======================

const state = {
  device_id: DEVICE_ID_DEFAULT,
  online: false,
  last_seen_at: null,
  last_state: "INIT",
  last_type: null,

  distance_cm: 999,
  rain: 0,
  metal_detected: false,
  trash_detected: false,
  auto_mode: true,

  bins: {
    wet: 0,
    dry: 0,
    metal: 0,
  },

  stats: {
    wet: 0,
    dry: 0,
    metal: 0,
    total: 0,
  },

  daily_stats: {
    wet: 0,
    dry: 0,
    metal: 0,
    total: 0,
    date: null,
  },

  motor: {
    stepper_angle: 0,
    servo: "closed",
  },

  alerts: [],
  history: [],
  telemetry_history: [],
  state_history: [],
  command_history: [],
  command_ack_history: [],

  latency_ms: 0,
};

const pendingCommands = new Map();
const wsClients = new Set();

let lastTelemetryForSpike = null;
let offlineTimer = null;

// ======================
// UTILS
// ======================

function nvl(value, fallback) {
  return value === null || value === undefined ? fallback : value;
}

function nowISO() {
  return new Date().toISOString();
}

function nowTime() {
  return new Date().toLocaleTimeString("vi-VN", { hour12: false });
}

function todayKey() {
  return new Date().toISOString().slice(0, 10);
}

function safeJsonParse(buffer) {
  try {
    const text = buffer ? buffer.toString() : "";
    if (!text) return {};
    return JSON.parse(text);
  } catch (err) {
    return null;
  }
}

function normalizeEspPayload(body) {
  if (!body || typeof body !== "object") return body;

  const out = Object.assign({}, body);

  if (body.dev !== undefined && out.device_id === undefined) out.device_id = body.dev;

  if (body.s !== undefined && out.state === undefined) out.state = body.s;
  if (body.ty !== undefined && out.type === undefined) out.type = body.ty;
  if (body.tt !== undefined && out.trash_type === undefined) out.trash_type = body.tt;

  if (body.d !== undefined && out.distance_cm === undefined) out.distance_cm = body.d;
  if (body.r !== undefined && out.rain === undefined) out.rain = body.r;
  if (body.m !== undefined && out.metal_detected === undefined) out.metal_detected = Boolean(body.m);
  if (body.t !== undefined && out.trash_detected === undefined) out.trash_detected = Boolean(body.t);
  if (body.a !== undefined && out.auto_mode === undefined) out.auto_mode = Boolean(body.a);

  if (body.msg !== undefined && out.message === undefined) out.message = body.msg;
  if (body.ang !== undefined && out.stepper_angle === undefined) out.stepper_angle = body.ang;

  if (body.sv !== undefined && out.servo === undefined) {
    if (body.sv === "oc") out.servo = "open→closed";
    else if (body.sv === "c") out.servo = "closed";
    else if (body.sv === "o") out.servo = "open";
    else out.servo = body.sv;
  }

  if (body.cid !== undefined && out.id === undefined) out.id = body.cid;
  if (body.st !== undefined && out.status === undefined) out.status = body.st;

  return out;
}

function jsonResponse(res, obj, code = "2.05") {
  res.code = code;
  res.setOption("Content-Format", "application/json");
  res.end(JSON.stringify(obj));
}

function addHistory(item) {
  state.history.unshift(item);

  if (state.history.length > 100) {
    state.history.pop();
  }
}

function broadcastWS(payload) {
  const msg = JSON.stringify(payload);
  let sent = 0;

  for (const client of wsClients) {
    if (client.readyState === WebSocket.OPEN) {
      client.send(msg);
      sent++;
    }
  }

  console.log("[WS BROADCAST]", {
    event: payload.event,
    sent_clients: sent,
    total_clients: wsClients.size,
    payload,
  });
}

// ======================
// FIREBASE HELPERS
// ======================

async function saveDoc(collectionPath, data) {
  if (!firebaseEnabled) return null;

  try {
    return await db.collection(collectionPath).add(
      Object.assign({}, data, {
        created_at: admin.firestore.FieldValue.serverTimestamp(),
        created_at_iso: nowISO(),
      })
    );
  } catch (err) {
    console.error("[Firestore] add failed:", err.message);
    return null;
  }
}

async function updateDeviceDoc(deviceId, data) {
  if (!firebaseEnabled) return;

  try {
    await db.collection("devices").doc(deviceId).set(
      Object.assign({}, data, {
        updated_at: admin.firestore.FieldValue.serverTimestamp(),
        updated_at_iso: nowISO(),
      }),
      { merge: true }
    );
  } catch (err) {
    console.error("[Firestore] update device failed:", err.message);
  }
}

async function loadDeviceFromFirebase(deviceId) {
  if (!firebaseEnabled) {
    console.log("[Firebase] Disabled, skip load device state");
    return;
  }

  try {
    const snap = await db.collection("devices").doc(deviceId).get();

    if (!snap.exists) {
      console.log("[Firebase] No device document found:", deviceId);
      return;
    }

    const data = snap.data();

    state.device_id = deviceId;
    state.online = nvl(data.online, state.online);
    state.last_seen_at = nvl(data.updated_at_iso, state.last_seen_at);
    state.last_state = nvl(data.last_state, state.last_state);
    state.last_type = nvl(data.last_type, state.last_type);

    state.distance_cm = nvl(data.last_distance_cm, state.distance_cm);
    state.rain = nvl(data.last_rain, state.rain);
    state.metal_detected = nvl(data.last_metal_detected, state.metal_detected);
    state.trash_detected = nvl(data.trash_detected, state.trash_detected);
    state.auto_mode = nvl(data.auto_mode, state.auto_mode);

    state.bins = nvl(data.bins, state.bins);
    state.stats = nvl(data.stats, state.stats);
    state.motor = nvl(data.motor, state.motor);

    if (data.last_alert) {
      state.alerts = [data.last_alert].concat(state.alerts);
    }

    console.log("[Firebase] Loaded device doc:", deviceId);
  } catch (err) {
    console.error("[Firebase] load device failed:", err.message);
  }
}

async function loadLatestDocFromSubcollection(deviceId, subcollectionName) {
  if (!firebaseEnabled) return null;

  try {
    const snap = await db
      .collection("devices")
      .doc(deviceId)
      .collection(subcollectionName)
      .orderBy("created_at_iso", "desc")
      .limit(1)
      .get();

    if (snap.empty) return null;

    let result = null;

    snap.forEach((doc) => {
      result = Object.assign({ firestore_id: doc.id }, doc.data());
    });

    return result;
  } catch (err) {
    console.error("[Firebase] load latest " + subcollectionName + " failed:", err.message);
    return null;
  }
}

async function loadRecentDocsFromSubcollection(deviceId, subcollectionName, limitCount) {
  if (!firebaseEnabled) return [];

  try {
    const snap = await db
      .collection("devices")
      .doc(deviceId)
      .collection(subcollectionName)
      .orderBy("created_at_iso", "desc")
      .limit(limitCount)
      .get();

    const docs = [];

    snap.forEach((doc) => {
      docs.push(Object.assign({ firestore_id: doc.id }, doc.data()));
    });

    return docs;
  } catch (err) {
    console.error("[Firebase] load recent " + subcollectionName + " failed:", err.message);
    return [];
  }
}

async function loadTodayStatsFromFirebase(deviceId) {
  if (!firebaseEnabled) return;

  try {
    const snap = await db
      .collection("devices")
      .doc(deviceId)
      .collection("daily_stats")
      .doc(todayKey())
      .get();

    if (!snap.exists) {
      console.log("[Firebase] No daily_stats for today");
      return;
    }

    const data = snap.data();

    state.daily_stats = {
      wet: nvl(data.wet, 0),
      dry: nvl(data.dry, 0),
      metal: nvl(data.metal, 0),
      total: nvl(data.total, 0),
      date: nvl(data.date, todayKey()),
    };

    state.stats = {
      wet: nvl(data.wet, state.stats.wet),
      dry: nvl(data.dry, state.stats.dry),
      metal: nvl(data.metal, state.stats.metal),
      total: nvl(data.total, state.stats.total),
    };

    console.log("[Firebase] Loaded today daily_stats:", state.daily_stats);
  } catch (err) {
    console.error("[Firebase] load daily_stats failed:", err.message);
  }
}

async function loadAllFirebaseState(deviceId) {
  await loadDeviceFromFirebase(deviceId);

  const latestTelemetry = await loadLatestDocFromSubcollection(deviceId, "telemetry");
  if (latestTelemetry) {
    state.distance_cm = nvl(latestTelemetry.distance_cm, state.distance_cm);
    state.rain = nvl(latestTelemetry.rain, state.rain);
    state.metal_detected = nvl(latestTelemetry.metal_detected, state.metal_detected);
    state.trash_detected = nvl(latestTelemetry.trash_detected, state.trash_detected);
    state.auto_mode = nvl(latestTelemetry.auto_mode, state.auto_mode);
    state.last_state = nvl(latestTelemetry.state, state.last_state);
    state.last_seen_at = nvl(latestTelemetry.created_at_iso, state.last_seen_at);
  }

  const latestState = await loadLatestDocFromSubcollection(deviceId, "states");
  if (latestState) {
    state.last_state = nvl(latestState.state, state.last_state);
    state.distance_cm = nvl(latestState.distance_cm, state.distance_cm);
    state.rain = nvl(latestState.rain, state.rain);
    state.metal_detected = nvl(latestState.metal_detected, state.metal_detected);
    state.trash_detected = nvl(latestState.trash_detected, state.trash_detected);
    state.auto_mode = nvl(latestState.auto_mode, state.auto_mode);
    state.last_seen_at = nvl(latestState.created_at_iso, state.last_seen_at);
  }

  const latestEvent = await loadLatestDocFromSubcollection(deviceId, "events");
  if (latestEvent) {
    state.last_type = nvl(latestEvent.type, state.last_type);
    state.bins = nvl(latestEvent.bins, state.bins);
    state.stats = nvl(latestEvent.stats, state.stats);
    state.motor = nvl(latestEvent.motor, state.motor);
  }

  const recentEvents = await loadRecentDocsFromSubcollection(deviceId, "events", 30);
  state.history = recentEvents.map((item) => {
    return {
      type: nvl(item.type, "event"),
      ts: nvl(item.ts, ""),
      msg: "Phân loại: " + nvl(item.type, "unknown"),
      created_at_iso: nvl(item.created_at_iso, ""),
    };
  });

  const recentAlerts = await loadRecentDocsFromSubcollection(deviceId, "alerts", 10);
  state.alerts = recentAlerts;

  const recentTelemetry = await loadRecentDocsFromSubcollection(deviceId, "telemetry", 20);
  state.telemetry_history = recentTelemetry;

  const recentStates = await loadRecentDocsFromSubcollection(deviceId, "states", 20);
  state.state_history = recentStates;

  const recentCommands = await loadRecentDocsFromSubcollection(deviceId, "commands", 20);
  state.command_history = recentCommands;

  const recentAcks = await loadRecentDocsFromSubcollection(deviceId, "command_acks", 20);
  state.command_ack_history = recentAcks;

  await loadTodayStatsFromFirebase(deviceId);

  console.log("[Firebase] Loaded all Firebase state:", {
    device_id: state.device_id,
    online: state.online,
    last_seen_at: state.last_seen_at,
    last_state: state.last_state,
    last_type: state.last_type,
    distance_cm: state.distance_cm,
    rain: state.rain,
    metal_detected: state.metal_detected,
    trash_detected: state.trash_detected,
    auto_mode: state.auto_mode,
    bins: state.bins,
    stats: state.stats,
    daily_stats: state.daily_stats,
    motor: state.motor,
    alerts_count: state.alerts.length,
    history_count: state.history.length,
    telemetry_count: state.telemetry_history.length,
    state_count: state.state_history.length,
    command_count: state.command_history.length,
    command_ack_count: state.command_ack_history.length,
  });
}

async function incrementDailyStats(deviceId, type) {
  if (!firebaseEnabled) return;

  try {
    const ref = db
      .collection("devices")
      .doc(deviceId)
      .collection("daily_stats")
      .doc(todayKey());

    const update = {
      total: admin.firestore.FieldValue.increment(1),
      updated_at: admin.firestore.FieldValue.serverTimestamp(),
      date: todayKey(),
    };

    if (["wet", "dry", "metal"].includes(type)) {
      update[type] = admin.firestore.FieldValue.increment(1);
    }

    await ref.set(update, { merge: true });
  } catch (err) {
    console.error("[Firestore] daily stats failed:", err.message);
  }
}

// ======================
// DOMAIN UTILS
// ======================

function mapTrashType(type) {
  const t = String(type || "").toLowerCase();

  if (t.includes("wet") || t.includes("uot") || t.includes("ẩm")) return "wet";
  if (t.includes("metal") || t.includes("kim")) return "metal";
  if (t.includes("dry") || t.includes("kho") || t.includes("khô")) return "dry";

  return "dry";
}

function stepperAngleByType(type) {
  if (type === "wet") return 120;
  if (type === "metal") return 240;
  return 0;
}

function markOnline(deviceId) {
  state.online = true;
  state.last_seen_at = nowISO();

  clearTimeout(offlineTimer);

  offlineTimer = setTimeout(async () => {
    state.online = false;

    const alert = {
      event: "alert",
      kind: "offline",
      bin: "system",
      level: 0,
      msg: "ESP32 mất kết nối hoặc không gửi dữ liệu trong thời gian dài",
      ts: nowTime(),
      created_at_iso: nowISO(),
    };

    state.alerts.unshift(alert);
    if (state.alerts.length > 50) state.alerts.pop();

    await updateDeviceDoc(deviceId, {
      online: false,
      last_alert: alert,
    });

    await saveDoc("devices/" + deviceId + "/alerts", alert);

    broadcastWS(alert);
    broadcastWS({
      event: "offline",
      online: false,
      ts: nowTime(),
    });
  }, 15000);
}

async function maybeCreateSpikeAlert(deviceId, data) {
  if (!lastTelemetryForSpike) {
    lastTelemetryForSpike = data;
    return;
  }

  const alerts = [];

  const prevDistance = Number(nvl(lastTelemetryForSpike.distance_cm, 999));
  const currDistance = Number(nvl(data.distance_cm, 999));
  const prevRain = Number(nvl(lastTelemetryForSpike.rain, 0));
  const currRain = Number(nvl(data.rain, 0));

  if (Math.abs(prevDistance - currDistance) >= 30) {
    alerts.push({
      kind: "distance_spike",
      bin: "sensor",
      level: Math.abs(prevDistance - currDistance),
      msg: "Khoảng cách đột biến: " + prevDistance + "cm → " + currDistance + "cm",
    });
  }

  if (Math.abs(prevRain - currRain) >= 1500) {
    alerts.push({
      kind: "rain_spike",
      bin: "sensor",
      level: Math.abs(prevRain - currRain),
      msg: "Cảm biến mưa đột biến: " + prevRain + " → " + currRain,
    });
  }

  lastTelemetryForSpike = data;

  for (const a of alerts) {
    const alert = Object.assign(
      {
        event: "alert",
        ts: nowTime(),
        created_at_iso: nowISO(),
      },
      a
    );

    state.alerts.unshift(alert);
    if (state.alerts.length > 50) state.alerts.pop();

    await saveDoc("devices/" + deviceId + "/alerts", alert);
    broadcastWS(alert);
  }
}

function getClientInitPayload() {
  return {
    event: "init",
    device_id: state.device_id,
    online: state.online,
    current_state: state.last_state,
    bins: state.bins,
    stats: state.stats,
    daily_stats: state.daily_stats,
    motor: state.motor,
    alerts: state.alerts.slice(0, 10),
    history: state.history.slice(0, 30),
    telemetry_history: state.telemetry_history.slice(0, 20),
    state_history: state.state_history.slice(0, 20),
    command_history: state.command_history.slice(0, 20),
    command_ack_history: state.command_ack_history.slice(0, 20),
    last_type: state.last_type,
    last_ts: state.last_seen_at,
    telemetry: {
      distance_cm: state.distance_cm,
      rain: state.rain,
      metal_detected: state.metal_detected,
      trash_detected: state.trash_detected,
      auto_mode: state.auto_mode,
    },
    firebase_loaded: firebaseEnabled,
    loaded_at: nowISO(),
  };
}

// ======================
// COAP HANDLERS
// ======================

async function handleState(req, res, body) {
  const t0 = Date.now();
  const deviceId = body.device_id || DEVICE_ID_DEFAULT;

  markOnline(deviceId);

  state.device_id = deviceId;
  state.last_state = nvl(body.state, state.last_state);
  state.distance_cm = nvl(body.distance_cm, state.distance_cm);
  state.rain = nvl(body.rain, state.rain);
  state.metal_detected = nvl(body.metal_detected, state.metal_detected);
  state.trash_detected = nvl(body.trash_detected, state.trash_detected);
  state.auto_mode = nvl(body.auto_mode, state.auto_mode);
  state.latency_ms = Date.now() - t0;

  const payload = {
    event: "state",
    device_id: deviceId,
    state: state.last_state,
    trash_type: nvl(body.trash_type, state.last_type),
    distance_cm: state.distance_cm,
    rain: state.rain,
    metal_detected: state.metal_detected,
    trash_detected: state.trash_detected,
    auto_mode: state.auto_mode,
    message: nvl(body.message, ""),
    ts: nowTime(),
    created_at_iso: nowISO(),
    latency_ms: state.latency_ms,
  };

  addHistory({
    type: "state",
    state: payload.state,
    ts: payload.ts,
    msg: "Trạng thái: " + payload.state,
  });

  await updateDeviceDoc(deviceId, {
    online: true,
    last_state: payload.state,
    last_distance_cm: payload.distance_cm,
    last_rain: payload.rain,
    last_metal_detected: payload.metal_detected,
    trash_detected: payload.trash_detected,
    auto_mode: payload.auto_mode,
  });

  await saveDoc("devices/" + deviceId + "/states", payload);

  broadcastWS(payload);
  jsonResponse(res, { ok: true }, "2.04");
}

async function handleTelemetry(req, res, body) {
  const t0 = Date.now();
  const deviceId = body.device_id || DEVICE_ID_DEFAULT;

  markOnline(deviceId);

  state.device_id = deviceId;
  state.distance_cm = nvl(body.distance_cm, state.distance_cm);
  state.rain = nvl(body.rain, state.rain);
  state.metal_detected = nvl(body.metal_detected, state.metal_detected);
  state.trash_detected = nvl(body.trash_detected, state.trash_detected);
  state.auto_mode = nvl(body.auto_mode, state.auto_mode);
  state.last_state = nvl(body.state, state.last_state);
  state.latency_ms = Date.now() - t0;

  const payload = {
    event: "telemetry",
    device_id: deviceId,
    distance_cm: state.distance_cm,
    rain: state.rain,
    metal_detected: state.metal_detected,
    trash_detected: state.trash_detected,
    auto_mode: state.auto_mode,
    state: state.last_state,
    ts: nowTime(),
    created_at_iso: nowISO(),
    latency_ms: state.latency_ms,
  };

  await updateDeviceDoc(deviceId, {
    online: true,
    last_state: payload.state,
    last_distance_cm: payload.distance_cm,
    last_rain: payload.rain,
    last_metal_detected: payload.metal_detected,
    trash_detected: payload.trash_detected,
    auto_mode: payload.auto_mode,
  });

  await saveDoc("devices/" + deviceId + "/telemetry", payload);
  await maybeCreateSpikeAlert(deviceId, payload);

  broadcastWS(payload);
  jsonResponse(res, { ok: true }, "2.04");
}

async function handleClassify(req, res, body) {
  const t0 = Date.now();
  const deviceId = body.device_id || DEVICE_ID_DEFAULT;

  markOnline(deviceId);

  console.log("[CLASSIFY RECEIVED]", body);

  const type = mapTrashType(nvl(body.type, body.trash_type));

  state.device_id = deviceId;
  state.last_type = type;
  state.last_state = "CLASSIFIED";

  state.stats[type] = nvl(state.stats[type], 0) + 1;
  state.stats.total = nvl(state.stats.total, 0) + 1;

  state.bins[type] = Math.min(100, nvl(state.bins[type], 0) + 5);

  state.motor.stepper_angle = nvl(body.stepper_angle, stepperAngleByType(type));
  state.motor.servo = nvl(body.servo, "open→closed");

  state.distance_cm = nvl(body.distance_cm, state.distance_cm);
  state.rain = nvl(body.rain, state.rain);
  state.metal_detected = nvl(body.metal_detected, state.metal_detected);
  state.latency_ms = Date.now() - t0;

  state.daily_stats[type] = nvl(state.daily_stats[type], 0) + 1;
  state.daily_stats.total = nvl(state.daily_stats.total, 0) + 1;
  state.daily_stats.date = todayKey();

  const payload = {
    event: "classify",
    device_id: deviceId,
    type,
    bins: state.bins,
    stats: state.stats,
    motor: state.motor,
    distance_cm: state.distance_cm,
    rain: state.rain,
    metal_detected: state.metal_detected,
    ts: nowTime(),
    created_at_iso: nowISO(),
    latency_ms: state.latency_ms,
  };

  console.log("[CLASSIFY UPDATED]", {
    type,
    stats: state.stats,
    bins: state.bins,
    motor: state.motor,
    wsClients: wsClients.size,
  });

  addHistory({
    type,
    ts: payload.ts,
    msg: "Phân loại: " + type,
  });

  await updateDeviceDoc(deviceId, {
    online: true,
    last_type: type,
    last_state: "CLASSIFIED",
    bins: state.bins,
    stats: state.stats,
    motor: state.motor,
  });

  await saveDoc("devices/" + deviceId + "/events", payload);
  await incrementDailyStats(deviceId, type);

  if (state.bins[type] >= 80) {
    const alert = {
      event: "alert",
      kind: "bin_full",
      bin: type,
      level: state.bins[type],
      msg: "Thùng " + type + " sắp đầy",
      ts: nowTime(),
      created_at_iso: nowISO(),
    };

    state.alerts.unshift(alert);
    if (state.alerts.length > 50) state.alerts.pop();

    await saveDoc("devices/" + deviceId + "/alerts", alert);
    broadcastWS(alert);
  }

  broadcastWS(payload);
  jsonResponse(res, { ok: true }, "2.04");
}

async function handleAlert(req, res, body) {
  const deviceId = body.device_id || DEVICE_ID_DEFAULT;

  markOnline(deviceId);

  const alert = {
    event: "alert",
    device_id: deviceId,
    kind: nvl(body.kind, "custom"),
    bin: nvl(body.bin, "system"),
    level: nvl(body.level, 0),
    msg: nvl(body.msg, "Cảnh báo hệ thống"),
    ts: nowTime(),
    created_at_iso: nowISO(),
  };

  state.alerts.unshift(alert);
  if (state.alerts.length > 50) state.alerts.pop();

  await saveDoc("devices/" + deviceId + "/alerts", alert);

  broadcastWS(alert);
  jsonResponse(res, { ok: true }, "2.04");
}

async function handleCommandGet(req, res) {
  const deviceId = DEVICE_ID_DEFAULT;
  const queue = pendingCommands.get(deviceId) || [];

  if (queue.length === 0) {
    jsonResponse(res, { has_command: false });
    return;
  }

  const cmd = queue.shift();
  pendingCommands.set(deviceId, queue);

  console.log("[CoAP COMMAND] ESP32 get command:", cmd);

  jsonResponse(res, {
    has_command: true,
    id: cmd.id,
    cmd: cmd.cmd,
    payload: cmd.payload || {},
  });
}

async function handleCommandAck(req, res, body) {
  const deviceId = body.device_id || DEVICE_ID_DEFAULT;

  const payload = {
    event: "command_ack",
    device_id: deviceId,
    id: nvl(body.id, null),
    cmd: nvl(body.cmd, ""),
    status: nvl(body.status, "done"),
    msg: nvl(body.msg, ""),
    ts: nowTime(),
    created_at_iso: nowISO(),
  };

  await saveDoc("devices/" + deviceId + "/command_acks", payload);

  broadcastWS(payload);
  jsonResponse(res, { ok: true }, "2.04");
}

// ======================
// COAP SERVER
// ======================

const coapServer = coap.createServer();

coapServer.on("request", async (req, res) => {
  try {
    const rawUrl = req.url || "";
    const url = "/" + rawUrl.replace(/^\/+/, "");
    const method = req.method;

    const remoteAddress = req.rsinfo ? req.rsinfo.address || "unknown" : "unknown";
    const remotePort = req.rsinfo ? req.rsinfo.port || "unknown" : "unknown";

    const rawPayload = req.payload ? req.payload.toString() : "";
    const parsedBody = safeJsonParse(req.payload);
    const body = normalizeEspPayload(parsedBody);

    console.log("========== COAP REQUEST ==========");
    console.log("[CoAP] Time   :", new Date().toISOString());
    console.log("[CoAP] From   :", remoteAddress + ":" + remotePort);
    console.log("[CoAP] Method :", method);
    console.log("[CoAP] Raw URL:", rawUrl);
    console.log("[CoAP] URL    :", url);
    console.log("[CoAP] Payload:", rawPayload);
    console.log("[CoAP] Parsed :", parsedBody);
    console.log("[CoAP] Normal :", body);
    console.log("==================================");

    if (parsedBody === null) {
      console.log("[CoAP] Invalid JSON");
      jsonResponse(res, { ok: false, error: "Invalid JSON" }, "4.00");
      return;
    }

    if (method === "PUT" && url === "/bin/state") {
      await handleState(req, res, body);
      return;
    }

    if (method === "PUT" && url === "/bin/telemetry") {
      await handleTelemetry(req, res, body);
      return;
    }

    if (method === "PUT" && url === "/bin/classify") {
      await handleClassify(req, res, body);
      return;
    }

    if (method === "PUT" && url === "/bin/alert") {
      await handleAlert(req, res, body);
      return;
    }

    if (method === "GET" && url === "/bin/command") {
      await handleCommandGet(req, res);
      return;
    }

    if (method === "PUT" && url === "/bin/command_ack") {
      await handleCommandAck(req, res, body);
      return;
    }

    if (method === "GET" && url === "/bin/status") {
      jsonResponse(res, getClientInitPayload());
      return;
    }

    console.warn("[CoAP] Not found:", method, url);
    jsonResponse(res, { ok: false, error: "Not found", method, url }, "4.04");
  } catch (err) {
    console.error("[CoAP ERROR]", err);
    jsonResponse(res, { ok: false, error: err.message }, "5.00");
  }
});

coapServer.listen(COAP_PORT, () => {
  console.log("[CoAP] Server listening on udp://0.0.0.0:" + COAP_PORT);
  console.log("[CoAP] Waiting ESP32 packets...");
});

// ======================
// WEBSOCKET SERVER
// ======================

const wss = new WebSocket.Server({ port: WS_PORT });

wss.on("connection", async (ws) => {
  wsClients.add(ws);

  console.log("[WS] Client connected. Total: " + wsClients.size);

  await loadAllFirebaseState(DEVICE_ID_DEFAULT);

  ws.send(JSON.stringify(getClientInitPayload()));

  ws.on("message", async (raw) => {
    try {
      const msg = JSON.parse(raw.toString());

      console.log("[WS MESSAGE]", msg);

      if (msg.event === "command") {
        const deviceId = msg.device_id || DEVICE_ID_DEFAULT;
        const id = Date.now() + "-" + Math.floor(Math.random() * 10000);

        const command = {
          id,
          cmd: msg.cmd,
          payload: msg.payload || {},
          source: "web",
          created_at: nowISO(),
        };

        const queue = pendingCommands.get(deviceId) || [];
        queue.push(command);
        pendingCommands.set(deviceId, queue);

        await saveDoc("devices/" + deviceId + "/commands", {
          id: command.id,
          cmd: command.cmd,
          payload: command.payload,
          source: command.source,
          created_at: command.created_at,
          created_at_iso: nowISO(),
          status: "pending",
        });

        broadcastWS({
          event: "command_pending",
          id,
          cmd: command.cmd,
          ts: nowTime(),
          msg: "Lệnh đã được server nhận, chờ ESP32 lấy qua CoAP",
        });

        return;
      }

      if (msg.event === "reset_stats") {
        state.stats = {
          wet: 0,
          dry: 0,
          metal: 0,
          total: 0,
        };

        state.daily_stats = {
          wet: 0,
          dry: 0,
          metal: 0,
          total: 0,
          date: todayKey(),
        };

        state.bins = {
          wet: 0,
          dry: 0,
          metal: 0,
        };

        await updateDeviceDoc(DEVICE_ID_DEFAULT, {
          stats: state.stats,
          bins: state.bins,
        });

        await db
          .collection("devices")
          .doc(DEVICE_ID_DEFAULT)
          .collection("daily_stats")
          .doc(todayKey())
          .set(
            {
              wet: 0,
              dry: 0,
              metal: 0,
              total: 0,
              date: todayKey(),
              updated_at: admin.firestore.FieldValue.serverTimestamp(),
              updated_at_iso: nowISO(),
            },
            { merge: true }
          );

        broadcastWS({
          event: "stats_reset",
          stats: state.stats,
          daily_stats: state.daily_stats,
          bins: state.bins,
          ts: nowTime(),
        });

        return;
      }

      if (msg.event === "get_status") {
        await loadAllFirebaseState(DEVICE_ID_DEFAULT);
        ws.send(JSON.stringify(getClientInitPayload()));
        return;
      }
    } catch (err) {
      console.error("[WS] Invalid message:", err.message);
    }
  });

  ws.on("close", () => {
    wsClients.delete(ws);
    console.log("[WS] Client disconnected. Total: " + wsClients.size);
  });

  ws.on("error", (err) => {
    console.error("[WS ERROR]", err.message);
  });
});

console.log("[WS] Server listening on ws://0.0.0.0:" + WS_PORT);

// ======================
// HTTP STATIC SERVER
// ======================

const httpServer = http.createServer((req, res) => {
  const filePath = req.url === "/" ? DASHBOARD_FILE : path.join(__dirname, req.url);

  if (!fs.existsSync(filePath) || fs.statSync(filePath).isDirectory()) {
    res.writeHead(404, {
      "Content-Type": "text/plain; charset=utf-8",
    });

    res.end("404 Not Found");
    return;
  }

  const ext = path.extname(filePath).toLowerCase();

  const contentType =
    ext === ".html"
      ? "text/html; charset=utf-8"
      : ext === ".js"
      ? "application/javascript; charset=utf-8"
      : ext === ".css"
      ? "text/css; charset=utf-8"
      : "text/plain; charset=utf-8";

  res.writeHead(200, {
    "Content-Type": contentType,
  });

  fs.createReadStream(filePath).pipe(res);
});

httpServer.listen(HTTP_PORT, async () => {
  await loadAllFirebaseState(DEVICE_ID_DEFAULT);

  console.log("[HTTP] Dashboard http://0.0.0.0:" + HTTP_PORT);
  console.log("==========================================");
  console.log("Smart Bin backend is ready");
  console.log("CoAP      : coap://<server-ip>:" + COAP_PORT);
  console.log("WebSocket : ws://<server-ip>:" + WS_PORT);
  console.log("HTTP      : http://<server-ip>:" + HTTP_PORT);
  console.log("==========================================");
});