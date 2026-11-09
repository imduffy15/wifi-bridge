#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
End-to-end tests of the firmware in QEMU (emulated OpenCores Ethernet, no Wi-Fi).

    tools/qemu-build.sh && python3 tools/qemu_test.py

Boots build-qemu/flash.bin, then drives the device over HTTP (port-forwarded), captive DNS and the serial
console: setup portal, API validation, auth, console commands, OTA upload (good and bad image), backup /
restore, setup -> bridge mode -> setup mode, factory reset. Uses qemu-system-xtensa from PATH, or the
espressif/idf docker image if it isn't installed.
"""
import argparse, base64, gzip, json, os, shutil, socket, subprocess, sys, tempfile, threading, time
import urllib.request, urllib.error

HTTP_PORT, DNS_PORT, SERIAL_PORT = 18080, 15353, 15555
GUEST = "10.0.2.15"

class Device:
    def __init__(self, flash, workdir):
        self.flash = flash
        self.work = workdir
        self.log = bytearray()
        self.lock = threading.Lock()
        self.proc = None
        self.auth = None

    # ---- QEMU + serial ----
    def start(self):
        img = os.path.join(self.work, "flash.bin")
        shutil.copy(self.flash, img)
        args = ["-nographic", "-machine", "esp32", "-m", "4M",
                "-drive", "file=%s,if=mtd,format=raw" % ("/w/flash.bin" if not shutil.which("qemu-system-xtensa") else img),
                "-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:%d-:80,hostfwd=udp:127.0.0.1:%d-:53" % (HTTP_PORT, DNS_PORT),
                "-net", "nic,model=open_eth,netdev=n0",
                "-serial", "tcp:127.0.0.1:%d,server=on,wait=on" % SERIAL_PORT, "-monitor", "none"]
        if shutil.which("qemu-system-xtensa"):
            cmd = ["qemu-system-xtensa"] + args
        else:
            cmd = ["docker", "run", "--rm", "--name", "wb-qemu-test", "--network", "host", "-v", "%s:/w" % self.work,
                   "espressif/idf:v5.4", "qemu-system-xtensa"] + args
        self.proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(150):
            try:
                self.sock = socket.create_connection(("127.0.0.1", SERIAL_PORT))
                break
            except OSError:
                time.sleep(0.2)
        else:
            raise RuntimeError("QEMU serial port never opened")
        threading.Thread(target=self._rx, daemon=True).start()

    def _rx(self):
        while True:
            try:
                d = self.sock.recv(4096)
            except OSError:
                return
            if not d:
                return
            with self.lock:
                self.log += d

    def stop(self):
        if self.proc:
            if not shutil.which("qemu-system-xtensa"):
                subprocess.run(["docker", "rm", "-f", "wb-qemu-test"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self.proc.kill()

    def mark(self):
        with self.lock:
            return len(self.log)

    def wait_serial(self, text, since=0, timeout=90):
        t0 = time.time()
        while time.time() - t0 < timeout:
            with self.lock:
                if text.encode() in self.log[since:]:
                    return True
            time.sleep(0.2)
        raise AssertionError("serial: %r not seen within %d s" % (text, timeout))

    def serial(self, line):
        m = self.mark()
        self.sock.sendall(line.encode() + b"\r")
        return m

    # ---- HTTP ----
    def http(self, method, path, body=None, headers=None, raw=False, host=GUEST, timeout=20):
        h = {"Host": host}
        if self.auth:
            h["Authorization"] = "Basic " + base64.b64encode(self.auth.encode()).decode()
        if body is not None and not isinstance(body, (bytes, bytearray)):
            body = json.dumps(body).encode()
            h["Content-Type"] = "application/json"
        h.update(headers or {})
        req = urllib.request.Request("http://127.0.0.1:%d%s" % (HTTP_PORT, path), data=body, method=method, headers=h)
        opener = urllib.request.build_opener(NoRedirect)
        try:
            r = opener.open(req, timeout=timeout)
            status, data, hdrs = r.status, r.read(), r.headers
        except urllib.error.HTTPError as e:
            status, data, hdrs = e.code, e.read(), e.headers
        if raw:
            return status, data, hdrs
        try:
            return status, json.loads(data) if data else None
        except ValueError:
            return status, data

    def wait_http(self, timeout=90):
        t0 = time.time()
        while time.time() - t0 < timeout:
            try:
                s, j = self.http("GET", "/api/status", timeout=3)
                if s in (200, 401):
                    return j
            except Exception:
                pass
            time.sleep(1)
        raise AssertionError("device did not answer HTTP within %d s" % timeout)


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *a, **k):
        return None


def dns_query(name):
    q = b"\x12\x34\x01\x00\x00\x01\x00\x00\x00\x00\x00\x00"
    for part in name.split("."):
        q += bytes([len(part)]) + part.encode()
    q += b"\x00\x00\x01\x00\x01"
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(3)
    s.sendto(q, ("127.0.0.1", DNS_PORT))
    r = s.recv(512)
    return socket.inet_ntoa(r[-4:]) if r[7] == 1 else None


RESULTS = []

def check(name, cond, detail=""):
    RESULTS.append((name, bool(cond)))
    print(("  PASS  " if cond else "  FAIL  ") + name + ("" if cond else "  -> %s" % (detail,)), flush=True)
    return cond


def run(dev, app_bin):
    print("boot")
    dev.wait_serial("SETUP MODE", timeout=120)
    st = dev.wait_http()
    check("boots into setup mode", st["mode"] == "setup", st)
    with open(app_bin, "rb") as image:
        image.seek(176)
        expected_fingerprint = image.read(32).hex()
    check("running build fingerprint matches the image", st["device"].get("elf_sha256") == expected_fingerprint,
          st["device"].get("elf_sha256"))
    check("dual OTA layout, running app0", st["device"]["ota_mode"] == "dual" and st["device"]["partition"] == "app0", st["device"])
    check("Ethernet in setup role", st["ethernet"]["role"] == "setup" and st["ethernet"]["state"] == "up", st["ethernet"])

    print("captive portal")
    s, _, h = dev.http("GET", "/generate_204", raw=True, host="connectivitycheck.gstatic.com")
    check("foreign host is redirected to the portal", s == 302 and h["Location"] == "http://%s/" % GUEST, (s, dict(h)))
    s, _, h = dev.http("GET", "/hotspot-detect.html", raw=True)
    check("unknown path on our address is redirected", s == 302, s)
    check("captive DNS answers with our address", dns_query("captive.apple.com") == GUEST)
    s, body, h = dev.http("GET", "/", raw=True)
    html = gzip.decompress(body).decode() if h.get("Content-Encoding") == "gzip" else ""
    check("web UI served gzip-compressed", s == 200 and "<!DOCTYPE html>" in html[:200].upper().replace("<!DOCTYPE HTML>", "<!DOCTYPE html>"), s)

    print("API")
    s, boards = dev.http("GET", "/api/boards")
    check("board presets", s == 200 and any(b["id"] == "wt32-eth01" for b in boards), boards)
    s, cfg = dev.http("GET", "/api/config")
    check("config hides secrets", s == 200 and "password1" not in cfg["wifi"] and cfg["wifi"]["password1_set"] is False, cfg.get("wifi"))
    s, j = dev.http("POST", "/api/config", {"network": {"ip": "999.1.1.1"}})
    check("invalid IP rejected naming the field", s == 400 and "network.ip" in j["error"], (s, j))
    for ip, mask, gateway, field in [
        ("192.168.1.10", "255.0.255.0", "", "network.netmask"),
        ("192.168.1.10", "255.255.255.255", "", "network.netmask"),
        ("192.168.1.0", "255.255.255.0", "", "network.ip"),
        ("192.168.1.255", "255.255.255.0", "", "network.ip"),
        ("127.0.0.2", "255.0.0.0", "", "network.ip"),
        ("224.0.0.2", "255.255.255.0", "", "network.ip"),
        ("192.168.1.10", "255.255.255.0", "192.168.1.255", "network.gateway"),
    ]:
        s, j = dev.http("POST", "/api/config", {"network": {"ip": ip, "netmask": mask, "gateway": gateway}})
        check("unusable address rejected: %s/%s via %s" % (ip, mask, gateway), s == 400 and field in j["error"], (s, j))
    s, cfg_after = dev.http("GET", "/api/config")
    check("invalid network changes leave settings intact", cfg_after["network"] == cfg["network"], cfg_after["network"])
    s, j = dev.http("POST", "/api/setup", {"ssid": "Must not save", "network": {"ip": "0.0.0.0"}})
    check("setup rejects zero management address before saving", s == 400, (s, j))
    s, j = dev.http("POST", "/api/setup", {"ssid": "Must not save", "password": False, "network": {"ip": "192.168.1.250"}})
    check("setup refuses malformed password", s == 400, (s, j))
    s, cfg_after = dev.http("GET", "/api/config")
    check("invalid setup leaves all settings intact", cfg_after == cfg, cfg_after.get("wifi"))
    s, j = dev.http("POST", "/api/config", {"wifi": "not an object"})
    check("malformed settings section rejected", s == 400 and "wifi" in j["error"], (s, j))
    s, j = dev.http("POST", "/api/config", {"ethernet": {"board": "not-a-board"}})
    check("unknown board preset rejected", s == 400 and "ethernet.board" in j["error"], (s, j))
    for invalid_backup in [{}, {"_firmware": "another-product"}, {"_firmware": "wifi-bridge", "wifi": []}]:
        s, j = dev.http("POST", "/api/config/restore", invalid_backup)
        check("non-backup JSON refused without restarting", s == 400 and "backup" in j["error"], (s, j))
    for mac in ["00:00:00:00:00:00", "ff:ff:ff:ff:ff:ff", "01:00:5e:00:00:01", " a:bb:cc:dd:ee:ff"]:
        s, j = dev.http("POST", "/api/config", {"bridge": {"client_mac": mac}})
        check("invalid fixed client MAC rejected: " + mac, s == 400, (s, j))
    s, j = dev.http("POST", "/api/config", {"mqtt": {"enabled": True}})
    check("cross-field validation (MQTT host)", s == 400 and "mqtt.host" in j["error"], (s, j))
    s, j = dev.http("POST", "/api/config", {"device": {"name": "Test Bridge"}, "mqtt": {"tele_period": 60}})
    check("valid change saved", s == 200 and j["ok"], (s, j))
    s, cfg = dev.http("GET", "/api/config")
    check("change visible in config", cfg["device"]["name"] == "Test Bridge" and cfg["mqtt"]["tele_period"] == 60, cfg["device"])
    s, j = dev.http("POST", "/api/config", {"device": {"name": "x"}}, headers={"Origin": "http://evil.example"})
    check("cross-origin POST refused", s == 403, (s, j))
    s, j = dev.http("GET", "/api/status", host="attacker.example")
    check("API refuses foreign Host (DNS rebinding)", s == 403, s)
    s, j = dev.http("GET", "/cm?cmnd=Status", headers={"Sec-Fetch-Site": "cross-site"})
    check("/cm refuses cross-site requests", s == 403, s)
    s, j = dev.http("GET", "/cm?cmnd=Status", headers={"Referer": "http://evil.example/x"})
    check("/cm refuses foreign Referer", s == 403, s)
    s, j = dev.http("POST", "/api/config", {"system": {"led_pin": 20}})
    check("nonexistent GPIO rejected", s == 400 and "led_pin" in j["error"], (s, j))
    s, j = dev.http("POST", "/api/config", {"system": {"led_pin": 6}})
    check("flash GPIO rejected", s == 400, (s, j))
    s, j = dev.http("POST", "/api/config", {"system": {"button_pin": 23}})
    check("pin clash with MDC rejected", s == 400 and "twice" in j["error"], (s, j))
    s, j = dev.http("POST", "/api/config", {"wifi": {"ssid1": "Test", "password1": "secretpw1"}})
    s, bk = dev.http("GET", "/api/config/backup")
    check("backup without admin password omits secrets", s == 200 and "password1" not in bk["wifi"], bk and bk.get("wifi"))
    s, j = dev.http("POST", "/api/cmd", {"cmd": "DeviceName"})
    check("web console command", s == 200 and j["result"] == {"DeviceName": "Test Bridge"}, j)
    s, j = dev.http("GET", "/cm?cmnd=Backlog%20TelePeriod%2090%3B%20TelePeriod")
    check("/cm without a password only reads (no CSRF through GET)", s == 403, (s, j))
    s, j = dev.http("GET", "/cm?cmnd=TelePeriod")
    check("/cm reads a setting without a password", s == 200 and "TelePeriod" in j, (s, j))
    s, j = dev.http("POST", "/api/cmd", {"cmd": "Backlog TelePeriod 90; TelePeriod"})
    check("Backlog through POST /api/cmd", s == 200 and j.get("result") == {"TelePeriod": 90}, j)
    s, j = dev.http("GET", "/api/log?since=0")
    check("log ring has boot messages", s == 200 and any("web server started" in l["msg"] for l in j["lines"]), j and len(j.get("lines", [])))
    nxt = j["next"]
    s, j = dev.http("GET", "/api/log?since=%d" % nxt)
    check("log since= returns only new lines", s == 200 and all(l["seq"] > nxt for l in j["lines"]), j)
    check("no crash report on a clean boot", st["device"].get("last_crash", "missing") is None, st["device"].get("last_crash", "missing"))
    s, j = dev.http("GET", "/api/coredump")
    check("coredump endpoint 404 without a report", s == 404, s)
    s, j = dev.http("GET", "/api/scan")
    check("scan endpoint answers", s == 200 and "networks" in j, j)

    print("serial console")
    m = dev.serial("Hostname qemu-bridge")
    dev.wait_serial('{"Hostname":"qemu-bridge"}', since=m, timeout=10)
    check("serial command sets hostname", True)
    m = dev.serial("Hostname -bad-")
    dev.wait_serial("error", since=m, timeout=10)
    check("serial command validates", True)
    s, st = dev.http("GET", "/api/status")
    check("reboot_required flagged", st["device"]["reboot_required"] is True, st["device"])

    print("firmware update")
    s, j = dev.http("POST", "/api/ota/upload", b"\0" * 8192, headers={"Content-Type": "application/octet-stream"})
    check("garbage image rejected", s == 400 and "not an ESP32" in j["error"], (s, j))
    fw = open(app_bin, "rb").read()
    t0 = time.time()
    s, j = dev.http("POST", "/api/ota/upload", fw, headers={"Content-Type": "application/octet-stream"}, timeout=300)
    check("firmware upload accepted (%d KB in %.0f s)" % (len(fw) // 1024, time.time() - t0), s == 200 and j["ok"], (s, j))
    time.sleep(4)
    st = dev.wait_http(120)
    check("rebooted into app1", st["device"]["partition"] == "app1", st["device"])
    check("settings survived the update", st["device"]["name"] == "Test Bridge" and st["device"]["hostname"] == "qemu-bridge", st["device"])
    m = dev.mark()
    time.sleep(1)
    s, j = dev.http("GET", "/api/log?since=0")
    check("new image confirmed (no rollback)", any("new firmware confirmed" in l["msg"] for l in j["lines"]), [l["msg"] for l in j["lines"]][-5:])

    print("authentication")
    s, j = dev.http("POST", "/api/config", {"admin": {"password": "s3cret"}})
    check("admin password set", s == 200, (s, j))
    s, j = dev.http("GET", "/api/status")
    check("API requires auth", s == 401, s)
    s, _, _ = dev.http("GET", "/", raw=True)
    check("UI requires auth", s == 401, s)
    dev.auth = "admin:wrong"
    s, j = dev.http("GET", "/api/status")
    check("wrong password refused", s == 401, s)
    dev.auth = "admin:s3cret"
    s, j = dev.http("GET", "/api/status")
    check("right password accepted", s == 200 and j["device"]["auth"] is True, s)
    dev.auth = None
    s, j = dev.http("GET", "/cm?user=admin&password=s3cret&cmnd=DeviceName")
    check("/cm password parameter", s == 200 and j == {"DeviceName": "Test Bridge"}, (s, j))
    s, j = dev.http("GET", "/cm?user=admin&password=s3cret&cmnd=TelePeriod%2060")
    check("/cm changes settings with the password in the URL", s == 200 and j == {"TelePeriod": 60}, (s, j))
    saved, dev.auth = dev.auth, "admin:s3cret"
    s, j = dev.http("GET", "/cm?cmnd=TelePeriod%2070")
    check("/cm refuses changes with only Basic auth (browser-cached credentials)", s == 403, (s, j))
    dev.auth = saved
    dev.auth = "admin:s3cret"

    print("backup / restore")
    s, bk = dev.http("GET", "/api/config/backup")
    check("backup includes secrets", s == 200 and bk["admin"]["password"] == "s3cret", bk and bk.get("admin"))
    bk["device"]["name"] = "Restored Bridge"
    s, j = dev.http("POST", "/api/config/restore", bk)
    check("restore accepted", s == 200, (s, j))
    time.sleep(4)
    st = dev.wait_http()
    check("restored settings active after restart", st["device"]["name"] == "Restored Bridge", st["device"])

    print("setup -> bridge -> setup")
    s, j = dev.http("POST", "/api/setup", {"ssid": "HomeNet", "password": "password123", "hostname": "qemu-bridge",
                                           "network": {"ip": "192.168.1.250", "netmask": "255.255.255.0", "gateway": "192.168.1.1", "dns": "192.168.1.1"}})
    check("setup accepted", s == 200 and j["url"] == "http://192.168.1.250/" and j["mdns"] == "http://qemu-bridge.local/", (s, j))
    m = dev.mark()
    dev.wait_serial("BRIDGE MODE", since=m, timeout=90)
    check("restarts into bridge mode", True)
    m = dev.serial("Restart 2")
    dev.wait_serial("SETUP MODE", since=m, timeout=90)
    st = dev.wait_http()
    s, cfg = dev.http("GET", "/api/config")
    check("Restart 2 enters setup mode, keeping settings", st["mode"] == "setup" and cfg["wifi"]["ssid1"] == "HomeNet" and cfg["wifi"]["password1_set"], cfg["wifi"])
    m = dev.serial("Restart 1")
    dev.wait_serial("BRIDGE MODE", since=m, timeout=90)
    check("next restart goes back to bridge mode", True)

    print("factory reset")
    m = dev.serial("Reset 1")
    dev.wait_serial("SETUP MODE", since=m, timeout=90)
    dev.auth = None
    st = dev.wait_http()
    check("factory reset clears everything", st and st["mode"] == "setup" and st["device"]["auth"] is False and st["device"]["name"] == "WiFi Bridge", st and st["device"])

    with dev.lock:
        crashed = b"Guru Meditation" in dev.log or b"abort()" in dev.log
    check("no crashes on the serial log", not crashed)
    check("no safe mode", st is None or not st["device"].get("safe_mode"), st and st["device"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--flash", default="build-qemu/flash.bin")
    ap.add_argument("--app", default="build-qemu/wifi-bridge.bin")
    ap.add_argument("--serial-log", default="build-qemu/qemu-serial.log")
    a = ap.parse_args()
    work = tempfile.mkdtemp(prefix="wbq")
    dev = Device(a.flash, work)
    try:
        dev.start()
        run(dev, a.app)
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
