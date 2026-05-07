"""Unit tests for custom_components/zhalt/protocol.py.

Run with:  .venv/bin/python -m pytest tests/ -v
Or, without pytest:  .venv/bin/python -m unittest tests.test_protocol_parsing
"""
from __future__ import annotations

import sys
import unittest
from datetime import datetime
from pathlib import Path

# Make the repo root importable so tests can reach custom_components.
ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from custom_components.zhalt import protocol as p  # noqa: E402


FIXTURES = ROOT / "tests" / "fixtures"


def _read(name: str) -> str:
    return (FIXTURES / name).read_text().strip()


class ParseGImpTests(unittest.TestCase):
    def test_header_fields(self) -> None:
        s = p.parse_g_imp(_read("g_imp_sample.txt"))
        self.assertEqual(s["cmd"], "G_imp")
        self.assertEqual(s["machine_type"], 1)
        self.assertEqual(s["progcic_type"], 1)
        self.assertEqual(s["firmware_minor"], "08")
        self.assertEqual(s["secondary_comm_state"], "A")

    def test_all_nine_cycles_parsed(self) -> None:
        s = p.parse_g_imp(_read("g_imp_sample.txt"))
        self.assertEqual(set(s["cycles"].keys()), set(p.ALL_CYCLE_LABELS))

    def test_c1_matches_spec(self) -> None:
        # Spec: C1 Direct, 05:45 -> 08:00, 70s spray, all days
        c1 = p.parse_g_imp(_read("g_imp_sample.txt"))["cycles"]["C1"]
        self.assertEqual(c1["act"], 1)
        self.assertEqual(c1["mode"], p.CYCLE_MODE_DIRECT)
        self.assertEqual(c1["start_hour"], 5)
        self.assertEqual(c1["start_minute"], 45)
        self.assertEqual(c1["end_hour"], 8)
        self.assertEqual(c1["end_minute"], 0)
        self.assertEqual(c1["days_bitmap"], 127)
        self.assertEqual(c1["duration_seconds"], 70)

    def test_cs_special_cycle_shape(self) -> None:
        # Spec: CS active, end 01:30, 70s spray
        cs = p.parse_g_imp(_read("g_imp_sample.txt"))["cycles"]["CS"]
        self.assertEqual(cs["act"], 1)
        self.assertEqual(cs["mode"], p.CYCLE_MODE_DIRECT)
        self.assertEqual(cs["end_hour"], 1)
        self.assertEqual(cs["end_minute"], 30)
        self.assertEqual(cs["duration_seconds"], 70)
        # Special cycles do NOT have start_hour
        self.assertNotIn("start_hour", cs)

    def test_disabled_cycles(self) -> None:
        cycles = p.parse_g_imp(_read("g_imp_sample.txt"))["cycles"]
        self.assertEqual(cycles["C5"]["act"], 0)
        self.assertEqual(cycles["C6"]["act"], 0)
        self.assertEqual(cycles["CE"]["act"], 0)
        self.assertEqual(cycles["CJ"]["act"], 0)

    def test_rejects_non_g_imp(self) -> None:
        with self.assertRaises(ValueError):
            p.parse_g_imp("CiaO")
        with self.assertRaises(ValueError):
            p.parse_g_imp("G_dat 1 2 3")


class ParseGDatTests(unittest.TestCase):
    def test_standby_state(self) -> None:
        d = p.parse_g_dat(_read("g_dat_standby.txt"))
        self.assertEqual(d["cmd"], "G_dat")
        self.assertEqual(d["operating_mode"], p.OP_STANDBY)
        self.assertEqual(d["operating_mode_name"], "Standby")
        self.assertEqual(d["device_year"], 2026)
        self.assertEqual(d["device_month"], 5)
        self.assertEqual(d["device_day"], 7)

    def test_misting_state(self) -> None:
        d = p.parse_g_dat(_read("g_dat_misting.txt"))
        self.assertEqual(d["operating_mode"], p.OP_MISTING)
        self.assertEqual(d["operating_mode_name"], "Misting")
        self.assertEqual(d["active_cycle_id"], p.ACTIVE_CYCLE_MANUAL)
        self.assertEqual(d["active_cycle_name"], "Manual")
        self.assertEqual(d["planned_duration_sec"], 70)
        self.assertEqual(d["elapsed_in_cycle"], 200)
        # remaining clamped to >=0; 70 - 200 -> 0
        self.assertEqual(d["remaining_sec"], 0)

    def test_just_stopped_state(self) -> None:
        d = p.parse_g_dat(_read("g_dat_just_stopped.txt"))
        self.assertEqual(d["operating_mode"], p.OP_STOPPED)
        self.assertEqual(d["operating_mode_name"], "Stopped")
        self.assertEqual(d["cycle_state"], 2)  # "right after stop"
        self.assertEqual(d["manual_stop_today_flag"], 1)

    def test_remaining_sec_when_planned_zero(self) -> None:
        d = p.parse_g_dat(_read("g_dat_standby.txt"))
        # Standby fixture has planned=10, elapsed=0 -> remaining=10
        self.assertEqual(d["planned_duration_sec"], 10)
        self.assertEqual(d["remaining_sec"], 10)

    def test_active_cycle_name_heuristic(self) -> None:
        d = p.parse_g_dat(_read("g_dat_standby.txt"))
        # standby field[44] = 1 -> heuristic -> "Cycle1"
        self.assertEqual(d["active_cycle_id"], 1)
        self.assertEqual(d["active_cycle_name"], "Cycle1")

    def test_rejects_non_g_dat(self) -> None:
        with self.assertRaises(ValueError):
            p.parse_g_dat("G_imp 1")


class BuildPDatTests(unittest.TestCase):
    def test_default_is_all_zeros(self) -> None:
        s = p.build_p_dat()
        # P_dat;1920;1080;0;0;0;0;0;0;0;0;0;0;1;0;0;
        self.assertTrue(s.startswith("P_dat;1920;1080;"))
        self.assertTrue(s.endswith(";"))
        # 10 action zeros then 1;0;0;
        self.assertIn(";0;0;0;0;0;0;0;0;0;0;1;0;0;", s)

    def test_action_flag_set(self) -> None:
        s = p.build_p_dat({"mist_send": 1})
        # mist_send is the 3rd field after winW;winH; -> position right after 1080
        self.assertEqual(s, "P_dat;1920;1080;1;0;0;0;0;0;0;0;0;0;1;0;0;")

    def test_stop_action(self) -> None:
        s = p.build_p_dat({"stop_send": 1})
        self.assertIn(";0;0;1;0;", s)


class BuildPImpHandshakeTests(unittest.TestCase):
    def test_format(self) -> None:
        # Thursday May 7 2026 08:55:12 -> weekday 4, chksum 7+5+2026+8+55+12=2113
        now = datetime(2026, 5, 7, 8, 55, 12)
        s = p.build_p_imp_handshake(now)
        self.assertEqual(s, "P_imp;4;7;5;2026;8;55;12;en-US;2113;A;0;")

    def test_chksum_is_sum(self) -> None:
        now = datetime(2026, 1, 1, 0, 0, 0)
        s = p.build_p_imp_handshake(now)
        # 1+1+2026+0+0+0 = 2028
        self.assertIn(";2028;A;", s)


class BuildPImpSettingsTests(unittest.TestCase):
    def test_round_trip_yields_b_form(self) -> None:
        settings = p.parse_g_imp(_read("g_imp_sample.txt"))
        now = datetime(2026, 5, 7, 8, 55, 12)
        s = p.build_p_imp_settings(settings, now)
        self.assertTrue(s.startswith("P_imp;4;7;5;2026;8;55;12;en-US;2113;B;1;"))
        self.assertTrue(s.endswith(";"))
        # Cycles must appear in canonical order
        for label in p.ALL_CYCLE_LABELS:
            self.assertIn(f";{label} ", s)

    def test_cicliplus_matches_spec_capture(self) -> None:
        # Captured G_imp shows 1001000 in trailing metadata; settings have C4=1, CS=1
        settings = p.parse_g_imp(_read("g_imp_sample.txt"))
        self.assertEqual(
            p.compute_cicliplus_abilitation(settings["cycles"]),
            1_001_000,
        )

    def test_disable_all_cycles_round_trip(self) -> None:
        settings = p.parse_g_imp(_read("g_imp_sample.txt"))
        disabled = p.disable_all_cycles(settings)
        for c in disabled["cycles"].values():
            self.assertEqual(c["act"], 0)
        # Original untouched
        self.assertEqual(settings["cycles"]["C1"]["act"], 1)

    def test_disabled_cicliplus_is_zero(self) -> None:
        settings = p.parse_g_imp(_read("g_imp_sample.txt"))
        disabled = p.disable_all_cycles(settings)
        self.assertEqual(p.compute_cicliplus_abilitation(disabled["cycles"]), 0)

    def test_missing_cycle_raises(self) -> None:
        settings = p.parse_g_imp(_read("g_imp_sample.txt"))
        del settings["cycles"]["CJ"]
        with self.assertRaises(ValueError):
            p.build_p_imp_settings(settings, datetime(2026, 5, 7, 0, 0, 0))


if __name__ == "__main__":
    unittest.main()
