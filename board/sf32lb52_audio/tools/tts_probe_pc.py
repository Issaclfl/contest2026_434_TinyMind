# -*- coding: utf-8 -*-
"""PC-side cross-check of the MiMo TTS contract (the same request the board sends).

Why this exists: the board calls `mimo-v2.5-tts` through
`POST /v1/chat/completions` (see packages/ai_agent/src/voice/mimo_tts.c) and
sometimes comes back `HTTP 200 ... no audio`.  Running the byte-identical
request from the PC separates "the cloud contract is wrong" from "the board's
link cannot carry the answer".

Safety rules (same as the rest of this project):
  * the API key is read from the gitignored gateway `sdkconfig` and is never
    printed, never logged, never written to a file;
  * the raw response and the decoded WAV go to the OS temp directory only --
    nothing is written inside the repository.

Usage:
    python tts_probe_pc.py ["要说的话"] [sdkconfig 路径]

Default sdkconfig path (also settable via $GATEWAY_SDKCONFIG):
    C:\\Users\\Lawson\\sf32lb52_net_build\\esp32s3_gateway\\sdkconfig
"""
import json
import os
import re
import struct
import sys
import urllib.error
import urllib.request

URL = "https://token-plan-cn.xiaomimimo.com/v1/chat/completions"
MODEL = "mimo-v2.5-tts"
VOICE = "冰糖"

DEFAULT_SDKCONFIG = os.environ.get(
    "GATEWAY_SDKCONFIG",
    r"C:\Users\Lawson\sf32lb52_net_build\esp32s3_gateway\sdkconfig",
)

TEXT = sys.argv[1] if len(sys.argv) > 1 else "你好，我是 TinyMind"
SDKCONFIG = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_SDKCONFIG

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:  # noqa: BLE001
    pass


def load_key(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = re.match(r'^CONFIG_GATEWAY_CLOUD_API_KEY="(.*)"', line.strip())
            if m and m.group(1):
                return m.group(1)
    return None


def main():
    key = load_key(SDKCONFIG)
    if not key:
        print("FAIL: no key in %s" % SDKCONFIG)
        return 2
    print("key loaded: %d chars (not printed)" % len(key))

    # Byte-identical to mimo_tts.c build_body(): no spaces, same key order.
    body = ('{"model":"%s","messages":[{"role":"assistant","content":"%s"}],'
            '"audio":{"format":"wav","voice":"%s"}}' % (MODEL, TEXT, VOICE))
    print("request body: %d bytes  text=%r" % (len(body.encode("utf-8")), TEXT))

    req = urllib.request.Request(URL, data=body.encode("utf-8"), method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("Authorization", "Bearer " + key)

    try:
        with urllib.request.urlopen(req, timeout=300) as r:
            code, clen, raw = r.status, r.headers.get("Content-Length"), r.read()
    except urllib.error.HTTPError as e:
        code, clen, raw = e.code, e.headers.get("Content-Length"), e.read()
    except Exception as e:  # noqa: BLE001
        print("FAIL: transport: %r" % (e,))
        return 3

    tmp = os.environ.get("TEMP", os.environ.get("TMPDIR", "/tmp"))
    print("HTTP %s  Content-Length header=%s  body=%d bytes" % (code, clen, len(raw)))

    out = os.path.join(tmp, "mimo_tts_response.json")
    with open(out, "wb") as fh:
        fh.write(raw)
    print("raw response saved to %s" % out)

    txt = raw.decode("utf-8", "replace")
    print("ends with '}': %s" % txt.rstrip().endswith("}"))

    try:
        doc = json.loads(txt)
        print("full JSON parses: True")
    except Exception as e:  # noqa: BLE001
        print("full JSON parses: False (%s)" % e)
        return 4

    if isinstance(doc, dict) and "error" in doc:
        print("SERVER ERROR: %s" % json.dumps(doc["error"], ensure_ascii=False)[:400])

    try:
        msg = doc["choices"][0]["message"]
    except Exception as e:  # noqa: BLE001
        print("message shape unexpected: %r" % (e,))
        return 5

    print("message keys: %s" % sorted(msg.keys()))
    audio = msg.get("audio")
    if not isinstance(audio, dict):
        print("NO audio field; content=%r" % (str(msg.get("content"))[:300],))
        return 6

    print("audio keys: %s" % sorted(audio.keys()))
    b64 = audio.get("data") or ""
    print("base64 chars: %d  -> PCM bytes ~%d" % (len(b64), len(b64) // 4 * 3))

    import base64
    wav = base64.b64decode(b64 + "=" * (-len(b64) % 4))
    wpath = os.path.join(tmp, "mimo_tts_audio.wav")
    with open(wpath, "wb") as fh:
        fh.write(wav)
    print("wav saved to %s (%d bytes)" % (wpath, len(wav)))

    if wav[:4] == b"RIFF":
        ch, rate, _br, _ba, bits = struct.unpack("<HIIHH", wav[22:36])
        secs = (len(wav) - 44) / (rate * ch * bits / 8)
        print("WAV: %d Hz, %d ch, %d bits, %d bytes of PCM (~%.1f s)"
              % (rate, ch, bits, len(wav) - 44, secs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
