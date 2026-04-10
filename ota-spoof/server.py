#!/usr/bin/env python3
"""
Clockwise OTA Spoof Server

Spoofs the topyuan.top update server to push custom firmware to the
ESP32 Clockwise clock via its built-in auto-update mechanism.

Usage:
  1. Point DNS for www.topyuan.top to this machine's IP
  2. Place your firmware binary as firmware.bin in this directory
  3. Run: python3 server.py
  4. Restart the clock (or wait for next update check)

The clock checks: GET /ledhub75/updatecheck?id=<chipid>&ver=<current_ver>
If we return a version higher than the clock's, it downloads:
  GET /ledhub75/firmware/<version>.bin
"""

import http.server
import os
import sys
import json
from urllib.parse import urlparse, parse_qs
from datetime import datetime

# --- Configuration ---
SPOOF_VERSION = "99.0"  # Must be "higher" than current firmware (3.11)
FIRMWARE_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "firmware.bin")
LISTEN_PORT = 8088
# ---------------------

class OTASpoofHandler(http.server.BaseHTTPRequestHandler):

    def log_message(self, format, *args):
        timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        print(f"[{timestamp}] {self.client_address[0]} - {format % args}")

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        params = parse_qs(parsed.query)

        # --- Update check endpoint ---
        if path == "/ledhub75/updatecheck":
            device_id = params.get("id", ["unknown"])[0]
            device_ver = params.get("ver", ["unknown"])[0]
            print(f"  >> UPDATE CHECK from device {device_id}, current version: {device_ver}")

            if not os.path.exists(FIRMWARE_FILE):
                # No custom firmware ready — return current version (no update)
                print(f"  << No firmware.bin found, returning current version {device_ver} (no update)")
                self._respond_text(device_ver)
            else:
                size = os.path.getsize(FIRMWARE_FILE)
                print(f"  << Firmware ready ({size} bytes), returning spoof version {SPOOF_VERSION}")
                self._respond_text(SPOOF_VERSION)
            return

        # --- Firmware download endpoint ---
        if path.startswith("/ledhub75/firmware/") and path.endswith(".bin"):
            requested_version = path.split("/")[-1]
            print(f"  >> FIRMWARE DOWNLOAD requested: {requested_version}")

            if os.path.exists(FIRMWARE_FILE):
                size = os.path.getsize(FIRMWARE_FILE)
                print(f"  << Serving firmware.bin ({size} bytes)")
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(size))
                self.send_header("Connection", "close")
                self.end_headers()
                with open(FIRMWARE_FILE, "rb") as f:
                    self.wfile.write(f.read())
            else:
                print(f"  << ERROR: firmware.bin not found!")
                self.send_response(404)
                self.send_header("Content-Length", "0")
                self.end_headers()
            return

        # --- Heartbeat/check-in endpoint ---
        if path == "/ledhub75/check":
            device_id = params.get("id", ["unknown"])[0]
            print(f"  >> HEARTBEAT from device {device_id}")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=UTF-8")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        # --- Pass through everything else (JS, CSS, etc.) ---
        # Return 404 for anything else — the clock's config page will
        # show the fallback "basic" page, which is fine
        print(f"  >> OTHER REQUEST: {self.path}")
        self.send_response(404)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def _respond_text(self, text):
        body = text.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=UTF-8")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    if not os.path.exists(FIRMWARE_FILE):
        print(f"WARNING: {FIRMWARE_FILE} not found!")
        print(f"The server will run but won't trigger updates until you place a firmware.bin file.")
        print()

    print(f"=== Clockwise OTA Spoof Server ===")
    print(f"Listening on port {LISTEN_PORT}")
    print(f"Spoof version: {SPOOF_VERSION}")
    print(f"Firmware file: {FIRMWARE_FILE}")
    print()
    print(f"Make sure DNS for 'www.topyuan.top' points to this machine.")
    print(f"Then restart the clock or wait for the next auto-check.")
    print()

    server = http.server.HTTPServer(("0.0.0.0", LISTEN_PORT), OTASpoofHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down.")
        server.server_close()


if __name__ == "__main__":
    main()
