"""
PlatformIO extra script — firmware release management.

Custom targets:
  pio run -t release          Bump patch version, build, copy firmware, commit & push
  pio run -t release_minor    Bump minor version
  pio run -t release_major    Bump major version
  pio run -t release_nobump   Rebuild and copy firmware without bumping version

Set RELEASE_NOTES env var before running to attach notes:
  $env:RELEASE_NOTES = "Fixed OTA"; pio run -t release
"""
import json
import os
import re
import shutil
import subprocess
import sys

Import("env")  # noqa: F821  — injected by PlatformIO

PROJECT_DIR = env.subst("$PROJECT_DIR")
PLATFORMIO_INI = os.path.join(PROJECT_DIR, "platformio.ini")
FIRMWARE_DIR = os.path.join(PROJECT_DIR, "docs", "firmware")
RELEASES_JSON = os.path.join(FIRMWARE_DIR, "releases.json")
FRAMEWORK_DIR = os.path.join(
    os.path.expanduser("~"), ".platformio", "packages", "framework-arduinoespressif32"
)


def _get_hostname(env):
    """Extract OTA_HOSTNAME from the current environment's build flags."""
    for flag in env.get("BUILD_FLAGS", []):
        m = re.search(r'-DOTA_HOSTNAME=\\"([^"]+)\\"', flag)
        if m:
            return m.group(1)
    # Also try the flattened form after PlatformIO resolves ${env:base.build_flags}
    raw = env.GetProjectOption("build_flags", "")
    for line in raw.splitlines():
        m = re.search(r'-DOTA_HOSTNAME=\\"([^"]+)\\"', line.strip())
        if m:
            return m.group(1)
    return None


def _get_home_page(env):
    """Extract HOME_PAGE from build flags."""
    for flag in env.get("BUILD_FLAGS", []):
        m = re.search(r'-DHOME_PAGE=\\"([^"]+)\\"', flag)
        if m:
            return m.group(1)
    raw = env.GetProjectOption("build_flags", "")
    for line in raw.splitlines():
        m = re.search(r'-DHOME_PAGE=\\"([^"]+)\\"', line.strip())
        if m:
            return m.group(1)
    return "phone.infinitebutts.com"


# ---------------------------------------------------------------------------
# Version helpers
# ---------------------------------------------------------------------------

def _read_version_from_ini():
    """Read current FIRMWARE_VERSION from platformio.ini."""
    with open(PLATFORMIO_INI, "r") as f:
        content = f.read()
    m = re.search(r'-DFIRMWARE_VERSION=\\"([^"]+)\\"', content)
    return m.group(1) if m else "0.0.0"


def _parse_version(ver):
    parts = ver.split(".")
    return [int(p) for p in parts] + [0] * (3 - len(parts))


def _bump_version(ver, part="patch"):
    major, minor, patch = _parse_version(ver)
    if part == "major":
        major += 1; minor = 0; patch = 0
    elif part == "minor":
        minor += 1; patch = 0
    else:
        patch += 1
    return f"{major}.{minor}.{patch}"


def _write_version_to_ini(new_version):
    """Update FIRMWARE_VERSION in platformio.ini."""
    with open(PLATFORMIO_INI, "r") as f:
        content = f.read()
    content = re.sub(
        r'-DFIRMWARE_VERSION=\\"[0-9]+\.[0-9]+\.[0-9]+\\"',
        f'-DFIRMWARE_VERSION=\\"{new_version}\\"',
        content,
    )
    with open(PLATFORMIO_INI, "w") as f:
        f.write(content)


# ---------------------------------------------------------------------------
# Firmware copy
# ---------------------------------------------------------------------------

def _copy_firmware(env, hostname):
    """Copy build artifacts into docs/firmware/<hostname>/."""
    build_dir = env.subst("$BUILD_DIR")
    dest_dir = os.path.join(FIRMWARE_DIR, hostname)
    os.makedirs(dest_dir, exist_ok=True)

    # Files from the build directory
    build_files = {
        "firmware.bin": "firmware.bin",
        "bootloader.bin": "bootloader.bin",
        "partitions.bin": "partitions.bin",
    }

    for src_name, dst_name in build_files.items():
        src = os.path.join(build_dir, src_name)
        dst = os.path.join(dest_dir, dst_name)
        if os.path.exists(src):
            shutil.copy2(src, dst)
            size_kb = os.path.getsize(src) / 1024
            print(f"  Copied {src_name} ({size_kb:.1f} KB)")
        else:
            # Try framework fallback for bootloader
            if src_name == "bootloader.bin":
                fallback = os.path.join(
                    FRAMEWORK_DIR, "tools", "sdk", "esp32", "bin", "bootloader_dio_40m.bin"
                )
                if os.path.exists(fallback):
                    shutil.copy2(fallback, dst)
                    print(f"  Copied {dst_name} from framework")
                    continue
            if os.path.exists(dst):
                print(f"  Using existing {dst_name}")
            else:
                print(f"  WARNING: {src_name} not found")

    # boot_app0.bin from framework
    boot_app0_src = os.path.join(FRAMEWORK_DIR, "tools", "partitions", "boot_app0.bin")
    boot_app0_dst = os.path.join(dest_dir, "boot_app0.bin")
    if os.path.exists(boot_app0_src):
        shutil.copy2(boot_app0_src, boot_app0_dst)
        print("  Copied boot_app0.bin from framework")


# ---------------------------------------------------------------------------
# releases.json / manifest.json updates
# ---------------------------------------------------------------------------

def _update_releases_json(hostname, new_version, home_page, release_notes):
    """Update (or create) the device entry in releases.json."""
    releases = {}
    if os.path.exists(RELEASES_JSON):
        with open(RELEASES_JSON, "r") as f:
            releases = json.load(f)

    firmware_url = f"https://{home_page}/firmware/{hostname}/firmware.bin"

    entry = releases.get(hostname, {})
    entry["version"] = new_version
    entry["firmware_url"] = firmware_url
    if release_notes:
        entry["release_notes"] = release_notes
    entry.setdefault("min_version", "0.0.0")
    entry.setdefault("action", "none")
    entry.setdefault("message", "")
    releases[hostname] = entry

    # Also update "default" to point to latest
    default_entry = releases.get("default", {})
    default_entry["version"] = new_version
    default_entry["firmware_url"] = firmware_url
    if release_notes:
        default_entry["release_notes"] = release_notes
    default_entry.setdefault("min_version", "0.0.0")
    default_entry.setdefault("action", "none")
    default_entry.setdefault("message", "")
    releases["default"] = default_entry

    with open(RELEASES_JSON, "w") as f:
        json.dump(releases, f, indent=2)
        f.write("\n")

    print(f"  Updated releases.json ({hostname} -> {new_version})")


def _update_manifest_json(hostname, new_version):
    """Update manifest.json version inside the device firmware directory."""
    manifest_path = os.path.join(FIRMWARE_DIR, hostname, "manifest.json")
    if not os.path.exists(manifest_path):
        return
    with open(manifest_path, "r") as f:
        manifest = json.load(f)
    manifest["version"] = new_version
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print(f"  Updated manifest.json -> {new_version}")


# ---------------------------------------------------------------------------
# Git helpers
# ---------------------------------------------------------------------------

def _git_commit_and_push(new_version, release_notes, no_push=False):
    """Stage firmware files and version changes, commit, and optionally push."""
    try:
        subprocess.run(["git", "add", "platformio.ini"], cwd=PROJECT_DIR, check=True)
        subprocess.run(["git", "add", "docs/firmware/"], cwd=PROJECT_DIR, check=True)

        msg = f"Release v{new_version}"
        if release_notes:
            msg += f"\n\n{release_notes}"

        subprocess.run(["git", "commit", "-m", msg], cwd=PROJECT_DIR, check=True)
        print(f"  Committed: v{new_version}")

        if not no_push:
            subprocess.run(["git", "push"], cwd=PROJECT_DIR, check=True)
            print("  Pushed to remote")
    except subprocess.CalledProcessError as e:
        print(f"  Git error: {e}")


# ---------------------------------------------------------------------------
# Core release action
# ---------------------------------------------------------------------------

def _do_release(env, bump_part=None):
    """
    Main release workflow:
    1. Optionally bump version
    2. Build firmware (already done by PlatformIO before custom target)
    3. Copy firmware files to docs/firmware/<hostname>/
    4. Update releases.json and manifest.json
    5. Git commit and push
    """
    hostname = _get_hostname(env)
    if not hostname:
        print("ERROR: Could not determine OTA_HOSTNAME from build flags")
        sys.exit(1)

    home_page = _get_home_page(env)
    release_notes = os.environ.get("RELEASE_NOTES", "")
    no_push = os.environ.get("RELEASE_NO_PUSH", "").lower() in ("1", "true", "yes")

    current_version = _read_version_from_ini()

    if bump_part:
        new_version = _bump_version(current_version, bump_part)
        print(f"\n  Version: {current_version} -> {new_version}")
        _write_version_to_ini(new_version)
    else:
        new_version = current_version
        print(f"\n  Version: {current_version} (no bump)")

    # Build
    print(f"\n  Building firmware for {hostname}...")
    env.Execute(env.VerboseAction(
        env.Action("$BUILD_SCRIPT_IDEDATA" if False else ""),
        "Building..."
    ))

    # Actually trigger the build via PlatformIO
    ret = subprocess.run(
        [sys.executable, "-m", "platformio", "run", "-e", env.subst("$PIOENV")],
        cwd=PROJECT_DIR,
    )
    if ret.returncode != 0:
        print("ERROR: Build failed!")
        sys.exit(1)

    # Copy firmware files
    print(f"\n  Copying firmware to docs/firmware/{hostname}/...")
    _copy_firmware(env, hostname)

    # Update JSON files
    print("\n  Updating release metadata...")
    _update_releases_json(hostname, new_version, home_page, release_notes)
    _update_manifest_json(hostname, new_version)

    # Git
    print("\n  Committing release...")
    _git_commit_and_push(new_version, release_notes, no_push)

    print(f"\n  Release v{new_version} complete!")


# ---------------------------------------------------------------------------
# PlatformIO custom target registrations
# ---------------------------------------------------------------------------

def release_patch(source, target, env):
    _do_release(env, bump_part="patch")

def release_minor(source, target, env):
    _do_release(env, bump_part="minor")

def release_major(source, target, env):
    _do_release(env, bump_part="major")

def release_nobump(source, target, env):
    _do_release(env, bump_part=None)

env.AddCustomTarget("release", None, release_patch,
    title="Release (patch)", description="Bump patch version, build, deploy, commit & push")
env.AddCustomTarget("release_minor", None, release_minor,
    title="Release (minor)", description="Bump minor version, build, deploy, commit & push")
env.AddCustomTarget("release_major", None, release_major,
    title="Release (major)", description="Bump major version, build, deploy, commit & push")
env.AddCustomTarget("release_nobump", None, release_nobump,
    title="Release (no bump)", description="Rebuild and deploy without bumping version")
