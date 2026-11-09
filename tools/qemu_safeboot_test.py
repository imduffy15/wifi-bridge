#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
End-to-end test of the safeboot layout in QEMU: safeboot firmware in the factory slot, WiFi Bridge in the
single app slot.

    tools/qemu-build.sh && python3 tools/qemu_safeboot_test.py

Covers: safeboot boots and serves its page, refuses images it can't install, installs the firmware by
upload and from a URL, hands over to the firmware; the firmware reports the layout, refuses to overwrite
itself, installs a new safeboot firmware, and restarts into safeboot and back.
"""
import argparse, functools, gzip, http.server, os, shutil, sys, tempfile, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_test import Device, check, RESULTS, GUEST  # noqa: E402

FILE_PORT = 18765          # QEMU user networking: the guest reaches the host's loopback at 10.0.2.2


def mode(dev, timeout=120):
    """Wait for the device to answer and return its mode ("safeboot", "setup", "bridge")."""
    return dev.wait_http(timeout=timeout).get("mode")


def wait_mode(dev, want, timeout=150):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            s, j = dev.http("GET", "/api/status", timeout=3)
            if s == 200 and j.get("mode") == want:
                return j
        except Exception:
            pass
        time.sleep(1)
    raise AssertionError("device not in %s mode within %d s" % (want, timeout))


def run(dev, app, safeboot, workdir):
    print("safeboot firmware")
    st = wait_mode(dev, "safeboot")
    d = st["device"]
    check("boots the safeboot firmware from the factory slot", st["mode"] == "safeboot", st)
    check("safeboot sees the installed firmware", bool(d.get("firmware_version")), d)
    s, body, h = dev.http("GET", "/", raw=True)
    html = gzip.decompress(body).decode() if h.get("Content-Encoding") == "gzip" else ""
    check("safeboot page served", s == 200 and "Safeboot" in html, s)
    s, _, h = dev.http("GET", "/generate_204", raw=True)
    check("unknown paths redirect to the page", s == 302, s)

    s, j = dev.http("POST", "/api/ota/upload", b"\x00" * 4096, {"Content-Type": "application/octet-stream"})
    check("safeboot rejects a non-image", s == 400 and "magic" in j.get("error", ""), (s, j))
    s, j = dev.http("POST", "/api/ota/upload", open(safeboot, "rb").read(), {"Content-Type": "application/octet-stream"})
    check("safeboot refuses to replace itself", s == 409 and "itself" in j.get("error", ""), (s, j))

    s, j = dev.http("POST", "/api/ota/upload", open(app, "rb").read(), {"Content-Type": "application/octet-stream"},
                    timeout=180)
    check("safeboot installs the firmware by upload", s == 200 and j.get("installed") == "firmware" and j.get("restart"), (s, j))

    print("firmware on the safeboot layout")
    st = dev.wait_http(timeout=150)
    t0 = time.time()
    while st.get("mode") == "safeboot" and time.time() - t0 < 120:
        time.sleep(2)
        st = dev.wait_http()
    d = st["device"]
    check("firmware boots after the safeboot install", st["mode"] in ("setup", "bridge"), st.get("mode"))
    check("firmware reports the safeboot layout", d["ota_mode"] == "safeboot" and d["partition"] == "app0", d)
    check("firmware recognises our safeboot firmware", (d.get("safeboot") or {}).get("ours") is True, d.get("safeboot"))

    s, j = dev.http("POST", "/api/ota/upload", open(app, "rb").read(), {"Content-Type": "application/octet-stream"},
                    timeout=180)
    check("firmware refuses to overwrite its own slot (409)", s == 409 and "safeboot" in j.get("error", ""), (s, j))
    s, j = dev.http("POST", "/api/ota/upload", open(safeboot, "rb").read(), {"Content-Type": "application/octet-stream"},
                    timeout=180)
    check("firmware installs the safeboot firmware, no restart", s == 200 and j.get("installed") == "safeboot" and
          j.get("restart") is False, (s, j))
    st = dev.wait_http()
    check("still running the firmware afterwards", st["mode"] in ("setup", "bridge"), st.get("mode"))

    s, j = dev.http("POST", "/api/reboot", {"target": "safeboot"})
    check("restart into safeboot accepted", s == 200, (s, j))
    st = wait_mode(dev, "safeboot")
    check("the newly installed safeboot firmware boots", st["mode"] == "safeboot", st.get("mode"))

    print("install from a URL")
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", FILE_PORT),
                                          functools.partial(http.server.SimpleHTTPRequestHandler, directory=workdir))
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    shutil.copy(app, os.path.join(workdir, "fw.bin"))
    try:
        s, j = dev.http("POST", "/api/ota/url", {"url": "http://10.0.2.2:%d/fw.bin" % FILE_PORT})
        check("safeboot accepts a URL", s == 200, (s, j))
        t0, r = time.time(), {}
        while time.time() - t0 < 180:
            try:
                _, r = dev.http("GET", "/api/ota", timeout=3)
            except Exception:
                break                       # restarting
            if r.get("state") in ("done", "error"):
                break
            time.sleep(1)
        check("URL download installed the firmware", r.get("state") in ("done", None) and not r.get("error"), r)
        st = dev.wait_http(timeout=150)
        t0 = time.time()
        while st.get("mode") == "safeboot" and time.time() - t0 < 120:
            time.sleep(2)
            st = dev.wait_http()
        check("firmware boots after the URL install", st["mode"] in ("setup", "bridge"), st.get("mode"))
    finally:
        srv.shutdown()

    print("leave safeboot without installing")
    dev.http("POST", "/api/reboot", {"target": "safeboot"})
    wait_mode(dev, "safeboot")
    s, j = dev.http("POST", "/api/reboot", {"target": "normal"})
    check("safeboot restarts into the firmware on request", s == 200, (s, j))
    st = dev.wait_http(timeout=150)
    t0 = time.time()
    while st.get("mode") == "safeboot" and time.time() - t0 < 60:
        time.sleep(2)
        st = dev.wait_http()
    check("back in the firmware", st["mode"] in ("setup", "bridge"), st.get("mode"))

    print("command-line updater through WiFi Bridge safeboot")
    import subprocess
    result = subprocess.run(["bash", "tools/ota.sh", "127.0.0.1:18080", app],
                            capture_output=True, text=True, timeout=240)
    check("updater installs through native safeboot and verifies the build",
          result.returncode == 0 and "verified:" in result.stdout,
          result.stdout + result.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--flash", default="build-qemu/flash-safeboot.bin")
    ap.add_argument("--app", default="build-qemu/wifi-bridge.bin")
    ap.add_argument("--safeboot", default="build-qemu-safeboot/wifi-bridge-safeboot.bin")
    ap.add_argument("--serial-log", default="build-qemu/qemu-safeboot-serial.log")
    a = ap.parse_args()
    work = tempfile.mkdtemp(prefix="wbqs")
    dev = Device(a.flash, work)
    try:
        dev.start()
        run(dev, a.app, a.safeboot, work)
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
