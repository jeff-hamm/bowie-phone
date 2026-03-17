import importlib
import importlib.util
import io
import pathlib
import sys
import types
import unittest
from contextlib import redirect_stdout
from datetime import datetime
from unittest.mock import patch


def load_serial_tester_module():
    # Provide lightweight stubs when pyserial is unavailable in the test environment.
    try:
        importlib.import_module("serial")
    except Exception:
        serial_mod = types.ModuleType("serial")

        class SerialException(Exception):
            pass

        class DummySerial:
            def __init__(self, *args, **kwargs):
                self.is_open = True
                self.in_waiting = 0

            def close(self):
                self.is_open = False

        list_ports_mod = types.ModuleType("serial.tools.list_ports")
        list_ports_mod.comports = lambda: []

        tools_mod = types.ModuleType("serial.tools")
        tools_mod.list_ports = list_ports_mod

        serial_mod.SerialException = SerialException
        serial_mod.Serial = DummySerial
        serial_mod.tools = tools_mod

        sys.modules["serial"] = serial_mod
        sys.modules["serial.tools"] = tools_mod
        sys.modules["serial.tools.list_ports"] = list_ports_mod

    repo_root = pathlib.Path(__file__).resolve().parents[2]
    script_path = repo_root / "tools" / "serial_test.py"

    spec = importlib.util.spec_from_file_location("serial_test", script_path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


serial_test_module = load_serial_tester_module()
BowiePhoneTester = serial_test_module.BowiePhoneTester


class FakeSerial:
    def __init__(self):
        self.is_open = True
        self.writes = []
        self.flush_calls = 0

    def write(self, payload):
        self.writes.append(payload)

    def flush(self):
        self.flush_calls += 1

    def close(self):
        self.is_open = False


class FailingWriteSerial(FakeSerial):
    def write(self, payload):
        raise serial_test_module.serial.SerialException("write failed")


class CapturingSerial(FakeSerial):
    def __init__(self, *args, **kwargs):
        super().__init__()
        self.args = args
        self.kwargs = kwargs


class FakePortInfo:
    def __init__(self, device, description, manufacturer=None):
        self.device = device
        self.description = description
        self.manufacturer = manufacturer


class FakeThread:
    def __init__(self):
        self.join_called = False
        self.join_timeout = None

    def join(self, timeout=None):
        self.join_called = True
        self.join_timeout = timeout


class SerialTesterIntegrationTests(unittest.TestCase):
    def test_find_port_prefers_known_adapter_keywords(self):
        ports = [
            FakePortInfo("COM1", "Generic Serial Device", "ACME"),
            FakePortInfo("COM7", "USB-SERIAL CH340", None),
        ]

        with patch.object(serial_test_module.serial.tools.list_ports, "comports", return_value=ports):
            tester = BowiePhoneTester()
            self.assertEqual(tester.find_port(), "COM7")

    def test_find_port_falls_back_to_first_port(self):
        ports = [
            FakePortInfo("COM9", "Unknown Device A"),
            FakePortInfo("COM10", "Unknown Device B"),
        ]

        with patch.object(serial_test_module.serial.tools.list_ports, "comports", return_value=ports):
            tester = BowiePhoneTester()
            self.assertEqual(tester.find_port(), "COM9")

    def test_find_port_returns_none_when_no_ports_exist(self):
        with patch.object(serial_test_module.serial.tools.list_ports, "comports", return_value=[]):
            tester = BowiePhoneTester()
            self.assertIsNone(tester.find_port())

    def test_connect_returns_false_when_no_port_available(self):
        tester = BowiePhoneTester(port=None)

        with patch.object(BowiePhoneTester, "find_port", return_value=None):
            self.assertFalse(tester.connect())

    def test_connect_success_uses_selected_port_and_baud(self):
        tester = BowiePhoneTester(port="COM77", baudrate=57600)

        with patch.object(serial_test_module.serial, "Serial", CapturingSerial):
            self.assertTrue(tester.connect())
            self.assertIsNotNone(tester.serial)
            self.assertEqual(tester.serial.kwargs["port"], "COM77")
            self.assertEqual(tester.serial.kwargs["baudrate"], 57600)

    def test_connect_handles_serial_exception(self):
        tester = BowiePhoneTester(port="COM_BAD")

        with patch.object(
            serial_test_module.serial,
            "Serial",
            side_effect=serial_test_module.serial.SerialException("boom")
        ):
            self.assertFalse(tester.connect())

    def test_disconnect_stops_reader_and_closes_serial(self):
        tester = BowiePhoneTester(port="COM_TEST")
        tester.running = True
        tester.reader_thread = FakeThread()
        tester.serial = FakeSerial()

        tester.disconnect()

        self.assertFalse(tester.running)
        self.assertTrue(tester.reader_thread.join_called)
        self.assertEqual(tester.reader_thread.join_timeout, 1)
        self.assertFalse(tester.serial.is_open)

    def test_parse_output_updates_hook_audio_sequence_warning_and_error(self):
        tester = BowiePhoneTester(port="COM_TEST")

        tester.parse_output("Phone picked up (OFF HOOK)")
        tester.parse_output("Playing dialtone")
        tester.parse_output("Current sequence: '911'")
        tester.parse_output("⚠️ Minor warning")
        tester.parse_output("❌ Failed to play file")
        tester.parse_output("Phone hung up (ON HOOK)")
        tester.parse_output("Stopped audio")

        self.assertEqual(tester.last_state["hook"], "ON_HOOK")
        self.assertFalse(tester.last_state["audio_playing"])
        self.assertEqual(tester.last_state["last_sequence"], "911")
        self.assertEqual(len(tester.last_state["warnings"]), 1)
        self.assertEqual(len(tester.last_state["errors"]), 1)

    def test_parse_output_accepts_alternate_sequence_format(self):
        tester = BowiePhoneTester(port="COM_TEST")
        tester.parse_output("sequence 6969 complete")
        self.assertEqual(tester.last_state["last_sequence"], "6969")

    def test_parse_output_ignores_non_numeric_sequence(self):
        tester = BowiePhoneTester(port="COM_TEST")
        tester.parse_output("Current sequence: '*#A'")
        self.assertIsNone(tester.last_state["last_sequence"])

    def test_parse_output_tracks_hook_sequence_and_errors(self):
        tester = BowiePhoneTester(port="COM_TEST")

        tester.parse_output("Phone picked up (OFF HOOK)")
        tester.parse_output("Current sequence: '911'")
        tester.parse_output("Failed to play audio")

        self.assertEqual(tester.last_state["hook"], "OFF_HOOK")
        self.assertEqual(tester.last_state["last_sequence"], "911")
        self.assertEqual(len(tester.last_state["errors"]), 1)

    def test_send_command_returns_false_when_not_connected(self):
        tester = BowiePhoneTester(port="COM_TEST")
        tester.serial = None
        self.assertFalse(tester.send_command("hook"))

    def test_send_command_handles_serial_write_error(self):
        tester = BowiePhoneTester(port="COM_TEST")
        tester.serial = FailingWriteSerial()
        self.assertFalse(tester.send_command("hook"))

    def test_send_command_writes_newline_terminated_payload(self):
        tester = BowiePhoneTester(port="COM_TEST")
        tester.serial = FakeSerial()

        ok = tester.send_command("hook")

        self.assertTrue(ok)
        self.assertEqual(tester.serial.writes[-1], b"hook\n")
        self.assertEqual(tester.serial.flush_calls, 1)

    def test_wait_and_confirm_normalizes_user_response(self):
        tester = BowiePhoneTester(port="COM_TEST")

        with patch("time.sleep", return_value=None), patch("builtins.input", return_value="Y"):
            response = tester.wait_and_confirm("Prompt", timeout=1)

        self.assertEqual(response, "y")

    def test_show_state_prints_current_values(self):
        tester = BowiePhoneTester(port="COM_TEST")
        tester.last_state["hook"] = "OFF_HOOK"
        tester.last_state["audio_playing"] = True
        tester.last_state["last_sequence"] = "123"

        capture = io.StringIO()
        with redirect_stdout(capture):
            tester.show_state()

        output = capture.getvalue()
        self.assertIn("Hook State: OFF_HOOK", output)
        self.assertIn("Audio Playing: True", output)
        self.assertIn("Last Sequence: 123", output)

    def test_show_errors_and_warnings_handle_empty_and_non_empty(self):
        tester = BowiePhoneTester(port="COM_TEST")

        empty_capture = io.StringIO()
        with redirect_stdout(empty_capture):
            tester.show_errors()
            tester.show_warnings()
        empty_output = empty_capture.getvalue()
        self.assertIn("No errors recorded", empty_output)
        self.assertIn("No warnings recorded", empty_output)

        tester.last_state["errors"].append((datetime.now(), "err line"))
        tester.last_state["warnings"].append((datetime.now(), "warn line"))

        populated_capture = io.StringIO()
        with redirect_stdout(populated_capture):
            tester.show_errors()
            tester.show_warnings()
        populated_output = populated_capture.getvalue()
        self.assertIn("err line", populated_output)
        self.assertIn("warn line", populated_output)

    def test_main_returns_1_when_no_serial_ports(self):
        with patch.object(serial_test_module.serial.tools.list_ports, "comports", return_value=[]), \
             patch.object(sys, "argv", ["serial_test.py"]):
            result = serial_test_module.main()
        self.assertEqual(result, 1)


if __name__ == "__main__":
    unittest.main()
