#!/usr/bin/env python3
"""Unit tests for the Mac companion sync & statusline bridge scripts."""

from __future__ import annotations

import base64
import io
import json
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.request
from pathlib import Path
from unittest import mock

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

import agy_mosaico_sync  # noqa: E402
import agy_statusline_bridge  # noqa: E402


class TestStatuslineBridge(unittest.TestCase):
    def test_normalize_direct_quota_and_workspace(self) -> None:
        payload = {
            "model": {"display_name": "Gemini 4 Neon (High)"},
            "agent_state": "working",
            "context_window": {"used_percentage": 34.4},
            "quota": {
                "tier": "Pro",
                "remaining_percent": 78,
                "window": "5H 周期",
                "reset_in_seconds": 11520,
            },
            "workspace": {
                "current_dir": "/Users/eric/Desktop/ESP-MOSAICO",
                "git_branch": "main",
                "git_dirty": False,
            },
            "background_tasks_count": 2,
        }
        norm = agy_statusline_bridge.normalize_statusline_payload(payload)
        self.assertEqual(norm["model"], "Gemini 4 Neon (High)")
        self.assertEqual(norm["state"], "RUNNING")
        self.assertEqual(norm["quota_pct"], 78)
        self.assertEqual(norm["quota_window"], "5H 周期")
        self.assertEqual(norm["quota_reset"], "重置 3h 12m")
        self.assertEqual(norm["quota_sub"], "Pro · 额度正常")
        self.assertEqual(norm["ctx_pct"], 34)
        self.assertEqual(norm["active_sessions"], 2)
        self.assertEqual(norm["workspace"], "ESP-MOSAICO · main")

    def test_normalize_rolling_5h_used_percent(self) -> None:
        payload = {
            "model": "claude-opus-5-5-high",
            "agent_state": "READY",
            "context_window": {"used_tokens": 25000, "total_tokens": 100000},
            "quota": {"tier": "Ultra", "rolling_5h": {"used_percent": 21, "reset_in_seconds": 3600}},
        }
        norm = agy_statusline_bridge.normalize_statusline_payload(payload)
        self.assertEqual(norm["quota_pct"], 79)
        self.assertEqual(norm["quota_window"], "5H 周期")
        self.assertEqual(norm["quota_reset"], "重置 1h 0m")
        self.assertEqual(norm["ctx_pct"], 25)
        self.assertEqual(norm["state"], "IDLE")

    def test_context_only_fallback_and_cache_write(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            cache = Path(tmp) / "status.json"
            proc = subprocess.run(
                [sys.executable, str(SCRIPT_DIR / "agy_statusline_bridge.py")],
                input=json.dumps({"model": "Gemini 3.8 Flash", "context_window": {"used_percentage": 12}}),
                text=True,
                capture_output=True,
                env={"AGY_MOSAICO_STATUS_FILE": str(cache)},
                check=True,
            )
            self.assertIn("Gemini 3.8 Flash", proc.stdout)
            saved = json.loads(cache.read_text(encoding="utf-8"))
            self.assertEqual(saved["normalized"]["quota_pct"], -1)
            self.assertEqual(saved["normalized"]["ctx_pct"], 12)

    def test_real_agy_quota_pools(self) -> None:
        raw = {
            "model": {"id": "Orchestrator (Neon)", "display_name": "Orchestrator (Neon)"},
            "workspace": {"current_dir": "/Users/eric/Desktop/ESP-MOSAICO"},
            "context_window": {"used_percentage": 29.85},
            "quota": {
                "3p-5h": {"remaining_fraction": 1, "reset_in_seconds": 0},
                "3p-weekly": {"remaining_fraction": 0.98, "reset_in_seconds": 300},
                "gemini-5h": {"remaining_fraction": 0.6545, "reset_in_seconds": 14145},
                "gemini-weekly": {"remaining_fraction": 0.9051, "reset_in_seconds": 600945},
            },
            "agent_state": "working",
            "plan_tier": "Google AI Pro",
        }
        norm_gem = agy_statusline_bridge.normalize_statusline_payload(raw)
        self.assertEqual(norm_gem["quota_pct"], 65)
        self.assertEqual(norm_gem["quota_window"], "5H 周期")
        self.assertEqual(norm_gem["quota_reset"], "重置 3h 55m")
        self.assertEqual(norm_gem["quota_sub"], "Pro · 周余 91%")
        self.assertEqual(norm_gem["ctx_pct"], 30)
        self.assertEqual(norm_gem["state"], "RUNNING")

        raw_3p = dict(raw)
        raw_3p["model"] = {"display_name": "Claude Opus 5.5 (High)"}
        norm_3p = agy_statusline_bridge.normalize_statusline_payload(raw_3p)
        self.assertEqual(norm_3p["quota_pct"], 100)
        self.assertEqual(norm_3p["quota_window"], "3P 5H 周期")
        self.assertEqual(norm_3p["quota_reset"], "已重置")
        self.assertEqual(norm_3p["quota_sub"], "Pro · 周余 98%")

    def test_detect_lan_ip_rejects_vpn_tun(self) -> None:
        self.assertFalse(agy_mosaico_sync._is_usable_lan_ip("198.18.0.1"))
        self.assertFalse(agy_mosaico_sync._is_usable_lan_ip("127.0.0.1"))
        self.assertTrue(agy_mosaico_sync._is_usable_lan_ip("192.168.20.12"))

    def test_chinese_strings_covered_by_font_charset(self) -> None:
        charset_file = SCRIPT_DIR.parents[1] / "components" / "app_agy_ui" / "ui" / "assets" / "charset_zh.txt"
        charset = set(charset_file.read_text(encoding="utf-8"))
        for payload in (
            {"model": "Orchestrator (Neon)"},
            {
                "model": "Orchestrator (Neon)",
                "quota": {"gemini-5h": {"remaining_fraction": 0.65, "reset_in_seconds": 3600}, "gemini-weekly": {"remaining_fraction": 0.9}},
                "plan_tier": "Google AI Pro",
            },
        ):
            norm = agy_statusline_bridge.normalize_statusline_payload(payload)
            for key in ("quota_window", "quota_reset", "quota_sub"):
                missing = [ch for ch in norm[key] if ch not in charset]
                self.assertEqual(missing, [], f"missing glyphs in {key}: {missing}")

    def test_transient_init_preserves_recent_quota(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            cache = Path(tmp) / "status.json"
            agy_statusline_bridge.write_status_cache(
                {
                    "model": "Orchestrator (Neon)",
                    "state": "RUNNING",
                    "quota_pct": 65,
                    "quota_window": "5H 周期",
                    "quota_reset": "重置 3h 55m",
                    "quota_sub": "Pro · 周余 91%",
                    "ctx_pct": 30,
                    "active_sessions": -1,
                    "workspace": "ESP-MOSAICO",
                },
                {"agent_state": "working"},
                cache_path=cache,
                now_epoch=1000.0,
            )
            agy_statusline_bridge.write_status_cache(
                {
                    "model": "Orchestrator (Neon)",
                    "state": "INIT",
                    "quota_pct": -1,
                    "quota_window": "状态已连接",
                    "quota_reset": "暂未获取",
                    "quota_sub": "等待配额",
                    "ctx_pct": -1,
                    "active_sessions": -1,
                    "workspace": "ESP-MOSAICO",
                },
                {"agent_state": "init"},
                cache_path=cache,
                now_epoch=1120.0,
            )
            saved = json.loads(cache.read_text(encoding="utf-8"))
            self.assertEqual(saved["normalized"]["quota_pct"], 65)
            self.assertEqual(saved["normalized"]["quota_sub"], "Pro · 周余 91%")
            self.assertEqual(saved["normalized"]["state"], "INIT")


class TestMosaicoSync(unittest.TestCase):
    def test_dry_run_demo_and_live_cache(self) -> None:
        buf = io.StringIO()
        with mock.patch("agy_mosaico_sync.detect_lan_ip", return_value="192.168.1.50"):
            with mock.patch("sys.stdout", buf):
                rc = agy_mosaico_sync.main(["--demo", "--dry-run"])
        self.assertEqual(rc, 0)
        payload = json.loads(buf.getvalue())
        self.assertEqual(payload["q_pct"], 78)
        self.assertEqual(payload["model"], "Gemini 4 Neon (High)")
        self.assertEqual(payload["host"], "192.168.1.50:18765")
        self.assertLess(len(buf.getvalue().encode("utf-8")), 512)

        with tempfile.TemporaryDirectory() as tmp:
            cache = Path(tmp) / "status.json"
            agy_statusline_bridge.write_status_cache(
                {
                    "model": "Claude Sonnet 5.5",
                    "state": "THINKING",
                    "quota_pct": 64,
                    "quota_window": "5H 周期",
                    "quota_reset": "重置 2h 0m",
                    "quota_sub": "Pro · 额度正常",
                    "ctx_pct": 42,
                    "active_sessions": 1,
                    "workspace": "repo · feat",
                },
                {},
                cache_path=cache,
            )
            buf2 = io.StringIO()
            with mock.patch("sys.stdout", buf2):
                rc2 = agy_mosaico_sync.main(
                    ["--status-file", str(cache), "--dry-run", "--interval", "10", "--usb-only"]
                )
            self.assertEqual(rc2, 0)
            snap = json.loads(buf2.getvalue())
            self.assertEqual(snap["model"], "Claude Sonnet 5.5")
            self.assertEqual(snap["q_pct"], 64)
            self.assertEqual(snap["state"], "THINKING")
            self.assertNotIn("host", snap)

    def test_decode_rpc_ack_and_manual_refresh_loop(self) -> None:
        ack_bytes = json.dumps({"ok": True, "refresh_seq": 3, "syncing": True}).encode()
        cli_json = {"response": {"payload_base64": base64.b64encode(ack_bytes).decode()}}
        seq, syncing = agy_mosaico_sync.decode_rpc_ack(cli_json)
        self.assertEqual(seq, 3)
        self.assertTrue(syncing)

    def test_http_server_and_udp_discovery(self) -> None:
        calls: list[bool] = []

        def provider(force_probe: bool) -> dict[str, object]:
            calls.append(force_probe)
            return agy_mosaico_sync.build_demo_snapshot(10, 0, mac_host="127.0.0.1:18765")

        server, _ = agy_mosaico_sync.start_http_server(0, provider)
        http_port = server.server_address[1]
        disc_sock, disc_stop, _ = agy_mosaico_sync.start_udp_discovery_server(0, http_port)
        disc_port = disc_sock.getsockname()[1]

        try:
            with urllib.request.urlopen(f"http://127.0.0.1:{http_port}/api/status", timeout=2) as r:
                self.assertEqual(r.status, 200)
                data = json.loads(r.read().decode("utf-8"))
                self.assertEqual(data["model"], "Gemini 4 Neon (High)")

            req = urllib.request.Request(
                f"http://127.0.0.1:{http_port}/api/refresh", data=b"", method="POST"
            )
            with urllib.request.urlopen(req, timeout=2) as r2:
                self.assertEqual(r2.status, 200)
                _ = json.loads(r2.read().decode("utf-8"))

            self.assertEqual(calls, [False, True])

            client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            client.settimeout(2.0)
            try:
                client.sendto(b"AGY_DISCOVER", ("127.0.0.1", disc_port))
                reply, _ = client.recvfrom(64)
                self.assertEqual(reply.decode("ascii"), f"AGY_HOST {http_port}")
            finally:
                client.close()
        finally:
            disc_stop.set()
            disc_sock.close()
            server.shutdown()
            server.server_close()

    def test_http_server_is_nonblocking_when_probe_is_slow(self) -> None:
        def slow_probe(*_args: object, **_kwargs: object) -> list[str]:
            time.sleep(2.0)
            return ["Gemini 4 Neon"]

        with tempfile.TemporaryDirectory() as tmp:
            missing_cache = Path(tmp) / "missing_status.json"
            t0 = time.monotonic()
            with mock.patch("agy_mosaico_sync.probe_agy_models", side_effect=slow_probe):
                srv, _ = agy_mosaico_sync.start_http_server(
                    0,
                    lambda _force: agy_mosaico_sync.collect_snapshot(
                        missing_cache, 10, 0, cached_models=[], mac_host="192.168.20.6:18795"
                    ),
                )
                port = srv.server_address[1]
                try:
                    req = urllib.request.Request(
                        f"http://127.0.0.1:{port}/api/refresh", data=b"", method="POST"
                    )
                    with urllib.request.urlopen(req, timeout=1.0) as resp:
                        self.assertEqual(resp.status, 200)
                    elapsed = time.monotonic() - t0
                    self.assertLess(elapsed, 0.25)
                finally:
                    srv.shutdown()
                    srv.server_close()


if __name__ == "__main__":
    unittest.main()
