import time
from collections import deque

import pytest

from fakes import READY, FakeDevice, FakeLink
from qzctl import Client, ConnectTimeout, RequestTimeout, TransportClosed
from qzctl.transport import Transport


class Scripted(Transport):
    """Replays canned lines regardless of what is written."""

    def __init__(self, lines):
        self.lines, self.written = deque(lines), []

    def open(self, timeout_s):
        pass

    def close(self):
        pass

    def read_line(self, timeout_s):
        if self.lines:
            return self.lines.popleft()
        time.sleep(min(timeout_s, 0.01))
        return None

    def write_line(self, line):
        self.written.append(line)


def make(link, **kw):
    kw.setdefault("request_timeout_s", 1.0)
    kw.setdefault("reconnect_timeout_s", 2.0)
    kw.setdefault("probe_interval_s", 0.05)
    client = Client(link, **kw)
    client.connect()
    return client


def test_request_gets_ok_reply_with_fresh_ids():
    link = FakeLink()
    client = make(link)
    assert client.request("version").data["proto"] == 1
    client.request("status")
    assert link.device.received == ["#1 version", "#2 status"]


def test_err_reply_is_returned_not_raised():
    reply = make(FakeLink()).request("bogus")
    assert not reply.ok and reply.code == "unknown_cmd" and reply.data == {"msg": "bogus"}


def test_log_lines_mixed_into_the_stream_are_skipped_and_kept():
    client = make(FakeLink())
    assert client.request("noisy").data == {"n": 1}
    assert list(client.logs) == ["I (1) a: working", "W (2) b: saw @QZ1 in the middle"]
    assert not client.malformed


def test_replies_are_matched_by_id_and_stale_ones_recorded():
    link = Scripted([
        "@QZ1 99 OK {}",  # reply to some earlier request
        '@QZ1 ! EVT {"evt":"wake"}',
        "@QZ1 garbage",  # malformed protocol line is skipped
        "I (1) log",
        '@QZ1 - ERR bad_args {"msg":"x"}',  # id-less error is not ours either
        '@QZ1 1 OK {"a":1}',
    ])
    client = make(link)
    reply = client.request("status")
    assert reply.id == "1" and reply.data == {"a": 1}
    assert [r.id for r in client.stale] == ["99", None]
    assert [e.name for e in client.events] == ["wake"]
    assert client.malformed == ["@QZ1 garbage"]
    assert link.written == ["#1 status"]


def test_events_callback_and_log_callback():
    seen_events, seen_logs = [], []
    client = make(FakeLink(), on_event=seen_events.append, on_log=seen_logs.append)
    client.request("noisy")
    assert len(seen_logs) == 2 and seen_events == []


def test_request_timeout():
    link = FakeLink()
    client = make(link)
    link.mute = True
    start = time.monotonic()
    with pytest.raises(RequestTimeout, match="status"):
        client.request("status", timeout_s=0.1)
    assert time.monotonic() - start < 1.0


def test_reconnect_after_port_disappears_mid_request_retries_same_id():
    link = FakeLink(drop_before_reply=True, reappear_after_s=0.1)
    client = make(link)
    reply = client.request("version")
    assert reply.ok
    assert link.device.received == ["#1 version", "#1 version"]  # same id re-sent once
    assert link.open_calls == 2 and client.retries == 1
    assert [e.name for e in client.events] == ["ready"]


def test_reconnect_without_ready_event_falls_back_to_probe():
    link = FakeLink(drop_before_reply=True, emit_ready=False)
    client = make(link)
    assert client.request("version").ok
    assert link.device.received == ["#1 version", "#2 version", "#1 version"]  # probe then retry


def test_no_retry_raises_transport_closed():
    link = FakeLink(drop_before_reply=True)
    client = make(link)
    with pytest.raises(TransportClosed):
        client.request("version", retry=False)
    assert link.open_calls == 1


def test_connect_timeout_when_port_never_returns():
    link = FakeLink(drop_before_reply=True, never_reappear=True)
    client = make(link, reconnect_timeout_s=0.2)
    start = time.monotonic()
    with pytest.raises(ConnectTimeout):
        client.request("version")
    assert time.monotonic() - start < 1.5


def test_wait_ready_sees_event_emitted_on_connect():
    link = FakeLink(drop_before_reply=True)
    client = make(link)
    link.write_line("x")  # vanish; the port returns with a ready event after reconnect
    client.reconnect()
    result = client.wait_ready(1.0, probe=False)
    assert result.event is not None and result.event.data["reset"] == "deepsleep"


def test_wait_ready_strict_times_out_without_event():
    client = make(FakeLink())  # watch already running: no event is coming
    with pytest.raises(RequestTimeout, match="ready"):
        client.wait_ready(0.2, probe=False)


def test_wait_ready_probe_accepts_a_live_watch():
    assert make(FakeLink()).wait_ready(1.0).event is None


def test_wait_ready_times_out_when_nothing_answers():
    link = FakeLink()
    client = make(link)
    link.mute = True
    with pytest.raises(RequestTimeout):
        client.wait_ready(0.3)


def test_sleep_cycle_rides_out_reenumeration_and_waits_for_ready():
    link = FakeLink(reappear_after_s=0.15)
    client = make(link)
    result = client.run_expecting_reset("sleep 5")
    assert result.response is not None and result.response.ok
    assert result.ready.event is not None and result.ready.event.name == "ready"
    assert link.open_calls == 2
    assert client.request("version").ok  # usable again afterwards


def test_sleep_cycle_with_missed_ready_event_reports_probe():
    link = FakeLink(emit_ready=False)
    client = make(link)
    assert client.run_expecting_reset("reboot").ready.event is None


def test_sleep_cycle_fails_when_port_never_comes_back():
    client = make(FakeLink(never_reappear=True), reconnect_timeout_s=0.2)
    with pytest.raises(ConnectTimeout):
        client.run_expecting_reset("sleep 5")


def test_reset_command_without_drop_times_out():
    client = make(FakeLink())
    with pytest.raises(RequestTimeout, match="did not drop"):
        client.run_expecting_reset("status", vanish_timeout_s=0.1)


def test_ready_on_same_link_counts_when_port_does_not_drop():
    link = Scripted(['@QZ1 1 OK {}', READY])
    result = make(link).run_expecting_reset("reboot", vanish_timeout_s=0.5)
    assert result.ready.event is not None


def test_ids_wrap_within_eight_characters():
    link = FakeLink()
    client = make(link)
    client._counter = 0xFFFFFF
    client.request("status")
    assert link.device.received == ["#1 status"]
