#!/bin/bash
# ##############################################################################
# ppp_down.sh —— 拆掉 ppp_up.sh 建起来的链路
#
# 用法：同 ppp_up.sh，需要 root：
#   wsl.exe -d Ubuntu-24.04 -u root -e bash -lc 'bash .../ppp_down.sh'
# ##############################################################################

set -e

PTY_LINK=/tmp/ttyPPP
PPP_IF=ppp0
WAN="$(ip route show default | awk '{print $5; exit}')"

say() { echo "[ppp_down] $*"; }

pkill -f "pppd $PTY_LINK" 2>/dev/null && say "pppd 已停" || true
pkill -f "socat .*$PTY_LINK" 2>/dev/null && say "socat 已停" || true
sleep 1
rm -f "$PTY_LINK"

del_rule() {
  local table="$1" chain="$2"; shift 2
  if [ "$table" = filter ]; then
    iptables -C "$chain" "$@" 2>/dev/null && iptables -D "$chain" "$@"
  else
    iptables -t "$table" -C "$chain" "$@" 2>/dev/null && \
      iptables -t "$table" -D "$chain" "$@"
  fi
  return 0
}

if [ -n "$WAN" ]; then
  del_rule nat    POSTROUTING -o "$WAN" -s 10.0.0.2/32 -j MASQUERADE
  del_rule filter FORWARD     -i "$PPP_IF" -o "$WAN" -j ACCEPT
  del_rule filter FORWARD     -i "$WAN" -o "$PPP_IF" \
           -m state --state RELATED,ESTABLISHED -j ACCEPT
  del_rule mangle FORWARD     -o "$PPP_IF" -p tcp --tcp-flags SYN,RST SYN \
           -j TCPMSS --set-mss "$(( $(cat /sys/class/net/$WAN/mtu) - 40 ))"
  del_rule mangle FORWARD     -i "$PPP_IF" -p tcp --tcp-flags SYN,RST SYN \
           -j TCPMSS --set-mss "$(( $(cat /sys/class/net/$WAN/mtu) - 40 ))"
fi

say "规则已清，链路已拆"
