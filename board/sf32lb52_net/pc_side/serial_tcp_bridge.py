#!/usr/bin/env python3
"""Serial <-> TCP bridge, for putting a board's PPP link onto a PC.

The board's second UART carries PPP.  It reaches the PC through a USB-TTL
adapter, which appears as a COM port on Windows.  pppd, however, runs inside
WSL, where it can see neither the COM port nor any way to open it.

WSL does have a TCP path back to the Windows host, so this script turns the
COM port into a TCP endpoint that socat on the WSL side can hand to pppd as a
PTY.  Nothing here needs administrator rights or a kernel driver.

    [board UART2] --3 wires--> [USB-TTL] --USB--> [COMx]
                                                     |
                                          serial_tcp_bridge.py  (this file)
                                                     |  TCP
                                        socat  ->  /tmp/ttyPPP  ->  pppd

Usage:
    python serial_tcp_bridge.py COM7 460800
    python serial_tcp_bridge.py COM7 460800 --tcp-port 5555
    python serial_tcp_bridge.py --list

Defaults to listening on 0.0.0.0:5555 and serving one client at a time; a
client that disconnects is replaced when the next one connects.
"""

import argparse
import socket
import sys
import threading

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.stderr.write("pyserial is required:  pip install pyserial\n")
    raise SystemExit(2)

# Small enough that the relay is responsive, large enough that the loop is
# not a spin when the link is idle.
SERIAL_READ_TIMEOUT = 0.02
TCP_BACKLOG = 1


def list_serial_ports():
    ports = list(list_ports.comports())
    if not ports:
        print("no serial ports found")
        return

    width = max(len(p.device) for p in ports)
    for p in ports:
        print(f"{p.device:<{width}}  {p.description}")


def open_serial(device, baud):
    """Open the tty without touching the hardware handshake lines.

    DTR and RTS are left alone deliberately: on some boards they are wired to
    reset or boot-mode pins, and pyserial asserts them on open by default.
    """

    port = serial.Serial()
    port.port = device
    port.baudrate = baud
    port.bytesize = serial.EIGHTBITS
    port.parity = serial.PARITY_NONE
    port.stopbits = serial.STOPBITS_ONE
    port.timeout = SERIAL_READ_TIMEOUT
    port.write_timeout = 2
    port.rtscts = False
    port.dsrdtr = False
    port.xonxoff = False
    port.dtr = False
    port.rts = False
    port.open()
    return port


def pump_to_tcp(port, conn, stop):
    while not stop.is_set():
        try:
            data = port.read(4096)
        except serial.SerialException as exc:
            print(f"serial read failed: {exc}")
            break

        if not data:
            continue

        try:
            conn.sendall(data)
        except OSError as exc:
            print(f"tcp send failed: {exc}")
            break

    stop.set()


def pump_to_serial(port, conn, stop):
    conn.settimeout(SERIAL_READ_TIMEOUT)

    while not stop.is_set():
        try:
            data = conn.recv(4096)
        except socket.timeout:
            continue
        except OSError as exc:
            print(f"tcp recv failed: {exc}")
            break

        if not data:
            print("client disconnected")
            break

        try:
            port.write(data)
            port.flush()
        except serial.SerialException as exc:
            print(f"serial write failed: {exc}")
            break

    stop.set()


def serve_client(port, conn, peer):
    print(f"client connected from {peer[0]}:{peer[1]}")
    stop = threading.Event()

    reader = threading.Thread(
        target=pump_to_tcp, args=(port, conn, stop), daemon=True)
    writer = threading.Thread(
        target=pump_to_serial, args=(port, conn, stop), daemon=True)

    reader.start()
    writer.start()
    writer.join()

    stop.set()
    conn.close()
    reader.join(timeout=1)

    # A half-open link is worse than a closed one: refuse to keep the pty
    # alive with a stack of buffered bytes, and start the next client clean.
    try:
        port.reset_input_buffer()
        port.reset_output_buffer()
    except serial.SerialException:
        pass

    print("client gone")


def main():
    parser = argparse.ArgumentParser(
        description="Bridge a serial port to TCP for a WSL-side pppd.")
    parser.add_argument("device", nargs="?",
                        help="serial port, e.g. COM7")
    parser.add_argument("baud", nargs="?", type=int, default=460800,
                        help="line rate; must match the board's (default 460800)")
    parser.add_argument("--tcp-port", type=int, default=5555,
                        help="TCP port to listen on (default 5555)")
    parser.add_argument("--tcp-bind", default="0.0.0.0",
                        help="address to listen on (default 0.0.0.0)")
    parser.add_argument("--list", action="store_true",
                        help="list serial ports and exit")
    args = parser.parse_args()

    if args.list or not args.device:
        list_serial_ports()
        return 0 if args.list else 1

    port = open_serial(args.device, args.baud)
    print(f"{args.device} open at {args.baud} baud")

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((args.tcp_bind, args.tcp_port))
    server.listen(TCP_BACKLOG)
    print(f"listening on {args.tcp_bind}:{args.tcp_port}")

    try:
        while True:
            conn, peer = server.accept()
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            try:
                serve_client(port, conn, peer)
            except KeyboardInterrupt:
                raise
            except Exception as exc:  # keep serving after a client error
                print(f"client error: {exc}")
    except KeyboardInterrupt:
        print("interrupted")
    finally:
        server.close()
        port.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
