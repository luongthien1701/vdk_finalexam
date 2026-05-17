const coap = require("coap");

const HOST = "127.0.0.1";
const PORT = 5683;

function sendPut(path, payload) {
    const req = coap.request({
        hostname: HOST,
        port: PORT,
        pathname: path,
        method: "PUT",
        confirmable: true,
        options: {
            "Content-Format": "application/json",
        },
    });

    req.on("response", (res) => {
        let data = "";

        res.on("data", (chunk) => {
            data += chunk.toString();
        });

        res.on("end", () => {
            console.log(`[RESPONSE ${path}]`, data);
        });
    });

    req.on("error", (err) => {
        console.error(`[ERROR ${path}]`, err.message);
    });

    const body = JSON.stringify(payload);
    console.log(`[SEND PUT ${path}]`, body);

    req.write(body);
    req.end();
}

sendPut("/bin/telemetry", {
    device_id: "smartbin-01",
    state: "IDLE",
    distance_cm: 123,
    rain: 4095,
    metal_detected: false,
    trash_detected: false,
    auto_mode: true,
});

setTimeout(() => {
    sendPut("/bin/state", {
        device_id: "smartbin-01",
        state: "DETECTED",
        trash_type: "dry",
        distance_cm: 3,
        rain: 4095,
        metal_detected: false,
        trash_detected: true,
        auto_mode: true,
        message: "Test state from Node client",
    });
}, 1000);

setTimeout(() => {
    sendPut("/bin/classify", {
        device_id: "smartbin-01",
        type: "dry",
        distance_cm: 3,
        rain: 4095,
        metal_detected: false,
        state: "CLASSIFY",
        stepper_angle: 0,
        servo: "open_closed",
    });
}, 2000);