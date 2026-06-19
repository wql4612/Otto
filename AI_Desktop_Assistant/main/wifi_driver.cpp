#include "wifi_driver.h"
#include "esp_camera.h"

// ══════════════════════════════════════════════
// JSON 转义工具
// ══════════════════════════════════════════════
String json_escape(const String& input) {
    String out;
    out.reserve(input.length() + 8);
    for (size_t i = 0; i < input.length(); ++i) {
        char c = input[i];
        switch (c) {
            case '\"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

// ══════════════════════════════════════════════
// 内部状态
// ══════════════════════════════════════════════
namespace {

WebServer g_server(80);
bool g_server_started = false;

String g_device_label = "ai-desktop-assistant";
WifiCommandHandler   g_cmd_handler = nullptr;
WifiStatusProvider   g_status_provider = nullptr;
WifiImageHandler     g_image_handler = nullptr;
WifiWSMessageHandler g_ws_msg_handler = nullptr;

// WebSocket 客户端池
#define MAX_WS_CLIENTS 4
WiFiClient g_ws_clients[MAX_WS_CLIENTS];

// MJPEG 流边界
const char* STREAM_BOUNDARY = "FRAME_BOUNDARY";
const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace; boundary=FRAME_BOUNDARY";

// 内嵌默认首页（当 LittleFS 不可用时）
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>AI Desktop</title><style>
:root{--bg:#0f1418;--panel:#182028;--text:#eff6fb;--muted:#94a7b8;--accent:#44c18f;}
*{box-sizing:border-box}body{font-family:Arial,sans-serif;margin:0;padding:20px;
background:linear-gradient(160deg,#0d1115,#121a21);color:var(--text)}
.wrap{max-width:980px;margin:0 auto}
.card{padding:18px;border-radius:16px;background:var(--panel);margin-bottom:14px}
h1{font-size:24px;margin:0 0 6px}h2{font-size:16px;margin:0 0 10px;color:var(--muted)}
.buttons{display:grid;grid-template-columns:repeat(auto-fill,minmax(120px,1fr));gap:8px}
button{border:0;border-radius:10px;padding:10px 12px;background:var(--accent);color:#07110c;
font-weight:700;cursor:pointer;font-size:13px}button.alt{background:#ff9f43;color:#231300}
button:disabled{opacity:.5}
.log{padding:10px;border-radius:10px;background:#1f2932;min-height:60px;white-space:pre-wrap;
word-break:break-word;font-size:12px;margin-top:8px;max-height:200px;overflow:auto}
.fps{display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--accent);
margin-right:6px;animation:pulse 1s infinite}@keyframes pulse{50%{opacity:.2}}
img{max-width:100%;border-radius:12px}
</style></head><body><div class="wrap">
<div class="card"><h1>AI Desktop Assistant</h1>
<p style="color:var(--muted)">IP: <span id="ip">-</span> | Uptime: <span id="uptime">-</span></p>
<div class="log" id="status">-</div></div>
<div class="card"><h2>Camera</h2>
<img id="stream" src="/stream" alt="Camera" onerror="this.style.display='none'">
<div><span class="fps"></span><span id="camStatus">Waiting...</span></div></div>
<div class="card"><h2>Quick Actions</h2><div class="buttons">
<button onclick="run('ping')">Ping</button>
<button onclick="run('capture')">Capture</button>
<button onclick="run('mic_diag')" class="alt">Mic Diag</button>
<button onclick="run('screen_demo')">Screen</button>
<button onclick="run('rf_on')">RF ON</button>
<button onclick="run('rf_off')" class="alt">RF OFF</button>
<button onclick="run('servo180_90')">Servo 90</button>
<button onclick="run('loopback')">Loopback</button>
</div></div></div>
<script>
async function refresh(){try{const r=await fetch('/api/status');const d=await r.json();
document.getElementById('ip').textContent=d.ip||'-';
document.getElementById('uptime').textContent=(d.uptime_s||0)+' s';
document.getElementById('status').textContent=JSON.stringify(d,null,2);
document.getElementById('camStatus').textContent=d.camera||'?';
}catch(e){document.getElementById('uptime').textContent='offline'}}
async function run(cmd){try{const r=await fetch('/api/action?cmd='+encodeURIComponent(cmd),
{method:'POST'});const d=await r.json();document.getElementById('status').textContent=
JSON.stringify(d,null,2)}catch(e){}refresh()}
refresh();setInterval(refresh,2000);
</script></body></html>
)rawliteral";

// ══════════════════════════════════════════════
// WebSocket 帧发送（手动构造，无外部依赖）
// ══════════════════════════════════════════════
void ws_send_frame(WiFiClient& client, const uint8_t* data, size_t len, uint8_t opcode = 0x81) {
    if (!client || !client.connected()) return;
    uint8_t header[10];
    size_t header_len = 2;
    header[0] = opcode;
    if (len <= 125) {
        header[1] = len;
    } else if (len <= 65535) {
        header[1] = 126;
        header[2] = (len >> 8) & 0xFF;
        header[3] = len & 0xFF;
        header_len = 4;
    } else {
        header[1] = 127;
        for (int i = 0; i < 8; i++)
            header[2 + i] = (len >> (56 - i * 8)) & 0xFF;
        header_len = 10;
    }
    client.write(header, header_len);
    client.write(data, len);
    client.flush();
}

void ws_send_text(WiFiClient& client, const String& msg) {
    ws_send_frame(client, (const uint8_t*)msg.c_str(), msg.length(), 0x81);
}

// ══════════════════════════════════════════════
// WebSocket 广播
// ══════════════════════════════════════════════
void ws_broadcast(const String& msg) {
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (g_ws_clients[i] && g_ws_clients[i].connected()) {
            ws_send_text(g_ws_clients[i], msg);
        }
    }
}

void ws_broadcast_status_json(const String& json) {
    ws_broadcast(json);
}

// ══════════════════════════════════════════════
// WebSocket 握手 + 接收处理
// ══════════════════════════════════════════════
bool ws_try_accept(WiFiClient& client, const String& req) {
    // 找 Sec-WebSocket-Key
    int key_idx = req.indexOf("Sec-WebSocket-Key:");
    if (key_idx < 0) return false;
    key_idx += 19;
    int key_end = req.indexOf('\r', key_idx);
    String key = req.substring(key_idx, key_end);
    key.trim();
    key += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    uint8_t sha1[20];
    mbedtls_sha1_context ctx;
    mbedtls_sha1_init(&ctx);
    mbedtls_sha1_starts(&ctx);
    mbedtls_sha1_update(&ctx, (const uint8_t*)key.c_str(), key.length());
    mbedtls_sha1_finish(&ctx, sha1);
    mbedtls_sha1_free(&ctx);

    String accept = base64::encode(sha1, 20);

    client.println("HTTP/1.1 101 Switching Protocols");
    client.println("Upgrade: websocket");
    client.println("Connection: Upgrade");
    client.println("Sec-WebSocket-Accept: " + accept);
    client.println();
    client.flush();
    return true;
}

bool ws_read_frame(WiFiClient& client, uint8_t& opcode, uint8_t*& payload, size_t& len) {
    if (!client.available()) return false;

    uint8_t hdr[2];
    if (client.read(hdr, 2) != 2) return false;

    opcode = hdr[0] & 0x0F;
    bool masked = (hdr[1] & 0x80) != 0;
    len = hdr[1] & 0x7F;

    if (len == 126) {
        uint8_t ext[2];
        if (client.read(ext, 2) != 2) return false;
        len = ((size_t)ext[0] << 8) | ext[1];
    } else if (len == 127) {
        uint8_t ext[8];
        if (client.read(ext, 8) != 8) return false;
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | ext[i];
    }

    uint8_t mask_key[4] = {};
    if (masked) {
        if (client.read(mask_key, 4) != 4) return false;
    }

    payload = (uint8_t*)malloc(len + 1);
    if (!payload) return false;
    if (client.read(payload, len) != len) {
        free(payload);
        return false;
    }

    if (masked) {
        for (size_t i = 0; i < len; i++)
            payload[i] ^= mask_key[i & 3];
    }
    payload[len] = '\0';
    return true;
}

void ws_handle_clients() {
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (!g_ws_clients[i] || !g_ws_clients[i].connected()) {
            if (g_ws_clients[i]) { g_ws_clients[i].stop(); g_ws_clients[i] = WiFiClient(); }
            continue;
        }

        uint8_t opcode;
        uint8_t* payload;
        size_t len;
        while (ws_read_frame(g_ws_clients[i], opcode, payload, len)) {
            if (opcode == 0x08) {
                free(payload);
                g_ws_clients[i].stop();
                g_ws_clients[i] = WiFiClient();
                break;
            }
            if (opcode == 0x09) {
                ws_send_frame(g_ws_clients[i], payload, len, 0x8A);
                free(payload);
                continue;
            }
            if (opcode == 0x01 && g_ws_msg_handler) {
                String msg((const char*)payload);
                g_ws_msg_handler(msg);
            }
            free(payload);
        }
    }
}

bool ws_add_client(WiFiClient& client) {
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (!g_ws_clients[i] || !g_ws_clients[i].connected()) {
            g_ws_clients[i] = client;
            return true;
        }
    }
    return false;
}

// ══════════════════════════════════════════════
// MJPEG 摄像头流
// ══════════════════════════════════════════════
void handle_stream() {
    WiFiClient client = g_server.client();
    if (!client.connected()) return;

    client.println("HTTP/1.1 200 OK");
    client.print("Content-Type: "); client.println(STREAM_CONTENT_TYPE);
    client.println("Access-Control-Allow-Origin: *");
    client.println("Cache-Control: no-cache");
    client.println("Connection: close");
    client.println();

    unsigned long last_frame = 0;
    while (client.connected()) {
        if (millis() - last_frame < 33) { delay(1); continue; }  // ~30 FPS

        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) { delay(10); continue; }

        client.print("--"); client.println(STREAM_BOUNDARY);
        client.println("Content-Type: image/jpeg");
        client.printf("Content-Length: %u\r\n\r\n", fb->len);
        client.write(fb->buf, fb->len);
        client.print("\r\n");

        esp_camera_fb_return(fb);
        last_frame = millis();
    }
}

// ══════════════════════════════════════════════
// LittleFS 静态文件服务
// ══════════════════════════════════════════════
void serve_file(const char* path, const char* content_type) {
    if (!LittleFS.exists(path)) {
        g_server.send(404, "text/plain", "File not found");
        return;
    }
    File f = LittleFS.open(path, "r");
    if (!f) {
        g_server.send(404, "text/plain", "File not found");
        return;
    }
    g_server.streamFile(f, content_type);
    f.close();
}

// ══════════════════════════════════════════════
// 路由注册
// ══════════════════════════════════════════════
void start_webserver() {
    if (g_server_started) return;

    // 首页
    g_server.on("/", HTTP_GET, []() {
        if (LittleFS.exists("/control_panel.html")) {
            serve_file("/control_panel.html", "text/html; charset=utf-8");
        } else {
            g_server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
        }
    });

    // 摄像头流
    g_server.on("/stream", HTTP_GET, handle_stream);

    // API: 状态查询
    g_server.on("/api/status", HTTP_GET, []() {
        unsigned long uptime_ms = millis();
        String json = "{\"device\":\"" + g_device_label +
                      "\",\"ip\":\"" + WiFi.localIP().toString() +
                      "\",\"millis\":" + String(uptime_ms) +
                      ",\"uptime_s\":" + String(uptime_ms / 1000UL);
        if (g_status_provider) {
            json += g_status_provider();
        }
        json += "}";
        g_server.send(200, "application/json", json);
    });

    // API: 命令执行
    g_server.on("/api/action", HTTP_POST, []() {
        String cmd;
        if (g_server.hasArg("cmd")) {
            cmd = g_server.arg("cmd");
        }

        String message;
        bool ok = false;
        if (!cmd.length()) {
            message = "Missing cmd";
        } else if (g_cmd_handler) {
            ok = g_cmd_handler(cmd, message);
        } else {
            message = "No command handler";
        }

        String json = "{\"ok\":" + String(ok ? "true" : "false") +
                      ",\"message\":\"" + json_escape(message) + "\"}";
        g_server.send(ok ? 200 : 400, "application/json", json);
    });

    // API: 图片上传
    g_server.on("/api/upload", HTTP_POST,
        []() { g_server.send(200, "application/json", "{\"ok\":true}"); },
        []() {
            HTTPUpload& upload = g_server.upload();
            if (g_image_handler) {
                g_image_handler(upload.buf, upload.currentSize,
                               upload.totalSize - upload.currentSize,
                               upload.totalSize);
            }
        }
    );

    // WebSocket 升级
    g_server.on("/ws", HTTP_GET, []() {
        WiFiClient client = g_server.client();
        String req;
        while (client.available()) {
            req += (char)client.read();
        }
        if (ws_try_accept(client, req)) {
            ws_add_client(client);
        }
    });

    // 表情图片
    g_server.on("/faces/", HTTP_GET, []() {
        String path = g_server.uri();
        serve_file(path.c_str(), "image/jpeg");
    });

    // 404 → 回退首页
    g_server.onNotFound([]() {
        String uri = g_server.uri();
        if (uri == "/ws") return;
        if (uri.startsWith("/faces/")) {
            serve_file(uri.c_str(), "image/jpeg");
        } else if (LittleFS.exists(uri)) {
            String ct = "application/octet-stream";
            if (uri.endsWith(".html")) ct = "text/html; charset=utf-8";
            else if (uri.endsWith(".css")) ct = "text/css";
            else if (uri.endsWith(".js")) ct = "application/javascript";
            else if (uri.endsWith(".jpg") || uri.endsWith(".jpeg")) ct = "image/jpeg";
            else if (uri.endsWith(".png")) ct = "image/png";
            else if (uri.endsWith(".svg")) ct = "image/svg+xml";
            serve_file(uri.c_str(), ct.c_str());
        } else {
            if (LittleFS.exists("/control_panel.html"))
                serve_file("/control_panel.html", "text/html; charset=utf-8");
            else
                g_server.send(404, "text/plain", "Not Found");
        }
    });

    g_server.begin();
    g_server_started = true;
    Serial.println("[Server] Started on port 80");
}

}  // namespace

// ══════════════════════════════════════════════
// 公共 API 实现
// ══════════════════════════════════════════════

bool wifi_init(const char* ssid, const char* password, const char* device_name) {
    if (!ssid || !password) {
        Serial.println("[WiFi] Missing credentials");
        return false;
    }
    if (device_name) g_device_label = device_name;

    WiFi.mode(WIFI_STA);
    WiFi.setHostname(g_device_label.c_str());
    WiFi.begin(ssid, password);

    Serial.print("[WiFi] Connecting");
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000UL) {
        delay(500); Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] Connect failed");
        return false;
    }

    Serial.printf("[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    start_webserver();
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
        out.print("IP: "); out.println(wifi_ip_string());
    }
}

bool wifi_littlefs_init() {
    if (!LittleFS.begin(true)) {
        Serial.println("[LittleFS] Mount failed!");
        return false;
    }
    Serial.printf("[LittleFS] Total: %d KB, Used: %d KB\n",
                  LittleFS.totalBytes() / 1024,
                  LittleFS.usedBytes() / 1024);
    return true;
}

void wifi_set_command_handler(WifiCommandHandler handler)   { g_cmd_handler = handler; }
void wifi_set_status_provider(WifiStatusProvider provider)  { g_status_provider = provider; }
void wifi_set_image_handler(WifiImageHandler handler)       { g_image_handler = handler; }
void wifi_set_ws_message_handler(WifiWSMessageHandler h)    { g_ws_msg_handler = h; }

void wifi_handle_client() {
    g_server.handleClient();
    ws_handle_clients();
}
