#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Stateful mock of the WiFi Bridge HTTP API (docs/API.md) for web UI development.

    python3 tools/mock_server.py --mode setup --port 8080
    python3 tools/mock_server.py --mode bridge --password secret --ota-mode safeboot

Serves main/web/index.html at / (re-read on every request, so just reload the browser).
Test hooks: Wi-Fi test password "wrong" fails, SSID "nonexistent" is not found; OTA uploads must
start with the ESP image magic byte 0xE9; an OTA URL containing "fail" errors out.
"""

import argparse
import base64
import copy
import json
import random
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

ROOT = Path(__file__).resolve().parent.parent
INDEX = ROOT / "main" / "web" / "index.html"
HOSTNAME = "wifi-bridge-a1b2c3"
REBOOT_DOWNTIME = 4.0
CAPTIVE_PROBES = {"/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html",
                  "/ncsi.txt", "/connecttest.txt", "/canonical.html", "/success.txt"}

BOARDS = [
    {"id": "wt32-eth01", "name": "WT32-ETH01", "phy": "lan8720", "addr": 1, "mdc": 23, "mdio": 18,
     "clk_mode": "ext_gpio0", "power_pin": 16},
    {"id": "olimex-esp32-poe", "name": "Olimex ESP32-POE", "phy": "lan8720", "addr": 0, "mdc": 23,
     "mdio": 18, "clk_mode": "out_gpio17", "power_pin": 12},
    {"id": "olimex-esp32-gateway", "name": "Olimex ESP32-Gateway", "phy": "lan8720", "addr": 0,
     "mdc": 23, "mdio": 18, "clk_mode": "out_gpio17", "power_pin": 5},
    {"id": "lilygo-t-eth-poe", "name": "LilyGO T-Internet-POE", "phy": "lan8720", "addr": 0,
     "mdc": 23, "mdio": 18, "clk_mode": "out_gpio17", "power_pin": -1},
    {"id": "wesp32", "name": "wESP32", "phy": "lan8720", "addr": 0, "mdc": 16, "mdio": 17,
     "clk_mode": "ext_gpio0", "power_pin": -1},
]

FAKE_NETWORKS = [
    ("HomeNet", -48, 6, "WPA2"), ("HomeNet-5G-guest", -61, 11, "WPA2/WPA3"), ("Neighbour", -72, 1, "WPA2"),
    ("FreeCoffee", -80, 6, "OPEN"), ("IoT <test> & \"quotes\"", -67, 3, "WPA3"),
    ("VeryLongNetworkNameThatGoesOnAndOn32", -86, 13, "WPA2"), ("Printer-Direct", -90, 1, "WPA2"),
    ("HomeNet", -77, 6, "WPA2"),
]

DEFAULT_CONFIG = {
    "device": {"name": "WiFi Bridge", "hostname": HOSTNAME},
    "wifi": {"ssid1": "", "ssid2": "", "ap_fallback_min": 3, "power_save": "none", "tx_power": 20},
    "network": {"ip": "", "netmask": "255.255.255.0", "gateway": "", "dns": ""},
    "bridge": {"client_mac": ""},
    "ethernet": dict({k: v for k, v in BOARDS[0].items() if k not in ("id", "name")}, board="wt32-eth01"),
    "admin": {},
    "mqtt": {"enabled": False, "host": "", "port": 1883, "user": "", "topic": HOSTNAME, "discovery": True,
             "discovery_prefix": "homeassistant", "tele_period": 300},
    "time": {"ntp": "pool.ntp.org", "tz": "UTC0"},
    "syslog": {"host": "", "port": 514},
    "system": {"log_level": "info", "cpu_mhz": 240, "led_pin": -1, "led_inverted": False, "button_pin": -1,
               "powercycle_reset": True},
}
SECRETS = {("wifi", "password1"), ("wifi", "password2"), ("wifi", "ap_password"), ("admin", "password"),
           ("mqtt", "password")}
IP_RE = re.compile(r"^((25[0-5]|2[0-4]\d|1?\d?\d)\.){3}(25[0-5]|2[0-4]\d|1?\d?\d)$")
MAC_RE = re.compile(r"^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$")
HOST_RE = re.compile(r"^[A-Za-z0-9]([A-Za-z0-9-]{0,30}[A-Za-z0-9])?$")


class BadRequest(Exception):
    def __init__(self, msg, code=400):
        super().__init__(msg)
        self.code = code


def validate(sec, key, val):
    """Raise BadRequest if a config value is invalid. Mirrors plausible firmware checks."""
    name = f"{sec}.{key}"
    nums = {("wifi", "tx_power"): (2, 20), ("wifi", "ap_fallback_min"): (0, 1440), ("ethernet", "addr"): (0, 31),
            ("ethernet", "mdc"): (0, 39), ("ethernet", "mdio"): (0, 39), ("ethernet", "power_pin"): (-1, 39),
            ("mqtt", "port"): (1, 65535), ("mqtt", "tele_period"): (0, 3600), ("syslog", "port"): (1, 65535),
            ("system", "led_pin"): (-1, 39), ("system", "button_pin"): (-1, 39)}
    enums = {("wifi", "power_save"): ("none", "min", "max"), ("system", "log_level"): ("error", "warn", "info", "debug"),
             ("system", "cpu_mhz"): (80, 160, 240),
             ("ethernet", "clk_mode"): ("ext_gpio0", "out_gpio0", "out_gpio16", "out_gpio17"),
             ("ethernet", "board"): tuple(b["id"] for b in BOARDS) + ("custom",)}
    if (sec, key) in nums:
        lo, hi = nums[(sec, key)]
        if not isinstance(val, int) or isinstance(val, bool) or not lo <= val <= hi:
            raise BadRequest(f"{name} must be an integer {lo}..{hi}")
    elif (sec, key) in enums:
        if val not in enums[(sec, key)]:
            raise BadRequest(f"{name} must be one of {', '.join(map(str, enums[(sec, key)]))}")
    elif sec == "network":
        if not isinstance(val, str) or (val and not IP_RE.match(val)) or (key in ("ip", "netmask") and not val):
            raise BadRequest(f"{name} must be an IPv4 address")
    elif (sec, key) == ("bridge", "client_mac"):
        if not isinstance(val, str) or (val and not MAC_RE.match(val)):
            raise BadRequest(f"{name} must be empty or a MAC address like 11:22:33:44:55:66")
    elif (sec, key) == ("device", "hostname"):
        if not isinstance(val, str) or not HOST_RE.match(val):
            raise BadRequest(f"{name} may only contain letters, digits and '-' (max 32)")
    elif (sec, key) == ("wifi", "ap_password"):
        if not isinstance(val, str) or (val and not 8 <= len(val) <= 63):
            raise BadRequest(f"{name} must be empty or 8..63 characters")
    elif isinstance(DEFAULT_CONFIG.get(sec, {}).get(key), bool):
        if not isinstance(val, bool):
            raise BadRequest(f"{name} must be true or false")
    elif not isinstance(val, str):
        raise BadRequest(f"{name} must be a string")


class Device:
    def __init__(self, mode, password, ota_mode):
        self.lock = threading.RLock()
        self.mode = mode
        self.ota_mode = ota_mode
        self.config = copy.deepcopy(DEFAULT_CONFIG)
        self.secrets = {"admin.password": password} if password else {}
        if mode == "bridge":
            self.config["wifi"]["ssid1"] = "HomeNet"
            self.secrets["wifi.password1"] = "secret123"
            self.config["network"].update(ip="192.168.1.250", gateway="192.168.1.1", dns="192.168.1.1")
            self.config["bridge"]["client_mac"] = ""
        self.boot_count = 12
        self.reboot_required = False
        self.last_crash = None
        self.down_until = 0.0
        self.pending_reboot = None  # (at_time, next_mode)
        self.scan_until = 0.0  # running scan finishes at this time
        self.scan_at = 0.0  # time of the last completed scan
        self.scan_result = []
        self.pending_scan = []
        self.test = {"state": "idle", "error": ""}
        self.ota = {"state": "idle", "progress": 0, "error": "", "bytes": 0, "total": 0}
        self.log = []
        self.seq = 0
        self.log_t = 0.0
        self.boot()

    # --- lifecycle -------------------------------------------------------------------------------
    def boot(self):
        self.started = time.time()
        self.frames = dict(wired_to_wifi=0, wifi_to_wired=0, to_bridge=0, wifi_tx_fail=0, wifi_drop=0, wired_drop=0)
        self.reboot_required = False
        self.ota = {"state": "idle", "progress": 0, "error": "", "bytes": 0, "total": 0}
        self.test = {"state": "idle", "error": ""}
        self.log, self.seq, self.log_t = [], 0, time.time()
        self.add_log("I", f"boot: WiFi Bridge 0.1.0 starting, mode={self.mode}, boot #{self.boot_count}")
        self.add_log("I", f"eth: {self.config['ethernet']['board']} PHY {self.config['ethernet']['phy']} link up 100 Mbps full")
        if self.mode == "setup":
            self.add_log("I", f"ap: setup AP '{self.config['device']['hostname']}' on 192.168.4.1")
            self.add_log("I", "eth: setup role, DHCP server on 192.168.5.1")
        else:
            self.add_log("I", f"wifi: connected to {self.config['wifi']['ssid1']} ch 6 rssi -55")

    def schedule_reboot(self, delay=1.0, next_mode=None):
        self.pending_reboot = (time.time() + delay, next_mode or self.mode)
        self.add_log("W", f"system: restarting in {delay:.0f} s")

    def tick(self):
        """Advance simulated time-based state. Called on every request."""
        now = time.time()
        if self.pending_reboot and now >= self.pending_reboot[0]:
            self.mode = self.pending_reboot[1]
            self.pending_reboot = None
            self.boot_count += 1
            self.down_until = now + REBOOT_DOWNTIME
            self.boot()
            self.started = self.down_until
        if self.test["state"] == "connecting" and now >= self.test["until"]:
            pw, ssid = self.test["password"], self.test["ssid"]
            if ssid == "nonexistent":
                self.test.update(state="failed", error="network not found")
            elif pw == "wrong":
                self.test.update(state="failed", error="wrong password")
            else:
                self.test.update(state="connected", error="", ip="192.168.1.37", netmask="255.255.255.0",
                                 gateway="192.168.1.1", dns="192.168.1.1", suggested_ip="192.168.1.250")
            self.add_log("I" if self.test["state"] == "connected" else "E",
                         f"wifi test: {ssid}: {self.test['state']} {self.test['error']}")
        if self.ota["state"] == "running" and "url_started" in self.ota:
            frac = min(1.0, (now - self.ota["url_started"]) / 8.0)
            if self.ota.get("fail") and frac > 0.4:
                self.ota.update(state="error", error="HTTP 404 Not Found")
                self.add_log("E", "ota: download failed: HTTP 404")
            else:
                self.ota.update(progress=int(frac * 100), bytes=int(frac * self.ota["total"]))
                if frac >= 1.0:
                    self.ota["state"] = "done"
                    self.add_log("I", "ota: image verified, restarting")
                    self.schedule_reboot(2.0)
        # Background log chatter, roughly every 1.5 s.
        while now - self.log_t > 1.5:
            self.log_t += 1.5
            lvl, msg = random.choice([
                ("I", "bridge: DHCP ACK for wired client 192.168.1.42"), ("D", "bridge: ARP reply rewritten"),
                ("D", "eth: rx 1514 bytes"), ("W", "wifi: beacon timeout, rssi -78"), ("I", "mqtt: tele sent"),
                ("E", "wifi: tx fail (no mem)"), ("D", "httpd: GET /api/status"), ("I", "time: NTP sync ok")])
            self.add_log(lvl, msg, self.log_t)

    def add_log(self, lvl, msg, t=None):
        self.seq += 1
        t_ms = int(((t or time.time()) - self.started) * 1000)
        self.log.append({"seq": self.seq, "t": max(0, t_ms), "lvl": lvl, "msg": msg})
        del self.log[:-200]

    # --- views -----------------------------------------------------------------------------------
    def config_view(self, with_secrets=False):
        c = copy.deepcopy(self.config)
        for sec, key in SECRETS:
            val = self.secrets.get(f"{sec}.{key}", "")
            if with_secrets:
                c[sec][key] = val
            else:
                c[sec][f"{key}_set"] = bool(val)
        return c

    def status(self):
        now = time.time()
        up = int(now - self.started)
        setup = self.mode == "setup"
        connected = not setup or self.test["state"] == "connected"
        if connected:
            for k, rate in (("wired_to_wifi", 140), ("wifi_to_wired", 260), ("to_bridge", 3)):
                self.frames[k] += int(rate * random.uniform(0.5, 1.5))
            if random.random() < 0.2:
                self.frames["wifi_tx_fail"] += 1
        rssi = random.randint(-60, -50)
        cfg = self.config
        wifi = {"state": "connected" if connected else "idle",
                "ssid": (cfg["wifi"]["ssid1"] or "HomeNet") if connected else "", "bssid": "f0:9f:c2:11:22:33" if connected else "",
                "channel": 6, "rssi": rssi if connected else 0, "quality": min(100, 2 * (rssi + 100)) if connected else 0,
                "ip": None if setup else cfg["network"]["ip"], "netmask": None if setup else cfg["network"]["netmask"],
                "gateway": None if setup else cfg["network"]["gateway"], "dns": None if setup else cfg["network"]["dns"],
                "connected_s": up if connected else 0, "disconnects": 2, "last_reason": "AUTH_EXPIRE",
                "ap_active": setup, "ap_ssid": cfg["device"]["hostname"], "ap_ip": "192.168.4.1",
                "ap_clients": 1 if setup else 0}
        return {
            "mode": self.mode,
            "device": {"name": cfg["device"]["name"], "hostname": cfg["device"]["hostname"], "version": "0.1.0",
                       "idf": "v5.4", "build": "Nov  8 2026 23:00:00", "board": cfg["ethernet"]["board"],
                       "partition": "app0", "ota_mode": self.ota_mode, "uptime_s": up, "reset_reason": "poweron",
                       "boot_count": self.boot_count, "free_heap": random.randint(118000, 126000),
                       "min_free_heap": 100000, "chip": "ESP32-D0WD-V3 rev 3.0", "cpu_mhz": cfg["system"]["cpu_mhz"],
                       "flash_mb": 4, "mac_wifi": "aa:bb:cc:dd:ee:ff", "mac_eth": "aa:bb:cc:dd:ee:02",
                       "time": None if setup else time.strftime("%Y-%m-%dT%H:%M:%S+0000", time.gmtime()),
                       "auth": bool(self.secrets.get("admin.password")), "reboot_required": self.reboot_required,
                       "last_crash": self.last_crash},
            "wifi": wifi,
            "ethernet": {"state": "up", "speed": 100, "duplex": "full", "role": "setup" if setup else "bridge",
                         "ip": "192.168.5.1" if setup else None},
            "client": {"mac": None if setup else "11:22:33:44:55:66", "ip": None if setup else "192.168.1.42"},
            "frames": dict(self.frames),
            "mqtt": {"enabled": cfg["mqtt"]["enabled"], "connected": cfg["mqtt"]["enabled"] and bool(cfg["mqtt"]["host"])},
            "ota": {"state": self.ota["state"], "progress": self.ota["progress"]},
        }

    # --- mutations -------------------------------------------------------------------------------
    def apply_config(self, patch, replace=False):
        if not isinstance(patch, dict):
            raise BadRequest("body must be a JSON object")
        new_cfg, new_sec = copy.deepcopy(self.config), dict(self.secrets)
        for sec, vals in patch.items():
            if sec not in new_cfg or not isinstance(vals, dict):
                raise BadRequest(f"unknown section '{sec}'")
            for key, val in vals.items():
                if (sec, key) in SECRETS:
                    if not isinstance(val, str):
                        raise BadRequest(f"{sec}.{key} must be a string")
                    validate(sec, key, val)
                    new_sec[f"{sec}.{key}"] = val
                elif key.endswith("_set") and (sec, key[:-4]) in SECRETS:
                    continue  # tolerate echoed flags
                elif key in new_cfg[sec] or (replace and key in DEFAULT_CONFIG[sec]):
                    validate(sec, key, val)
                    new_cfg[sec][key] = val
                else:
                    raise BadRequest(f"unknown field '{sec}.{key}'")
        board = new_cfg["ethernet"]["board"]
        if board != "custom":
            preset = next(b for b in BOARDS if b["id"] == board)
            new_cfg["ethernet"].update({k: v for k, v in preset.items() if k not in ("id", "name")})
        self.config, self.secrets = new_cfg, new_sec
        self.reboot_required = True
        self.add_log("I", f"config: saved {', '.join(sorted(patch))}")


def make_handler(dev: Device, verbose: bool):
    class Handler(BaseHTTPRequestHandler):
        server_version = "WiFiBridgeMock/1.0"

        def log_message(self, fmt, *args):
            if verbose:
                super().log_message(fmt, *args)

        # --- helpers -----------------------------------------------------------------------------
        def send(self, code, body, ctype="application/json", headers=None):
            if not isinstance(body, (bytes, bytearray)):
                body = json.dumps(body).encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            for k, v in (headers or {}).items():
                self.send_header(k, v)
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)

        def err(self, code, msg, headers=None):
            self.send(code, {"ok": False, "error": msg}, headers=headers)

        def body(self, limit=1 << 20):
            n = int(self.headers.get("Content-Length") or 0)
            if n > limit:
                raise BadRequest("body too large", 413)
            return self.rfile.read(n) if n else b""

        def json_body(self):
            try:
                return json.loads(self.body() or b"null")
            except (ValueError, UnicodeDecodeError):
                raise BadRequest("invalid JSON")

        def preflight(self, path):
            """Simulated downtime, auth and Origin checks. Returns False if the request was handled."""
            with dev.lock:
                dev.tick()
                if time.time() < dev.down_until:
                    self.close_connection = True  # device "rebooting": drop the connection without a reply
                    return False
                password = dev.secrets.get("admin.password")
            if path in CAPTIVE_PROBES:
                return True
            if password:
                want = "Basic " + base64.b64encode(f"admin:{password}".encode()).decode()
                if self.headers.get("Authorization") != want:
                    self.err(401, "authentication required", {"WWW-Authenticate": 'Basic realm="wifi-bridge"'})
                    return False
            origin = self.headers.get("Origin")
            if origin and urlsplit(origin).netloc != self.headers.get("Host"):
                self.err(403, "cross-origin request rejected")
                return False
            return True

        # --- GET ---------------------------------------------------------------------------------
        def do_GET(self):
            url = urlsplit(self.path)
            path, q = url.path, parse_qs(url.query)
            if not self.preflight(path):
                return
            try:
                self.route_get(path, q)
            except BadRequest as e:
                self.err(e.code, str(e))

        do_HEAD = do_GET

        def route_get(self, path, q):
            if path in ("/", "/index.html"):
                try:
                    return self.send(200, INDEX.read_bytes(), "text/html; charset=utf-8")
                except FileNotFoundError:
                    return self.err(500, f"{INDEX} not found")
            with dev.lock:
                if path == "/api/status":
                    return self.send(200, dev.status())
                if path == "/api/config":
                    return self.send(200, dev.config_view())
                if path == "/api/boards":
                    return self.send(200, BOARDS)
                if path == "/api/scan":
                    return self.send(200, self.scan())
                if path == "/api/wifi/test":
                    t = {k: v for k, v in dev.test.items() if k not in ("password", "until", "ssid")}
                    return self.send(200, dict({"error": "", "ip": None, "netmask": None, "gateway": None,
                                                "dns": None, "suggested_ip": None}, **t))
                if path == "/api/coredump":
                    if not dev.last_crash:
                        raise BadRequest("no crash report stored", 404)
                    return self.send(200, b"\x7fELF-mock-coredump", "application/octet-stream")
                if path == "/api/ota":
                    return self.send(200, {k: dev.ota[k] for k in ("state", "progress", "error", "bytes", "total")})
                if path == "/api/log":
                    since = int((q.get("since") or ["0"])[0] or 0)
                    lines = [ln for ln in dev.log if ln["seq"] > since]
                    return self.send(200, {"next": dev.seq, "lines": lines})
                if path == "/api/config/backup":
                    name = f"wifi-bridge-{dev.config['device']['hostname']}.json"
                    return self.send(200, json.dumps(dev.config_view(True), indent=2).encode(), headers={
                        "Content-Disposition": f'attachment; filename="{name}"'})
                if path == "/cm":
                    return self.send(200, run_cmd((q.get("cmnd") or [""])[0]))
                if path in CAPTIVE_PROBES or dev.mode == "setup":
                    host = self.headers.get("Host", "192.168.4.1")
                    return self.send(302, b"", "text/plain", {"Location": f"http://{host}/"})
            self.err(404, "not found")

        def scan(self):
            now = time.time()
            if dev.scan_until and now >= dev.scan_until:
                dev.scan_result, dev.scan_at, dev.scan_until = dev.pending_scan, dev.scan_until, 0.0
            if not dev.scan_until and now - dev.scan_at > 10:
                best = {}
                for ssid, rssi, ch, auth in FAKE_NETWORKS:
                    rssi += random.randint(-4, 4)
                    if ssid not in best or best[ssid]["rssi"] < rssi:
                        best[ssid] = {"ssid": ssid, "rssi": rssi, "quality": max(0, min(100, 2 * (rssi + 100))),
                                      "channel": ch, "auth": auth, "secure": auth != "OPEN"}
                dev.pending_scan = sorted(best.values(), key=lambda n: -n["rssi"])
                dev.scan_until = now + 2.5
                dev.add_log("D", "wifi: scan started")
            return {"scanning": bool(dev.scan_until), "networks": dev.scan_result}

        # --- POST --------------------------------------------------------------------------------
        def do_POST(self):
            path = urlsplit(self.path).path
            if not self.preflight(path):
                return
            try:
                self.route_post(path)
            except BadRequest as e:
                self.err(e.code, str(e))

        def route_post(self, path):
            if path == "/api/ota/upload":
                return self.ota_upload()
            if path == "/api/config/restore":
                raw = self.body()
                with dev.lock:
                    try:
                        cfg = json.loads(raw)
                    except ValueError:
                        raise BadRequest("backup is not valid JSON")
                    dev.apply_config(cfg, replace=True)
                    dev.schedule_reboot(2.0, "bridge" if dev.config["wifi"]["ssid1"] else "setup")
                return self.send(200, {"ok": True})
            data = self.json_body()
            with dev.lock:
                if path == "/api/config":
                    dev.apply_config(data)
                    return self.send(200, {"ok": True, "reboot_required": dev.reboot_required})
                if not isinstance(data, dict):
                    raise BadRequest("body must be a JSON object")
                if path == "/api/setup":
                    return self.do_setup(data)
                if path == "/api/wifi/test":
                    if dev.mode != "setup":
                        raise BadRequest("connection test is only available in setup mode", 409)
                    if not data.get("ssid"):
                        raise BadRequest("ssid is required")
                    dev.test = {"state": "connecting", "error": "", "ssid": data["ssid"],
                                "password": data.get("password", ""), "until": time.time() + 3.5}
                    dev.add_log("I", f"wifi test: connecting to {data['ssid']}")
                    return self.send(200, {"ok": True})
                if path == "/api/reboot":
                    target = data.get("target", "normal")
                    if target not in ("normal", "safeboot", "setup"):
                        raise BadRequest("target must be normal, safeboot or setup")
                    if target == "safeboot" and dev.ota_mode != "safeboot":
                        raise BadRequest("no safeboot partition", 409)
                    dev.schedule_reboot(1.0, "setup" if target == "setup" else None)
                    return self.send(200, {"ok": True})
                if path == "/api/reset":
                    scope = data.get("scope")
                    if scope not in ("all", "wifi"):
                        raise BadRequest("scope must be all or wifi")
                    if scope == "all":
                        dev.config = copy.deepcopy(DEFAULT_CONFIG)
                        dev.secrets = {}
                    else:
                        dev.config["wifi"].update(ssid1="", ssid2="")
                        for k in ("wifi.password1", "wifi.password2"):
                            dev.secrets.pop(k, None)
                    dev.schedule_reboot(1.0, "setup")
                    return self.send(200, {"ok": True})
                if path == "/api/coredump/erase":
                    dev.last_crash = None
                    return self.send(200, {"ok": True})
                if path == "/api/ota/url":
                    if dev.ota_mode != "dual":
                        raise BadRequest("OTA not possible with this partition layout", 409)
                    url = data.get("url", "")
                    if not re.match(r"^https?://\S+$", url):
                        raise BadRequest("url must start with http:// or https://")
                    if dev.ota["state"] == "running":
                        raise BadRequest("an update is already running", 409)
                    dev.ota = {"state": "running", "progress": 0, "error": "", "bytes": 0, "total": 1_234_567,
                               "url_started": time.time(), "fail": "fail" in url}
                    dev.add_log("I", f"ota: downloading {url}")
                    return self.send(200, {"ok": True})
                if path == "/api/cmd":
                    return self.send(200, {"ok": True, "result": run_cmd(str(data.get("cmd", "")))})
            self.err(404, "not found")

        def do_setup(self, data):
            ssid, net = data.get("ssid"), data.get("network") or {}
            if not ssid:
                raise BadRequest("ssid is required")
            patch = {"wifi": {"ssid1": ssid, "password1": data.get("password", "")}, "network": net}
            if data.get("hostname"):
                patch["device"] = {"hostname": data["hostname"]}
            if data.get("admin_password"):
                patch["admin"] = {"password": data["admin_password"]}
            dev.apply_config(patch)
            dev.schedule_reboot(2.0, "bridge")
            host = dev.config["device"]["hostname"]
            return self.send(200, {"ok": True, "url": f"http://{dev.config['network']['ip']}/",
                                   "mdns": f"http://{host}.local/"})

        def ota_upload(self):
            n = int(self.headers.get("Content-Length") or 0)
            if dev.ota_mode == "safeboot":
                self.rfile.read(n)
                raise BadRequest("this partition layout has a single app slot: restart into safeboot to update", 409)
            if dev.ota_mode == "none":
                self.rfile.read(n)
                raise BadRequest("OTA not supported by this partition layout", 409)
            if n <= 0:
                raise BadRequest("empty body")
            with dev.lock:
                dev.ota = {"state": "running", "progress": 0, "error": "", "bytes": 0, "total": n}
            first, got = None, 0
            while got < n:
                chunk = self.rfile.read(min(65536, n - got))
                if not chunk:
                    break
                first = chunk[0] if first is None else first
                got += len(chunk)
                with dev.lock:
                    dev.ota.update(bytes=got, progress=got * 100 // n)
                time.sleep(0.02)  # make progress visible
            with dev.lock:
                if got < n:
                    dev.ota.update(state="error", error="upload truncated")
                    raise BadRequest("upload truncated")
                if first != 0xE9:
                    dev.ota.update(state="error", error="not an ESP32 app image")
                    raise BadRequest("not an ESP32 app image (bad magic byte)")
                time.sleep(1.0)  # "verifying"
                dev.ota["state"] = "done"
                dev.add_log("I", f"ota: {n} bytes written and verified")
                dev.schedule_reboot(2.0)
            self.send(200, {"ok": True})

    def run_cmd(cmd):
        """Very small fake of the command set."""
        with dev.lock:
            dev.add_log("I", f"cmd: {cmd}")
            name, _, arg = cmd.strip().partition(" ")
            low = name.lower()
            st = dev.status()
            if low == "help":
                return "Commands: Help, Status [0], Restart 1, IPAddress, Hostname [name], LogLevel [0-4], Wifi, Uptime, Heap"
            if low == "status":
                if arg.strip() in ("0", ""):
                    return {"Status": {"DeviceName": st["device"]["name"], "Hostname": st["device"]["hostname"]},
                            "StatusFWR": {"Version": st["device"]["version"], "Core": st["device"]["idf"]},
                            "StatusNET": {"IPAddress": st["wifi"]["ip"], "Mac": st["device"]["mac_wifi"]},
                            "StatusSTS": {"UptimeSec": st["device"]["uptime_s"], "Heap": st["device"]["free_heap"],
                                          "Wifi": {"SSId": st["wifi"]["ssid"], "RSSI": st["wifi"]["rssi"]}}}
                return {"Command": "Error", "Error": f"unknown status {arg}"}
            if low == "restart" and arg.strip() == "1":
                dev.schedule_reboot(1.0)
                return {"Restart": "Restarting"}
            if low == "uptime":
                return {"Uptime": st["device"]["uptime_s"]}
            if low == "heap":
                return {"Heap": st["device"]["free_heap"]}
            if low == "hostname":
                if arg:
                    dev.apply_config({"device": {"hostname": arg.strip()}})
                return {"Hostname": dev.config["device"]["hostname"]}
            if low == "wifi":
                return {"Wifi": st["wifi"]}
            if low == "ipaddress":
                return {"IPAddress1": st["wifi"]["ip"]}
            if low == "loglevel":
                return {"LogLevel": dev.config["system"]["log_level"]}
            return {"Command": "Unknown", "Input": cmd}

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mode", choices=("setup", "bridge"), default="setup")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--password", default="", help="admin password (enables HTTP Basic auth, user 'admin')")
    ap.add_argument("--crash", action="store_true", help="pretend a crash report is stored")
    ap.add_argument("--ota-mode", choices=("dual", "safeboot", "none"), default="dual")
    ap.add_argument("-v", "--verbose", action="store_true", help="log every request")
    args = ap.parse_args()
    dev = Device(args.mode, args.password, args.ota_mode)
    if args.crash:
        dev.last_crash = {"task": "bridge", "pc": "0x400d1234", "backtrace": "0x400d1234 0x400d5678 0x4008a011", "size": 23456}
    srv = ThreadingHTTPServer((args.bind, args.port), make_handler(dev, args.verbose))

    def ticker():  # advance reboots, tests and OTA in real time, not only when a request arrives
        while True:
            with dev.lock:
                dev.tick()
            time.sleep(0.2)
    threading.Thread(target=ticker, daemon=True).start()
    srv.daemon_threads = True
    print(f"WiFi Bridge mock ({args.mode} mode, ota {args.ota_mode}) on http://localhost:{args.port}/  "
          f"serving {INDEX}")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
