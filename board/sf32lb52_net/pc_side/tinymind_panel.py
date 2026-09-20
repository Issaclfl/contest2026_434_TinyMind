#!/usr/bin/env python3
"""TinyMind 演示面板 —— 一块板子 + 一个网关 + 一台（模拟的）米家设备，同屏看全。

为什么要有它：这条链路的"界面"原本是板子的串口控制台和 PC 上的模拟器终端，
看着像调试现场。这个面板把**运行过程**和**控制**放到一个网页里：

  · 网关状态：模型、Wi-Fi、PPP 会话、板子是否在线（读网关的 /status）
  · 设备卡片：台灯 / 空调 的真实状态（用 python-miio 直接问被控设备，
    不是面板自己记的账）
  · 控制：输入框 + 快捷按钮，走的是**和板子完全同一条离线路径**
    （POST /v1/chat/completions，model="cmd" → 网关上的本地分类器 → miIO）
  · 事件流：谁问的（面板 / 板子）、问了什么、网关怎么答、设备状态怎么变

板子自己的提问也看得见：网关的 /status 里有 `asked : "<最近一次问句>"`，
面板轮询它就能显示"板子刚问了什么"（答复在板子的屏和喇叭上，由摄像头拍）。

用法（在 PC 上跑，需要能访问网关）：
    python tinymind_panel.py --gateway 10.138.138.122
    python tinymind_panel.py --gateway 10.138.138.122 --port 8080
然后浏览器打开 http://127.0.0.1:8080

被控端的凭据：默认按 **python-miio 官方模拟器**的固定全零 token（公开常量）
直连 127.0.0.1:54321；接真机时用 `--miio <ip>` 与 `--token <32 位>` 覆盖。
"""

import argparse
import json
import os
import re
import ssl
import subprocess
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DEFAULT_GATEWAY = "10.138.138.122"
SIM_TOKEN = "0" * 32          # 官方模拟器写死的公开常量，不是凭据

STATE = {
    "gateway": DEFAULT_GATEWAY,
    "scheme": "http://" + DEFAULT_GATEWAY,
    "miio_ip": "127.0.0.1",
    "miio_token": SIM_TOKEN,
    "sim_port": 54321,
    "devices": [],            # 由设备表填充
    "events": [],             # 面板与板子的事件流
    "last_board_ask": "",
    "recent_panel": {},       # 面板刚发过的句子 -> 时间，用来避免误记成板子
    "last_gateway": None,
    "lock": threading.Lock(),
}

# 设备表：从 S3 的 sdkconfig 读（本地文件），读不到就用这个默认（官方模拟器）
FALLBACK_DEVICES = [
    {"name": "lamp", "label": "台灯", "model": "yeelink.light.color3"},
    {"name": "ac", "label": "空调", "model": "zhimi.aircondition.v1"},
]
OP_LABEL = {"on": "开", "off": "关", "brightness": "亮度", "color_temp": "色温"}


def ssl_ctx():
    return ssl._create_unverified_context()


def fetch(url, data=None, timeout=8, headers=None):
    req = urllib.request.Request(url, data=data, headers=headers or {})
    with urllib.request.urlopen(req, timeout=timeout, context=ssl_ctx()) as resp:
        return resp.read().decode("utf-8", "replace")


def gateway_status():
    """读 /status 并在里面找我们关心的几行。"""
    text = fetch("%s/status" % STATE["scheme"], timeout=4)
    out = {"raw": text}
    for line in text.split("\n"):
        line = line.strip()
        for key, pat in (("route", r"^route\s*:\s*(.+)$"),
                         ("asked", r'^asked\s*:\s*"?(.+?)"?$'),
                         ("uplink", r"^uplink\s*:\s*(.+)$"),
                         ("board", r"^board\s*:\s*(.+)$"),
                         ("rssi", r"^RSSI\s*(\S+)\s*dBm$"),
                         ("model", r"^model\s*:\s*(.+)$")):
            m = re.match(pat, line)
            if m:
                out[key] = m.group(1).strip()
    return out


def miio_rpc(method, params, ip=None, token=None, timeout=3):
    """用**官方 miio.protocol 模块**组包/解包，直接发给被控设备。

    为什么不走 python-miio 的 `Device.send()`：它默认要先做一次"发现"
    （明文握手），而官方模拟器不回这种握手，于是会报 "Unable to discover
    the device"。token 已知时直接发加密指令才是对的做法——报文格式本身
    仍由官方模块负责，我们没自己实现协议。
    """
    import datetime
    import socket

    from miio.protocol import Message

    token = bytes.fromhex(token or STATE["miio_token"])
    msg = {"data": {"value": {"id": 1, "method": method, "params": params}},
           "header": {"value": {"length": 0, "unknown": 0, "device_id": 0x01020304,
                                "ts": datetime.datetime.fromtimestamp(
                                    1234567890, datetime.timezone.utc)}},
           "checksum": 0}

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    try:
        sock.sendto(Message.build(msg, token=token),
                    (ip or STATE["miio_ip"], STATE["sim_port"]))
        data, _ = sock.recvfrom(2048)
    finally:
        sock.close()
    return Message.parse(data, token=token).data.value


# 每种设备真有的属性：空调没有亮度/色温，硬问会拿回错误而不是数字。
DEV_PROPS = {
    "lamp": [("power", "开关"), ("bright", "亮度"), ("ct", "色温")],
    "ac": [("power", "开关")],
}


def device_state(dev):
    """读被控设备的真实状态——面板不自己记账，每次都去问设备。"""
    props = DEV_PROPS.get(dev.get("name"), DEV_PROPS["lamp"])
    try:
        vals = miio_rpc("get_prop", [p for p, _ in props],
                        ip=dev.get("ip"), token=dev.get("token"))["result"]
        st = {"ok": True, "at": time.strftime("%H:%M:%S"),
              "fields": [{"label": lbl, "value": v} for (_, lbl), v in zip(props, vals)]}
        st["power"] = vals[0]
        for (key, _), v in zip(props, vals):
            st[key] = v
        return st
    except ImportError:
        return {"ok": False, "error": "python-miio 未安装：pip install python-miio"}
    except Exception as exc:                       # 设备没起来/被占用
        return {"ok": False, "error": "%s" % (exc,)[:120]}


def ask_gateway(text):
    """走和板子完全同一条路：本地分类器 → miIO → 一句中文。

    默认走 80 端口：:443 是同一台 httpd 的 TLS 监听，本板实测会周期性拒绝
    新连接（板子经 PPP 那条不受影响），面板不需要为它担这个风险。
    """
    body = json.dumps({"model": "cmd",
                       "messages": [{"role": "user", "content": text}]}).encode()
    t0 = time.time()
    reply = fetch("%s/v1/chat/completions" % STATE["scheme"],
                  data=body, timeout=30,
                  headers={"Content-Type": "application/json",
                           "Authorization": "Bearer local"})
    ms = int((time.time() - t0) * 1000)
    doc = json.loads(reply)
    return doc["choices"][0]["message"]["content"], ms


def console_alive(console, baud):
    """板子的控制台还活着吗？敲一个回车，看它回不回显/提示符。"""
    import serial

    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = console, baud, 0.15
    ser.rtscts = ser.dsrdtr = False
    ser.open()
    try:
        ser.reset_input_buffer()
        ser.write(b"\r\n")
        buf = b""
        end = time.time() + 2.0
        while time.time() < end:
            buf += ser.read(4096)
        return b"vela>" in buf or b"nsh>" in buf or len(buf) > 0
    finally:
        ser.close()


def console_send(console, baud, line, wait=1.5):
    """往板子控制台敲一行（不等待回显解析）。"""
    import serial

    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = console, baud, 0.15
    ser.rtscts = ser.dsrdtr = False
    ser.open()
    try:
        ser.write(line.encode("utf-8") + b"\r\n")
        time.sleep(wait)
    finally:
        ser.close()


def board_restart(console, baud, sftool, image, revive_bin):
    """把板子救回来：复位芯片 → 等它到 nsh> → 重拨 PPP → 起 Agent → 指到网关。

    板子会进入"控制台不回显、答复不派发"的卡死状态（本项目实测过多次，**只有复位能解**）。
    手工做要三五分钟：先 sftool 把芯片从下载态带出来，再一条条敲命令。演示时这一步
    放在镜头外点一下就好，所以做成按钮。

    返回 (是否成功, 过程说明)。
    """
    log = []

    if sftool and os.path.isfile(sftool):
        # 优先整包重烧：板子卡死时它最可靠（只写镜像头 4 KB 有时救不回来，
        # 今天实测过两种结果都有）。整包约 5.7 MB、几分钟；演示前不该省这一步。
        target = image if (image and os.path.isfile(image)) else revive_bin
        cmd = [sftool, "-c", "SF32LB52", "-p", console, "-b", str(baud),
               "--before", "default_reset", "--after", "soft_reset",
               "write_flash", "%s@0x12010000" % target]
        log.append("重烧 %s" % os.path.basename(target))
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        log.append("sftool rc=%d" % r.returncode)
        if r.returncode != 0:
            return False, "；".join(log + ["复位失败：%s" % r.stderr.strip()[:120]])
    else:
        return False, "未配置 sftool（启动时加 --sftool 指定路径）——请手动复位板子后重试"

    # 等它起来
    for _ in range(20):
        time.sleep(1.5)
        try:
            if console_alive(console, baud):
                break
        except Exception:
            continue

    console_send(console, baud, "pppd /dev/ttyS0 460800 &", wait=6)
    log.append("pppd 已下发")
    console_send(console, baud, "ai_agent", wait=6)
    log.append("ai_agent 已下发")
    console_send(console, baud, "set_llm https://10.0.0.1/v1/chat/completions cmd local", wait=3)
    log.append("set_llm 已下发")

    try:
        alive = console_alive(console, baud)
    except Exception as exc:
        alive = False
        log.append("检查公告台失败：%s" % (exc,)[:80])
    log.append("控制台%s" % ("有响应" if alive else "**仍无响应**"))
    return alive, "；".join(log)


def strip_ansi(text):
    return re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", text.replace("\r", ""))


# 板子在思考时会先念一句提示语，那不是答案。
THINKING_PHRASES = ("让我查一下", "稍等，处理中", "正在分析", "正在思考")

AGENT_LINE = re.compile(r"\[Agent\]:\s*([^\n]*)")


def agent_lines(raw):
    return [m.strip().split("vela>")[0].strip() for m in AGENT_LINE.findall(raw)]


def board_answer(raw):
    """板子的**最终答复**：跳过思考提示语。没有答案就返回空串。

    注意不要在这里回退到"最后一行"——思考提示语会让读取循环误以为已经答完了。
    """
    for line in reversed(agent_lines(raw)):
        if line and not any(line.startswith(p) for p in THINKING_PHRASES):
            return line
    return ""


def board_last_line(raw):
    lines = agent_lines(raw)
    return lines[-1] if lines else ""


def board_ask(text, console, baud, wait=25.0, quiet=3.0):
    """把这一句打进**板子的控制台**，把板子自己的回话读回来。

    为什么这么做：主控是板子——屏和喇叭都在它那边。面板想让观众看到/听到设备被控制，
    最直接的办法就是替人敲这一句 `ask`（和人在终端里敲的一模一样），然后在旁边等着
    把板子的回话读回来显示。这不是绕过板子，恰恰是让板子走完整条链。

    读的规矩（都踩过，所以写下来）：
    1. **写之前等到串口彻底安静**（`quiet` 秒没有新字节）。板子一条命令的尾巴很长——
       答复之后还有片段播报的那十几行、而且答复本身会打印两遍；不等它说完就发下一条，
       会把上一条的残留当成这一条的答案（实测真的报错过）。
    2. **丢掉与上一条答复相同的行**：同一句话打印两遍，第二遍正好落在下一条的窗口里。
    3. **跳过思考提示语**（"让我查一下…"这类），那不是答案。

    串口注意：开端口后**不动 DTR/RTS**。本板 RTS 接自动下载/复位电路，手工改电平会把
    芯片按在 ROM 下载态，表现出来就是"只有回显、什么都不执行"。
    """
    import serial

    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = console, baud, 0.15
    ser.rtscts = ser.dsrdtr = False
    buf = b""
    ser.open()
    try:
        # 1) 等到串口安静
        drain_until = time.time() + 1.0
        while time.time() < drain_until:
            chunk = ser.read(8192)
            if chunk:
                buf += chunk
                drain_until = time.time() + quiet

        known = agent_lines(strip_ansi(buf.decode("utf-8", "replace")))
        seen_answers = set(known) | {STATE.get("last_board_answer", "")}
        ser.reset_input_buffer()

        ser.write(("ask " + text).encode("utf-8") + b"\r\n")

        t0 = time.time()
        deadline = t0 + wait
        answered_at, answer = None, ""
        while time.time() < deadline:
            chunk = ser.read(8192)
            if chunk:
                buf += chunk
            lines = agent_lines(strip_ansi(buf.decode("utf-8", "replace")))
            fresh = [l for l in lines
                     if l and l not in seen_answers
                     and not any(l.startswith(p) for p in THINKING_PHRASES)]
            if fresh and answered_at is None:
                answer, answered_at = fresh[0], time.time()
                STATE["last_board_answer"] = fresh[0]
            if answered_at is not None and time.time() - answered_at >= 1.5:
                break

        raw = strip_ansi(buf.decode("utf-8", "replace"))
        if not answer:                      # 没等到答案：把最后一句提示语当诊断信息
            tail = [l for l in agent_lines(raw) if l not in seen_answers]
            answer = tail[-1] if tail else ""
        return answer, raw, int((time.time() - t0) * 1000)
    finally:
        ser.close()


def clip_played(raw):
    """板子实际播的离线片段名（有的话）——它是"板子真的出声了"的证据。"""
    hits = re.findall(r"clip '([^']+)'", raw)
    return hits[-1] if hits else ""


def add_event(kind, text, reply, ms, before=None, after=None, via=""):
    with STATE["lock"]:
        if kind == "panel" or via.startswith("面板代问"):
            # 记下这一句：网关的 /status 只看得到"最近一次问句"，分不清是板子
            # 经 PPP 问的还是面板代问的，下面用它来避免重复记一条。
            STATE["recent_panel"][text] = time.time()
        STATE["events"].insert(0, {
            "at": time.strftime("%H:%M:%S"), "kind": kind, "text": text,
            "reply": reply, "ms": ms, "via": via,
            "before": before, "after": after,
            "delta": device_delta(before, after),
        })
        del STATE["events"][40:]


def device_delta(before, after):
    """把"设备怎么变"压成一行：台灯: 开→关，亮度 50→80。

    共用同一个模拟器实例的设备只报一次——否则事件流里会出现"两台设备一起变"，
    看着像 bug，其实是同一台设备挂了两个名字。
    """
    if not before or not after:
        return ""
    out, shared = [], []
    for b, a in zip(before, after):
        if a.get("shared_with"):
            shared.append(a.get("label", a.get("name", "")))
            continue
        bs, as_ = b.get("state", {}), a.get("state", {})
        if not (bs.get("ok") and as_.get("ok")):
            continue
        parts = []
        if bs.get("power") != as_.get("power"):
            parts.append("开→关" if as_.get("power") == "off" else "关→开")
        for key, label in (("bright", "亮度"), ("ct", "色温")):
            if key in bs and key in as_ and bs.get(key) != as_.get(key):
                parts.append("%s %s→%s" % (label, bs.get(key), as_.get(key)))
        if parts:
            out.append("%s: %s" % (a.get("label", b.get("name", "")), "，".join(parts)))

    text = "；".join(out)
    if shared:
        note = "%s 与它共用同一个模拟器实例，状态同步变化" % "、".join(shared)
        text = ("%s（%s）" % (text, note)) if text else note
    return text


def is_recent_panel_ask(text):
    """这条"最近问句"是不是面板自己在几秒前发的？"""
    with STATE["lock"]:
        at = STATE["recent_panel"].get(text)
        if at is None:
            return False
        if time.time() - at > 15:
            del STATE["recent_panel"][text]
            return False
        return True


def snapshot():
    """面板每次轮询要的整份状态。"""
    gw = None
    try:
        gw = gateway_status()
        with STATE["lock"]:
            STATE["last_gateway"] = gw
            if gw.get("asked") and gw["asked"] != STATE["last_board_ask"]:
                prev = STATE["last_board_ask"]
                STATE["last_board_ask"] = gw["asked"]
                if prev and not is_recent_panel_ask(gw["asked"]):
                    STATE["events"].insert(0, {
                        "at": time.strftime("%H:%M:%S"), "kind": "board",
                        "text": gw["asked"], "reply": "(板子上屏/播报)",
                        "ms": None, "before": None, "after": None, "delta": "",
                    })
                    del STATE["events"][40:]
    except Exception as exc:
        with STATE["lock"]:
            STATE["last_gateway"] = None
        gw = {"error": "%s" % (exc,)[:120]}

    devices = []
    for d in STATE["devices"]:
        st = device_state(d)
        devices.append(dict(d, state=st))

    with STATE["lock"]:
        events = list(STATE["events"])
    return {"gateway": gw, "devices": devices, "events": events,
            "gateway_ip": STATE["gateway"], "miio": STATE["miio_ip"],
            "board_console": STATE.get("board_console", ""),
            "can_reset": bool(STATE.get("sftool"))}


PAGE = """<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
<title>TinyMind 演示面板</title><meta name="viewport" content="width=device-width,initial-scale=1">
<style>
 body{margin:0;background:#0f1216;color:#e6e9ee;font:15px/1.6 -apple-system,"Segoe UI",sans-serif}
 .wrap{max-width:1000px;margin:0 auto;padding:20px}
 h1{font-size:20px;margin:0 0 4px} .sub{color:#8b94a3;font-size:13px;margin-bottom:18px}
 .grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}
 .card{background:#171b21;border:1px solid #232a34;border-radius:10px;padding:14px}
 .card h2{font-size:14px;margin:0 0 10px;color:#a9b3c1;font-weight:600;letter-spacing:.02em}
 .row{display:flex;justify-content:space-between;gap:10px;padding:3px 0;font-size:13px}
 .k{color:#8b94a3} .v{text-align:right;word-break:break-all}
 .on{color:#4ade80} .off{color:#9aa3b0} .err{color:#f87171}
 .big{font-size:26px;font-weight:600;letter-spacing:.01em}
 button{background:#1f2630;color:#e6e9ee;border:1px solid #2c3540;border-radius:8px;
        padding:8px 12px;font-size:13px;cursor:pointer}
 button:hover{background:#27303c} button:active{transform:translateY(1px)}
 .btns{display:flex;flex-wrap:wrap;gap:6px;margin-top:10px}
 input{flex:1;background:#12161c;border:1px solid #2c3540;border-radius:8px;
       color:#e6e9ee;padding:9px 11px;font-size:14px}
 form{display:flex;gap:8px;margin-top:10px}
 .feed{margin-top:14px;max-height:320px;overflow:auto}
 .ev{display:flex;gap:10px;padding:7px 0;border-bottom:1px solid #1e242c;font-size:13px}
 .t{color:#6b7482;min-width:64px} .src{min-width:44px;font-weight:600}
 .board{color:#facc15} .panel{color:#60a5fa} .delta{color:#c084fc;font-size:12px}
 .note{color:#8b94a3;font-weight:400;font-size:12px}
 .ok{color:#4ade80} .no{color:#f87171} code{color:#c8d1dc}
</style></head><body><div class="wrap">
<h1>TinyMind 演示面板</h1>
<div class="sub">板子（黄山派）· 网关（ESP32-S3）· 被控端（米家设备）—— 同屏看全。
命令走的是<b>和板子完全同一条离线路径</b>：网关上的本地分类器 → 局域网 miIO。<span id="mode"></span></div>

<div class="grid">
  <div class="card"><h2>网关 / 链路</h2><div id="gw"></div></div>
  <div class="card"><h2>说一句（走本地分类器，不联网）</h2>
    <form onsubmit="say(event)">
      <input id="q" placeholder="例如：打开台灯 / 关闭客厅的灯 / 台灯调暖一点 / 打开空调" autocomplete="off">
      <button type="submit">发送</button>
    </form>
    <div class="btns" id="quick"></div>
    <div class="btns"><button id="resetbtn" onclick="boardReset()" style="display:none">
      ↻ 复位板子并重连（约 40 秒；板子卡死时点这个）</button></div>
    <div class="row" style="margin-top:10px"><span class="k">最近答复</span><span class="v" id="last"></span></div>
  </div>
</div>

<div class="grid" id="devs" style="margin-top:12px"></div>

<div class="card feed" style="margin-top:12px">
  <h2>事件流（谁问的 · 问了什么 · 设备怎么变）</h2>
  <div id="feed"></div>
</div>
</div>
<script>
const QUICK=["打开台灯","关闭台灯","台灯调亮一点","台灯暗一点","台灯调暖一点","打开空调","关闭空调"];
function esc(s){return (s==null?"":String(s)).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
async function say(e){e.preventDefault();const q=document.getElementById('q');
  const t=q.value.trim(); if(!t)return; q.value='';
  document.getElementById('last').innerHTML='<i class="k">执行中…</i>';
  await fetch('/api/say',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({text:t})}); tick();}
async function tick(){
 try{const s=await (await fetch('/api/state')).json();
  const g=s.gateway||{};
  document.getElementById('mode').innerHTML = s.board_console
    ? '　<b class="on">控制方式：由板子执行</b>（面板替你敲 ask，屏与喇叭都在板子上）'
    : '　<b class="off">控制方式：面板直接问网关 —— 板子不参与，屏与喇叭不会有反应</b>'+
      '（启动时加 --board-console COM5 可改为由板子执行）';
  var rb=document.getElementById('resetbtn');
  if(rb) rb.style.display = s.can_reset ? '' : 'none';
  document.getElementById('gw').innerHTML=
   row('网关地址',esc(s.gateway_ip))+
   row('本地模型',esc((g.model||'—').slice(0,40)))+
   row('链路',g.error? '<span class="err">连不上：'+esc(g.error)+'</span>':esc(g.uplink||'—'))+
   row('板子',g.error?'<span class="err">未知</span>':
     '<span class="'+(String(g.board).includes('up')?'on':'off')+'">'+esc(g.board||'—')+'</span>')+
   row('最近路由',esc(g.route||'—'))+
   row('信号',esc(g.rssi||'—'))+
   row('板子刚问',esc(g.asked||'（还没有）'));
  document.getElementById('devs').innerHTML=s.devices.map(d=>{
   const st=d.state||{}; const on=String(st.power)==='on';
   const fields=(st.fields||[]).map(f=>row(esc(f.label),
       esc(f.value==null?'—':String(f.value)+(f.label==='色温'?' K':'')))).join('')
     +row('读取时间',esc(st.at||''));
   const mine=d.name==='lamp'?QUICK.filter(q=>!q.includes('空调'))
                             :QUICK.filter(q=>q.includes('空调'));
   return '<div class="card"><h2>'+esc(d.label)+' · '+esc(d.model)+
     (d.shared_with?' <span class="note">与「'+esc(d.shared_with)+
       '」共用同一个官方模拟器实例（UDP 54321，一台主机只能起一个）</span>':'')+'</h2>'+
    '<div class="big '+(st.ok?(on?'on':'off'):'err')+'">'+
      (st.ok?(on?'已打开':'已关闭'):'读不到状态')+'</div>'+
    (st.ok?fields:row('原因',esc(st.error)))+
    '<div class="btns">'+mine.map(q=>'<button onclick="send(\\''+q+'\\')">'+q+'</button>').join('')+
    '</div></div>';}).join('');
  document.getElementById('feed').innerHTML=(s.events||[]).map(e=>
   '<div class="ev"><span class="t">'+esc(e.at)+'</span>'+
   '<span class="src '+(e.kind==='board'?'board':'panel')+'">'+(e.kind==='board'?'板子':'面板')+'</span>'+
   '<span>'+esc(e.text)+' → <b>'+esc(e.reply)+'</b>'+
   (e.ms!=null?' <code>'+e.ms+' ms</code>':'')+
   (e.via?' <span class="note">'+esc(e.via)+'</span>':'')+
   (e.delta?'<br><span class="delta">设备变化：'+esc(e.delta)+'</span>':'')+'</span></div>').join('')
   ||'<div class="k">还没有事件</div>';
 }catch(err){document.getElementById('gw').innerHTML='<span class="err">面板内部错误：'+esc(err)+'</span>';}
}
function row(k,v){return '<div class="row"><span class="k">'+k+'</span><span class="v">'+v+'</span></div>'}
async function boardReset(){
  document.getElementById('resetbtn').disabled=true;
  document.getElementById('last').innerHTML='<i class="k">正在复位板子并重连（约 40 秒，别动串口）…</i>';
  try{const r=await (await fetch('/api/board-reset',{method:'POST'})).json();
    document.getElementById('last').innerHTML = r.ok
      ? '<span class="on">板子已就绪，试一句看看</span>'
      : '<span class="err">复位后仍无响应：'+esc(r.detail)+'</span>';
  }catch(e){document.getElementById('last').innerHTML='<span class="err">复位失败：'+esc(e)+'</span>';}
  document.getElementById('resetbtn').disabled=false; tick();
}
async function send(t){document.getElementById('last').innerHTML='<i class="k">执行中…</i>';
 await fetch('/api/say',{method:'POST',headers:{'Content-Type':'application/json'},
   body:JSON.stringify({text:t})}); tick();}
document.getElementById('quick').innerHTML=QUICK.map(q=>'<button onclick="send(\\''+q+'\\')">'+q+'</button>').join('');
tick(); setInterval(tick,2500);
</script></body></html>"""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):        # 面板自己不需要访问日志刷屏
        pass

    def _send(self, code, body, ctype):
        data = body.encode("utf-8") if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path == "/" or self.path.startswith("/index"):
            return self._send(200, PAGE, "text/html; charset=utf-8")
        if self.path.startswith("/api/state"):
            return self._send(200, json.dumps(snapshot(), ensure_ascii=False),
                              "application/json; charset=utf-8")
        return self._send(404, "not found", "text/plain; charset=utf-8")

    def do_POST(self):
        if self.path.startswith("/api/board-reset"):
            try:
                ok, detail = board_restart(STATE["board_console"], STATE["board_baud"],
                                           STATE["sftool"], STATE["image"],
                                           STATE["revive_bin"])
            except Exception as exc:
                ok, detail = False, "%s" % (exc,)[:200]
            add_event("panel", "面板按钮：复位板子并重连",
                      ("板子已就绪" if ok else "复位后仍无响应") + "（" + detail + "）",
                      None, None, None, via="面板操作")
            return self._send(200, json.dumps({"ok": ok, "detail": detail},
                                              ensure_ascii=False),
                              "application/json; charset=utf-8")

        if not self.path.startswith("/api/say"):
            return self._send(404, "not found", "text/plain; charset=utf-8")

        length = int(self.headers.get("Content-Length", "0"))
        try:
            text = json.loads(self.rfile.read(length) or b"{}").get("text", "").strip()
        except Exception:
            return self._send(400, json.dumps({"error": "bad json"}), "application/json")

        if not text:
            return self._send(400, json.dumps({"error": "empty"}), "application/json")

        before = [dict(d, state=device_state(d)) for d in STATE["devices"]]
        console = STATE.get("board_console")
        via = ""

        if console:
            # 走板子：面板替人敲 `ask`，让**板子**去问网关——屏和喇叭因此参与进来。
            try:
                reply, raw, relay_ms = board_ask(text, console, STATE["board_baud"])
                reply = reply or "(板子没回话：确认它停在 vela> 且已 set_llm … cmd)"
                clip = clip_played(raw)
                via = "面板代问 · 板子播报 clip '%s'" % clip if clip else \
                      "面板代问（板子出的屏与声）"
                ms, err = relay_ms, None
            except Exception as exc:
                reply, ms, via = "板子控制台打不开", None, ""
                err = "%s（可能被别的终端占用）" % (exc,)[:120]
        else:
            try:
                reply, ms = ask_gateway(text)
                err = None
            except Exception as exc:
                reply, ms, err = "网关没有回应", None, "%s" % (exc,)[:150]

        time.sleep(0.4)                       # 给模拟器留一拍再读状态
        after = [dict(d, state=device_state(d)) for d in STATE["devices"]]
        add_event("board" if console else "panel", text,
                  reply if err is None else "%s: %s" % (reply, err),
                  ms, before, after, via)
        return self._send(200, json.dumps({"reply": reply, "ms": ms, "error": err,
                                           "via": via}, ensure_ascii=False),
                          "application/json; charset=utf-8")


def load_devices(sdkconfig):
    """设备表优先从 S3 的 sdkconfig 读，读不到就用默认（官方模拟器两台）。

    这里只取名称、IP、型号与 token（token 只留在内存里用于组包，不回显）；
    两份相同的 (IP, token) 会被标出来——那是"同一个实例挂了两个设备名"，
    面板上必须说清楚，否则两张卡片一起变会让人以为是 bug。
    """
    rows = []
    try:
        with open(sdkconfig, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                m = re.match(r'CONFIG_GATEWAY_MIIO_TABLE="(.*)"', line.strip())
                if not m or not m.group(1):
                    continue
                for part in m.group(1).split(";"):
                    f = part.split(",")
                    if len(f) >= 4 and re.fullmatch(r"[0-9a-fA-F]{32}", f[2]):
                        rows.append({"name": f[0], "label": LABELS.get(f[0], f[0]),
                                     "model": f[3], "ip": f[1], "token": f[2].lower()})
    except OSError:
        pass

    if not rows:
        rows = [dict(d, ip="127.0.0.1", token=SIM_TOKEN) for d in FALLBACK_DEVICES]
    return mark_sharing(rows)


def mark_sharing(rows):
    """标出"同一实例挂了两个设备名"——面板上必须说清楚，否则两张卡片一起变像 bug。"""
    seen = {}
    for d in rows:
        key = (d["ip"], d["token"])
        d["shared_with"] = LABELS.get(seen[key], seen[key]) if key in seen else ""
        seen.setdefault(key, d["name"])
    return rows


LABELS = {"lamp": "台灯", "ac": "空调"}


def main():
    ap = argparse.ArgumentParser(description="TinyMind 演示面板")
    ap.add_argument("--gateway", default=DEFAULT_GATEWAY, help="S3 网关的 IP")
    ap.add_argument("--port", type=int, default=8080, help="面板监听端口")
    ap.add_argument("--https", action="store_true",
                    help="用 https 访问网关（默认 http；:443 是同一台 httpd 的 TLS 口，"
                         "本板实测会周期性拒绝新连接）")
    ap.add_argument("--miio-ip", default="127.0.0.1", help="被控设备 IP（默认本机的官方模拟器）")
    ap.add_argument("--token", default=SIM_TOKEN, help="设备 token（默认为官方模拟器的全零常量）")
    ap.add_argument("--sdkconfig", default="",
                    help="可选：从这份 sdkconfig 读设备表（只读名称与型号，不打印 token）")
    ap.add_argument("--board-console", default="",
                    help="板子的控制台串口（例如 COM5）。给了它就由面板替人敲 `ask`——"
                         "这样屏和喇叭都由板子出；不给则面板直接问网关（板子不参与）")
    ap.add_argument("--board-baud", type=int, default=1000000,
                    help="板子控制台波特率（默认 1000000）")
    ap.add_argument("--sftool", default="",
                    help="sftool 的路径。给了它面板就能一键把卡死的板子复位并重连"
                         "（板子会进入控制台不回显/答复不派发的卡死状态，只有复位能解）")
    ap.add_argument("--image", default="",
                    help="板子固件镜像（配合 --revive-bin 从镜像里取前 4 KB 做复位）")
    ap.add_argument("--revive-bin", default="head4k.bin",
                    help="复位用的 4 KB 片段文件（默认当前目录的 head4k.bin）")
    args = ap.parse_args()

    STATE["gateway"] = args.gateway
    STATE["scheme"] = ("https://" if args.https else "http://") + args.gateway
    STATE["miio_ip"] = args.miio_ip
    STATE["board_console"] = args.board_console
    STATE["board_baud"] = args.board_baud
    STATE["sftool"] = args.sftool
    STATE["image"] = args.image
    STATE["revive_bin"] = args.revive_bin
    STATE["miio_token"] = args.token
    STATE["devices"] = load_devices(args.sdkconfig) if args.sdkconfig else \
        mark_sharing([dict(d, ip=args.miio_ip, token=args.token)
                      for d in FALLBACK_DEVICES])

    print("网关    : %s" % args.gateway)
    print("被控端  : %s:%d（token %s）" % (args.miio_ip, STATE["sim_port"],
          "全零常量" if args.token == SIM_TOKEN else "已提供"))
    print("设备    : %s" % ", ".join("%s(%s)" % (d["label"], d["model"]) for d in STATE["devices"]))
    print("面板    : http://127.0.0.1:%d" % args.port)
    if args.board_console:
        print("控制方式: 打到板子控制台 %s —— 屏与喇叭由板子出" % args.board_console)
    else:
        print("控制方式: 面板直接问网关（**板子不参与**，屏与喇叭不会有反应）")
        print("          想让板子出屏与声：加 --board-console COM5（板子要停在 vela> 并已 set_llm … cmd）")
    ThreadingHTTPServer(("0.0.0.0", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
