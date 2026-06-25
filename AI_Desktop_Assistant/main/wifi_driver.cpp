#include "wifi_driver.h"
#include "debug_log.h"
#include <mbedtls/sha1.h>
#include <base64.h>

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
String g_last_ws_remote_ip = "";

String g_device_label = "ai-desktop-assistant";
WifiCommandHandler   g_cmd_handler = nullptr;
WifiStatusProvider   g_status_provider = nullptr;
WifiImageHandler     g_image_handler = nullptr;
WifiWSMessageHandler g_ws_msg_handler = nullptr;
WifiUiConfig         g_ui_config = { false, false };

// 内嵌默认首页（当 LittleFS 不可用时）
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Otto Debug Panel</title><style>
:root{--bg:#0b0f14;--panel:#131a22;--panel2:#1a2430;--text:#d7e2ee;--muted:#7f93a8;--ok:#69c17d;--line:#223041;--accent:#e3a84a;}
*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at top,#16202b,#0b0f14 55%);color:var(--text);font:14px "Microsoft YaHei",system-ui,sans-serif}
.wrap{max-width:1100px;margin:0 auto;padding:18px}.top{display:flex;justify-content:space-between;align-items:center;margin-bottom:14px}
.title{font-size:24px;font-weight:700}.sub{color:var(--muted);font-size:12px}.badge{display:inline-flex;align-items:center;gap:8px;padding:6px 10px;border:1px solid var(--line);border-radius:999px;background:var(--panel)}
.dot{width:8px;height:8px;border-radius:50%;background:#cc5b5b}.dot.on{background:var(--ok);box-shadow:0 0 10px var(--ok)}
.grid{display:grid;grid-template-columns:1.1fr .9fr;gap:14px}.card{background:var(--panel);border:1px solid var(--line);border-radius:16px;padding:16px}
.card h2{margin:0 0 12px;font-size:13px;color:var(--muted);letter-spacing:1px;text-transform:uppercase}.status{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px}
.kv{background:var(--panel2);border-radius:12px;padding:12px}.k{font-size:11px;color:var(--muted);margin-bottom:6px}.v{font-size:15px;font-weight:600;word-break:break-word}
.devices{display:grid;gap:8px;margin-top:12px}.dev{display:flex;justify-content:space-between;align-items:center;background:var(--panel2);padding:10px 12px;border-radius:12px}
.state{padding:2px 8px;border-radius:999px;border:1px solid var(--line);font-size:12px}.state.on{color:#111;background:var(--accent);border-color:var(--accent)}
.buttons{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:8px}.buttons button{border:1px solid var(--line);background:var(--panel2);color:var(--text);border-radius:12px;padding:12px 10px;font-weight:600;cursor:pointer}
.buttons button:hover{border-color:#3a526b}.log{margin-top:12px;max-height:360px;overflow:auto;background:#0f151d;border-radius:12px;padding:10px}
.entry{display:flex;gap:10px;padding:5px 0;border-bottom:1px solid rgba(255,255,255,.05)}.entry:last-child{border-bottom:0}.time{color:var(--muted);font:12px monospace;min-width:72px}.msg{white-space:pre-wrap;word-break:break-word}
pre{margin:0;white-space:pre-wrap;word-break:break-word;color:#bdd0e5}.hint{font-size:12px;color:var(--muted);margin-top:10px}
@media (max-width:800px){.grid{grid-template-columns:1fr}.buttons{grid-template-columns:1fr}.status{grid-template-columns:1fr}}
</style></head><body><div class="wrap">
<div class="top"><div><div class="title">Otto Debug Panel</div><div class="sub">HTTP only · no WebSocket · no video stream</div></div>
<div class="badge"><span class="dot" id="httpDot"></span><span id="httpLabel">连接中</span></div></div>
<div class="grid">
<div class="card"><h2>System Status</h2>
<div class="status">
<div class="kv"><div class="k">IP</div><div class="v" id="ip">-</div></div>
<div class="kv"><div class="k">Uptime</div><div class="v" id="uptime">-</div></div>
<div class="kv"><div class="k">Last Result</div><div class="v" id="lastResult">-</div></div>
<div class="kv"><div class="k">Person</div><div class="v" id="person">-</div></div>
<div class="kv"><div class="k">Wake Listen</div><div class="v" id="wakeGate">-</div></div>
</div>
<div class="devices">
<div class="dev"><span>台灯</span><span class="state" id="lightState">关</span></div>
<div class="dev"><span>风扇</span><span class="state" id="fanState">关</span></div>
<div class="dev"><span>音响</span><span class="state" id="speakerState">关</span></div>
</div>
<div class="hint">页面只用于日志输出和小测试，不依赖 WebSocket。</div>
</div>
<div class="card"><h2>Debug Actions</h2>
<div class="buttons">
<button onclick="runCmd('ping','Ping')">Ping</button>
<button onclick="runCmd('wifi_status','WiFi 状态')">WiFi 状态</button>
<button onclick="runCmd('mic_diag','麦克风诊断')">麦克风诊断</button>
<button onclick="runCmd('screen_demo','屏幕测试')">屏幕测试</button>
<button onclick="runCmd('capture','拍照测试')">拍照测试</button>
<button onclick="runCmd('qwen_status','Qwen 状态')">Qwen 状态</button>
<button onclick="runCmd('qwen_recognize','Qwen 识别')">Qwen 识别</button>
<button onclick="runCmd('qwen_voice_test','Qwen 语音测试')">Qwen 语音测试</button>
<button onclick="runCmd('qwen_voice_sample','官方样例语音')">官方样例语音</button>
<button onclick="runCmd('wake_test','本地唤醒测试')">本地唤醒测试</button>
<button onclick="runCmd('audio_test_on','应答音频 开')">应答音频 开</button>
<button onclick="runCmd('audio_test_off','应答音频 关')">应答音频 关</button>
<button onclick="runCmd('audio_test_wake','唤醒提示音')">唤醒提示音</button>
<button onclick="runCmd('rf_on','RF 开')">RF 开</button>
<button onclick="runCmd('rf_off','RF 关')">RF 关</button>
<button onclick="runCmd('servo180_90','舵机 80°')">舵机 80°</button>
<button onclick="runCmd('face_idle','待机表情')">待机表情</button>
</div>
</div></div>
<div class="card" style="margin-top:14px"><h2>Debug Log</h2><div class="log" id="log"></div></div>
<script>
let lastDebugLog='';
let lastBootId=null;
function addLogLine(text){if(!text)return;const log=document.getElementById('log');const row=document.createElement('div');row.className='entry';const t=new Date().toLocaleTimeString('zh-CN',{hour12:false});row.innerHTML='<span class="time">'+t+'</span><span class="msg"></span>';row.querySelector('.msg').textContent=text;log.appendChild(row);while(log.children.length>200)log.removeChild(log.firstChild);log.scrollTop=log.scrollHeight}
function clearLog(){document.getElementById('log').innerHTML=''}
function setConn(ok){document.getElementById('httpDot').classList.toggle('on',ok);document.getElementById('httpLabel').textContent=ok?'HTTP 已连接':'HTTP 未连接'}
function setState(id,on){const el=document.getElementById(id);el.textContent=on?'开':'关';el.classList.toggle('on',!!on)}
function applyStatus(s){document.getElementById('ip').textContent=s.ip||'-';document.getElementById('uptime').textContent=(s.uptime_s||0)+' s';document.getElementById('lastResult').textContent=s.last_result||'-';document.getElementById('person').textContent=s.person?'检测到人':'未检测到人';document.getElementById('wakeGate').textContent=((s.wake_energy||0)+(s.wake_listener?' · 监听中':' · 未监听'));setState('lightState',s.light);setState('fanState',s.fan);setState('speakerState',s.speaker);if(s.boot_id!==undefined&&s.boot_id!==lastBootId){lastBootId=s.boot_id;lastDebugLog='';clearLog()}if(typeof s.debug_log==='string'){if(!lastDebugLog||!s.debug_log.startsWith(lastDebugLog)){clearLog();s.debug_log.split('\n').map(x=>x.trim()).filter(Boolean).forEach(addLogLine)}else{s.debug_log.slice(lastDebugLog.length).split('\n').map(x=>x.trim()).filter(Boolean).forEach(addLogLine)}lastDebugLog=s.debug_log}}
async function fetchStatus(){const r=await fetch('/api/status');if(!r.ok)throw new Error('status');return r.json()}
async function refresh(){try{const s=await fetchStatus();setConn(true);applyStatus(s)}catch(e){setConn(false)}}
async function runCmd(cmd,label){
try{
const r=await fetch('/api/action?cmd='+encodeURIComponent(cmd),{method:'POST'});
let data=null;
let raw='';
try{raw=await r.text();data=raw?JSON.parse(raw):null}catch(_){}
if(!r.ok){
const msg=(data&&data.message)||raw||('HTTP '+r.status);
throw new Error(msg);
}
addLogLine(label+' · '+((data&&data.message)||'OK'));
await refresh()
}catch(e){
addLogLine(label+' · '+(e&&e.message?e.message:'HTTP 命令失败'))
}}
refresh();setInterval(refresh,2000);
</script></body></html>
)rawliteral";

void add_cors_headers() {
    g_server.sendHeader("Access-Control-Allow-Origin", "*");
    g_server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    g_server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    g_server.sendHeader("Access-Control-Max-Age", "600");
}

void send_options_response() {
    add_cors_headers();
    g_server.send(204, "text/plain", "");
}

void send_text_response(int code, const char* content_type, const String& body) {
    add_cors_headers();
    g_server.send(code, content_type, body);
}

void send_json_response(int code, const String& json) {
    send_text_response(code, "application/json", json);
}

}  // namespace

// WebSocket 客户端池
#define MAX_WS_CLIENTS 4
WiFiClient g_ws_clients[MAX_WS_CLIENTS];

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

namespace {

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
            g_last_ws_remote_ip = client.remoteIP().toString();
            return true;
        }
    }
    return false;
}

// ══════════════════════════════════════════════
// ══════════════════════════════════════════════
// LittleFS 静态文件服务
// ══════════════════════════════════════════════
void serve_file(const char* path, const char* content_type) {
    if (!LittleFS.exists(path)) {
        send_text_response(404, "text/plain", "File not found");
        return;
    }
    File f = LittleFS.open(path, "r");
    if (!f) {
        send_text_response(404, "text/plain", "File not found");
        return;
    }
    add_cors_headers();
    g_server.streamFile(f, content_type);
    f.close();
}

// ══════════════════════════════════════════════
// 路由注册
// ══════════════════════════════════════════════
void start_webserver() {
    if (g_server_started) return;

    g_server.on("/", HTTP_OPTIONS, send_options_response);
    g_server.on("/", HTTP_GET, []() {
        if (g_ui_config.serve_embedded_ui) {
            add_cors_headers();
            g_server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
            return;
        }

        String body = "{\"ok\":true,\"mode\":\"api-only\",\"message\":\"ESP32 API ready. Use /api/status and /api/action from the desktop panel.\"}";
        send_json_response(200, body);
    });

    // API: 状态查询
    g_server.on("/api/status", HTTP_OPTIONS, send_options_response);
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
        send_json_response(200, json);
    });

    // API: 命令执行
    g_server.on("/api/action", HTTP_OPTIONS, send_options_response);
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
        if (!ok && message.length()) {
            debug_log_append(String("[HTTP] ") + cmd + " failed: " + message, "system");
        }
        send_json_response(ok ? 200 : 400, json);
    });

    // API: 图片上传
    g_server.on("/api/upload", HTTP_OPTIONS, send_options_response);
    g_server.on("/api/upload", HTTP_POST,
        []() { send_json_response(200, "{\"ok\":true}"); },
        []() {
            HTTPUpload& upload = g_server.upload();
            if (g_image_handler) {
                g_image_handler(upload.buf, upload.currentSize,
                               upload.totalSize - upload.currentSize,
                               upload.totalSize);
            }
        }
    );

    // 表情图片
    g_server.on("/faces/", HTTP_OPTIONS, send_options_response);
    g_server.on("/faces/", HTTP_GET, []() {
        if (!g_ui_config.serve_static_files) {
            send_text_response(404, "text/plain", "Static file hosting disabled");
            return;
        }
        String path = g_server.uri();
        serve_file(path.c_str(), "image/jpeg");
    });

    // 404 → 回退首页
    g_server.onNotFound([]() {
        String uri = g_server.uri();
        if (g_server.method() == HTTP_OPTIONS) {
            send_options_response();
        } else if (uri.startsWith("/faces/") && g_ui_config.serve_static_files) {
            serve_file(uri.c_str(), "image/jpeg");
        } else if (g_ui_config.serve_static_files && LittleFS.exists(uri)) {
            String ct = "application/octet-stream";
            if (uri.endsWith(".html")) ct = "text/html; charset=utf-8";
            else if (uri.endsWith(".css")) ct = "text/css";
            else if (uri.endsWith(".js")) ct = "application/javascript";
            else if (uri.endsWith(".jpg") || uri.endsWith(".jpeg")) ct = "image/jpeg";
            else if (uri.endsWith(".png")) ct = "image/png";
            else if (uri.endsWith(".svg")) ct = "image/svg+xml";
            serve_file(uri.c_str(), ct.c_str());
        } else if (g_ui_config.serve_embedded_ui) {
            add_cors_headers();
            g_server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
        } else {
            send_text_response(404, "application/json",
                               "{\"ok\":false,\"message\":\"Not found\",\"mode\":\"api-only\"}");
        }
    });

    g_server.begin();
    g_server_started = true;
    debug_log_append("[Server] Started on port 80", "system");
}

}  // namespace

// ══════════════════════════════════════════════
// 公共 API 实现
// ══════════════════════════════════════════════

bool wifi_init(const char* ssid, const char* password, const char* device_name) {
    if (!ssid || !password) {
        debug_log_append("[WiFi] Missing credentials", "system");
        return false;
    }
    if (device_name) g_device_label = device_name;

    WiFi.mode(WIFI_STA);
    WiFi.setHostname(g_device_label.c_str());
    WiFi.begin(ssid, password);

    debug_log_printf("system", "[WiFi] Connecting to %s", ssid);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000UL) {
        delay(500);
    }

    if (WiFi.status() != WL_CONNECTED) {
        debug_log_append("[WiFi] Connect failed", "system");
        return false;
    }

    debug_log_printf("system", "[WiFi] Connected! IP: %s", WiFi.localIP().toString().c_str());
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
        debug_log_append("[LittleFS] Mount failed!", "system");
        return false;
    }
    debug_log_printf("system", "[LittleFS] Total: %d KB, Used: %d KB",
                     LittleFS.totalBytes() / 1024,
                     LittleFS.usedBytes() / 1024);
    return true;
}

void wifi_set_ui_config(const WifiUiConfig& config) {
    g_ui_config = config;
}

void wifi_set_command_handler(WifiCommandHandler handler)   { g_cmd_handler = handler; }
void wifi_set_status_provider(WifiStatusProvider provider)  { g_status_provider = provider; }
void wifi_set_image_handler(WifiImageHandler handler)       { g_image_handler = handler; }
void wifi_set_ws_message_handler(WifiWSMessageHandler h)    { g_ws_msg_handler = h; }
String wifi_last_ws_remote_ip()                             { return g_last_ws_remote_ip; }

void wifi_handle_client() {
    g_server.handleClient();
    ws_handle_clients();
}
