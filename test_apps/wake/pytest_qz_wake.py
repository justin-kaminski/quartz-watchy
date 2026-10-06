# Owner-run on a flashed Watchy v3 (HARDWARE_BRINGUP B10): the USB console must survive deep sleep.
#   QZ_DEVICE=1 [QZ_PORT=/dev/ttyACM0] .venv/bin/python -m pytest test_apps/wake -q
# SKELETON. `sleep <s>` deep-sleeps even while tethered; the CDC port disappears and returns, and a
# fresh `ready` event follows (ARCHITECTURE sections 16-17). [ASSUMED] the new ready event reports
# reset == "deepsleep"; [TUNE] the 20 s re-enumeration budget. If the host opens the port after the
# event was emitted, qzctl falls back to a probe (event is None); then `log wakes` is the evidence.
import time

import pytest


@pytest.mark.esp32s3
def test_sleep_5_reenumerates_and_reports_ready(qz):
    started = time.monotonic()
    result = qz.run_expecting_reset("sleep 5", vanish_timeout_s=10.0, ready_timeout_s=35.0)
    elapsed = time.monotonic() - started
    assert result.response is not None and result.response.ok
    assert elapsed >= 4.0, "the watch came back before the 5 s sleep elapsed"
    if result.ready.event is not None:
        assert result.ready.event.data.get("reset") == "deepsleep"
    else:
        wakes = qz.request("log wakes 1").data["wakes"]
        assert wakes, "no wake record after the sleep"


@pytest.mark.esp32s3
def test_console_works_after_wake_and_repeats(qz):
    for _ in range(2):
        qz.run_expecting_reset("sleep 3", ready_timeout_s=30.0)
        assert qz.request("version").ok
        assert qz.request("status").data.get("battery") is not None


@pytest.mark.esp32s3
def test_reboot_comes_back_with_ready(qz):
    result = qz.run_expecting_reset("reboot", ready_timeout_s=30.0)
    assert result.response is not None and result.response.ok
    assert qz.request("version").ok
