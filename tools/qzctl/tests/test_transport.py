import os
import select
import sys
import threading
import time
from types import SimpleNamespace

import pytest

from fakes import FakeDevice
from qzctl import Client, ConnectTimeout, QzError, TransportClosed
from qzctl.transport import LineBuffer, SerialTransport, SubprocessTransport, find_port

FAKES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fakes.py")


def port(device, vid=0x303A, pid=0x1001, sn=None):
    return SimpleNamespace(device=device, vid=vid, pid=pid, serial_number=sn)


def test_line_buffer_splits_and_strips():
    buf = LineBuffer()
    assert buf.feed(b"ab") == []
    assert buf.feed(b"c\r\nd\n\ne") == ["abc", "d", ""]
    assert buf.feed(b"\xc3\xa9\xff\n") == ["eé�"]  # bad UTF-8 never raises


def test_find_port_by_vid_pid_and_serial():
    ports = [port("/dev/ttyACM1", sn="B"), port("/dev/ttyUSB0", vid=0x10C4, pid=0xEA60), port("/dev/ttyACM0", sn="A")]
    lister = lambda: ports  # noqa: E731
    assert find_port(serial_number="A", lister=lister) == "/dev/ttyACM0"
    assert find_port(serial_number="Z", lister=lister) is None
    assert find_port(vid=0x10C4, pid=0xEA60, lister=lister) == "/dev/ttyUSB0"
    with pytest.raises(QzError, match="ambiguous|devices match"):
        find_port(lister=lister)  # two 303a:1001 devices, no serial given
    assert find_port(lister=lambda: []) is None


def test_find_port_by_path_requires_the_node(tmp_path):
    node = tmp_path / "ttyX"
    assert find_port(str(node)) is None
    node.touch()
    assert find_port(str(node)) == str(node)


def test_serial_open_times_out_when_device_absent():
    t = SerialTransport(lister=lambda: [])
    start = time.monotonic()
    with pytest.raises(ConnectTimeout, match="303a:1001"):
        t.open(0.25)
    assert time.monotonic() - start < 2.0


# -- a real SerialTransport against a pty "device" that unplugs and replugs ------------------

pty = pytest.importorskip("pty")
tty = pytest.importorskip("tty")


class PtyDevice:
    """Serves FakeDevice on a pty and publishes it as a symlink, like /dev/ttyACM0."""

    def __init__(self, link):
        self.link = link
        self.master = self.slave = -1
        self.stop = threading.Event()
        self.thread = None

    def plug(self):
        self.master, self.slave = pty.openpty()
        tty.setraw(self.slave)
        self.stop.clear()
        self.thread = threading.Thread(target=self._serve, args=(self.master, self.stop), daemon=True)
        self.thread.start()
        os.symlink(os.ttyname(self.slave), self.link)

    def unplug(self):
        self.stop.set()
        self.thread.join(2)
        os.unlink(self.link)
        os.close(self.master)
        os.close(self.slave)

    @staticmethod
    def _serve(master, stop):
        dev, buf = FakeDevice(), LineBuffer()
        while not stop.is_set():
            ready, _, _ = select.select([master], [], [], 0.02)
            if not ready:
                continue
            try:
                data = os.read(master, 4096)
            except OSError:
                return
            for line in buf.feed(data):
                for out in dev.handle(line):
                    os.write(master, out.encode() + b"\n")


def test_serial_transport_request_and_reconnect_over_pty(tmp_path):
    link = str(tmp_path / "ttyFAKE")
    dev = PtyDevice(link)
    dev.plug()
    client = Client(SerialTransport(link), request_timeout_s=2.0, reconnect_timeout_s=5.0, probe_interval_s=0.2)
    try:
        client.connect()
        assert client.request("version").data["proto"] == 1
        assert client.request("noisy").data == {"n": 1}
        assert len(client.logs) >= 2

        dev.unplug()  # the port disappears (deep sleep) ...
        replug = threading.Timer(0.4, dev.plug)  # ... and re-enumerates 0.4 s later
        replug.start()
        try:
            assert client.request("status").data["valid"] is True
        finally:
            replug.join()
        assert client.retries == 1
    finally:
        client.close()
        dev.unplug()


def test_serial_write_after_disappearance_raises_transport_closed(tmp_path):
    link = str(tmp_path / "ttyFAKE")
    dev = PtyDevice(link)
    dev.plug()
    t = SerialTransport(link)
    t.open(1.0)
    dev.unplug()
    with pytest.raises(TransportClosed):
        for _ in range(50):  # EIO may take a moment to surface
            t.write_line("status")
            t.read_line(0.05)
    t.close()


# -- subprocess transport --------------------------------------------------------------------

def test_subprocess_transport_round_trip_and_restart():
    t = SubprocessTransport([sys.executable, FAKES])
    client = Client(t, request_timeout_s=5.0, reconnect_timeout_s=5.0)
    try:
        client.connect()
        assert client.wait_ready(5.0, probe=False).event.name == "ready"
        assert client.request("version").ok
        result = client.run_expecting_reset("reboot")  # child exits, reconnect respawns it
        assert result.response.ok and result.ready.event.name == "ready"
        assert client.request("status").ok
    finally:
        client.close()


def test_subprocess_missing_program_is_a_connect_error():
    with pytest.raises(ConnectTimeout):
        SubprocessTransport(["/nonexistent/qz-sim"]).open(1.0)


def test_subprocess_read_after_exit_raises_and_keeps_raising():
    t = SubprocessTransport([sys.executable, "-c", "print('bye')"])
    t.open(1.0)
    assert t.read_line(5.0) == "bye"
    for _ in range(2):
        with pytest.raises(TransportClosed):
            t.read_line(5.0)
    t.close()
