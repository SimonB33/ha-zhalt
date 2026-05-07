#!/usr/bin/env python3
"""Standalone protocol exerciser for Zhalt Evolution Connect.

Phase 1 of the ha-zhalt build. No Home Assistant imports.

Default sequence:
  1. Connect to ws://<host>:<port>
  2. Receive 'CiaO'
  3. Send handshake P_imp
  4. Receive and parse G_imp; print
  5. Poll P_dat at 1.5s; parse G_dat responses; print field changes
  6. (Gated) prompt to fire mist_send=1
  7. Observe operating_mode 7->9 and elapsed counter rising
  8. Fire stop_send=1 after 6 seconds
  9. Confirm operating_mode returns to 6 then 7
 10. Print ALL TESTS PASSED or specific failure

Use --observe-only to run steps 1-5 for ~10s and exit (no mist firing).
"""
from __future__ import annotations

import argparse
import asyncio
import sys
from datetime import datetime
from typing import Any

import websockets
from websockets.asyncio.client import ClientConnection, connect


DEFAULT_HOST = "172.217.28.1"
DEFAULT_PORT = 81
POLL_INTERVAL_S = 1.5
HANDSHAKE_TIMEOUT_S = 10.0
OBSERVE_BEFORE_MIST_S = 5.0
MIST_DURATION_S = 15.0
POST_STOP_WATCH_S = 6.0


# ----------------------------- Parsing ---------------------------------------


CYCLE_LABELS_NUMBERED = ("C1", "C2", "C3", "C4", "C5", "C6")
CYCLE_LABELS_SPECIAL = ("CS", "CE", "CJ")
ALL_CYCLE_LABELS = CYCLE_LABELS_NUMBERED + CYCLE_LABELS_SPECIAL


OP_MODE_NAMES = {6: "Stopped", 7: "Standby", 9: "Misting"}


def parse_g_imp(text: str) -> dict[str, Any]:
    """Parse a G_imp settings dump into a structured dict.

    Tolerant of unexpected length: missing cycles are omitted, trailing
    metadata is preserved as raw_trailing.
    """
    tokens = text.strip().split()
    if not tokens or tokens[0] != "G_imp":
        raise ValueError(f"not a G_imp frame: {text[:80]!r}")

    out: dict[str, Any] = {
        "cmd": tokens[0],
        "machine_type": _to_int(tokens, 1),
        "progcic_type": _to_int(tokens, 3),
        "firmware_minor": tokens[5] if len(tokens) > 5 else None,
        "secondary_comm_state": tokens[6] if len(tokens) > 6 else None,
        "cycles": {},
        "raw": text,
    }

    i = 7
    while i < len(tokens):
        label = tokens[i]
        if label in CYCLE_LABELS_NUMBERED:
            need = 10
            if i + need >= len(tokens):
                break
            fields = tokens[i + 1 : i + 1 + need]
            out["cycles"][label] = {
                "act": int(fields[0]),
                "mode": int(fields[1]),
                "start_hour": int(fields[2]),
                "start_minute": int(fields[3]),
                "days_bitmap": int(fields[4]),
                "duration_seconds": int(fields[5]),
                "end_hour": int(fields[6]),
                "end_minute": int(fields[7]),
                "work_seconds": int(fields[8]),
                "pause_minutes": int(fields[9]),
            }
            i += 1 + need
        elif label in CYCLE_LABELS_SPECIAL:
            need = 8
            if i + need >= len(tokens):
                break
            fields = tokens[i + 1 : i + 1 + need]
            out["cycles"][label] = {
                "act": int(fields[0]),
                "mode": int(fields[1]),
                "days_bitmap": int(fields[2]),
                "duration_seconds": int(fields[3]),
                "end_hour": int(fields[4]),
                "end_minute": int(fields[5]),
                "work_seconds": int(fields[6]),
                "pause_minutes": int(fields[7]),
            }
            i += 1 + need
        else:
            break

    out["raw_trailing"] = tokens[i:]
    return out


def parse_g_dat(text: str) -> dict[str, Any]:
    """Parse a G_dat live status frame.

    Returns dict with named fields + 'raw_fields' (full token list).
    """
    tokens = text.strip().split()
    if not tokens or tokens[0] != "G_dat":
        raise ValueError(f"not a G_dat frame: {text[:80]!r}")

    def at(idx: int, default: Any = None) -> Any:
        return tokens[idx] if idx < len(tokens) else default

    op_mode = _to_int_at(tokens, 38)
    planned = _to_int_at(tokens, 48) or 0
    elapsed = _to_int_at(tokens, 52) or 0

    return {
        "cmd": tokens[0],
        "tick": _to_int_at(tokens, 1),
        "mist_done": _to_int_at(tokens, 2),
        "pulse_done": _to_int_at(tokens, 3),
        "stop_done": _to_int_at(tokens, 4),
        "stopday_done": _to_int_at(tokens, 5),
        "device_day": _to_int_at(tokens, 27),
        "device_month": _to_int_at(tokens, 28),
        "device_year": _to_int_at(tokens, 29),
        "device_hour": _to_int_at(tokens, 30),
        "device_minute": _to_int_at(tokens, 31),
        "device_second": _to_int_at(tokens, 32),
        "device_weekday": _to_int_at(tokens, 33),
        "operating_mode": op_mode,
        "operating_mode_name": OP_MODE_NAMES.get(op_mode, f"Unknown({op_mode})"),
        "active_cycle_id": _to_int_at(tokens, 44),
        "cycle_state": _to_int_at(tokens, 45),
        "planned_duration_sec": planned,
        "pause_seconds": _to_int_at(tokens, 50),
        "elapsed_in_cycle": elapsed,
        "remaining_sec": max(0, planned - elapsed) if planned else 0,
        "secondary_comm_state": at(43),
        "manual_stop_today_flag": _to_int_at(tokens, 54),
        "raw_fields": tokens,
        "raw": text,
    }


def _to_int(tokens: list[str], idx: int) -> int | None:
    return _to_int_at(tokens, idx)


def _to_int_at(tokens: list[str], idx: int) -> int | None:
    if idx >= len(tokens):
        return None
    try:
        return int(tokens[idx])
    except (ValueError, TypeError):
        return None


# ----------------------------- Building --------------------------------------


def build_p_imp_handshake(now: datetime, lang: str = "en-US") -> str:
    """Build the minimal handshake P_imp (seconda_comunic = 'A')."""
    weekday = now.weekday() + 1  # Monday=1..Sunday=7
    day, month, year = now.day, now.month, now.year
    hour, minute, second = now.hour, now.minute, now.second
    chksum = day + month + year + hour + minute + second
    return (
        f"P_imp;{weekday};{day};{month};{year};{hour};{minute};{second};"
        f"{lang};{chksum};A;0;"
    )


def build_p_dat(actions: dict[str, int] | None = None) -> str:
    """Build a P_dat polling/action frame.

    actions keys: mist_send, pulse_send, stop_send, stopday_send,
    provapump_send, provaled_send, provabuz_send, provaprddue_send,
    provascar_send, prevendita_send.
    """
    a = actions or {}
    return ";".join(
        [
            "P_dat",
            "1920",  # winW
            "1080",  # winH
            str(a.get("mist_send", 0)),
            str(a.get("pulse_send", 0)),
            str(a.get("stop_send", 0)),
            str(a.get("stopday_send", 0)),
            str(a.get("provapump_send", 0)),
            str(a.get("provaled_send", 0)),
            str(a.get("provabuz_send", 0)),
            str(a.get("provaprddue_send", 0)),
            str(a.get("provascar_send", 0)),
            str(a.get("prevendita_send", 0)),
            "1",  # page_open
            "0",  # snd_test_send
            "0",  # snd_set_send
            "",  # trailing empty so the joined string ends with ';'
        ]
    )


# ----------------------------- I/O helpers -----------------------------------


def _fmt_dat_summary(d: dict[str, Any]) -> str:
    return (
        f"mode={d['operating_mode']}({d['operating_mode_name']}) "
        f"cycle={d['active_cycle_id']} "
        f"elapsed={d['elapsed_in_cycle']} "
        f"planned={d['planned_duration_sec']} "
        f"tick={d['tick']}"
    )


WATCHED_FIELDS = (
    "operating_mode",
    "operating_mode_name",
    "active_cycle_id",
    "cycle_state",
    "planned_duration_sec",
    "pause_seconds",
    "mist_done",
    "stop_done",
    "manual_stop_today_flag",
)


def _diff(prev: dict[str, Any] | None, curr: dict[str, Any]) -> list[str]:
    if prev is None:
        return [f"{k}={curr[k]}" for k in WATCHED_FIELDS]
    return [
        f"{k}: {prev[k]} -> {curr[k]}"
        for k in WATCHED_FIELDS
        if prev.get(k) != curr.get(k)
    ]


async def _recv_text(ws: ClientConnection, timeout: float) -> str:
    msg = await asyncio.wait_for(ws.recv(), timeout=timeout)
    return msg if isinstance(msg, str) else msg.decode("utf-8", errors="replace")


async def _send_p_dat(ws: ClientConnection, actions: dict[str, int] | None = None) -> None:
    await ws.send(build_p_dat(actions))


# ----------------------------- Test sequence ---------------------------------


async def run(
    host: str,
    port: int,
    observe_only: bool,
    observe_seconds: float,
    mist_seconds: float,
) -> int:
    url = f"ws://{host}:{port}"
    print(f"[connect] {url}")
    try:
        ws = await asyncio.wait_for(connect(url), timeout=HANDSHAKE_TIMEOUT_S)
    except (asyncio.TimeoutError, OSError) as e:
        print(f"FAIL: cannot connect: {e}")
        return 2

    async with ws:
        # 1+2: receive CiaO
        try:
            greeting = await _recv_text(ws, HANDSHAKE_TIMEOUT_S)
        except asyncio.TimeoutError:
            print("FAIL: no CiaO received within timeout")
            return 2
        print(f"[recv ] {greeting!r}")
        if greeting.strip() != "CiaO":
            print(f"FAIL: expected 'CiaO', got {greeting!r}")
            return 2

        # 3: send handshake P_imp
        now = datetime.now()
        p_imp = build_p_imp_handshake(now)
        print(f"[send ] {p_imp}")
        await ws.send(p_imp)

        # 4: receive G_imp
        # Some firmwares send a few keepalive G_dat first; loop briefly.
        g_imp_text = None
        deadline = asyncio.get_event_loop().time() + HANDSHAKE_TIMEOUT_S
        while asyncio.get_event_loop().time() < deadline:
            try:
                msg = await _recv_text(ws, HANDSHAKE_TIMEOUT_S)
            except asyncio.TimeoutError:
                print("FAIL: no G_imp received after handshake")
                return 2
            print(f"[recv ] {msg[:120]}{'...' if len(msg) > 120 else ''}")
            if msg.startswith("G_imp"):
                g_imp_text = msg
                break
        if g_imp_text is None:
            print("FAIL: no G_imp received in window")
            return 2

        try:
            settings = parse_g_imp(g_imp_text)
        except Exception as e:
            print(f"FAIL: parse_g_imp: {e}")
            return 3
        _print_settings(settings)

        # 5: polling loop
        print("\n[poll ] starting P_dat at 1.5s ...")
        prev: dict[str, Any] | None = None
        elapsed_high_water = 0
        seen_misting = False
        seen_stopped = False
        seen_standby_after_stop = False
        observe_deadline = asyncio.get_event_loop().time() + observe_seconds

        # initial observe phase
        while asyncio.get_event_loop().time() < observe_deadline:
            await _send_p_dat(ws)
            try:
                msg = await _recv_text(ws, timeout=POLL_INTERVAL_S * 3)
            except asyncio.TimeoutError:
                print("FAIL: no G_dat response within 4.5s")
                return 4
            if not msg.startswith("G_dat"):
                print(f"[recv ] (skip non-G_dat) {msg[:80]}")
                await asyncio.sleep(POLL_INTERVAL_S)
                continue
            curr = parse_g_dat(msg)
            changes = _diff(prev, curr)
            if changes:
                print(f"[dat  ] {_fmt_dat_summary(curr)}  | changed: {', '.join(changes)}")
            prev = curr
            await asyncio.sleep(POLL_INTERVAL_S)

        if observe_only:
            print("\nOBSERVE-ONLY MODE: skipping mist test")
            print("ALL TESTS PASSED (handshake + parsing + observe)")
            return 0

        # 6: gate before firing mist
        print()
        print("=" * 60)
        print(f"ABOUT TO FIRE MISTERS for ~{mist_seconds:.0f} seconds (will spray garden).")
        print("Type 'y' + Enter to proceed, anything else to abort.")
        print("=" * 60)
        try:
            answer = await asyncio.get_event_loop().run_in_executor(None, input, "fire mist? [y/N]: ")
        except (EOFError, KeyboardInterrupt):
            print("\nABORTED at gate (no input)")
            return 0
        if answer.strip().lower() != "y":
            print("ABORTED at gate")
            return 0

        # fire mist
        print("[send ] mist_send=1")
        await ws.send(build_p_dat({"mist_send": 1}))
        try:
            msg = await _recv_text(ws, timeout=POLL_INTERVAL_S * 3)
        except asyncio.TimeoutError:
            print("FAIL: no response to mist command")
            return 4
        if msg.startswith("G_dat"):
            curr = parse_g_dat(msg)
            print(f"[dat  ] {_fmt_dat_summary(curr)} (post-mist)")
            prev = curr

        # 7: watch for misting state
        mist_deadline = asyncio.get_event_loop().time() + mist_seconds
        while asyncio.get_event_loop().time() < mist_deadline:
            await _send_p_dat(ws)  # subsequent polls with mist_send=0
            try:
                msg = await _recv_text(ws, timeout=POLL_INTERVAL_S * 3)
            except asyncio.TimeoutError:
                print("FAIL: G_dat timeout during mist watch")
                return 4
            if not msg.startswith("G_dat"):
                continue
            curr = parse_g_dat(msg)
            changes = _diff(prev, curr)
            tag = ""
            if curr["operating_mode"] == 9:
                seen_misting = True
                tag = " <MISTING>"
            elapsed_high_water = max(elapsed_high_water, curr["elapsed_in_cycle"] or 0)
            if changes:
                print(f"[dat  ] {_fmt_dat_summary(curr)}{tag}  | changed: {', '.join(changes)}")
            prev = curr
            await asyncio.sleep(POLL_INTERVAL_S)

        # 8: fire stop
        print("[send ] stop_send=1")
        await ws.send(build_p_dat({"stop_send": 1}))

        # 9: confirm we leave mode 9 (and ideally land on 7).
        # The device may transit through mode 6 too briefly to catch at 1.5s
        # poll cadence, so don't require it.
        seen_non_misting_post_stop = False
        post_deadline = asyncio.get_event_loop().time() + POST_STOP_WATCH_S
        while asyncio.get_event_loop().time() < post_deadline:
            try:
                msg = await _recv_text(ws, timeout=POLL_INTERVAL_S * 3)
            except asyncio.TimeoutError:
                print("FAIL: G_dat timeout after stop")
                return 4
            if not msg.startswith("G_dat"):
                continue
            curr = parse_g_dat(msg)
            mode = curr["operating_mode"]
            changes = _diff(prev, curr)
            if changes:
                print(f"[dat  ] {_fmt_dat_summary(curr)}  | changed: {', '.join(changes)}")
            if mode == 6:
                seen_stopped = True
            if mode is not None and mode != 9:
                seen_non_misting_post_stop = True
            if mode == 7:
                seen_standby_after_stop = True
                prev = curr
                break
            prev = curr
            await _send_p_dat(ws)
            await asyncio.sleep(POLL_INTERVAL_S)

        # report
        print()
        problems = []
        if not seen_misting:
            problems.append("did not observe operating_mode == 9 during mist")
        if not seen_non_misting_post_stop:
            problems.append("device did not leave operating_mode 9 after stop")
        if not seen_standby_after_stop:
            problems.append("did not return to operating_mode == 7 within window")
        notes = []
        if not seen_stopped:
            notes.append("transient operating_mode=6 not observed (likely too brief to catch at 1.5s poll cadence)")
        if elapsed_high_water == 0:
            notes.append("elapsed_in_cycle stayed at 0 during the short test (counter may not advance for sub-second sprays or uses non-second units)")

        if problems:
            for p in problems:
                print(f"FAIL: {p}")
            return 5

        for n in notes:
            print(f"NOTE: {n}")
        print("ALL TESTS PASSED")
        return 0


def _print_settings(settings: dict[str, Any]) -> None:
    print("\n[G_imp parsed]")
    print(f"  machine_type        : {settings['machine_type']}")
    print(f"  firmware_minor      : {settings['firmware_minor']}  (-> 2.{settings['firmware_minor']})")
    print(f"  secondary_comm_state: {settings['secondary_comm_state']}")
    print(f"  progcic_type        : {settings['progcic_type']}")
    print(f"  cycles ({len(settings['cycles'])}):")
    for label, c in settings["cycles"].items():
        if label in CYCLE_LABELS_NUMBERED:
            print(
                f"    {label}: act={c['act']} mode={c['mode']} "
                f"{c['start_hour']:02d}:{c['start_minute']:02d}->{c['end_hour']:02d}:{c['end_minute']:02d} "
                f"days={c['days_bitmap']:07b} dur={c['duration_seconds']}s "
                f"work={c['work_seconds']}s pause={c['pause_minutes']}m"
            )
        else:
            print(
                f"    {label}: act={c['act']} mode={c['mode']} "
                f"end={c['end_hour']:02d}:{c['end_minute']:02d} "
                f"days={c['days_bitmap']:07b} dur={c['duration_seconds']}s "
                f"work={c['work_seconds']}s pause={c['pause_minutes']}m"
            )


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--host", default=DEFAULT_HOST)
    p.add_argument("--port", type=int, default=DEFAULT_PORT)
    p.add_argument(
        "--observe-only",
        action="store_true",
        help="connect, handshake, observe G_dat for --observe-seconds, then exit (no mist)",
    )
    p.add_argument(
        "--observe-seconds",
        type=float,
        default=OBSERVE_BEFORE_MIST_S,
        help="how long to passively observe before the mist gate (or before exit in --observe-only)",
    )
    p.add_argument(
        "--mist-seconds",
        type=float,
        default=MIST_DURATION_S,
        help="how long to spray before sending stop_send=1 (default 15s)",
    )
    args = p.parse_args()
    return asyncio.run(
        run(
            args.host,
            args.port,
            args.observe_only,
            args.observe_seconds,
            args.mist_seconds,
        )
    )


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\ninterrupted")
        sys.exit(130)
