# -*- coding: utf-8 -*-
"""一个最小的 miIO 设备端模拟器（UDP 54321），给"没有真台灯"时当被控端用。

为什么自己写：python-miio 0.5.12 已经把 `devtools miio-simulator` 移除了，官方
模拟器现在跑不起来。报文格式不是猜的——`miio_protocol_check.py` 已经拿 python-miio
的参考实现逐字段对拍过（magic / 大端 length / 16 字节头 / checksum 按密文算 /
key=MD5(token) / iv=MD5(key+token) / PKCS7 / json+0），本脚本用的是同一套格式。

诚实标注：它是**模拟设备**，不是真台灯。协议是真的，状态机是假的。
换成真设备的做法：把 S3 设备表里的 IP 与 token 换成真灯的即可，代码不用改。

用法：
    set MIIO_TOKEN=<32 位十六进制>            &  python miio_device_sim.py
    python miio_device_sim.py --token <32hex> --model yeelink.light.color3 --port 54321

安全：token 只从命令行/环境变量读，**从不打印**；日志里只出现指令与状态。
"""
import argparse
import hashlib
import json
import os
import socket
import struct
import sys
import time

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

MAGIC = 0x2131
PORT = 54321
DEVICE_ID = 0x0A0B0C0D
MODEL_DEFAULT = "yeelink.light.color3"


def key_iv(token):
    key = hashlib.md5(token).digest()
    return key, hashlib.md5(key + token).digest()


def encrypt(token, obj):
    payload = json.dumps(obj, separators=(",", ":")).encode("utf-8") + b"\x00"
    pad = 16 - (len(payload) % 16)
    payload += bytes([pad]) * pad
    key, iv = key_iv(token)
    enc = Cipher(algorithms.AES(key), modes.CBC(iv)).encryptor().update(payload)
    hdr = struct.pack(">HHIII", MAGIC, len(enc) + 32, 0, DEVICE_ID,
                      int(time.time()) & 0xFFFFFFFF)
    return hdr + hashlib.md5(hdr + token + enc).digest() + enc


def decrypt(token, pkt):
    key, iv = key_iv(token)
    dec = Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor().update(pkt[32:])
    dec = dec[: len(dec) - dec[-1]]
    return json.loads(dec.rstrip(b"\x00").decode("utf-8"))


class Lamp:
    """一台 Yeelight 台灯该有的那点状态。"""

    def __init__(self, model, token_hex):
        self.model = model
        self.token_hex = token_hex
        self.power = "off"
        self.bright = 50
        self.ct = 4000
        self.color_mode = 2
        self.hue = 0
        self.sat = 0
        self.name = "台灯"

    def prop(self, which):
        table = {
            "power": self.power,
            "bright": self.bright,
            "ct": self.ct,
            "color_mode": self.color_mode,
            "hue": self.hue,
            "sat": self.sat,
            "name": self.name,
            "rgb": 0xFFFFFF,
            "flowing": 0,
            "delayoff": 0,
        }
        return table.get(which, "unknown")

    def handle(self, req):
        method = req.get("method", "")
        params = req.get("params", []) or []
        rid = req.get("id", 0)

        if method == "miIO.info":
            return {"id": rid, "result": {
                "life": 1, "uid": 0, "model": self.model,
                "mac": "AA:BB:CC:DD:EE:FF", "fw_ver": "1.0.0", "hw_ver": "0.1",
                "token": self.token_hex}}

        if method == "get_prop":
            return {"id": rid, "result": [self.prop(p) for p in params]}

        if method == "set_power":
            want = params[0] if params else "on"
            if want == "toggle":
                self.power = "off" if self.power == "on" else "on"
            else:
                self.power = want
            print("  <- set_power %s    => power=%s" % (want, self.power), flush=True)
            return {"id": rid, "result": ["ok"]}

        if method == "toggle":
            self.power = "off" if self.power == "on" else "on"
            print("  <- toggle         => power=%s" % self.power, flush=True)
            return {"id": rid, "result": ["ok"]}

        if method == "set_bright":
            if params:
                self.bright = int(params[0])
            print("  <- set_bright %s  => bright=%d" % (params[:1], self.bright), flush=True)
            return {"id": rid, "result": ["ok"]}

        if method == "set_ct_abx":
            if params:
                self.ct = int(params[0])
            print("  <- set_ct_abx %s  => ct=%d" % (params[:1], self.ct), flush=True)
            return {"id": rid, "result": ["ok"]}

        if method in ("set_name", "set_default", "set_scene", "set_adjust"):
            print("  <- %s %s (接受但不改状态)" % (method, params), flush=True)
            return {"id": rid, "result": ["ok"]}

        print("  <- %s (不支持)" % method, flush=True)
        return {"id": rid, "error": {"code": -1, "message": "unsupported method %s" % method}}


def main():
    ap = argparse.ArgumentParser(description="最小 miIO 设备端模拟器")
    ap.add_argument("--token", default=os.environ.get("MIIO_TOKEN", ""))
    ap.add_argument("--model", default=MODEL_DEFAULT)
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--host", default="0.0.0.0")
    args = ap.parse_args()

    if len(args.token) != 32:
        print("需要 32 位十六进制 token：--token 或环境变量 MIIO_TOKEN")
        return 2

    token = bytes.fromhex(args.token)
    lamp = Lamp(args.model, args.token)

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    # Windows 会把对端发来的 ICMP 端口不可达当成 recvfrom 上的连接重置
    # （WinError 10054）。跑在被控端的进程当然不能因此退出。
    try:
        s.ioctl(socket.SIO_UDP_CONNRESET, False)
    except (AttributeError, OSError):
        pass
    s.bind((args.host, args.port))

    print("模拟设备已启动：%s:%d  model=%s  power=%s（token 已加载，不显示）"
          % (args.host, args.port, args.model, lamp.power), flush=True)
    print("hello 会回 32 字节里的 token；之后按已验证的报文格式收发。", flush=True)

    while True:
        try:
            data, addr = s.recvfrom(4096)
        except ConnectionResetError:
            continue                      # 见上面的 SIO_UDP_CONNRESET 说明
        if len(data) == 32:                       # hello
            reply = (struct.pack(">HHIII", MAGIC, 32, 0, DEVICE_ID,
                                 int(time.time()) & 0xFFFFFFFF) + token)
            s.sendto(reply, addr)
            print("-> hello from %s:%d（回了 token，不显示）" % addr, flush=True)
            continue

        if len(data) <= 32:
            print("-> 忽略了 %d 字节的怪包" % len(data), flush=True)
            continue

        try:
            req = decrypt(token, data)
        except Exception as ex:                   # noqa: BLE001
            print("-> 解不开来自 %s:%d 的包：%s（token 不对？）"
                  % (addr[0], addr[1], type(ex).__name__), flush=True)
            continue

        print("-> %s:%d  %s" % (addr[0], addr[1], req.get("method", "?")), flush=True)
        resp = lamp.handle(req)
        s.sendto(encrypt(token, resp), addr)


if __name__ == "__main__":
    sys.exit(main())
