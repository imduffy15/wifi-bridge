#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Improv Wi-Fi serial protocol in QEMU (tools/qemu-build.sh first). QEMU has no Wi-Fi, so a settings command
must end in "unable to connect"; everything else is checked against the protocol.
"""
import argparse, os, shutil, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_test import Device, check, RESULTS  # noqa: E402


def packet(ptype, data):
    p = b"IMPROV" + bytes([1, ptype, len(data)]) + bytes(data)
    return p + bytes([sum(p) & 0xFF])


def rpc(cmd, data=b""):
    return packet(3, bytes([cmd, len(data)]) + data)


def packets(buf):
    """All well-formed Improv packets in a byte stream (logs in between are skipped)."""
    out, i = [], 0
    while True:
        i = buf.find(b"IMPROV", i)
        if i < 0 or i + 9 > len(buf):
            return out
        n = buf[i + 8]
        end = i + 9 + n
        if end >= len(buf):
            return out
        if sum(buf[i:end]) & 0xFF == buf[end]:
            out.append((buf[i + 7], bytes(buf[i + 9:end])))
        i = end + 1


def strings(data):
    """RPC result payload: command, length, then length-prefixed strings."""
    s, i = [], 2
    while i < len(data):
        s.append(data[i + 1:i + 1 + data[i]].decode())
        i += 1 + data[i]
    return data[0], s


def exchange(dev, raw, want, timeout=20):
    m = dev.mark()
    dev.sock.sendall(raw)
    t0 = time.time()
    while time.time() - t0 < timeout:
        with dev.lock:
            got = packets(bytes(dev.log[m:]))
        if want(got):
            return got
        time.sleep(0.2)
    with dev.lock:
        return packets(bytes(dev.log[m:]))


def run(dev):
    dev.wait_serial("SETUP MODE", timeout=120)
    dev.wait_http()

    got = exchange(dev, rpc(2), lambda g: any(t == 1 for t, _ in g))
    check("current state: ready (not provisioned)", (1, b"\x02") in got, got)

    got = exchange(dev, rpc(3), lambda g: any(t == 4 for t, _ in g))
    res = [strings(d) for t, d in got if t == 4]
    check("device info names the firmware", bool(res) and res[0][0] == 3 and res[0][1][0] == "WiFi Bridge" and
          res[0][1][2] == "ESP32", res)

    bad = bytearray(rpc(2))
    bad[-1] ^= 0xFF
    got = exchange(dev, bytes(bad), lambda g: any(t == 2 for t, _ in g))
    check("bad checksum gives 'invalid packet'", (2, b"\x01") in got, got)

    got = exchange(dev, rpc(9), lambda g: any(t == 2 for t, _ in g))
    check("unknown command gives 'unknown RPC'", (2, b"\x02") in got, got)

    ssid, pw = b"HomeNet", b"secret-pass"
    got = exchange(dev, rpc(1, bytes([len(ssid)]) + ssid + bytes([len(pw)]) + pw),
                   lambda g: any(t == 2 for t, _ in g), timeout=90)
    check("Wi-Fi settings: provisioning, then 'unable to connect' (no Wi-Fi in QEMU)",
          (1, b"\x03") in got and (2, b"\x03") in got, got)

    m = dev.serial("Status")
    dev.wait_serial('"mode"', since=m, timeout=10)
    check("the text console still works after Improv traffic", True)
    s, st = dev.http("GET", "/api/status")
    check("still in setup mode, nothing saved", s == 200 and st["mode"] == "setup", st.get("mode"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--flash", default="build-qemu/flash.bin")
    ap.add_argument("--serial-log", default="build-qemu/qemu-improv-serial.log")
    a = ap.parse_args()
    work = tempfile.mkdtemp(prefix="wbqi")
    dev = Device(a.flash, work)
    try:
        dev.start()
        run(dev)
    except Exception as e:                      # noqa: BLE001 - report and keep the serial log
        check("unexpected: %s: %s" % (type(e).__name__, e), False)
    finally:
        dev.stop()
        with open(a.serial_log, "wb") as f:
            f.write(dev.log)
        shutil.rmtree(work, ignore_errors=True)
    failed = [n for n, ok in RESULTS if not ok]
    print("\n%d passed, %d failed (serial log: %s)" % (len(RESULTS) - len(failed), len(failed), a.serial_log))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
