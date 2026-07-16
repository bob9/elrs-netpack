#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "mbedtls/md5.h"
#include "msp.h"
#include "msptypes.h"
#include "rtc_sync.h"
#include "test_server.h"

static const char *TAG = "test_server";

static RingbufHandle_t xRingEspnowOut = NULL;

// ── ELRS helpers ────────────────────────────────────────────────────────────

// Derive a binding UID the same way the ExpressLRS configurator does: the
// first 6 bytes of MD5("-DMY_BINDING_PHRASE=\"<phrase>\""), first byte made
// even (ESP-NOW requires a unicast MAC).
static void uid_from_phrase(const char *phrase, uint8_t uid[6])
{
    char buf[160];
    snprintf(buf, sizeof(buf), "-DMY_BINDING_PHRASE=\"%s\"", phrase);
    unsigned char digest[16];
    mbedtls_md5((const unsigned char *)buf, strlen(buf), digest);
    memcpy(uid, digest, 6);
    uid[0] &= ~0x01;
}

static bool queue_packet(mspPacket_t *packet)
{
    if (xRingbufferSend(xRingEspnowOut, packet, sizeof(mspPacket_t), pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to queue test packet");
        return false;
    }
    return true;
}

static void packet_init(mspPacket_t *packet, uint16_t function)
{
    packet->reset();
    packet->makeCommand();
    packet->function = function;
}

// Queue count packets wrapped in a set-send-uid / reset-send-uid pair so
// they are delivered to the one goggle bound to the given phrase.
static bool queue_targeted(const uint8_t uid[6], mspPacket_t *packets, size_t count)
{
    mspPacket_t p;
    packet_init(&p, MSP_ELRS_SET_SEND_UID);
    p.addByte(0x01);
    for (int i = 0; i < 6; i++)
        p.addByte(uid[i]);
    if (!queue_packet(&p))
        return false;

    bool ok = true;
    for (size_t i = 0; i < count && ok; i++)
        ok = queue_packet(&packets[i]);

    packet_init(&p, MSP_ELRS_SET_SEND_UID);
    p.addByte(0x00);
    return queue_packet(&p) && ok;
}

// ── form parsing (application/x-www-form-urlencoded) ───────────────────────

static bool form_value(const char *body, const char *key, char *out, size_t outlen)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p)
    {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=')
        {
            p += klen + 1;
            size_t n = 0;
            while (*p && *p != '&' && n < outlen - 1)
            {
                char c = *p++;
                if (c == '+')
                {
                    c = ' ';
                }
                else if (c == '%' && isxdigit((unsigned char)p[0]) && isxdigit((unsigned char)p[1]))
                {
                    char hex[3] = {p[0], p[1], 0};
                    c = (char)strtol(hex, NULL, 16);
                    p += 2;
                }
                out[n++] = c;
            }
            out[n] = 0;
            return n > 0;
        }
        p = strchr(p, '&');
        if (p)
            p++;
    }
    out[0] = 0;
    return false;
}

// ── test actions ────────────────────────────────────────────────────────────

static const char *do_osd(const uint8_t uid[6], const char *text, int row)
{
    size_t len = strlen(text);
    if (len == 0 || len > 50)
        return "message must be 1-50 characters";
    if (row < 0 || row > 17)
        return "row must be 0-17";

    mspPacket_t seq[3];
    packet_init(&seq[0], MSP_ELRS_SET_OSD);
    seq[0].addByte(0x02); // clear

    packet_init(&seq[1], MSP_ELRS_SET_OSD);
    seq[1].addByte(0x03); // stage text
    seq[1].addByte((uint8_t)row);
    seq[1].addByte((uint8_t)((50 - len) / 2)); // centered
    seq[1].addByte(0);
    for (size_t i = 0; i < len; i++)
    {
        // The goggles index their Betaflight-layout OSD font directly with
        // this byte: only 0x20-0x5F match ASCII (no lowercase - that range
        // holds the arrow glyphs), so fold to uppercase and blank the rest.
        char c = (char)toupper((unsigned char)text[i]);
        if (c < 0x20 || c > 0x5F)
            c = ' ';
        seq[1].addByte(c);
    }

    packet_init(&seq[2], MSP_ELRS_SET_OSD);
    seq[2].addByte(0x04); // display

    return queue_targeted(uid, seq, 3) ? NULL : "send queue full";
}

static const char *do_clear(const uint8_t uid[6])
{
    mspPacket_t seq[2];
    packet_init(&seq[0], MSP_ELRS_SET_OSD);
    seq[0].addByte(0x02);
    packet_init(&seq[1], MSP_ELRS_SET_OSD);
    seq[1].addByte(0x04);
    return queue_targeted(uid, seq, 2) ? NULL : "send queue full";
}

static const char *do_channel(const uint8_t uid[6], int index)
{
    if (index < 0 || index > 47)
        return "invalid channel index";
    mspPacket_t p;
    packet_init(&p, MSP_SET_VTX_CONFIG);
    p.addByte((uint8_t)index);
    return queue_targeted(uid, &p, 1) ? NULL : "send queue full";
}

static const char *do_time(const uint8_t uid[6])
{
    time_t now = time(NULL);
    if (now < 1704067200) // 1 Jan 2024
        return "netpack clock not set - connect dd-pits or configure NTP (timeconfig)";
    struct tm timeData;
    localtime_r(&now, &timeData);

    mspPacket_t p;
    packet_init(&p, MSP_ELRS_BACKPACK_SET_RTC);
    p.addByte(timeData.tm_year);
    p.addByte(timeData.tm_mon);
    p.addByte(timeData.tm_mday);
    p.addByte(timeData.tm_hour);
    p.addByte(timeData.tm_min);
    p.addByte(timeData.tm_sec);
    // Our packet loops through the ESPNOW task's SET_RTC handling; without
    // this it would register as an external time source and pause the
    // netpack's own periodic sync for the holdoff period
    rtc_sync_note_self_send();
    return queue_targeted(uid, &p, 1) ? NULL : "send queue full";
}

static const char *do_dvr(const uint8_t uid[6], const char *label)
{
    size_t len = strlen(label);
    if (len == 0 || len > 43)
        return "label must be 1-43 characters";
    mspPacket_t p;
    packet_init(&p, MSP_ELRS_BACKPACK_SET_DVR_NAME);
    for (size_t i = 0; i < len; i++)
        p.addByte(label[i]);
    return queue_targeted(uid, &p, 1) ? NULL : "send queue full";
}

// ── HTTP handlers ───────────────────────────────────────────────────────────

static const char PAGE[] = R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ELRS Netpack - Goggle Test</title>
<style>
 body{font-family:system-ui,sans-serif;background:#14161a;color:#e8e8e8;max-width:560px;margin:24px auto;padding:0 16px}
 h1{font-size:20px} h2{font-size:14px;margin:22px 0 8px;color:#9ad}
 .card{background:#1d2026;border:1px solid #2c3038;border-radius:10px;padding:14px;margin-bottom:12px}
 label{display:block;font-size:12px;color:#aab;margin-bottom:4px}
 input,select{width:100%;box-sizing:border-box;background:#12141a;color:#e8e8e8;border:1px solid #363b45;border-radius:6px;padding:8px;font-size:14px;margin-bottom:10px}
 button{background:#2b6cb0;color:#fff;border:0;border-radius:6px;padding:9px 14px;font-size:14px;cursor:pointer;margin:2px 4px 2px 0}
 button:hover{background:#3182ce} button.warn{background:#805ad5}
 #status{margin-top:12px;padding:10px;border-radius:6px;font-size:13px;display:none}
 .ok{background:#1c4532;color:#9ae6b4} .err{background:#553030;color:#feb2b2}
 small{color:#889}
</style></head><body>
<h1>Goggle test</h1>
<div class="card">
 <label>Pilot's ELRS bind phrase</label>
 <input id="phrase" placeholder="the phrase flashed/bound to the goggles">
 <small>The UID is derived exactly like the ELRS Configurator, so type the phrase as the pilot set it.</small>
</div>
<div class="card"><h2>OSD</h2>
 <label>Message</label><input id="text" value="NETPACK TEST OK" maxlength="50">
 <label>Row (0-17)</label><input id="row" type="number" min="0" max="17" value="4">
 <button onclick="send('osd')">Show message</button>
 <button class="warn" onclick="send('clear')">Clear OSD</button>
 <small>The goggle OSD font is uppercase-only; lowercase is sent as capitals.</small>
</div>
<div class="card"><h2>Channel change</h2>
 <select id="channel">
  <optgroup label="Raceband">
   <option value="32">R1</option><option value="33">R2</option><option value="34" selected>R3</option>
   <option value="35">R4</option><option value="36">R5</option><option value="37">R6</option>
   <option value="38">R7</option><option value="39">R8</option>
  </optgroup>
  <optgroup label="Other">
   <option value="24">F1</option><option value="25">F2</option><option value="27">F4</option><option value="16">E1</option>
  </optgroup>
  <optgroup label="Low Band">
   <option value="40">L1</option><option value="41">L2</option><option value="42">L3</option><option value="43">L4</option>
   <option value="44">L5</option><option value="45">L6</option><option value="46">L7</option><option value="47">L8</option>
  </optgroup>
 </select>
 <button onclick="send('channel')">Change channel</button>
 <small>Low Band needs goggle firmware with remote band switching.</small>
</div>
<div class="card"><h2>Netpack clock</h2>
 <div style="margin-bottom:8px;font-size:14px">Netpack time: <b id="npclock">loading...</b></div>
 <button onclick="setClock()">Set from this device's clock</button>
 <small>Normally set automatically by dd-pits or NTP; use this when neither is available.</small>
</div>
<div class="card"><h2>Extras</h2>
 <button onclick="send('time')">Send time sync</button>
 <label style="margin-top:10px">DVR label</label><input id="dvr" value="NetpackTest" maxlength="32">
 <button onclick="send('dvr')">Send DVR name</button>
 <small>Time sets the goggle clock; DVR names the goggles' next recording.</small>
</div>
<div id="status"></div>
<script>
const $=id=>document.getElementById(id);
$('phrase').value=localStorage.getItem('bindphrase')||'';
async function refreshClock(){
 try{const r=await fetch('/api/clock');const j=await r.json();
  document.getElementById('npclock').textContent=j.set?j.time:'not set';
 }catch(e){document.getElementById('npclock').textContent='?'}
}
refreshClock();setInterval(refreshClock,10000);
async function setClock(){
 const n=new Date();
 const st=$('status');st.style.display='block';st.className='';st.textContent='Setting clock...';
 const body=new URLSearchParams({action:'setclock',y:n.getFullYear(),mo:n.getMonth()+1,d:n.getDate(),
   h:n.getHours(),mi:n.getMinutes(),s:n.getSeconds()});
 try{
  const r=await fetch('/api/test',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
  const j=await r.json();
  st.className=j.ok?'ok':'err';
  st.textContent=j.ok?'Netpack clock set - broadcasting to goggles':'Error: '+j.error;
  refreshClock();
 }catch(e){st.className='err';st.textContent='Request failed: '+e}
}
async function send(action){
 const phrase=$('phrase').value.trim();
 const st=$('status'); st.style.display='block';
 if(!phrase){st.className='err';st.textContent='Enter the bind phrase first';return}
 localStorage.setItem('bindphrase',phrase);
 const body=new URLSearchParams({action,phrase,text:$('text').value,row:$('row').value,
   channel:$('channel').value,label:$('dvr').value});
 st.className='';st.textContent='Sending...';
 try{
  const r=await fetch('/api/test',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
  const j=await r.json();
  st.className=j.ok?'ok':'err';
  st.textContent=j.ok?('Sent '+action+' to '+j.uid+' - check the goggles'):('Error: '+j.error);
 }catch(e){st.className='err';st.textContent='Request failed: '+e}
}
</script></body></html>
)HTML";

// Current netpack clock as JSON, for the test page's clock card
static esp_err_t clock_get(httpd_req_t *req)
{
    char resp[96];
    time_t now = time(NULL);
    if (now < 1704067200)
    {
        snprintf(resp, sizeof(resp), "{\"set\":false}");
    }
    else
    {
        struct tm timeData;
        char buf[32];
        localtime_r(&now, &timeData);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeData);
        snprintf(resp, sizeof(resp), "{\"set\":true,\"time\":\"%s\"}", buf);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_post(httpd_req_t *req)
{
    char body[512];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    body[len] = 0;

    char action[16], phrase[96], text[64], param[40];
    form_value(body, "action", action, sizeof(action));

    // Setting the netpack clock needs no bind phrase - the browser sends its
    // own local wall time
    if (strcmp(action, "setclock") == 0)
    {
        uint8_t clk[6];
        const char *keys[6] = {"y", "mo", "d", "h", "mi", "s"};
        int vals[6];
        bool ok = true;
        for (int i = 0; i < 6; i++)
        {
            ok = ok && form_value(body, keys[i], param, sizeof(param));
            vals[i] = atoi(param);
        }
        if (!ok || vals[0] < 2024)
        {
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad clock values\"}");
        }
        clk[0] = (uint8_t)(vals[0] - 1900);
        clk[1] = (uint8_t)(vals[1] - 1);
        clk[2] = (uint8_t)vals[2];
        clk[3] = (uint8_t)vals[3];
        clk[4] = (uint8_t)vals[4];
        clk[5] = (uint8_t)vals[5];
        rtc_sync_set_clock(clk, sizeof(clk));
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":true,\"uid\":\"netpack clock\"}");
    }

    if (!form_value(body, "phrase", phrase, sizeof(phrase)))
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bind phrase required\"}");
    }

    uint8_t uid[6];
    uid_from_phrase(phrase, uid);

    const char *err = "unknown action";
    if (strcmp(action, "osd") == 0)
    {
        form_value(body, "text", text, sizeof(text));
        form_value(body, "row", param, sizeof(param));
        err = do_osd(uid, text, atoi(param));
    }
    else if (strcmp(action, "clear") == 0)
    {
        err = do_clear(uid);
    }
    else if (strcmp(action, "channel") == 0)
    {
        form_value(body, "channel", param, sizeof(param));
        err = do_channel(uid, atoi(param));
    }
    else if (strcmp(action, "time") == 0)
    {
        err = do_time(uid);
    }
    else if (strcmp(action, "dvr") == 0)
    {
        form_value(body, "label", text, sizeof(text));
        err = do_dvr(uid, text);
    }

    char resp[160];
    if (err == NULL)
    {
        ESP_LOGI(TAG, "test %s sent to uid %02x%02x%02x%02x%02x%02x", action,
                 uid[0], uid[1], uid[2], uid[3], uid[4], uid[5]);
        snprintf(resp, sizeof(resp),
                 "{\"ok\":true,\"uid\":\"%02X:%02X:%02X:%02X:%02X:%02X\"}",
                 uid[0], uid[1], uid[2], uid[3], uid[4], uid[5]);
    }
    else
    {
        snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"%s\"}", err);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

void test_server_start(RingbufHandle_t espnow_out)
{
    xRingEspnowOut = espnow_out;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start HTTP test server");
        return;
    }

    static const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get, .user_ctx = NULL};
    static const httpd_uri_t api = {.uri = "/api/test", .method = HTTP_POST, .handler = api_post, .user_ctx = NULL};
    static const httpd_uri_t clk = {.uri = "/api/clock", .method = HTTP_GET, .handler = clock_get, .user_ctx = NULL};
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &api);
    httpd_register_uri_handler(server, &clk);

    ESP_LOGI(TAG, "Goggle test page on http://<netpack-ip>/ (port 80)");
}
