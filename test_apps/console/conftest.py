# Owner-run (HARDWARE_BRINGUP B10). Skeleton: see tools/qzctl/README.md "Device suites".
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))

from qzctl.pytest_support import client_from_env, device_enabled  # noqa: E402


def pytest_configure(config):
    config.addinivalue_line("markers", "esp32s3: needs a Watchy v3 attached over USB")
    config.addinivalue_line("markers", "destructive: erases watch state; needs QZ_ALLOW_DESTRUCTIVE=1")
    config.addinivalue_line("markers", "radio: needs the radio build (version.build.radio)")


@pytest.fixture(scope="session")
def qz():
    if not device_enabled():
        pytest.skip("device suite: set QZ_DEVICE=1 with a flashed, USB-attached watch")
    client = client_from_env()
    client.wait_ready(30.0)  # the watch is tethered and awake, or just woke
    yield client
    client.close()
