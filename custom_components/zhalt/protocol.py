"""Zhalt Evolution Connect WebSocket protocol — pure Python, no HA deps.

Exposes:
  parse_g_imp(text)              -> dict
  parse_g_dat(text)              -> dict
  build_p_imp_handshake(now)     -> str   (A form, sent right after CiaO)
  build_p_imp_settings(settings, now) -> str  (B form, full settings push)
  build_p_dat(actions=None)      -> str
  disable_all_cycles(settings)   -> dict   (returns a settings dict with all act=0)
  set_cycles_active(settings, active) -> dict

Plus named constants for state mappings.
"""
from __future__ import annotations

from datetime import datetime
from typing import Any


# ----------------------------- Constants -------------------------------------


GREETING = "CiaO"

CYCLE_LABELS_NUMBERED: tuple[str, ...] = ("C1", "C2", "C3", "C4", "C5", "C6")
CYCLE_LABELS_SPECIAL: tuple[str, ...] = ("CS", "CE", "CJ")
ALL_CYCLE_LABELS: tuple[str, ...] = CYCLE_LABELS_NUMBERED + CYCLE_LABELS_SPECIAL

OPERATING_MODE = {
    6: "Stopped",
    7: "Standby",
    9: "Misting",
}
OP_STOPPED = 6
OP_STANDBY = 7
OP_MISTING = 9

# active_cycle_id meanings (12 verified during manual mist test)
ACTIVE_CYCLE_NONE = 0
ACTIVE_CYCLE_MANUAL = 12
ACTIVE_CYCLE_NAMES = {
    0: "None",
    12: "Manual",
}

CYCLE_MODE_DIRECT = 1
CYCLE_MODE_PULSE = 2
CYCLE_MODE_NAMES = {1: "Direct", 2: "Pulse"}

# cycle_state (G_dat token 45) — per-cycle state machine observed during
# DIRECT and PULSE mists (Manual mode, 2026-05-12):
#   0 = idle, no cycle in progress
#   1 = phase 1: active spray. op=9. Lasts ~70-80% of configured duration
#       (e.g. ~8s of a 10s PULSE, ~13s of a 70s DIRECT). elapsed_in_cycle
#       counts at ~22 units/s (likely pump-pulse count, not seconds).
#   2 = phase 2: continued spray. op=6 ("Stopped") despite continued visible
#       output. elapsed resets to 0. Lasts the remaining 20-30% of duration.
#       Mains water flows through both phases per V1 owner; the phase 1 / 2
#       distinction is likely a pump-cadence or pressure-mode change, not a
#       dilution change.
#   3 = wind-down: cycle complete, no spray. op=7 (Standby). Brief transition.
# is_pump_running is true when cycle_state in (1, 2) — see derived field below.
PUMP_RUNNING_CYCLE_STATES = (1, 2)

DEFAULT_LANG = "en-US"
DEFAULT_PASSWORD = "freezanz"

P_DAT_DEFAULTS = {
    "winW": 1920,
    "winH": 1080,
    "page_open": 1,
    "snd_test_send": 0,
    "snd_set_send": 0,
}

P_DAT_ACTION_KEYS: tuple[str, ...] = (
    "mist_send",
    "pulse_send",
    "stop_send",
    "stopday_send",
    "provapump_send",
    "provaled_send",
    "provabuz_send",
    "provaprddue_send",
    "provascar_send",
    "prevendita_send",
)


# ----------------------------- Parsers ---------------------------------------


def parse_g_imp(text: str) -> dict[str, Any]:
    """Parse a G_imp settings dump into a structured dict.

    Tolerant: missing trailing fields are stored as raw_trailing for diagnostics.
    Raises ValueError if the frame is not a G_imp.
    """
    tokens = text.strip().split()
    if not tokens or tokens[0] != "G_imp":
        raise ValueError(f"not a G_imp frame: {text[:80]!r}")

    out: dict[str, Any] = {
        "cmd": tokens[0],
        "machine_type": _to_int_at(tokens, 1),
        "progcic_type": _to_int_at(tokens, 3),
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
            f = tokens[i + 1 : i + 1 + need]
            out["cycles"][label] = {
                "act": int(f[0]),
                "mode": int(f[1]),
                "start_hour": int(f[2]),
                "start_minute": int(f[3]),
                "days_bitmap": int(f[4]),
                "duration_seconds": int(f[5]),
                "end_hour": int(f[6]),
                "end_minute": int(f[7]),
                "work_seconds": int(f[8]),
                "pause_minutes": int(f[9]),
            }
            i += 1 + need
        elif label in CYCLE_LABELS_SPECIAL:
            need = 8
            if i + need >= len(tokens):
                break
            f = tokens[i + 1 : i + 1 + need]
            out["cycles"][label] = {
                "act": int(f[0]),
                "mode": int(f[1]),
                "days_bitmap": int(f[2]),
                "duration_seconds": int(f[3]),
                "end_hour": int(f[4]),
                "end_minute": int(f[5]),
                "work_seconds": int(f[6]),
                "pause_minutes": int(f[7]),
            }
            i += 1 + need
        else:
            break

    out["raw_trailing"] = tokens[i:]
    return out


def parse_g_dat(text: str) -> dict[str, Any]:
    """Parse a G_dat live status frame into a structured dict.

    Returns dict with named fields plus raw_fields (full token list) and raw.
    Tolerant of unexpected length: missing fields are None.
    """
    tokens = text.strip().split()
    if not tokens or tokens[0] != "G_dat":
        raise ValueError(f"not a G_dat frame: {text[:80]!r}")

    op_mode = _to_int_at(tokens, 38)
    planned = _to_int_at(tokens, 48) or 0
    elapsed = _to_int_at(tokens, 52) or 0
    active_cycle = _to_int_at(tokens, 44)
    cycle_state = _to_int_at(tokens, 45)

    return {
        "cmd": tokens[0],
        "tick": _to_int_at(tokens, 1),
        "mist_done": _to_int_at(tokens, 2),
        "pulse_done": _to_int_at(tokens, 3),
        "stop_done": _to_int_at(tokens, 4),
        "stopday_done": _to_int_at(tokens, 5),
        "provapump_done": _to_int_at(tokens, 6),
        "provaled_done": _to_int_at(tokens, 7),
        "provabuz_done": _to_int_at(tokens, 8),
        "provaprddue_done": _to_int_at(tokens, 9),
        "provascar_done": _to_int_at(tokens, 10),
        "prevendita_done": _to_int_at(tokens, 11),
        "snd_test_done": _to_int_at(tokens, 12),
        "sensor_voltage_x10": _to_int_at(tokens, 14),
        "stop_today_state": _to_int_at(tokens, 21),
        "device_day": _to_int_at(tokens, 27),
        "device_month": _to_int_at(tokens, 28),
        "device_year": _to_int_at(tokens, 29),
        "device_hour": _to_int_at(tokens, 30),
        "device_minute": _to_int_at(tokens, 31),
        "device_second": _to_int_at(tokens, 32),
        "device_weekday": _to_int_at(tokens, 33),
        "machine_type_echo": _to_int_at(tokens, 35),
        "operating_mode": op_mode,
        "operating_mode_name": OPERATING_MODE.get(op_mode, f"Unknown({op_mode})"),
        "substate": _to_int_at(tokens, 39),
        "voltage2_x10": _to_int_at(tokens, 40),
        "secondary_comm_state": tokens[43] if len(tokens) > 43 else None,
        "active_cycle_id": active_cycle,
        "active_cycle_name": _active_cycle_name(active_cycle),
        "cycle_state": cycle_state,
        "is_pump_running": cycle_state in PUMP_RUNNING_CYCLE_STATES,
        "planned_duration_sec": planned,
        "pause_seconds": _to_int_at(tokens, 50),
        "elapsed_in_cycle": elapsed,
        "remaining_sec": max(0, planned - elapsed) if planned else 0,
        "manual_stop_today_flag": _to_int_at(tokens, 54),
        "raw_fields": tokens,
        "raw": text,
    }


def _active_cycle_name(value: int | None) -> str:
    if value is None:
        return "Unknown"
    if value in ACTIVE_CYCLE_NAMES:
        return ACTIVE_CYCLE_NAMES[value]
    # Heuristic: 1..9 may map to C1..C6/CS/CE/CJ. Mapping not yet verified;
    # surface as Cycle{n} until we confirm by toggling individual cycles.
    if 1 <= value <= 9:
        return f"Cycle{value}"
    return f"Unknown({value})"


def _to_int_at(tokens: list[str], idx: int) -> int | None:
    if idx >= len(tokens):
        return None
    try:
        return int(tokens[idx])
    except (ValueError, TypeError):
        return None


# ----------------------------- Builders --------------------------------------


def _chksum(now: datetime) -> int:
    return now.day + now.month + now.year + now.hour + now.minute + now.second


def _weekday_1to7(now: datetime) -> int:
    """Monday=1..Sunday=7."""
    return now.weekday() + 1


def build_p_imp_handshake(now: datetime, lang: str = DEFAULT_LANG) -> str:
    """Minimal handshake P_imp (seconda_comunic = 'A'), sent right after CiaO."""
    return (
        f"P_imp;{_weekday_1to7(now)};{now.day};{now.month};{now.year};"
        f"{now.hour};{now.minute};{now.second};"
        f"{lang};{_chksum(now)};A;0;"
    )


def build_p_imp_settings(
    settings: dict[str, Any],
    now: datetime,
    lang: str = DEFAULT_LANG,
    password: str = DEFAULT_PASSWORD,
) -> str:
    """Full settings push P_imp (seconda_comunic = 'B').

    settings must have shape returned by parse_g_imp (with all 9 cycles and
    machine_type / progcic_type). Trailing fields default to safe values.
    """
    cycles = settings.get("cycles", {})
    for label in ALL_CYCLE_LABELS:
        if label not in cycles:
            raise ValueError(f"missing cycle {label} in settings")

    progcic_type = settings.get("progcic_type", 0) or 0
    machine_type = settings.get("machine_type", 1) or 1

    parts: list[str] = [
        "P_imp",
        str(_weekday_1to7(now)),
        str(now.day),
        str(now.month),
        str(now.year),
        str(now.hour),
        str(now.minute),
        str(now.second),
        lang,
        str(_chksum(now)),
        "B",
        str(progcic_type),
    ]

    for label in CYCLE_LABELS_NUMBERED:
        c = cycles[label]
        # Cycle label and first field are joined with a space; the rest with
        # semicolons. This matches post_impo() in the device's web UI JS.
        parts.append(f"{label} {c['act']}")
        parts.extend(
            [
                str(c["mode"]),
                str(c["start_hour"]),
                str(c["start_minute"]),
                str(c["days_bitmap"]),
                str(c["duration_seconds"]),
                str(c["end_hour"]),
                str(c["end_minute"]),
                str(c["work_seconds"]),
                str(c["pause_minutes"]),
            ]
        )

    for label in CYCLE_LABELS_SPECIAL:
        c = cycles[label]
        parts.append(f"{label} {c['act']}")
        parts.extend(
            [
                str(c["mode"]),
                str(c["days_bitmap"]),
                str(c["duration_seconds"]),
                str(c["end_hour"]),
                str(c["end_minute"]),
                str(c["work_seconds"]),
                str(c["pause_minutes"]),
            ]
        )

    cicliplus = compute_cicliplus_abilitation(cycles)
    parts.extend(
        [
            "0",  # vbatcalib_request
            str(cicliplus),  # cicliplus_abilitation
            "0",  # change_machinetype_send
            str(machine_type),  # machine_typevoluto
            "0",  # psw_changed
            password,  # psw_tosend
            "0",  # lingua_voluta
            "0",  # captive_voluto
            "0",  # chan_voluto
            "1",  # secure_save — persist settings to device NVS
        ]
    )

    return ";".join(parts) + ";"


def compute_cicliplus_abilitation(cycles: dict[str, dict[str, Any]]) -> int:
    """Decimal-encoded bitmap of which 'plus' cycles are enabled.

    Per spec: 1000000*C4 + 100000*C5 + 10000*C6 + 1000*CS + 100*CE + 10*CJ + 1*cycloP.
    cycloP/cycloPi is a separate flag we don't expose; treat as 0 for v0.1.
    """
    def en(label: str) -> int:
        return 1 if cycles.get(label, {}).get("act", 0) else 0

    return (
        1_000_000 * en("C4")
        + 100_000 * en("C5")
        + 10_000 * en("C6")
        + 1_000 * en("CS")
        + 100 * en("CE")
        + 10 * en("CJ")
        + 0  # cycloP unused in v0.1
    )


def build_p_dat(actions: dict[str, int] | None = None) -> str:
    """Polling/action P_dat frame.

    actions keys: any of P_DAT_ACTION_KEYS, set to 1 to trigger.
    """
    a = actions or {}
    parts: list[str] = [
        "P_dat",
        str(P_DAT_DEFAULTS["winW"]),
        str(P_DAT_DEFAULTS["winH"]),
    ]
    for k in P_DAT_ACTION_KEYS:
        parts.append(str(a.get(k, 0)))
    parts.extend(
        [
            str(P_DAT_DEFAULTS["page_open"]),
            str(P_DAT_DEFAULTS["snd_test_send"]),
            str(P_DAT_DEFAULTS["snd_set_send"]),
        ]
    )
    return ";".join(parts) + ";"


# ----------------------------- Settings helpers ------------------------------


def disable_all_cycles(settings: dict[str, Any]) -> dict[str, Any]:
    """Return a copy of settings with every cycle's act set to 0."""
    return set_cycles_active(settings, active=False)


def set_cycles_active(settings: dict[str, Any], active: bool) -> dict[str, Any]:
    """Return a copy of settings with every cycle's act set to 0 or 1."""
    val = 1 if active else 0
    new_cycles = {
        label: {**cycle, "act": val} for label, cycle in settings["cycles"].items()
    }
    return {**settings, "cycles": new_cycles}
