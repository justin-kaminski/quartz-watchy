import io
import json
import os
import sys

import pytest

from fakes import KNOWN_FRAME
from qzctl.cli import command_line, main, selftest_table
from test_png import read_png

FAKES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fakes.py")
EXEC = ["--exec", f"{sys.executable} {FAKES}", "--timeout", "5", "--connect-timeout", "5"]


def cli(*args):
    out, err = io.StringIO(), io.StringIO()
    code = main([*EXEC, *args], out=out, err=err)
    return code, out.getvalue(), err.getvalue()


def test_run_ok_prints_json_and_exits_zero():
    code, out, _ = cli("run", "version")
    assert code == 0 and json.loads(out)["fw"] == "1.0.0"


def test_run_err_exits_one_with_code_on_stderr():
    code, out, err = cli("run", "bogus")
    assert code == 1 and json.loads(out) == {"msg": "bogus"} and "ERR unknown_cmd" in err


def test_run_words_are_joined_and_quoted():
    assert command_line(["tz set UTC"]) == "tz set UTC"
    assert command_line(["wifi", "set", "My Net", "pw"]) == 'wifi set "My Net" pw'


def test_run_sleep_reconnects_and_reports_ready():
    code, out, err = cli("run", "sleep 5")
    assert code == 0 and json.loads(out) == {} and "ready:" in err and '"reset": "deepsleep"' in err


def test_wait_ready_prints_event():
    code, out, _ = cli("wait-ready", "--strict", "--wait", "5")
    assert code == 0 and json.loads(out)["evt"] == "ready"


def test_screenshot_writes_png(tmp_path):
    target = tmp_path / "shot.png"
    code, out, _ = cli("screenshot", str(target))
    assert code == 0 and "crc32 1abd04d4" in out
    w, h, depth, ctype, rows = read_png(target.read_bytes())
    assert (w, h, depth, ctype) == (200, 200, 1, 0)
    assert b"".join(rows) == bytes(b ^ 0xFF for b in KNOWN_FRAME)


def test_selftest_table_and_exit_status():
    code, out, _ = cli("selftest")
    assert code == 0 and "rtc/xtal" in out and "2 passed, 0 failed, 1 skipped" in out
    code, out, _ = cli("selftest", "failme")
    assert code == 1 and "1 failed" in out and "crc mismatch" in out


def test_selftest_table_columns_align():
    lines = selftest_table([{"name": "a/b", "status": "pass", "ms": 5, "detail": ""},
                            {"name": "long/name", "status": "fail", "ms": 123, "detail": "why"}]).splitlines()
    assert lines[0].startswith("TEST") and lines[2].endswith("why")
    assert lines[1].index("pass") == lines[2].index("fail")


def test_shell_session(monkeypatch):
    script = iter(["version", "", "bogus", "sleep 1", "status", "exit"])
    monkeypatch.setattr("builtins.input", lambda prompt="": next(script))
    code, out, _ = cli("shell")
    assert code == 0
    assert out.count("\n") == 4  # version, bogus, sleep, status each printed one JSON line


def test_connection_failure_exits_three():
    out, err = io.StringIO(), io.StringIO()
    code = main(["--exec", "/nonexistent/qz-sim", "run", "version"], out=out, err=err)
    assert code == 3 and "qzctl:" in err.getvalue()


def test_usage_errors_exit_two(capsys):
    assert main([], out=io.StringIO(), err=io.StringIO()) == 2
    assert main(["--usb", "nonsense", "run", "x"], out=io.StringIO(), err=io.StringIO()) == 2
    assert main(["--help"], out=io.StringIO(), err=io.StringIO()) == 0
    capsys.readouterr()


def test_timeout_exits_three(tmp_path):
    silent = tmp_path / "silent.py"
    silent.write_text("import sys, time\nfor _ in sys.stdin: pass\n")
    out, err = io.StringIO(), io.StringIO()
    code = main(["--exec", f"{sys.executable} {silent}", "--timeout", "0.3", "run", "version"], out=out, err=err)
    assert code == 3 and "no reply" in err.getvalue()
