#include "wifi_driver.h"

#include <WiFi.h>
#include <ESPAsyncWebServer.h>

String json_escape(const String& input) {
    String out;
    out.reserve(input.length() + 8);
    for (size_t i = 0; i < input.length(); ++i) {
        char c = input[i];
        switch (c) {
            case '\"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

namespace {

AsyncWebServer server(80);
bool server_started = false;
String device_label = "ai-desktop-assistant";
WifiCommandHandler command_handler = nullptr;
WifiStatusProvider status_provider = nullptr;
WifiImageHandler image_handler = nullptr;

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>AI Desktop Assistant</title>
  <style>
    :root { --bg: #0f1418; --panel: #182028; --panel2: #1f2932; --text: #eff6fb; --muted: #94a7b7; --accent: #44c18f; --warn: #ff9f43; }
    * { box-sizing: border-box; }
    body { font-family: Arial, sans-serif; margin: 0; padding: 20px; background: linear-gradient(160deg, #0d1115, #121a21 65%, #0b1014); color: var(--text); }
    .wrap { max-width: 980px; margin: 0 auto; }
    .hero { padding: 20px 22px; border-radius: 18px; background: rgba(24,32,40,0.9); border: 1px solid rgba(148,167,183,0.15); }
    .hero h1 { margin: 0; font-size: 28px; }
    .hero p { margin: 8px 0 0; color: var(--muted); }
    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(260px, 1fr)); gap: 14px; margin-top: 16px; }
    .card { padding: 18px; border-radius: 16px; background: rgba(24,32,40,0.92); border: 1px solid rgba(148,167,183,0.15); }
    .label { color: var(--muted); font-size: 13px; margin-top: 10px; }
    .value { font-size: 24px; font-weight: bold; margin-top: 4px; }
    .buttons { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 10px; }
    button { border: 0; border-radius: 12px; padding: 11px 10px; background: var(--accent); color: #07110c; font-weight: 700; cursor: pointer; }
    button.alt { background: var(--warn); color: #231300; }
    button:disabled { opacity: 0.5; cursor: wait; }
    .log { margin-top: 10px; padding: 12px; border-radius: 12px; background: var(--panel2); color: #d8e4ec; min-height: 74px; white-space: pre-wrap; word-break: break-word; }
    .log.tall { min-height: 180px; max-height: 280px; overflow: auto; }
  </style>
</head>
<body>
  <div class="wrap">
    <div class="hero">
      <h1>AI Desktop Assistant</h1>
      <p>网页控制面板。状态每秒刷新，执行结果会显示在页面上，串口只保留备用调试输出。</p>
    </div>
    <div class="grid">
      <div class="card">
        <h2>Status</h2>
        <div class="label">Device</div>
        <div class="value" id="device">-</div>
        <div class="label">Uptime</div>
        <div class="value" id="uptime">-</div>
        <div class="label">Millis</div>
        <div class="value" id="millis">-</div>
        <div class="label">Last Result</div>
        <div class="log" id="last-result">-</div>
        <div class="label">Quick Check</div>
        <div class="buttons">
          <button onclick="runCmd('ping')">Ping</button>
          <button onclick="runCmd('wifi_status')" class="alt">WiFi</button>
        </div>
      </div>

      <div class="card">
        <h2>Audio</h2>
        <div class="buttons">
          <button onclick="runCmd('mic_record')">Mic 3s</button>
          <button onclick="runCmd('mic_diag')" class="alt">Mic Diag</button>
          <button onclick="runCmd('loopback')">Loopback</button>
          <button onclick="runCmd('tone_16k')" class="alt">Tone 16k</button>
          <button onclick="runCmd('tone_44k')">Tone 44.1k</button>
        </div>
      </div>

      <div class="card">
        <h2>Vision / Display</h2>
        <div class="buttons">
          <button onclick="runCmd('capture')">Capture</button>
          <button onclick="runCmd('screen_demo')" class="alt">Screen Demo</button>
        </div>
      </div>

      <div class="card">
        <h2>Servo</h2>
        <div class="buttons">
          <button onclick="runCmd('servo180_0')">180: 0</button>
          <button onclick="runCmd('servo180_90')" class="alt">180: 90</button>
          <button onclick="runCmd('servo180_180')">180: 180</button>
          <button onclick="runCmd('servo360_rev')" class="alt">360: Rev</button>
          <button onclick="runCmd('servo360_stop')">360: Stop</button>
          <button onclick="runCmd('servo360_fwd')" class="alt">360: Fwd</button>
        </div>
      </div>

      <div class="card">
        <h2>Screen Image</h2>
        <input type="file" id="imgFile" accept="image/*" style="margin-bottom:6px;color:var(--text);">
        <div class="buttons">
          <button onclick="uploadImage()">Upload to Screen</button>
        </div>
        <div class="label" id="imgStatus">Ready</div>
      </div>

      <div class="card">
        <h2>433MHz RF</h2>
        <div class="buttons">
          <button onclick="runCmd('rf_on')">Send ON</button>
          <button onclick="runCmd('rf_off')" class="alt">Send OFF</button>
        </div>
      </div>

      <div class="card">
        <h2>Debug Log</h2>
        <div class="log tall" id="debug-log">-</div>
      </div>
    </div>
  </div>
  <script>
    let busy = false;
    function setButtonsDisabled(disabled) {
      document.querySelectorAll('button').forEach(btn => btn.disabled = disabled);
    }
    async function refreshStatus() {
      try {
        const res = await fetch('/api/status', { cache: 'no-store' });
        const data = await res.json();
        document.getElementById('device').textContent = data.device;
        document.getElementById('uptime').textContent = data.uptime_s + ' s';
        document.getElementById('millis').textContent = data.millis;
        document.getElementById('last-result').textContent = data.last_result || '-';
        document.getElementById('debug-log').textContent = data.debug_log || '-';
      } catch (err) {
        document.getElementById('uptime').textContent = 'offline';
      }
    }
    async function runCmd(cmd) {
      if (busy) return;
      busy = true;
      setButtonsDisabled(true);
      document.getElementById('last-result').textContent = 'Running: ' + cmd;
      try {
        const res = await fetch('/api/action?cmd=' + encodeURIComponent(cmd), { method: 'POST' });
        const data = await res.json();
        document.getElementById('last-result').textContent = data.message || (data.ok ? 'ok' : 'failed');
      } catch (err) {
        document.getElementById('last-result').textContent = 'Request failed';
      } finally {
        busy = false;
        setButtonsDisabled(false);
        refreshStatus();
      }
    }
    async function uploadImage() {
      const file = document.getElementById('imgFile').files[0];
      if (!file) { document.getElementById('imgStatus').textContent = 'No file selected'; return; }
      if (busy) return;
      busy = true; setButtonsDisabled(true);
      document.getElementById('imgStatus').textContent = 'Processing...';
      try {
        const img = new Image();
        const blob = await new Promise((res, rej) => { const r = new FileReader(); r.onload = e => { img.onload = () => res(img); img.onerror = rej; img.src = e.target.result; }; r.readAsDataURL(file); });
        const c = document.createElement('canvas'); c.width = 240; c.height = 284;
        const ctx = c.getContext('2d');
        const sw = img.width, sh = img.height, dw = 240, dh = 284;
        let sx = 0, sy = 0, sW = sw, sH = sh;
        if (sW / sH > dw / dh) { sW = Math.floor(sH * dw / dh); sx = Math.floor((sw - sW) / 2); }
        else { sH = Math.floor(sW * dh / dw); sy = Math.floor((sh - sH) / 2); }
        ctx.drawImage(img, sx, sy, sW, sH, 0, 0, dw, dh);
        const pixels = ctx.getImageData(0, 0, dw, dh).data;
        const buf = new ArrayBuffer(dw * dh * 2);
        const view = new DataView(buf);
        for (let i = 0, o = 0; i < dw * dh; i++, o += 2) {
          const r = pixels[i * 4], g = pixels[i * 4 + 1], b = pixels[i * 4 + 2];
          view.setUint16(o, ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3), true);
        }
        document.getElementById('imgStatus').textContent = 'Sending ' + buf.byteLength + ' bytes...';
        const res = await fetch('/api/upload', { method: 'POST', body: buf });
        document.getElementById('imgStatus').textContent = res.ok ? 'Uploaded OK' : 'Upload failed';
      } catch (err) {
        document.getElementById('imgStatus').textContent = 'Error: ' + err.message;
      } finally {
        busy = false; setButtonsDisabled(false); refreshStatus();
      }
    }
    refreshStatus();
    setInterval(refreshStatus, 1000);
  </script>
</body>
</html>
)rawliteral";

void start_server() {
    if (server_started) {
        return;
    }

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send_P(200, "text/html", index_html);
    });

    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        unsigned long uptime_ms = millis();
        String json = "{\"device\":\"" + device_label +
                      "\",\"millis\":" + String(uptime_ms) +
                      ",\"uptime_s\":" + String(uptime_ms / 1000UL);
        if (status_provider) {
            json += status_provider();
        }
        json += "}";
        request->send(200, "application/json", json);
    });

    server.on("/api/action", HTTP_POST, [](AsyncWebServerRequest *request) {
        String cmd;
        if (request->hasParam("cmd", true)) {
            cmd = request->getParam("cmd", true)->value();
        } else if (request->hasParam("cmd")) {
            cmd = request->getParam("cmd")->value();
        }

        String message;
        bool ok = false;
        if (!cmd.length()) {
            message = "Missing cmd";
        } else if (command_handler) {
            ok = command_handler(cmd, message);
        } else {
            message = "No command handler";
        }

        String json = String("{\"ok\":") + (ok ? "true" : "false") +
                      ",\"message\":\"" + json_escape(message) + "\"}";
        request->send(ok ? 200 : 400, "application/json", json);
    });

    server.on("/api/upload", HTTP_POST,
        [](AsyncWebServerRequest *request) {
            request->send(200, "application/json", "{\"ok\":true}");
        },
        NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            (void)request;
            if (image_handler) {
                image_handler(data, len, index, total);
            }
        }
    );

    server.begin();
    server_started = true;
}

}  // namespace

bool wifi_init(const char* ssid, const char* password, const char* device_name) {
    if (!ssid || !password) {
        Serial.println("WiFi init failed: missing credentials");
        return false;
    }

    if (device_name) {
        device_label = device_name;
    }

    WiFi.mode(WIFI_STA);
    WiFi.setHostname(device_label.c_str());
    WiFi.begin(ssid, password);

    Serial.print("Connecting WiFi");
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000UL) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi connect failed");
        return false;
    }

    start_server();
    return true;
}

bool wifi_is_connected() {
    return WiFi.status() == WL_CONNECTED;
}

String wifi_ip_string() {
    return WiFi.localIP().toString();
}

void wifi_print_status(Stream& out) {
    out.print("WiFi: ");
    out.println(wifi_is_connected() ? "connected" : "disconnected");
    if (wifi_is_connected()) {
        out.print("IP: ");
        out.println(wifi_ip_string());
    }
}

void wifi_set_command_handler(WifiCommandHandler handler) {
    command_handler = handler;
}

void wifi_set_status_provider(WifiStatusProvider provider) {
    status_provider = provider;
}

void wifi_set_image_handler(WifiImageHandler handler) {
    image_handler = handler;
}
