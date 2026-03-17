"""
PlatformIO custom upload script — HTTP OTA over WireGuard VPN.

Unlike espota (UDP + reverse TCP), this uses plain HTTP POST to /update,
which works reliably over WireGuard tunnels.

Flow:
  1. GET /prepareota  — tells the device to unmount SD and prepare
  2. POST /update     — streams firmware.bin via multipart upload
"""
import os
import sys
import time

Import("env")  # noqa: F821  — injected by PlatformIO

def http_ota_upload(source, target, env):
    device_ip = env.GetProjectOption("upload_port", "10.253.0.2")
    firmware = str(source[0])
    fw_size = os.path.getsize(firmware)

    print(f"HTTP OTA: uploading {firmware} ({fw_size} bytes) to {device_ip}")

    # --- Use urllib so we have zero extra dependencies ---
    import urllib.request
    import urllib.error

    # Step 1: Prepare device
    prepare_url = f"http://{device_ip}/prepareota"
    print(f"  → GET {prepare_url}")
    try:
        req = urllib.request.Request(prepare_url)
        with urllib.request.urlopen(req, timeout=120) as resp:
            body = resp.read().decode()
            print(f"  ← {resp.status}: {body}")
    except Exception as exc:
        print(f"  ⚠ Prepare request failed ({exc}) — continuing anyway")

    time.sleep(3)

    # Step 2: Upload firmware via multipart/form-data POST
    upload_url = f"http://{device_ip}/update?size={fw_size}"
    print(f"  → POST {upload_url}")

    boundary = "----PlatformIOFirmwareUpload"
    with open(firmware, "rb") as f:
        fw_data = f.read()

    body = (
        f"--{boundary}\r\n"
        f'Content-Disposition: form-data; name="firmware"; filename="firmware.bin"\r\n'
        f"Content-Type: application/octet-stream\r\n\r\n"
    ).encode() + fw_data + f"\r\n--{boundary}--\r\n".encode()

    req = urllib.request.Request(
        upload_url,
        data=body,
        headers={
            "Content-Type": f"multipart/form-data; boundary={boundary}",
            "Content-Length": str(len(body)),
        },
        method="POST",
    )

    try:
        with urllib.request.urlopen(req, timeout=600) as resp:
            result = resp.read().decode()
            print(f"  ← {resp.status}: {result}")
    except urllib.error.URLError as exc:
        # Device reboots immediately on success, so a connection reset is expected
        if "reset" in str(exc).lower() or "EOF" in str(exc):
            print("  ← Device rebooted (connection reset) — upload likely succeeded")
        else:
            print(f"  ✗ Upload failed: {exc}", file=sys.stderr)
            sys.exit(1)

    print("HTTP OTA: done")


env.Replace(UPLOADCMD=http_ota_upload)
