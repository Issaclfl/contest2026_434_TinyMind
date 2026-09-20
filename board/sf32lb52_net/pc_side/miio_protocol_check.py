# -*- coding: utf-8 -*-
"""拿 python-miio 的参考实现当裁判，逐字段核对 net_miio.c 的报文布局。

为什么不用官方模拟器：0.5.12 已把 `devtools miio-simulator` 移除（老文档里的写法
现在跑不起来）。而报文格式的**事实标准就是这份参考实现**——真机认它、社区命令
行工具也认它，所以直接跟它逐字节对拍，比跟自己写的模拟器对拍更有说服力。

判定四件事：
  1. 头部字段的偏移与字节序（magic / 大端 length / 16 字节头）
  2. 密文是否逐字节相同（key=MD5(token) / iv=MD5(key+token) / PKCS7 / json+0）
  3. **checksum 是按密文还是按明文算**（参考实现自己建的包说了算）
  4. 我这边组出来的包，参考实现能不能解析回来

用法：python miio_protocol_check.py
"""
import datetime
import hashlib
import json
import struct
import sys

from miio.protocol import Message, Utils

TOKEN = bytes(range(16))          # 测试用 token，不是任何真设备的
OBJ = {"id": 1, "method": "get_prop", "params": ["power"]}


# ---------------------------------------------------------------- 我的实现
# 与 board/sf32lb52_net/esp32s3_gateway/main/net_miio.c 一一对应。

def my_key_iv(token):
    key = hashlib.md5(token).digest()
    iv = hashlib.md5(key + token).digest()
    return key, iv


def my_build(token, obj, checksum_over_ciphertext=True):
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

    payload = json.dumps(obj, separators=(",", ":")).encode("utf-8") + b"\x00"
    pad = 16 - (len(payload) % 16)
    payload += bytes([pad]) * pad

    key, iv = my_key_iv(token)
    enc = Cipher(algorithms.AES(key), modes.CBC(iv)).encryptor().update(payload)

    hdr = struct.pack(">HHIII", 0x2131, len(enc) + 32, 0, 0, 0)   # 16 字节
    assert len(hdr) == 16

    hashed = enc if checksum_over_ciphertext else payload
    checksum = hashlib.md5(hdr + token + hashed).digest()
    return hdr + checksum + enc


def my_parse(token, pkt):
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

    key, iv = my_key_iv(token)
    dec = Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor().update(pkt[32:])
    dec = dec[: len(dec) - dec[-1]]           # 去 PKCS7 填充
    return dec.rstrip(b"\x00")


# ------------------------------------------------------------ 参考实现组包

def ref_build(obj):
    msg = {
        "data": {"value": obj},
        "header": {"value": {"length": 0, "unknown": 0,
                             "device_id": 0x01020304,
                             "ts": datetime.datetime.fromtimestamp(1234567890, datetime.timezone.utc)}},
        "checksum": 0,
    }
    return Message.build(msg, token=TOKEN)


def main():
    pkt = ref_build(OBJ)
    mine = my_build(TOKEN, OBJ)
    fails = []

    print("== 1. 头部 ==")
    print("  参考实现: magic=%s length_be=%d 总长=%d 头长=%d"
          % (pkt[0:2].hex(), int.from_bytes(pkt[2:4], "big"), len(pkt), 32))
    print("  我的实现: magic=%s length_be=%d 总长=%d 头长=%d"
          % (mine[0:2].hex(), int.from_bytes(mine[2:4], "big"), len(mine), 32))
    if pkt[0:2] != mine[0:2] or int.from_bytes(pkt[2:4], "big") != len(pkt):
        fails.append("头部字段不一致")
    if len(pkt) != len(mine):
        fails.append("总长不一致")

    print("== 2. 明文（密文依赖明文，而两端 JSON 的空格可以不同）==")
    ref_plain = my_parse(TOKEN, pkt)
    my_plain = my_parse(TOKEN, mine)
    same = json.loads(ref_plain.decode()) == json.loads(my_plain.decode())
    print("  参考实现明文:", ref_plain)
    print("  我的明文    :", my_plain)
    print("  语义相同:", same)
    if not same:
        fails.append("明文内容不同（key/iv/填充/末尾 0 的约定有出入）")
    if pkt[32:] == mine[32:]:
        print("  （顺带：密文也逐字节相同）")

    print("== 3. checksum 按什么算 ==")
    plain = Utils.decrypt(pkt[32:], TOKEN)
    over_cipher = hashlib.md5(pkt[:16] + TOKEN + pkt[32:]).digest()
    over_plain = hashlib.md5(pkt[:16] + TOKEN + plain).digest()
    got = pkt[16:32]
    print("  参考实现的 checksum:", got.hex())
    print("  按密文重算        :", over_cipher.hex(), "匹配" if over_cipher == got else "不匹配")
    print("  按明文重算        :", over_plain.hex(), "匹配" if over_plain == got else "不匹配")
    over_cipher_is_it = over_cipher == got
    if not (over_cipher_is_it or over_plain == got):
        fails.append("两种算法都不匹配（说明还有别的约定）")

    print("== 4. 我的包给参考实现解析 ==")
    try:
        m = Message.parse(mine, token=TOKEN)
        val = m.data.value if hasattr(m.data, "value") else m.data
        back = val if isinstance(val, dict) else json.loads(val.rstrip(b"\x00").decode())
        print("  解析成功:", back)
        if back != OBJ:
            fails.append("解出来的内容与原始对象不一致")
    except Exception as ex:                                    # noqa: BLE001
        print("  解析失败:", type(ex).__name__, ex)
        fails.append("参考实现解析不了我的包")

    print("== 5. 参考实现的包给我的解析 ==")
    got_obj = json.loads(my_parse(TOKEN, pkt).decode())
    print("  解析结果:", got_obj)
    if got_obj != OBJ:
        fails.append("我解不出参考实现的包")

    print()
    if fails:
        print("结论：不通过 —— %s" % "；".join(fails))
        if over_plain == got and not over_cipher_is_it:
            print("      注意：checksum 要按**明文**算，net_miio.c 需要改。")
        return 1
    print("结论：通过（checksum 按%s算）" % ("密文" if over_cipher_is_it else "明文"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
