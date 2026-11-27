#!/usr/bin/env python3
"""Mac companion sync daemon for ESP-Mosaico `agy_monitor`.

Supports automatic USB-wired + Wi-Fi LAN failover:
- Wired USB (priority): pushes telemetry + Mac clock via ESP-Iris RPC (0x4147 / 1 & 2).
- Wi-Fi Fallback: serves LAN HTTP (/api/status, /api/refresh) and UDP discovery
  (port 18766) so the device keeps updating automatically once unplugged from USB.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import shutil
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Callable

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import agy_statusline_bridge  # noqa: E402

RPC_SERVICE_ID = "0x4147"
RPC_METHOD_SYNC = "1"
RPC_METHOD_POLL = "2"

DEFAULT_HTTP_PORT = 18765
DEFAULT_DISCOVER_PORT = 18766
DEFAULT_STATUS_FILE = Path.home() / ".cache" / "agy-mosaico" / "status.json"
DEFAULT_PROJECT = Path(__file__).resolve().parents[2]
DEFAULT_WORKSPACE = Path(__file__).resolve().parents[4]
STATUS_STALE_SEC = 600


def local_tz_offset_seconds() -> int:
    offset = datetime.now().astimezone().utcoffset()
    return int(offset.total_seconds()) if offset is not None else 28800


def _is_usable_lan_ip(ip: str) -> bool:
    if not ip:
        return False
    if ip.startswith(("127.", "169.254.", "198.18.", "198.19.")):
        return False
    return True


def detect_lan_ip() -> str:
    if sys.platform == "darwin":
        for iface in ("en0", "en1"):
            try:
                proc = subprocess.run(
                    ["ipconfig", "getifaddr", iface],
                    capture_output=True,
                    text=True,
                    timeout=1.0,
                )
                if proc.returncode == 0:
                    candidate = proc.stdout.strip()
                    if _is_usable_lan_ip(candidate):
                        return candidate
            except Exception:
                pass
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect(("8.8.8.8", 80))
        ip = sock.getsockname()[0]
        if _is_usable_lan_ip(ip):
            return ip
    except OSError:
        pass
    finally:
        sock.close()
    return ""


def read_statusline_cache(status_file: Path) -> tuple[dict[str, Any] | None, float]:
    if not status_file.exists():
        return None, 0.0
    try:
        data = json.loads(status_file.read_text(encoding="utf-8"))
        updated_at = float(data.get("updated_at", 0.0))
        raw = data.get("raw")
        if isinstance(raw, dict) and raw:
            return agy_statusline_bridge.normalize_statusline_payload(raw), updated_at
        norm = data.get("normalized")
        if isinstance(norm, dict):
            return norm, updated_at
    except Exception:
        return None, 0.0
    return None, 0.0


def query_conversation_service(timeout_sec: float = 0.5) -> dict[str, Any] | None:
    """Query local Antigravity ConversationService when env credentials exist."""
    address = os.environ.get("ANTIGRAVITY_LS_ADDRESS", "").strip()
    csrf = os.environ.get("ANTIGRAVITY_CSRF_TOKEN", "").strip()
    if not address or not csrf:
        return None

    base = address if address.startswith(("http://", "https://")) else f"http://{address}"
    url = base.rstrip("/") + "/antigravity.ConversationService/ListConversations"
    req = urllib.request.Request(
        url,
        data=b"{}",
        method="POST",
        headers={
            "Content-Type": "application/json",
            "Connect-Protocol-Version": "1",
            "x-codeium-csrf-token": csrf,
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout_sec) as resp:
            body = json.loads(resp.read().decode("utf-8"))
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError, OSError):
        return None

    conversations = body.get("conversations")
    if not isinstance(conversations, list):
        return None

    running = 0
    latest_summary = ""
    for item in conversations:
        if not isinstance(item, dict):
            continue
        state = item.get("state")
        if not isinstance(state, dict):
            continue
        run_state = str(state.get("runState", "")).upper()
        if "RUNNING" in run_state or "ACTIVE" in run_state:
            running += 1
        if not latest_summary and isinstance(state.get("summary"), str):
            latest_summary = state["summary"].strip()

    return {
        "running_count": running,
        "total_count": len(conversations),
        "latest_summary": latest_summary,
    }


def probe_agy_models(timeout_sec: float = 4.0) -> list[str]:
    agy_bin = shutil.which("agy") or str(Path.home() / ".local" / "bin" / "agy")
    if not Path(agy_bin).exists():
        return []
    try:
        proc = subprocess.run(
            [agy_bin, "models"],
            capture_output=True,
            text=True,
            timeout=timeout_sec,
        )
    except Exception:
        return []
    if proc.returncode != 0:
        return []
    models: list[str] = []
    for line in proc.stdout.splitlines():
        clean = line.strip()
        if not clean or clean.startswith(("Fetching", "Usage:", "Flags:")):
            continue
        parts = clean.split(None, 1)
        if len(parts) == 2:
            models.append(parts[1].strip())
        elif parts:
            models.append(parts[0].strip())
    return models


def build_demo_snapshot(
    auto_interval: int, ack_seq: int, mac_host: str = ""
) -> dict[str, Any]:
    snap: dict[str, Any] = {
        "ts": int(time.time()),
        "tz": local_tz_offset_seconds(),
        "q_pct": 78,
        "q_win": "5H 周期",
        "q_rst": "重置 3h 12m",
        "q_sub": "Pro · 额度正常",
        "model": "Gemini 4 Neon (High)",
        "state": "RUNNING",
        "ctx": 34,
        "sess": 2,
        "ws": "ESP-MOSAICO · main",
        "auto": auto_interval,
        "ack": ack_seq,
    }
    if mac_host:
        snap["host"] = mac_host
    return snap


def collect_snapshot(
    status_file: Path,
    auto_interval: int,
    ack_seq: int,
    cached_models: list[str] | None = None,
    mac_host: str = "",
) -> dict[str, Any]:
    now = time.time()
    norm, updated_at = read_statusline_cache(status_file)
    conv = query_conversation_service()

    if norm is not None:
        age = max(0.0, now - updated_at) if updated_at > 0 else 0.0
        state = str(norm.get("state") or "IDLE")
        quota_sub = str(norm.get("quota_sub") or "")
        if age > STATUS_STALE_SEC and state == "RUNNING":
            state = "IDLE"
        if conv and conv["running_count"] > 0:
            state = "RUNNING"

        sessions = int(norm.get("active_sessions", -1))
        if conv is not None:
            sessions = conv["running_count"] if conv["running_count"] > 0 else conv["total_count"]

        snap: dict[str, Any] = {
            "ts": int(now),
            "tz": local_tz_offset_seconds(),
            "q_pct": int(norm.get("quota_pct", -1)),
            "q_win": str(norm.get("quota_window") or "--"),
            "q_rst": str(norm.get("quota_reset") or "--"),
            "q_sub": quota_sub or "状态已同步",
            "model": str(norm.get("model") or "Antigravity"),
            "state": state,
            "ctx": int(norm.get("ctx_pct", -1)),
            "sess": sessions,
            "ws": str(norm.get("workspace") or "Antigravity CLI"),
            "auto": auto_interval,
            "ack": ack_seq,
        }
        if mac_host:
            snap["host"] = mac_host
        return snap

    models = cached_models if cached_models is not None else probe_agy_models()
    model_str = models[0] if models else "等待同步..."
    state_str = "IDLE"
    sess = -1
    ws_str = "请在 CLI 运行状态脚本"
    if conv is not None:
        sess = conv["running_count"] if conv["running_count"] > 0 else conv["total_count"]
        if conv["running_count"] > 0:
            state_str = "RUNNING"
        if conv["latest_summary"]:
            ws_str = conv["latest_summary"]

    snap = {
        "ts": int(now),
        "tz": local_tz_offset_seconds(),
        "q_pct": -1,
        "q_win": "等待配额",
        "q_rst": "请运行脚本",
        "q_sub": f"模型可用 ({len(models)})" if models else "连接状态未知",
        "model": model_str,
        "state": state_str,
        "ctx": -1,
        "sess": sess,
        "ws": ws_str,
        "auto": auto_interval,
        "ack": ack_seq,
    }
    if mac_host:
        snap["host"] = mac_host
    return snap


def decode_rpc_ack(cli_json: dict[str, Any]) -> tuple[int, bool]:
    resp = cli_json.get("response")
    if not isinstance(resp, dict):
        return 0, False
    b64 = resp.get("payload_base64")
    if not isinstance(b64, str) or not b64:
        return 0, False
    raw = base64.b64decode(b64).decode("utf-8", errors="replace")
    parsed = json.loads(raw)
    return int(parsed.get("refresh_seq", 0)), bool(parsed.get("syncing", False))


def call_mosaico_rpc(
    workspace: Path,
    method_id: str,
    payload: str,
    project: Path = DEFAULT_PROJECT,
    device_id: str | None = None,
    deadline_ms: int = 2500,
) -> dict[str, Any]:
    cmd = [
        sys.executable,
        str(workspace / "mosaico.py"),
        "--json",
        "iris",
        "rpc",
        "--project",
        str(project),
        RPC_SERVICE_ID,
        method_id,
        "--payload",
        payload,
        "--deadline-ms",
        str(deadline_ms),
    ]
    if device_id:
        cmd.extend(["--device-id", device_id])

    proc = subprocess.run(
        cmd,
        cwd=str(workspace),
        capture_output=True,
        text=True,
        timeout=max(5.0, deadline_ms / 1000.0 + 4.0),
    )
    if proc.returncode != 0:
        detail = (proc.stderr or proc.stdout).strip()
        raise RuntimeError(detail or f"mosaico.py exited {proc.returncode}")
    return json.loads(proc.stdout)


class ReuseThreadingHTTPServer(ThreadingHTTPServer):
    allow_reuse_address = True


def start_http_server(
    http_port: int,
    snapshot_provider: Callable[[bool], dict[str, Any]],
) -> tuple[ThreadingHTTPServer, threading.Thread]:
    wifi_log_lock = threading.Lock()
    wifi_logged = {"connected": False}

    class AgyRequestHandler(BaseHTTPRequestHandler):
        def _send_snapshot(self, force_probe: bool) -> None:
            snap = snapshot_provider(force_probe)
            with wifi_log_lock:
                first_hit = not wifi_logged["connected"]
                wifi_logged["connected"] = True
            if first_hit or force_probe:
                tag = "WIFI:MANUAL" if force_probe else "WIFI"
                print(
                    f"[agy-sync:{tag}] client={self.client_address[0]} "
                    f"model={snap['model']} quota={snap['q_pct']}% state={snap['state']}"
                )
            body = json.dumps(snap, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self) -> None:
            if self.path.split("?", 1)[0] == "/api/status":
                self._send_snapshot(force_probe=False)
            else:
                self.send_error(404)

        def do_POST(self) -> None:
            if self.path.split("?", 1)[0] == "/api/refresh":
                length = int(self.headers.get("Content-Length") or 0)
                if length > 0:
                    _ = self.rfile.read(min(length, 4096))
                self._send_snapshot(force_probe=True)
            else:
                self.send_error(404)

        def log_message(self, format: str, *args: Any) -> None:
            return

    server = ReuseThreadingHTTPServer(("0.0.0.0", http_port), AgyRequestHandler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, name="agy-http", daemon=True)
    thread.start()
    return server, thread


def start_udp_discovery_server(
    discover_port: int,
    advertised_http_port: int,
) -> tuple[socket.socket, threading.Event, threading.Thread]:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", discover_port))
    sock.settimeout(0.5)
    stop_event = threading.Event()
    reply = f"AGY_HOST {advertised_http_port}".encode("ascii")

    def serve() -> None:
        while not stop_event.is_set():
            try:
                data, addr = sock.recvfrom(128)
            except TimeoutError:
                continue
            except OSError:
                break
            if data.strip().startswith(b"AGY_DISCOVER"):
                try:
                    sock.sendto(reply, addr)
                except OSError:
                    pass

    thread = threading.Thread(target=serve, name="agy-discover", daemon=True)
    thread.start()
    return sock, stop_event, thread


def inspect_cache(status_file: Path) -> int:
    if not status_file.exists():
        print(f"Status cache not found: {status_file}")
        print(
            "Tip: Inside `agy`, run:\n"
            f"  /statusline {Path(__file__).with_name('agy_statusline_bridge.py')}"
        )
        return 1
    print(status_file.read_text(encoding="utf-8"))
    return 0


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Sync Antigravity CLI telemetry & Mac clock to ESP-Mosaico (USB + Wi-Fi)."
    )
    parser.add_argument(
        "--interval", type=int, default=10, help="Auto-sync interval in seconds (default: 10)"
    )
    parser.add_argument(
        "--poll-interval",
        type=float,
        default=1.5,
        help="USB manual-refresh poll interval in seconds (default: 1.5)",
    )
    parser.add_argument(
        "--http-port",
        type=int,
        default=DEFAULT_HTTP_PORT,
        help=f"Wi-Fi LAN HTTP server port (default: {DEFAULT_HTTP_PORT})",
    )
    parser.add_argument(
        "--discover-port",
        type=int,
        default=DEFAULT_DISCOVER_PORT,
        help=f"Wi-Fi UDP discovery port (default: {DEFAULT_DISCOVER_PORT})",
    )
    parser.add_argument(
        "--usb-only", action="store_true", help="Disable Wi-Fi LAN HTTP & UDP discovery server"
    )
    parser.add_argument(
        "--wifi-only", action="store_true", help="Run Wi-Fi LAN HTTP & UDP server without USB RPC"
    )
    parser.add_argument(
        "--workspace",
        type=Path,
        default=DEFAULT_WORKSPACE,
        help="ESP-Mosaico workspace containing mosaico.py",
    )
    parser.add_argument(
        "--project",
        type=Path,
        default=DEFAULT_PROJECT,
        help="ESP-Mosaico application project directory (default: agy_monitor)",
    )
    parser.add_argument("--device-id", default=None, help="Target ESP-Iris device ID")
    parser.add_argument(
        "--status-file",
        type=Path,
        default=DEFAULT_STATUS_FILE,
        help="Path to normalized /statusline JSON cache",
    )
    parser.add_argument("--once", action="store_true", help="Push one USB snapshot and exit")
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print serialized JSON payload without calling mosaico.py or starting servers",
    )
    parser.add_argument(
        "--demo",
        action="store_true",
        help="Serve/push realistic demo telemetry instead of live local files",
    )
    parser.add_argument(
        "--inspect",
        action="store_true",
        help="Print current cached /statusline raw and normalized JSON",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args.usb_only and args.wifi_only:
        print("Error: --usb-only and --wifi-only cannot be used together", file=sys.stderr)
        return 2

    interval = max(2, min(3600, args.interval))

    if args.inspect:
        return inspect_cache(args.status_file)

    snap_lock = threading.Lock()
    cached_models: list[str] = []
    last_model_probe = 0.0
    model_probe_inflight = False

    lan_ip = "" if args.usb_only else detect_lan_ip()
    last_ip_check = time.monotonic()
    actual_http_port = args.http_port
    mac_host = f"{lan_ip}:{actual_http_port}" if (not args.usb_only and lan_ip) else ""

    def start_async_model_probe() -> None:
        nonlocal model_probe_inflight
        with snap_lock:
            if model_probe_inflight:
                return
            model_probe_inflight = True

        def worker() -> None:
            nonlocal cached_models, last_model_probe, model_probe_inflight
            try:
                models = probe_agy_models()
                with snap_lock:
                    if models:
                        cached_models = models
                    last_model_probe = time.monotonic()
            finally:
                with snap_lock:
                    model_probe_inflight = False

        threading.Thread(target=worker, name="agy-models-probe", daemon=True).start()

    def get_snapshot(ack_seq: int, force_probe: bool = False) -> dict[str, Any]:
        nonlocal lan_ip, last_ip_check, mac_host
        now = time.monotonic()
        if not args.usb_only and (now - last_ip_check >= 30.0):
            detected = detect_lan_ip()
            with snap_lock:
                if detected:
                    lan_ip = detected
                last_ip_check = now

        with snap_lock:
            current_host = f"{lan_ip}:{actual_http_port}" if (not args.usb_only and lan_ip) else ""
            mac_host = current_host
            if args.demo:
                return build_demo_snapshot(interval, ack_seq, mac_host=current_host)
            models_snapshot = list(cached_models)
            should_probe = (
                not args.status_file.exists()
                and (force_probe or (now - last_model_probe >= 300.0))
            )

        if should_probe:
            start_async_model_probe()

        return collect_snapshot(
            args.status_file,
            interval,
            ack_seq,
            cached_models=models_snapshot,
            mac_host=current_host,
        )

    if args.dry_run:
        snap = get_snapshot(0, force_probe=True)
        print(json.dumps(snap, ensure_ascii=False, separators=(",", ":")))
        return 0

    http_server: ThreadingHTTPServer | None = None
    disc_sock: socket.socket | None = None
    disc_stop: threading.Event | None = None

    if not args.usb_only and not args.once:
        try:
            http_server, _ = start_http_server(
                args.http_port, lambda force: get_snapshot(0, force_probe=force)
            )
            actual_http_port = http_server.server_address[1]
            mac_host = f"{lan_ip}:{actual_http_port}" if lan_ip else ""
        except OSError as exc:
            print(f"[agy-sync] Wi-Fi HTTP server unavailable: {exc}")

        try:
            disc_sock, disc_stop, _ = start_udp_discovery_server(
                args.discover_port, actual_http_port
            )
        except OSError as exc:
            print(f"[agy-sync] UDP discovery unavailable: {exc}")

    print(
        f"[agy-sync] interval={interval}s status_file={args.status_file}"
        + (f" wifi_http=http://{mac_host or f'0.0.0.0:{actual_http_port}'}" if not args.usb_only and not args.once else " usb-only")
    )

    try:
        if args.wifi_only:
            while True:
                time.sleep(1.0)

        last_refresh_seq = 0
        last_push_monotonic = 0.0
        usb_state: str | None = None

        while True:
            now = time.monotonic()
            manual_requested = False

            if last_push_monotonic > 0.0 and not args.once:
                try:
                    poll_res = call_mosaico_rpc(
                        args.workspace,
                        RPC_METHOD_POLL,
                        "",
                        project=args.project,
                        device_id=args.device_id,
                    )
                    dev_seq, dev_syncing = decode_rpc_ack(poll_res)
                    if dev_seq > last_refresh_seq or dev_syncing:
                        manual_requested = True
                        last_refresh_seq = max(last_refresh_seq, dev_seq)
                except Exception as exc:
                    if usb_state != "detached":
                        usb_state = "detached"
                        wifi_hint = (
                            f" (Wi-Fi fallback ready at http://{mac_host})"
                            if mac_host
                            else ""
                        )
                        print(f"[agy-sync] USB detached{wifi_hint}: {exc}")
                    time.sleep(5.0)
                    continue

            if (
                manual_requested
                or (now - last_push_monotonic >= interval)
                or last_push_monotonic == 0.0
            ):
                snap = get_snapshot(last_refresh_seq, force_probe=manual_requested)
                payload = json.dumps(snap, ensure_ascii=False, separators=(",", ":"))
                try:
                    res = call_mosaico_rpc(
                        args.workspace,
                        RPC_METHOD_SYNC,
                        payload,
                        project=args.project,
                        device_id=args.device_id,
                    )
                    dev_seq, _ = decode_rpc_ack(res)
                    last_refresh_seq = max(last_refresh_seq, dev_seq)
                    last_push_monotonic = time.monotonic()
                    if usb_state != "connected":
                        usb_state = "connected"
                        print("[agy-sync] USB active")
                    tag = "MANUAL" if manual_requested else "AUTO"
                    print(
                        f"[agy-sync:{tag}] model={snap['model']} quota={snap['q_pct']}% "
                        f"state={snap['state']} ack={last_refresh_seq}"
                    )
                except Exception as exc:
                    if args.once:
                        print(f"[agy-sync] USB sync failed: {exc}")
                        return 1
                    if usb_state != "detached":
                        usb_state = "detached"
                        wifi_hint = (
                            f" (Wi-Fi fallback ready at http://{mac_host})"
                            if mac_host
                            else ""
                        )
                        print(f"[agy-sync] USB detached{wifi_hint}: {exc}")
                    time.sleep(5.0)
                    continue

            if args.once:
                return 0
            time.sleep(args.poll_interval)
    finally:
        if disc_stop is not None:
            disc_stop.set()
        if disc_sock is not None:
            disc_sock.close()
        if http_server is not None:
            http_server.shutdown()
            http_server.server_close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\n[agy-sync] stopped.")
        raise SystemExit(0)
