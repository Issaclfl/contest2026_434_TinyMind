#!/bin/bash
# ##############################################################################
# ppp_up.sh —— 把板子的 PPP 链路接进 WSL 的网络
#
# 数据通路：
#   板子 UART2 --3 根线--> USB-TTL --USB--> Windows COMx
#                          serial_tcp_bridge.py (Windows 上跑)
#                                | TCP
#                          socat 造出 PTY /tmp/ttyPPP
#                                |
#                              pppd  10.0.0.1 <-> 10.0.0.2
#                                |
#                              ppp0  --MASQUERADE--> eth0 --> 外网
#
# 需要 root：pppd 要开 /dev/ppp，iptables 也要。用下面的方式跑：
#   wsl.exe -d Ubuntu-24.04 -u root -e bash -lc 'bash .../ppp_up.sh'
#
# 用法：
#   ppp_up.sh [tcp_host] [tcp_port] [local_ip] [remote_ip] [baud]
# 默认：
#   ppp_up.sh <自动探测> 5555 10.0.0.1 10.0.0.2 460800
#
# 自动探测的是 WSL 的默认网关——在 NAT 模式下那就是 Windows 宿主，
# 也正是 serial_tcp_bridge.py 所在的地方。这个地址每次 WSL 重启都可能变，
# 所以不要写死。
# ##############################################################################

set -e

# iptables / sysctl 在 /usr/sbin 与 /sbin 下，而 root 的非登录 shell 默认不带
# 这两个目录（用 wsl -u root -e bash 跑时尤其明显）。
export PATH="$PATH:/usr/sbin:/sbin"

# Windows 宿主在 WSL 视角下的地址（NAT 模式下即 eth0 的默认网关）
HOST="${1:-$(ip route show default | awk '{print $3; exit}')}"
PORT="${2:-5555}"
LOCAL_IP="${3:-10.0.0.1}"
REMOTE_IP="${4:-10.0.0.2}"
BAUD="${5:-460800}"

PTY_LINK=/tmp/ttyPPP
PPP_LOG=/tmp/ppp.log
SOCAT_LOG=/tmp/socat.log
PPP_IF=ppp0

say() { echo "[ppp_up] $*"; }

# ---------- 0. 先算好网关接口与 MSS ------------------------------------------
WAN="$(ip route show default | awk '{print $5; exit}')"
if [ -z "$WAN" ]; then
  say "错误：找不到默认路由接口" >&2
  exit 1
fi

WAN_MTU="$(cat "/sys/class/net/$WAN/mtu")"
# 板子的 TUN 缓冲即链路 MTU，是 1500；而 WSL 的虚拟网卡通常只有 1280。
# 不在 SYN 上夹住 MSS，板子会按 1500 发，转发到 $WAN 时就会超 MTU。
MSS=$((WAN_MTU - 40))
say "出口接口 $WAN (MTU $WAN_MTU)，MSS 夹到 $MSS"

# ---------- 1. 清掉上一次的残留 ----------------------------------------------
pkill -f "pppd $PTY_LINK" 2>/dev/null || true
pkill -f "socat .*$PTY_LINK" 2>/dev/null || true
sleep 0.5
rm -f "$PTY_LINK"

# ---------- 2. socat：TCP <-> PTY --------------------------------------------
# waitslave 让 socat 先不连 TCP，等 pppd 打开从设备后再连，
# 这样板子最早的 LCP 帧不会被丢在没人读的管道里。
setsid socat -d -d \
  "TCP:$HOST:$PORT,forever,intervall=1" \
  "PTY,link=$PTY_LINK,raw,echo=0,waitslave" \
  >"$SOCAT_LOG" 2>&1 &

for _ in $(seq 1 50); do
  [ -e "$PTY_LINK" ] && break
  sleep 0.1
done
if [ ! -e "$PTY_LINK" ]; then
  say "错误：socat 没有造出 $PTY_LINK，看 $SOCAT_LOG" >&2
  exit 1
fi
say "PTY 就绪：$PTY_LINK -> $HOST:$PORT"

# ---------- 3. 转发与 NAT ----------------------------------------------------
sysctl -q -w net.ipv4.ip_forward=1

add_rule() {  # add_rule <table> <chain> <rule...>
  local table="$1" chain="$2"; shift 2
  if [ "$table" = filter ]; then
    iptables -C "$chain" "$@" 2>/dev/null || iptables -A "$chain" "$@"
  else
    iptables -t "$table" -C "$chain" "$@" 2>/dev/null || \
      iptables -t "$table" -A "$chain" "$@"
  fi
}

add_rule nat    POSTROUTING -o "$WAN" -s "$REMOTE_IP/32" -j MASQUERADE
add_rule filter FORWARD     -i "$PPP_IF" -o "$WAN" -j ACCEPT
add_rule filter FORWARD     -i "$WAN" -o "$PPP_IF" \
         -m state --state RELATED,ESTABLISHED -j ACCEPT
add_rule mangle FORWARD     -o "$PPP_IF" -p tcp --tcp-flags SYN,RST SYN \
         -j TCPMSS --set-mss "$MSS"
add_rule mangle FORWARD     -i "$PPP_IF" -p tcp --tcp-flags SYN,RST SYN \
         -j TCPMSS --set-mss "$MSS"

say "转发与 NAT 就绪"

# ---------- 4. pppd ----------------------------------------------------------
# noauth      对端不需要认证（这是直连串口，不是运营商）
# local       不走调制解调器控制线
# persist     ppp0 掉线后自动重连
# maxfail 0   一直重试
# noipdefault 本端地址由命令行给定，不要去找
# ms-dns      给对端下发 DNS（板子的 pppd 目前不解析该选项，只是留个正规做法）
setsid pppd "$PTY_LINK" "$BAUD" "$LOCAL_IP:$REMOTE_IP" \
  noauth local nocrtscts persist maxfail 0 noipdefault \
  mtu 1500 mru 1500 ms-dns 223.5.5.5 \
  debug nodetach \
  >"$PPP_LOG" 2>&1 &

# ---------- 5. 等链路起来 ----------------------------------------------------
# 板子的 pppd 会反复拒绝 CCP / IPv6CP，一来一回要花些时间；30 秒偶尔不够，
# 给到 90 秒。等不到也不拆链路，好让下一次重试能接上。
for _ in $(seq 1 180); do
  if ip -4 addr show "$PPP_IF" 2>/dev/null | grep -q "inet "; then
    say "链路已建立："
    ip -4 addr show "$PPP_IF" | sed 's/^/         /'
    say "板子侧地址 $REMOTE_IP，日志 $PPP_LOG"
    exit 0
  fi

  if grep -q "LCP: timeout\|Modem hangup\|Serial link is not 8-bit" \
       "$PPP_LOG" 2>/dev/null; then
    say "pppd 报错，日志尾部：" >&2
    tail -20 "$PPP_LOG" | sed 's/^/         /' >&2
    exit 1
  fi

  sleep 0.5
done

say "超时：ppp0 没起来。日志尾部：" >&2
tail -30 "$PPP_LOG" 2>/dev/null | sed 's/^/         /' >&2
exit 1
