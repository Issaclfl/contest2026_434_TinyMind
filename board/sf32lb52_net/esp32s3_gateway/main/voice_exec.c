/*
 * 语音入口：说一句英文口令，灯就亮。
 *
 * 链路（每一段都单独验证过，见 docs/ESP32-S3网关台架实测证据.md §十一）：
 *
 *     POST /voice 的音频
 *       -> 云端 mimo 的 chat 接口（把音频当 input_audio 内容块发过去）
 *       -> 它只做转写，返回文本
 *       -> 本地命令模型（就是我们自己训、自己量化的那个）把文本翻成动作 JSON
 *       -> cmd_exec_run() 在本板执行
 *
 * 为什么转写放云端、意图放本地：云端那个模型一听就懂自由说法，但它是思考型模型，
 * 让它直接输出动作 JSON 会绕远（实测 800 token 都在自说自话）；而"几种说法对应
 * 哪个动作"正是我们本地 0.26M 模型的强项（留出集 43/46），还不用把动作词汇表
 * 交给外面。所以两边各干自己擅长的：**云负责听，端负责选**。
 *
 * 代价说明白：语音这条腿需要联网。断了网还能用打字的口令（走 cmd 那条路），
 * 那一条完全是本地的。
 *
 * Copyright (C) 2026 TinyMind
 * SPDX-License-Identifier: Apache-2.0
 */

#include "voice_exec.h"

#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "espllm.h"
#include "cmd_exec.h"
#include "net_cloud.h"

static const char *TAG = "gw_voice";

/* 转写是短任务，给 200 token 足够；音频上限就是 espllm 那边的 512 KB。
 *
 * 注意这里**不带文字提示**：实测给 ASR 模型配一段文字会被拒
 * （"ASR request must not include text parts; text prompt is injected by the
 * gateway"），只发音频它才 1.6 秒返回；换通用 chat 模型也能用，但它会先
 * 自言自语 7~8 秒才肯转写。 */
#define ASR_MAX_TOKENS 200

/* ---------------------------------------------------------------- 小工具 -- */

static size_t b64_len(size_t n) { return 4 * ((n + 2) / 3); }

/* 找 WAV 的 data 块；不是 RIFF 就当裸 PCM（我们自己约定的 16k/单声道/16bit）。 */
static const char *pcm_start(const char *audio, size_t len, size_t *pcm_len)
{
    if (len > 12 && memcmp(audio, "RIFF", 4) == 0 && memcmp(audio + 8, "WAVE", 4) == 0) {
        size_t pos = 12;
        while (pos + 8 <= len) {
            const char *id = audio + pos;
            uint32_t sz = (uint8_t)audio[pos + 4] | ((uint32_t)(uint8_t)audio[pos + 5] << 8) |
                          ((uint32_t)(uint8_t)audio[pos + 6] << 16) |
                          ((uint32_t)(uint8_t)audio[pos + 7] << 24);
            if (memcmp(id, "data", 4) == 0) {
                size_t avail = len - pos - 8;
                *pcm_len = sz < avail ? sz : avail;
                return audio + pos + 8;
            }
            pos += 8 + sz + (sz & 1);
        }
        *pcm_len = 0;
        return NULL;
    }
    *pcm_len = len;
    return audio;
}

static void json_escape_into(const char *src, char *dst, size_t dst_size)
{
    size_t o = 0;
    for (const char *p = src; *p != '\0' && o + 3 < dst_size; p++) {
        if (*p == '"' || *p == '\\') {
            dst[o++] = '\\';
            dst[o++] = *p;
        } else if (*p == '\n') {
            dst[o++] = '\\';
            dst[o++] = 'n';
        } else if ((unsigned char)*p >= 0x20) {
            dst[o++] = *p;
        }
    }
    dst[o] = '\0';
}

/* ------------------------------------------------------------------- ASR -- */

/* 把一段 WAV 交给云端的 mimo，返回它转写出的文本（写进 out）。 */
static bool asr_transcribe(const char *wav, size_t wav_len, char *out, size_t out_size)
{
    const char *key = CONFIG_GATEWAY_CLOUD_API_KEY;
    if (key[0] == '\0') {
        snprintf(out, out_size, "no cloud key configured");
        return false;
    }

    size_t b64_size = b64_len(wav_len) + 4;
    char *b64 = heap_caps_malloc(b64_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t body_cap = b64_size + 1024;
    char *body = heap_caps_malloc(body_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *resp = heap_caps_malloc(CONFIG_GATEWAY_CLOUD_MAX_RESP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (b64 == NULL || body == NULL || resp == NULL) {
        free(b64);
        free(body);
        free(resp);
        snprintf(out, out_size, "out of memory for %u B of audio", (unsigned)wav_len);
        return false;
    }

    extern int mbedtls_base64_encode(unsigned char *dst, size_t dlen, size_t *olen,
                                     const unsigned char *src, size_t slen);
    size_t b64_used = 0;
    if (mbedtls_base64_encode((unsigned char *)b64, b64_size, &b64_used,
                              (const unsigned char *)wav, wav_len) != 0) {
        free(b64);
        free(body);
        free(resp);
        snprintf(out, out_size, "base64 failed");
        return false;
    }
    b64[b64_used] = '\0';

    int n = snprintf(body, body_cap,
                     "{\"model\":\"%s\",\"max_tokens\":%d,\"messages\":[{\"role\":\"user\","
                     "\"content\":[{\"type\":\"input_audio\","
                     "\"input_audio\":{\"data\":\"%s\",\"format\":\"wav\"}}]}]}",
                     CONFIG_GATEWAY_ASR_MODEL, ASR_MAX_TOKENS, b64);
    free(b64);
    if (n <= 0 || (size_t)n >= body_cap) {
        free(body);
        free(resp);
        snprintf(out, out_size, "request body too large");
        return false;
    }

    esp_http_client_config_t cfg = {
        .url = CONFIG_GATEWAY_CLOUD_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = CONFIG_GATEWAY_CLOUD_TIMEOUT_MS,
        .buffer_size = 4096,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        free(body);
        free(resp);
        snprintf(out, out_size, "http client init failed");
        return false;
    }
    esp_http_client_set_method(c, HTTP_METHOD_POST);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", key);
    esp_http_client_set_header(c, "Authorization", auth);
    esp_http_client_set_post_field(c, body, n);

    size_t got = 0;
    int status = 0;
    esp_err_t err = esp_http_client_open(c, n);
    if (err == ESP_OK) {
        esp_http_client_write(c, body, n);
        esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        int r;
        while (got + 1 < CONFIG_GATEWAY_CLOUD_MAX_RESP &&
               (r = esp_http_client_read(c, resp + got, CONFIG_GATEWAY_CLOUD_MAX_RESP - got - 1)) > 0) {
            got += (size_t)r;
        }
        resp[got] = '\0';
    } else {
        snprintf(out, out_size, "upload failed: %s", esp_err_to_name(err));
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    free(body);

    if (err != ESP_OK) {
        free(resp);
        return false;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "asr HTTP %d: %.160s", status, resp);
        free(resp);
        snprintf(out, out_size, "asr failed (HTTP %d)", status);
        return false;
    }
    if (!espllm_json_field(resp, "content", out, out_size)) {
        ESP_LOGW(TAG, "asr answered without content: %.160s", resp);
        free(resp);
        snprintf(out, out_size, "no transcript in the answer");
        return false;
    }
    free(resp);
    if (out[0] == '\0') {
        /* 200 但转写是空的：录到的是静音或一段纯音。实测过——按住说话那页在
         * 没有麦克风的机器上用振荡器顶替时就是这个结果，别把空错误丢给页面。 */
        snprintf(out, out_size, "the recording held no speech (silence or a tone)");
        return false;
    }
    espllm_utf8_sanitize(out);         /* 转写也可能被云端切在多字节字符中间 */
    ESP_LOGI(TAG, "transcript: \"%s\"", out);
    return true;
}

/* ------------------------------------------------------------ 请求入口 -- */

static esp_err_t voice_handle(const char *audio, size_t audio_len, char **response)
{
    size_t pcm_len = 0;
    const char *pcm = pcm_start(audio, audio_len, &pcm_len);
    if (pcm == NULL || pcm_len < 1600) {          /* < 50 ms：当没录到东西 */
        *response = strdup("{\"error\":\"too short to contain speech\"}");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "%u B uploaded, %u B of pcm (%u ms at 16 kHz mono)",
             (unsigned)audio_len, (unsigned)pcm_len, (unsigned)(pcm_len / 32));

    /* 云端要一个完整容器：原本是 WAV 就整段送，裸 PCM 就给它补个 WAV 头。 */
    static const unsigned char kHdr[44] = {
        'R','I','F','F', 0,0,0,0, 'W','A','V','E','f','m','t',' ', 16,0,0,0,
        1,0,1,0, 0x80,0x3e,0,0, 0x00,0x7d,0,0, 2,0,16,0, 'd','a','t','a', 0,0,0,0 };
    char *wav = NULL;
    size_t wav_len = 0;
    if (memcmp(audio, "RIFF", 4) == 0) {
        wav = (char *)audio;
        wav_len = audio_len;
    } else {
        wav_len = pcm_len + 44;
        wav = heap_caps_malloc(wav_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (wav == NULL) {
            *response = strdup("{\"error\":\"out of memory\"}");
            return ESP_OK;
        }
        memcpy(wav, kHdr, 44);
        uint32_t sz = (uint32_t)pcm_len;
        memcpy(wav + 4, &(uint32_t){ sz + 36 }, 4);
        memcpy(wav + 40, &sz, 4);
        memcpy(wav + 44, pcm, pcm_len);
    }

    char transcript[256] = { 0 };
    bool ok = asr_transcribe(wav, wav_len, transcript, sizeof(transcript));
    if (wav != audio) {
        free(wav);
    }
    if (!ok) {
        char esc[512];
        json_escape_into(transcript, esc, sizeof(esc));
        char *out = malloc(strlen(esc) + 64);
        if (out != NULL) {
            snprintf(out, strlen(esc) + 64, "{\"error\":\"%s\"}", esc);
            *response = out;
        }
        return ESP_OK;
    }

    /* 听懂之后交给本地命令模型：动作的词汇表不出这块板子。 */
    char answer[640];
    int rc = cmd_exec_run(transcript, answer, sizeof(answer));

    char esc[512];
    json_escape_into(transcript, esc, sizeof(esc));
    size_t cap = strlen(esc) + strlen(answer) + 128;
    char *out = malloc(cap);
    if (out == NULL) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(out, cap, "{\"transcript\":\"%s\",\"ok\":%s,\"result\":%s}",
             esc, rc == 0 ? "true" : "false", answer);
    *response = out;
    ESP_LOGI(TAG, "voice -> %s", answer);
    return ESP_OK;
}

/* ------------------------------------------------------------ 说话页面 -- */

/* 三条腿画在同一张页面上，因为它会落在两种来源里：
 *
 *   https://<网关>/talk   安全上下文 → 按住说话（getUserMedia 才有定义）
 *   http://<网关>/talk    非安全上下文 → 浏览器**不给** navigator.mediaDevices。
 *                        页面自己把原因说清楚，并给出另外两条路：传录音、打字。
 *
 * 第一版只有按住说话，于是手机上点下去毫无反应 —— 控制台里是
 * "Cannot read properties of undefined (reading 'getUserMedia')"，页面上一片安静。
 * 现在：先看 isSecureContext 再决定露出哪条腿，任何一条腿失败都把原因写出来，
 * 不装作没发生。
 *
 * 录音按浏览器给的采样率取，边抽边做盒式平均降到 16 kHz：云端转写对 16 kHz 足够，
 * 请求体也小四倍。传文件那条腿用 decodeAudioData，手机录音机存的 m4a 通常能解。 */
static const char kVoicePage[] =
    "<!doctype html><html lang='zh'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>TinyMind 口令</title><style>"
    "body{background:#101418;color:#d8e0e8;font-family:ui-monospace,monospace;"
    "text-align:center;padding:20px;margin:0}"
    "h3{margin:14px 0 10px;font-weight:600}"
    "#mode{max-width:660px;margin:0 auto 14px;font-size:14px;line-height:1.7;"
    "color:#9fb0c0;text-align:left;white-space:pre-wrap}"
    "button{font-size:20px;padding:18px 30px;border-radius:12px;border:0;"
    "background:#7fd0a0;color:#08120c;font-weight:bold;font-family:inherit}"
    "button:active{background:#4f9f78}button:disabled{background:#3a4750;color:#8b98a3}"
    "#b{touch-action:none;user-select:none;-webkit-user-select:none}"
    "input[type=text]{font-size:16px;padding:10px;border-radius:8px;"
    "border:1px solid #33404c;background:#182028;color:#d8e0e8;"
    "width:min(320px,66vw);font-family:inherit}"
    "#o{text-align:left;white-space:pre-wrap;font-size:14px;line-height:1.65;"
    "max-width:660px;margin:16px auto;color:#9fb0c0;background:#161c22;"
    "padding:12px;border-radius:8px}"
    "a{color:#7fd0a0}</style></head><body>"
    "<h3>TinyMind 口令</h3>"
    "<div id='mode'>检查中…</div>"
    "<p id='pl' hidden>第 1 条路：<a id='lnk' href='/talk'>换 https 打开这一页</a>"
    "（自签证书，浏览器会先问一次，点“继续前往”）</p>"
    "<p><button id='b' disabled>按住说话</button></p>"
    "<p id='fp' hidden>或者传一段录音：<input id='f' type='file' accept='audio/*'></p>"
    "<p><input id='t' type='text' placeholder='也可以打字：turn on the red light'> "
    "<button id='s2'>发送</button></p>"
    "<pre id='o'>（口令交给网关上的本地命令模型，云只负责听清这句话）</pre>"
    "<script>(function(){"
    "var $=function(i){return document.getElementById(i)};"
    "var out=function(m){$('o').textContent=m};"
    "var canMic=!!(window.isSecureContext&&navigator.mediaDevices"
    "&&navigator.mediaDevices.getUserMedia);"
    "var ac=null,stream=null,src=null,node=null,gain=null,rate=16000;"
    "var chunks=[],rec=false,busy=false,holding=false;"

    "function banner(){var b=$('b');"
    "if(canMic){"
    "$('mode').textContent='麦克风可用（https 安全上下文）。按住下面的按钮说一句英文口令，"
    "松开就会上传。';"
    "$('fp').hidden=true;$('pl').hidden=true;b.disabled=false;return;}"
    "b.disabled=true;b.textContent='按住说话（当前不可用）';"
    "$('pl').hidden=false;$('fp').hidden=false;"
    "$('lnk').href='https://'+location.hostname+'/talk';"
    "$('mode').textContent='这个页面是 http 打开的，浏览器在这种来源下不提供麦克风"
    "（navigator.mediaDevices 是 undefined），所以按住说话点不动。三条替代路：\\n"
    "  1. 点上面那个 https 链接，换安全上下文打开（那条腿能按住说话）\\n"
    "  2. 用系统录音机录一句，再选文件传上来\\n"
    "  3. 直接在下面打字（这条完全走本地，不需要联网）';}"

    /* 单声道 16 kHz 16 bit 的 WAV；抽样的同时做盒式平均，少一点混叠。 */
    "function wav16(all,sr){"
    "var step=Math.max(1,Math.round(sr/16000)),m=Math.floor(all.length/step);"
    "var buf=new ArrayBuffer(44+m*2),v=new DataView(buf);"
    "var w=function(p,s){for(var i=0;i<s.length;i++)v.setUint8(p+i,s.charCodeAt(i))};"
    "w(0,'RIFF');v.setUint32(4,36+m*2,true);w(8,'WAVEfmt ');v.setUint32(16,16,true);"
    "v.setUint16(20,1,true);v.setUint16(22,1,true);v.setUint32(24,16000,true);"
    "v.setUint32(28,32000,true);v.setUint16(32,2,true);v.setUint16(34,16,true);"
    "w(36,'data');v.setUint32(40,m*2,true);"
    "for(var i=0;i<m;i++){var acc=0,cnt=0;"
    "for(var j=0;j<step&&i*step+j<all.length;j++){acc+=all[i*step+j];cnt++;}"
    "var s=cnt?acc/cnt:0;s=Math.max(-1,Math.min(1,s));"
    "v.setInt16(44+i*2,s<0?s*32768:s*32767,true);}"
    "return new Blob([buf],{type:'audio/wav'});}"

    "function send(blob){out('上传 '+blob.size+' B …');var t0=Date.now();"
    "fetch('/voice',{method:'POST',body:blob}).then(function(r){"
    "return r.text().then(function(t){"
    "out(t+'\\n\\n(HTTP '+r.status+'，'+(Date.now()-t0)+' ms)')})})"
    ".catch(function(e){out('请求失败：'+e)});}"

    /* 录音的开关与"麦克风"的开关是两件事：松开按钮只停录音，**不释放已经拿到
     * 的流**。理由是手机上的第一次必然要弹权限框：用户一边按着按钮一边去点
     * "允许"是做不到的，那一按什么都录不到；保持流常驻之后，第二按起就是即时的。
     * 离开页面（pagehide）才把轨道停掉，免得手机上一直亮着"正在使用麦克风"。 */
    "function stopCapture(){"
    "try{if(node)node.disconnect();if(src)src.disconnect();if(gain)gain.disconnect();}"
    "catch(e){}"
    "node=null;src=null;gain=null;}"

    "function releaseMic(){stopCapture();"
    "if(stream){stream.getTracks().forEach(function(t){t.stop()});stream=null;}}"

    "function beginCapture(){"
    "if(!holding){out('按得太快了，按住说完整一句再松开。\\n"
    "（如果刚才弹了麦克风授权框，先点“允许”，再重新按住一次。）');return;}"
    "chunks=[];rec=true;"
    "src=ac.createMediaStreamSource(stream);"
    "node=ac.createScriptProcessor(4096,1,1);"
    "gain=ac.createGain();gain.gain.value=0;"
    "node.onaudioprocess=function(e){"
    "if(rec)chunks.push(new Float32Array(e.inputBuffer.getChannelData(0)))};"
    "src.connect(node);node.connect(gain);gain.connect(ac.destination);"
    "out('录音中…说一句英文口令，松开就发。');}"

    "function startRec(){if(rec||busy||!canMic)return;"
    "if(!ac){try{ac=new (window.AudioContext||window.webkitAudioContext)();}"
    "catch(e){out('这个浏览器没有 AudioContext');return;}}"
    "if(ac.state==='suspended')ac.resume();"
    "if(stream){beginCapture();return;}"
    "busy=true;"
    "navigator.mediaDevices.getUserMedia("
    "{audio:{echoCancellation:true,noiseSuppression:true}})"
    ".then(function(st){busy=false;stream=st;rate=ac.sampleRate;beginCapture();})"
    ".catch(function(e){busy=false;$('fp').hidden=false;"
    "out((e&&e.name==='NotAllowedError'"
    "?'麦克风授权被拒绝：把浏览器地址栏里的麦克风权限改成“允许”，再按住一次。'"
    ":'打不开麦克风：'+e.message)"
    "+'\\n也可以用下面那两条腿：传一段录音，或者直接打字。');});}"

    "function stopRec(){if(!rec)return;rec=false;stopCapture();"
    "var n=0;for(var i=0;i<chunks.length;i++)n+=chunks[i].length;"
    "if(n<rate*0.5){out('只录到 '+(n/rate).toFixed(2)+' 秒，太短了，再按一次说长点。');return;}"
    "var all=new Float32Array(n),o=0;"
    "for(var i=0;i<chunks.length;i++){all.set(chunks[i],o);o+=chunks[i].length;}"
    "send(wav16(all,rate));}"
    "window.addEventListener('pagehide',releaseMic);"

    "function sendText(){var text=$('t').value.trim();if(!text)return;"
    "out('口令：'+text+'\\n（发给网关上的本地命令模型，不走云）…');var t0=Date.now();"
    "fetch('/v1/chat/completions',{method:'POST',"
    "headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({model:'cmd',"
    "messages:[{role:'user',content:text}],max_tokens:40})})"
    ".then(function(r){return r.text().then(function(t){"
    "out(t+'\\n\\n(HTTP '+r.status+'，'+(Date.now()-t0)+' ms)')})})"
    ".catch(function(e){out('请求失败：'+e)});}"

    "var b=$('b');"
    "b.addEventListener('pointerdown',function(e){e.preventDefault();"
    "try{b.setPointerCapture(e.pointerId)}catch(x){}"
    "holding=true;startRec();});"
    "b.addEventListener('pointerup',function(e){e.preventDefault();"
    "holding=false;stopRec();});"
    "b.addEventListener('pointercancel',function(){holding=false;stopRec();});"
    "b.addEventListener('contextmenu',function(e){e.preventDefault();});"
    "$('s2').addEventListener('click',sendText);"
    "$('t').addEventListener('keydown',function(e){if(e.key==='Enter')sendText();});"
    "$('f').addEventListener('change',function(ev){"
    "var f=ev.target.files&&ev.target.files[0];if(!f)return;"
    "out('读 '+f.name+'（'+f.size+' B）…');"
    "f.arrayBuffer().then(function(buf){"
    "var c=new (window.AudioContext||window.webkitAudioContext)();"
    "return c.decodeAudioData(buf).then(function(dec){var sr=dec.sampleRate;"
    "out('解出 '+dec.duration.toFixed(1)+' 秒 / '+sr+' Hz，降到 16 kHz 再上传…');"
    "c.close();send(wav16(dec.getChannelData(0),sr));})"
    ".catch(function(e){c.close();"
    "out('这个文件解不开（'+(e&&e.message?e.message:e)+'）。"
    "浏览器只认它自己能解码的音频：手机录音机存的 m4a 通常可以，WAV/MP3 也可以。\\n"
    "实在不行就用上面那条 https 链接按住说话。');});});});"

    "banner();"
    "})();</script></body></html>";

/* 两个 PEM 由 main/CMakeLists.txt 的 EMBED_TXTFILES 嵌进来，符号名是 ESP-IDF 的
 * 惯例：_binary_<文件名>_start/_end（含结尾的 NUL，长度直接相减）。 */
extern const uint8_t servercert_pem_start[] asm("_binary_servercert_pem_start");
extern const uint8_t servercert_pem_end[]   asm("_binary_servercert_pem_end");
extern const uint8_t prvtkey_pem_start[]    asm("_binary_prvtkey_pem_start");
extern const uint8_t prvtkey_pem_end[]      asm("_binary_prvtkey_pem_end");

void voice_start(void)
{
    espllm_set_voice(voice_handle, kVoicePage);
    ESP_LOGI(TAG, "voice route on: hold the button at https://<gateway>/talk "
                  "(the plain-http page cannot get the microphone -- it offers the "
                  "file and text paths instead)");
}

void voice_https_start(void)
{
    esp_err_t err = espllm_http_start_secure(
        CONFIG_GATEWAY_VOICE_TLS_PORT,
        servercert_pem_start, (size_t)(servercert_pem_end - servercert_pem_start),
        prvtkey_pem_start, (size_t)(prvtkey_pem_end - prvtkey_pem_start));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no TLS listener (%s): the microphone path needs one, "
                      "the plain page still serves the file and text paths",
                 esp_err_to_name(err));
    }
}
