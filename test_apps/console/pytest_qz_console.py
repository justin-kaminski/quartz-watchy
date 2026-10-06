# Owner-run on a flashed Watchy v3 (HARDWARE_BRINGUP B10): every console command once.
#   QZ_DEVICE=1 [QZ_PORT=/dev/ttyACM0] [QZ_ALLOW_DESTRUCTIVE=1] \
#       .venv/bin/python -m pytest test_apps/console -q
# SKELETON: replies are checked for OK/ERR and for the main fields documented in
# docs/ARCHITECTURE.md section 16 only. [ASSUMED] the argument values below are valid on the
# first image; tighten the field checks as the owner confirms real output. Nothing here counts
# as hardware-tested until the owner has run it.
import pytest
from qzctl.pytest_support import destructive_enabled

# (command, fields that must be present in the OK reply, flags)
SAFE = [
    ("help", ["cmds"], ""),
    ("version", ["fw", "git", "proto"], ""),
    ("status", ["battery"], ""),
    ("tz list Chicago", [], ""),
    ("tz get", ["name"], ""),
    ("tz set America/Chicago", [], ""),
    ("time set 2026-10-06T14:30:00", [], ""),
    ("time get", ["utc", "local"], ""),
    ("time drift", [], ""),
    ("settings list", [], ""),
    ("settings get time_format", [], ""),
    ("btn menu click", ["screen"], ""),
    ("btn back click", ["screen"], ""),
    ("steps get", ["today", "goal"], ""),
    ("steps history", ["history"], ""),
    ("steps inject 10", [], ""),
    ("battery get", ["mv", "pct"], ""),
    ("battery fake 3800", [], ""),
    ("battery fake off", [], ""),
    ("weather get", [], ""),
    ("weather fake 215 1 250 120", [], ""),
    ("weather clear", [], ""),
    ("wifi status", ["ssid"], ""),
    ("sync status", [], ""),
    ("provision stop", [], ""),
    ("display refresh partial", ["crc32"], ""),
    ("display dump", ["b64", "crc32"], ""),
    ("display crc", ["crc32"], ""),
    ("screen list", [], ""),
    ("screen get", [], ""),
    ("face list", [], ""),
    ("log wakes 3", ["wakes"], ""),
    ("diag info", [], ""),
    ("diag power", [], ""),
    ("diag radio", [], ""),
    ("diag rtc", [], ""),
    ("diag nvs", [], ""),
    ("diag clock", [], ""),
    ("diag sensors", [], ""),
    ("selftest list", [], ""),
    ("selftest run all", ["results"], ""),
    ("vibrate 100", [], ""),
]
RADIO = [  # R flag: needs the radio build
    ("weather fetch", [], "radio"),
    ("sync now", [], "radio"),
    ("provision start", ["ssid"], "radio"),
]
DESTRUCTIVE = [  # D flag: erase state; opt-in
    ("steps reset-today", [], "destructive"),
    ("settings reset", [], "destructive"),
    ("wifi clear", [], "destructive"),
    ("log clear", [], "destructive"),
]
# `sleep`, `reboot` and `factory-reset confirm` live in test_apps/wake (they drop the port).
COVERED_ELSEWHERE = ("sleep", "reboot", "factory-reset")
ALL = SAFE + RADIO + DESTRUCTIVE


def _params():
    for command, fields, flag in ALL:
        marks = [getattr(pytest.mark, flag)] if flag else []
        yield pytest.param(command, fields, flag, id=command, marks=marks)


@pytest.mark.esp32s3
@pytest.mark.parametrize("command,fields,flag", _params())
def test_command_once(qz, command, fields, flag):
    if flag == "destructive" and not destructive_enabled():
        pytest.skip("set QZ_ALLOW_DESTRUCTIVE=1")
    if flag == "radio" and not qz.request("version").data.get("build", {}).get("radio"):
        pytest.skip("firmware built without the radio")
    reply = qz.request(command, timeout_s=60.0)
    assert reply.ok, f"{command}: ERR {reply.code} {reply.data}"
    for name in fields:
        assert name in reply.data, f"{command}: reply lacks {name!r}"


@pytest.mark.esp32s3
def test_every_catalog_command_has_a_row(qz):
    """The device's own `help` is the catalog; a command with no row here is untested."""
    catalog = [c["name"] for c in qz.request("help").data["cmds"]]
    rows = [c for c, _, _ in ALL]
    missing = [
        name for name in catalog
        if not name.startswith(COVERED_ELSEWHERE) and not any(r == name or r.startswith(name + " ") for r in rows)
    ]
    assert not missing, f"console commands with no test row: {missing}"


@pytest.mark.esp32s3
def test_unknown_command_and_bad_args_are_errors(qz):
    assert qz.request("no-such-command").code == "unknown_cmd"
    assert qz.request("vibrate 99999").code == "bad_args"
