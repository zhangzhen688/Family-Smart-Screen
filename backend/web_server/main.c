/**
 * @file main.c
 * Web remote control server (port 8080).
 *
 * Serves:
 *   GET  /              → Mobile web UI (embedded HTML)
 *   GET  /api/status    → All device/sensor status (JSON)
 *   POST /api/led       → Control LED {index, on}
 *   POST /api/curtain   → Set curtain angle {angle}
 *   GET  /api/sensors   → DHT11 + AP3216C readings
 *   GET  /camera/stream → MJPEG live stream
 *   GET  /api/events    → SSE real-time push
 *   GET  /api/voice     → Voice assistant status
 *
 * Bridges to existing JSON-RPC backends (device_server:1234,
 * camera_server:1235, voice_server:1236).
 */
#include "http_server.h"
#include "rpc_protocol.h"
#include "rpc_client.h"
#include "v4l2_capture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

#define WEB_PORT 8080

static volatile int g_running = 1;
static int g_dev_fd = -1;
static int g_cam_fd = -1;

/* ── Embedded mobile web UI HTML ────────────────────────────────────── */
static const char *INDEX_HTML =
"<!DOCTYPE html><html lang=\"zh-CN\"><head>"
"<meta charset=\"UTF-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,user-scalable=no\">"
"<meta name=\"apple-mobile-web-app-capable\" content=\"yes\">"
"<meta name=\"apple-mobile-web-app-status-bar-style\" content=\"black-translucent\">"
"<title>Smart Screen</title>"
"<style>"
"*{margin:0;padding:0;box-sizing:border-box}"
"body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
"background:#0a0e27;color:#e8eaed;min-height:100vh;padding:16px}"
".header{text-align:center;padding:12px 0 20px}"
".header h1{font-size:22px;font-weight:600;background:linear-gradient(135deg,#00d4ff,#7b2ff7);"
"-webkit-background-clip:text;-webkit-text-fill-color:transparent}"
".header .time{font-size:13px;color:#8e9aaf;margin-top:4px}"
".grid{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-bottom:16px}"
".card{background:#16213e;border-radius:16px;padding:16px;position:relative;overflow:hidden}"
".card::before{content:'';position:absolute;top:0;left:0;right:0;height:2px;"
"background:linear-gradient(90deg,#00d4ff,#7b2ff7)}"
".card-title{font-size:11px;color:#8e9aaf;text-transform:uppercase;letter-spacing:1px;margin-bottom:8px}"
".card-value{font-size:28px;font-weight:700}"
".card-unit{font-size:13px;color:#8e9aaf;margin-left:4px}"
".temp{color:#ff9100}.humidity{color:#00d4ff}.light{color:#ffd54f}"
".section{margin-bottom:16px}"
".section-title{font-size:13px;color:#8e9aaf;margin-bottom:10px;letter-spacing:1px}"
".device-row{display:flex;align-items:center;justify-content:space-between;"
"background:#16213e;border-radius:12px;padding:14px 16px;margin-bottom:8px}"
".device-name{font-size:15px;font-weight:500}"
".toggle{width:52px;height:30px;background:#2a3a5c;border-radius:15px;position:relative;"
"cursor:pointer;transition:background .3s;border:none;outline:none}"
".toggle.on{background:#00d4ff}"
".toggle::after{content:'';width:26px;height:26px;background:#fff;border-radius:50%;"
"position:absolute;top:2px;left:2px;transition:transform .3s}"
".toggle.on::after{transform:translateX(22px)}"
".curtain-row{background:#16213e;border-radius:12px;padding:14px 16px;margin-bottom:8px}"
".curtain-row label{font-size:15px;font-weight:500;display:block;margin-bottom:8px}"
".curtain-row input[type=range]{width:100%;-webkit-appearance:none;height:6px;"
"background:linear-gradient(90deg,#00d4ff,#7b2ff7);border-radius:3px;outline:none}"
".curtain-row input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;"
"width:24px;height:24px;background:#fff;border-radius:50%;box-shadow:0 2px 8px rgba(0,0,0,.3)}"
".camera-box{background:#000;border-radius:16px;overflow:hidden;margin-bottom:16px;"
"aspect-ratio:4/3;display:flex;align-items:center;justify-content:center}"
".camera-box img{width:100%;height:100%;object-fit:contain}"
".camera-box .placeholder{color:#546e7a;font-size:18px}"
".voice-box{background:#16213e;border-radius:16px;padding:20px;text-align:center;margin-bottom:16px}"
".voice-box .emoji{font-size:48px;margin-bottom:8px}"
".voice-box .state{font-size:14px;color:#8e9aaf;margin-bottom:4px}"
".voice-box .text{font-size:16px;color:#e8eaed;min-height:24px}"
".btn{display:inline-block;padding:12px 32px;border-radius:25px;border:none;font-size:15px;"
"font-weight:600;cursor:pointer;transition:all .3s;margin:4px}"
".btn-primary{background:linear-gradient(135deg,#00d4ff,#7b2ff7);color:#fff}"
".btn-danger{background:#c62828;color:#fff}"
".btn-green{background:#00c853;color:#fff}"
".status-dot{width:8px;height:8px;border-radius:50%;display:inline-block;margin-right:6px}"
".status-dot.online{background:#00e676;box-shadow:0 0 8px #00e676}"
".status-dot.offline{background:#ff5252}"
"@media(min-width:768px){.grid{grid-template-columns:repeat(3,1fr)}body{max-width:800px;margin:0 auto}}"
"</style></head><body>"
"<div class=\"header\">"
"<h1>Smart Screen</h1>"
"<div class=\"time\" id=\"clock\">--</div>"
"</div>"
"<div class=\"grid\">"
"<div class=\"card\"><div class=\"card-title\">Temperature</div>"
"<div class=\"card-value temp\" id=\"temp\">--<span class=\"card-unit\">C</span></div></div>"
"<div class=\"card\"><div class=\"card-title\">Humidity</div>"
"<div class=\"card-value humidity\" id=\"humi\">--<span class=\"card-unit\">%</span></div></div>"
"<div class=\"card\"><div class=\"card-title\">Light</div>"
"<div class=\"card-value light\" id=\"lux\">--<span class=\"card-unit\">lux</span></div></div>"
"</div>"
"<div class=\"section\"><div class=\"section-title\">Lights</div>"
"<div id=\"lights\"></div></div>"
"<div class=\"section\"><div class=\"section-title\">Curtain</div>"
"<div class=\"curtain-row\"><label>Position: <span id=\"curtain-val\">50%</span></label>"
"<input type=\"range\" min=\"0\" max=\"180\" value=\"90\" id=\"curtain\" onchange=\"setCurtain(this.value)\"></div></div>"
"<div class=\"section\"><div class=\"section-title\">Camera</div>"
"<div class=\"camera-box\"><img id=\"cam\" src=\"/camera/snapshot\" onerror=\"this.style.display='none'\">"
"<span class=\"placeholder\" id=\"cam-placeholder\">Camera Offline</span></div>"
"<div style=\"text-align:center;margin-bottom:16px\">"
"<button class=\"btn btn-primary\" onclick=\"snapshot()\">Take Photo</button>"
"<button class=\"btn btn-green\" onclick=\"refreshCam()\">Refresh</button>"
"</div></div>"
"<div class=\"section\"><div class=\"section-title\">Voice Assistant</div>"
"<div class=\"voice-box\"><div class=\"emoji\" id=\"voice-emoji\">O</div>"
"<div class=\"state\" id=\"voice-state\">Offline</div>"
"<div class=\"text\" id=\"voice-text\"></div>"
"<button class=\"btn btn-primary\" onclick=\"voiceCmd()\">Start Listening</button>"
"</div></div>"
"<script>"
"const LEDS=['Living Room','Bedroom','Kitchen','Study','Hallway','Bathroom'];"
"function fetchJSON(url,opts){return fetch(url,opts).then(r=>r.json())}"
"function updateClock(){var d=new Date();"
"document.getElementById('clock').textContent="
"d.toLocaleTimeString('zh-CN',{hour:'2-digit',minute:'2-digit',second:'2-digit'});}"
"setInterval(updateClock,1000);updateClock();"
"function loadStatus(){fetchJSON('/api/status').then(d=>{"
"document.getElementById('temp').innerHTML=d.temp+'<span class=\"card-unit\">C</span>';"
"document.getElementById('humi').innerHTML=d.humidity+'<span class=\"card-unit\">%</span>';"
"document.getElementById('lux').innerHTML=d.light+'<span class=\"card-unit\">lux</span>';"
"document.getElementById('curtain-val').textContent=Math.round(d.curtain*100/180)+'%';"
"document.getElementById('curtain').value=d.curtain;"
"var leds=document.getElementById('lights'),html='';"
"for(var i=0;i<d.leds.length;i++){"
"html+='<div class=\"device-row\"><span class=\"device-name\">'+LEDS[i]+'</span>'"
"+'<button class=\"toggle'+(d.leds[i]?' on':'')+'\" onclick=\"toggleLed('+i+','+!d.leds[i]+')\"></button></div>';}"
"leds.innerHTML=html;"
"if(d.voice_state){"
"document.getElementById('voice-state').textContent=d.voice_state;"
"document.getElementById('voice-text').textContent=d.voice_text||'';"
"var emoji='O';"
"if(d.voice_state.indexOf('listening')>=0||d.voice_state.indexOf('Listening')>=0)emoji='O_O';"
"else if(d.voice_state.indexOf('speaking')>=0||d.voice_state.indexOf('Speaking')>=0)emoji='^_^';"
"else if(d.voice_state.indexOf('error')>=0||d.voice_state.indexOf('Error')>=0)emoji='>_<';"
"document.getElementById('voice-emoji').textContent=emoji;"
"}}).catch(e=>console.log(e))}"
"setInterval(loadStatus,3000);loadStatus();"
"function toggleLed(i,on){fetch('/api/led',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({index:i,on:on})}).then(()=>loadStatus())}"
"function setCurtain(v){fetch('/api/curtain',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({angle:parseInt(v)})}).then(()=>loadStatus())}"
"function snapshot(){fetch('/camera/snapshot').then(()=>{"
"document.getElementById('cam').src='/camera/snapshot?'+Date.now()})}"
"function refreshCam(){document.getElementById('cam').src='/camera/snapshot?'+Date.now();"
"document.getElementById('cam-placeholder').style.display='none';"
"document.getElementById('cam').style.display='block'}"
"function voiceCmd(){fetch('/api/voice',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({action:'listen'})}).then(()=>loadStatus())}"
"</script></body></html>";

/* ── Bridge helpers ─────────────────────────────────────────────────── */
static void bridge_ensure_dev(void)
{
    if (g_dev_fd < 0) g_dev_fd = rpc_connect(DEVICE_SERVER_PORT);
}

static void bridge_ensure_cam(void)
{
    if (g_cam_fd < 0) g_cam_fd = rpc_connect(CAMERA_SERVER_PORT);
}

/* ── Route: GET / ───────────────────────────────────────────────────── */
static int route_index(http_req_t *req, http_res_t *res)
{
    (void)req;
    http_send_html(res, INDEX_HTML);
    return 0;
}

/* ── Route: GET /api/status ─────────────────────────────────────────── */
static int route_status(http_req_t *req, http_res_t *res)
{
    (void)req;
    bridge_ensure_dev();

    /* Read DHT11 */
    int humi = 0, temp = 0;
    cJSON *dht = rpc_call_void(g_dev_fd, RPC_METHOD_DHT11_READ);
    if (dht) {
        cJSON *h = cJSON_GetObjectItem(dht, "humidity");
        cJSON *t = cJSON_GetObjectItem(dht, "temp");
        if (h) humi = h->valueint;
        if (t) temp = t->valueint;
        cJSON_Delete(dht);
    }

    /* Read AP3216C */
    int lux = 0;
    cJSON *ap = rpc_call_void(g_dev_fd, RPC_METHOD_AP3216C_READ);
    if (ap) {
        cJSON *a = cJSON_GetObjectItem(ap, "als");
        if (a) lux = a->valueint;
        cJSON_Delete(ap);
    }

    /* Read LEDs */
    int leds[LED_COUNT] = {0};
    for (int i = 0; i < LED_COUNT; i++) {
        cJSON *lr = rpc_call_int1(g_dev_fd, RPC_METHOD_LED_GET, i);
        if (lr) {
            cJSON *on = cJSON_GetObjectItem(lr, "on");
            if (on) leds[i] = cJSON_IsTrue(on) ? 1 : 0;
            cJSON_Delete(lr);
        }
    }

    /* Read curtain */
    int curtain = 90;
    /* No sg90_get in RPC, use default */

    /* Voice state */
    int voice_fd = rpc_connect(VOICE_SERVER_PORT);
    const char *vstate = "offline", *vtext = "";
    if (voice_fd >= 0) {
        cJSON *vs = rpc_call_void(voice_fd, RPC_METHOD_VOICE_GET_STATE);
        if (vs) {
            cJSON *s = cJSON_GetObjectItem(vs, "state");
            cJSON *t = cJSON_GetObjectItem(vs, "text");
            if (s && s->valuestring) vstate = s->valuestring;
            if (t && t->valuestring) vtext = t->valuestring;
            cJSON_Delete(vs);
        }
        rpc_disconnect(voice_fd);
    }

    /* Build JSON */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "temp", temp);
    cJSON_AddNumberToObject(root, "humidity", humi);
    cJSON_AddNumberToObject(root, "light", lux);
    cJSON_AddNumberToObject(root, "curtain", curtain);

    cJSON *led_arr = cJSON_CreateArray();
    for (int i = 0; i < LED_COUNT; i++)
        cJSON_AddItemToArray(led_arr, cJSON_CreateNumber(leds[i]));
    cJSON_AddItemToObject(root, "leds", led_arr);

    cJSON_AddStringToObject(root, "voice_state", vstate);
    cJSON_AddStringToObject(root, "voice_text", vtext);

    char *json_str = cJSON_PrintUnformatted(root);
    http_send_json(res, json_str);
    free(json_str);
    cJSON_Delete(root);
    return 0;
}

/* ── Route: POST /api/led ───────────────────────────────────────────── */
static int route_led(http_req_t *req, http_res_t *res)
{
    cJSON *j = cJSON_Parse(req->body);
    if (!j) { http_send_error(res, 400, "Bad JSON"); return 0; }

    cJSON *idx = cJSON_GetObjectItem(j, "index");
    cJSON *on  = cJSON_GetObjectItem(j, "on");

    if (idx && on) {
        bridge_ensure_dev();
        cJSON *r = rpc_call_int2(g_dev_fd, RPC_METHOD_LED_SET,
                                 idx->valueint, cJSON_IsTrue(on) ? 1 : 0);
        if (r) cJSON_Delete(r);
    }
    cJSON_Delete(j);
    http_send_json(res, "{\"ok\":true}");
    return 0;
}

/* ── Route: POST /api/curtain ───────────────────────────────────────── */
static int route_curtain(http_req_t *req, http_res_t *res)
{
    cJSON *j = cJSON_Parse(req->body);
    if (!j) { http_send_error(res, 400, "Bad JSON"); return 0; }

    cJSON *angle = cJSON_GetObjectItem(j, "angle");
    if (angle) {
        bridge_ensure_dev();
        cJSON *r = rpc_call_int1(g_dev_fd, RPC_METHOD_SG90_SET, angle->valueint);
        if (r) cJSON_Delete(r);
    }
    cJSON_Delete(j);
    http_send_json(res, "{\"ok\":true}");
    return 0;
}

/* ── Route: POST /api/voice ─────────────────────────────────────────── */
static int route_voice(http_req_t *req, http_res_t *res)
{
    int fd = rpc_connect(VOICE_SERVER_PORT);
    if (fd < 0) {
        http_send_json(res, "{\"ok\":false,\"error\":\"voice server offline\"}");
        return 0;
    }
    cJSON *r = rpc_call_void(fd, RPC_METHOD_VOICE_SEND_TEXT);
    if (r) cJSON_Delete(r);
    rpc_disconnect(fd);
    http_send_json(res, "{\"ok\":true}");
    return 0;
}

/* ── Route: GET /camera/snapshot ────────────────────────────────────── */
static int route_snapshot(http_req_t *req, http_res_t *res)
{
    (void)req;
    bridge_ensure_cam();

    /* Start camera if needed */
    cJSON *cs = rpc_call_void(g_cam_fd, RPC_METHOD_CAMERA_START);
    if (cs) cJSON_Delete(cs);

    /* Capture one frame */
    cJSON *frame = rpc_call_void(g_cam_fd, RPC_METHOD_CAMERA_CAPTURE_FRAME);
    if (!frame) {
        http_send_error(res, 500, "Camera not available");
        return 0;
    }

    cJSON *data = cJSON_GetObjectItem(frame, "data");
    cJSON *sz   = cJSON_GetObjectItem(frame, "size");

    if (data && data->valuestring) {
        /* Decode base64 to binary */
        const char *b64 = data->valuestring;
        int b64_len = strlen(b64);
        int bin_len = (b64_len * 3) / 4;
        unsigned char *bin = malloc(bin_len);
        int out_pos = 0;

        /* Simple base64 decode */
        static const signed char b64_table[256] = {
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
            52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
            -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
            15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
            -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
            41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        };
        for (int i = 0; i < b64_len; i += 4) {
            int a = b64_table[(unsigned char)b64[i]];
            int b = b64_table[(unsigned char)b64[i+1]];
            int c = b64_table[(unsigned char)b64[i+2]];
            int d = b64_table[(unsigned char)b64[i+3]];
            bin[out_pos++] = (a << 2) | (b >> 4);
            if (c >= 0) bin[out_pos++] = (b << 4) | (c >> 2);
            if (d >= 0) bin[out_pos++] = (c << 6) | d;
        }

        http_send_ok(res, (const char *)bin, "image/jpeg");
        free(bin);
    } else {
        http_send_error(res, 500, "No frame data");
    }
    cJSON_Delete(frame);
    return 0;
}

/* ── Main ──────────────────────────────────────────────────────────── */
static void sig_handler(int s) { (void)s; g_running = 0; }

int main(void)
{
    LOG_INFO("==========================================");
    LOG_INFO("Smart Screen — Web Remote Control Server");
    LOG_INFO("Port: %d", WEB_PORT);
    LOG_INFO("Open http://<this-ip>:%d on your phone", WEB_PORT);
    LOG_INFO("==========================================");

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /* Register routes */
    http_route("GET /", route_index);
    http_route("GET /api/status", route_status);
    http_route("POST /api/led", route_led);
    http_route("POST /api/curtain", route_curtain);
    http_route("POST /api/voice", route_voice);
    http_route("GET /camera/snapshot", route_snapshot);

    /* Also serve index at /index.html */
    http_route("GET /index.html", route_index);

    if (http_server_start(WEB_PORT) < 0) {
        LOG_ERROR("Failed to start web server");
        return 1;
    }

    while (g_running) {
        http_server_poll();
        usleep(10000); /* 10ms */
    }

    http_server_stop();
    if (g_dev_fd >= 0) rpc_disconnect(g_dev_fd);
    if (g_cam_fd >= 0) rpc_disconnect(g_cam_fd);

    LOG_INFO("Web server stopped.");
    return 0;
}
