#!/usr/bin/env python3
"""Antigravity CLI (/statusline) bridge for ESP-Mosaico agy_monitor.

Configure inside Antigravity CLI TUI:
    /statusline /path/to/agy_statusline_bridge.py

This script reads the live JSON state payload piped to stdin by `agy`,
normalizes remaining quota, current model, agent run state, context usage,
and workspace info, atomically writes ~/.cache/agy-mosaico/status.json,
and prints a concise single-line status string to stdout.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

DEFAULT_CACHE_PATH = Path.home() / ".cache" / "agy-mosaico" / "status.json"


def _as_dict(value: Any) -> dict[str, Any]:
    return value if isinstance(value, dict) else {}


def _first_non_empty(*values: Any) -> str:
    for val in values:
        if isinstance(val, str) and val.strip():
            return val.strip()
    return ""


def _clamp_pct(value: Any) -> int | None:
    if value is None or isinstance(value, bool):
        return None
    try:
        num = float(value)
    except (TypeError, ValueError):
        return None
    if num < 0:
        return None
    return max(0, min(100, int(round(num))))


def _format_duration_compact(seconds: Any) -> str:
    if seconds is None or isinstance(seconds, bool):
        return ""
    try:
        total = int(float(seconds))
    except (TypeError, ValueError):
        return ""
    if total <= 0:
        return "即将重置"
    days, rem = divmod(total, 86400)
    hours, rem = divmod(rem, 3600)
    minutes = rem // 60
    if days > 0:
        return f"重置 {days}d {hours}h"
    if hours > 0:
        return f"重置 {hours}h {minutes}m"
    return f"重置 {max(1, minutes)}m"


def _normalize_state(raw_state: str) -> str:
    upper = raw_state.strip().upper()
    if not upper:
        return "IDLE"
    if any(k in upper for k in ("ERR", "FAIL", "CRASH")):
        return "ERROR"
    if any(k in upper for k in ("THINK", "REASON")):
        return "THINKING"
    if any(k in upper for k in ("WAIT", "PROMPT", "APPROV")):
        return "WAITING"
    if any(k in upper for k in ("RUN", "WORK", "ACT", "EXEC", "STREAM", "BUSY")):
        return "RUNNING"
    if "INIT" in upper:
        return "INIT"
    if any(k in upper for k in ("IDLE", "READY", "DONE", "COMPLET")):
        return "IDLE"
    return upper[:10]


def _shorten_tier(tier: str) -> str:
    if "ULTRA" in tier.upper():
        return "Ultra"
    if "PRO" in tier.upper():
        return "Pro"
    return tier[:16]


def _extract_quota(payload: dict[str, Any], model_name: str = "") -> tuple[int, str, str, str]:
    quota = _as_dict(payload.get("quota"))
    rate_limit = _as_dict(payload.get("rate_limit"))
    rolling_5h = _as_dict(quota.get("rolling_5h"))
    weekly = _as_dict(quota.get("weekly"))

    tier = _first_non_empty(quota.get("tier"), payload.get("plan_tier"))
    short_tier = _shorten_tier(tier) if tier else ""

    lower_model = model_name.lower()
    if "claude" in lower_model or "gpt" in lower_model or "oss" in lower_model:
        pool_5h_key, pool_weekly_key, pool_label = "3p-5h", "3p-weekly", "3P 5H 周期"
    else:
        pool_5h_key, pool_weekly_key, pool_label = "gemini-5h", "gemini-weekly", "5H 周期"

    pool_5h = _as_dict(quota.get(pool_5h_key))
    pool_weekly = _as_dict(quota.get(pool_weekly_key))
    pool_frac = pool_5h.get("remaining_fraction")
    if isinstance(pool_frac, (int, float)) and not isinstance(pool_frac, bool):
        pct = _clamp_pct(pool_frac * 100.0 if pool_frac <= 1.0 else pool_frac)
        if pct is not None:
            reset_sec = pool_5h.get("reset_in_seconds")
            if isinstance(reset_sec, (int, float)) and reset_sec <= 0 and pct >= 99:
                reset_str = "已重置"
            else:
                reset_str = _format_duration_compact(reset_sec) or "周期正常"
            weekly_frac = pool_weekly.get("remaining_fraction")
            weekly_pct = (
                _clamp_pct(weekly_frac * 100.0 if weekly_frac <= 1.0 else weekly_frac)
                if isinstance(weekly_frac, (int, float)) and not isinstance(weekly_frac, bool)
                else None
            )
            health = "额度告急" if pct <= 15 else ("额度偏低" if pct <= 35 else "额度正常")
            if weekly_pct is not None:
                sub = f"{short_tier + ' · ' if short_tier else ''}周余 {weekly_pct}%"
            else:
                sub = f"{short_tier + ' · ' if short_tier else ''}{health}"
            return pct, pool_label, reset_str, sub

    pct = _clamp_pct(
        quota.get("remaining_percent")
        or quota.get("remaining_pct")
        or rate_limit.get("remaining_percent")
        or rate_limit.get("remaining_pct")
        or rolling_5h.get("remaining_percent")
        or rolling_5h.get("remaining_pct")
        or payload.get("remaining_quota_percent")
    )

    if pct is None:
        frac = (
            quota.get("remaining_fraction")
            if quota.get("remaining_fraction") is not None
            else rate_limit.get("remaining_fraction")
        )
        if isinstance(frac, (int, float)) and not isinstance(frac, bool):
            pct = _clamp_pct(frac * 100.0 if frac <= 1.0 else frac)

    window_label = "5H 周期"
    reset_str = ""
    if pct is None:
        used = rolling_5h.get("used_percent")
        if used is not None:
            used_pct = _clamp_pct(used)
            if used_pct is not None:
                pct = max(0, 100 - used_pct)
                window_label = "5H 周期"
                reset_str = _format_duration_compact(rolling_5h.get("reset_in_seconds"))
    if pct is None:
        used_w = weekly.get("used_percent")
        if used_w is not None:
            used_pct = _clamp_pct(used_w)
            if used_pct is not None:
                pct = max(0, 100 - used_pct)
                window_label = "周周期"
                reset_str = _format_duration_compact(weekly.get("reset_in_seconds"))

    win_override = _first_non_empty(
        quota.get("window"),
        quota.get("window_label"),
        rate_limit.get("window"),
    )
    if win_override:
        window_label = win_override

    if not reset_str:
        reset_str = _first_non_empty(
            quota.get("reset_label"),
            rate_limit.get("reset_label"),
        )
    if not reset_str:
        reset_str = _format_duration_compact(
            quota.get("reset_in_seconds")
            or rolling_5h.get("reset_in_seconds")
            or rate_limit.get("reset_in_seconds")
        )

    tier = _first_non_empty(quota.get("tier"), payload.get("plan_tier"))
    credits = quota.get("credits_remaining") or payload.get("credits_remaining")

    if pct is None:
        window_label = "状态已连接"
        reset_str = reset_str or "暂未获取"
        sub = f"{short_tier} · 等待配额" if short_tier else "等待配额"
        return -1, window_label, reset_str, sub

    if pct <= 15:
        health = "额度告急"
    elif pct <= 35:
        health = "额度偏低"
    else:
        health = "额度正常"

    if credits is not None and not isinstance(credits, bool):
        sub = f"{tier + ' · ' if tier else ''}Credits {credits}"
    elif tier:
        sub = f"{tier} · {health}"
    else:
        sub = health
    return pct, window_label, reset_str or "周期正常", sub


def normalize_statusline_payload(payload: dict[str, Any]) -> dict[str, Any]:
    model_raw = payload.get("model")
    if isinstance(model_raw, dict):
        model_name = _first_non_empty(
            model_raw.get("display_name"),
            model_raw.get("name"),
            model_raw.get("id"),
        )
    else:
        model_name = _first_non_empty(model_raw, payload.get("model_name"))

    cw = _as_dict(payload.get("context_window"))
    ctx_pct = _clamp_pct(cw.get("used_percentage") or cw.get("used_percent"))
    if ctx_pct is None:
        used_tok = cw.get("used_tokens") or cw.get("total_tokens")
        total_tok = cw.get("total_tokens") or cw.get("context_window_size")
        if (
            isinstance(used_tok, (int, float))
            and isinstance(total_tok, (int, float))
            and total_tok > 0
            and cw.get("used_tokens") is not None
        ):
            ctx_pct = _clamp_pct((float(used_tok) / float(total_tok)) * 100.0)

    state = _normalize_state(
        _first_non_empty(
            payload.get("agent_state"),
            payload.get("state"),
            payload.get("status"),
        )
    )

    ws = _as_dict(payload.get("workspace"))
    vcs = _as_dict(payload.get("vcs"))
    cur_dir = _first_non_empty(ws.get("current_dir"), payload.get("cwd"))
    dir_name = Path(cur_dir).name if cur_dir else ""
    branch = _first_non_empty(ws.get("git_branch"), vcs.get("branch"))
    dirty = "*" if (ws.get("git_dirty") or vcs.get("dirty")) else ""
    if dir_name and branch:
        ws_label = f"{dir_name} · {branch}{dirty}"
    elif dir_name:
        ws_label = dir_name
    elif branch:
        ws_label = f"{branch}{dirty}"
    else:
        ws_label = _first_non_empty(payload.get("conversation_title"), "Antigravity CLI")

    quota_pct, quota_window, quota_reset, quota_sub = _extract_quota(payload, model_name)

    bg_tasks = payload.get("background_tasks_count")
    active_sessions = (
        int(bg_tasks) if isinstance(bg_tasks, int) and bg_tasks >= 0 else -1
    )

    return {
        "model": model_name or "Antigravity",
        "state": state,
        "quota_pct": quota_pct,
        "quota_window": quota_window,
        "quota_reset": quota_reset,
        "quota_sub": quota_sub,
        "ctx_pct": ctx_pct if ctx_pct is not None else -1,
        "active_sessions": active_sessions,
        "workspace": ws_label,
    }


def _preserve_recent_quota(
    normalized: dict[str, Any],
    cache_path: Path,
    now_epoch: float,
    max_age_sec: float = 900.0,
) -> dict[str, Any]:
    if normalized.get("quota_pct", -1) >= 0 or not cache_path.exists():
        return normalized
    try:
        prev = json.loads(cache_path.read_text(encoding="utf-8"))
        prev_norm = prev.get("normalized")
        prev_ts = float(prev.get("updated_at", 0.0))
        if (
            isinstance(prev_norm, dict)
            and int(prev_norm.get("quota_pct", -1)) >= 0
            and 0.0 <= (now_epoch - prev_ts) <= max_age_sec
        ):
            merged = dict(normalized)
            for key in ("quota_pct", "quota_window", "quota_reset", "quota_sub"):
                merged[key] = prev_norm[key]
            return merged
    except Exception:
        pass
    return normalized


def write_status_cache(
    normalized: dict[str, Any],
    raw: dict[str, Any],
    cache_path: Path = DEFAULT_CACHE_PATH,
    now_epoch: float | None = None,
) -> None:
    cache_path.parent.mkdir(parents=True, exist_ok=True)
    ts = time.time() if now_epoch is None else now_epoch
    merged_norm = _preserve_recent_quota(normalized, cache_path, ts)
    record = {
        "updated_at": ts,
        "normalized": merged_norm,
        "raw": raw,
    }
    fd, tmp_name = tempfile.mkstemp(
        prefix=".status-", suffix=".tmp", dir=str(cache_path.parent)
    )
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            json.dump(record, handle, ensure_ascii=False, indent=2)
        os.replace(tmp_name, cache_path)
    except Exception:
        try:
            os.unlink(tmp_name)
        except OSError:
            pass
        raise


def format_terminal_statusline(normalized: dict[str, Any]) -> str:
    parts = [f"✦ {normalized['model']}", normalized["state"]]
    if normalized["quota_pct"] >= 0:
        parts.append(f"Quota:{normalized['quota_pct']}%")
    if normalized["ctx_pct"] >= 0:
        parts.append(f"Ctx:{normalized['ctx_pct']}%")
    if normalized["workspace"]:
        parts.append(normalized["workspace"])
    return " | ".join(parts)


def main() -> int:
    raw_text = sys.stdin.read()
    cache_path = Path(
        os.environ.get("AGY_MOSAICO_STATUS_FILE", str(DEFAULT_CACHE_PATH))
    ).expanduser()

    try:
        raw_payload = json.loads(raw_text) if raw_text.strip() else {}
        if not isinstance(raw_payload, dict):
            raw_payload = {"value": raw_payload}
    except json.JSONDecodeError:
        print("✦ Antigravity")
        return 0

    normalized = normalize_statusline_payload(raw_payload)
    try:
        write_status_cache(normalized, raw_payload, cache_path=cache_path)
    except OSError:
        pass

    chained_cmd = os.environ.get("AGY_MOSAICO_CHAIN_STATUSLINE", "").strip()
    if chained_cmd:
        try:
            proc = subprocess.run(
                chained_cmd,
                input=raw_text,
                text=True,
                shell=True,
                capture_output=True,
                timeout=1.5,
            )
            if proc.returncode == 0 and proc.stdout.strip():
                print(proc.stdout.strip())
                return 0
        except Exception:
            pass

    print(format_terminal_statusline(normalized))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
