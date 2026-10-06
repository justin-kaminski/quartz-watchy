import pytest

from qzctl.errors import ProtocolError
from qzctl.protocol import (MAX_REQUEST_BYTES, Event, Response, build_request, join_command,
                            parse_line, quote_arg)

PARSE_OK = [
    ('@QZ1 7 OK {"a":1}', Response("7", True, None, {"a": 1})),
    ("@QZ1 abc12345 OK {}", Response("abc12345", True, None, {})),
    ("@QZ1 - OK {}", Response(None, True, None, {})),
    ('@QZ1 3 ERR bad_args {"msg":"nope"}', Response("3", False, "bad_args", {"msg": "nope"})),
    ('@QZ1 - ERR unknown_cmd {"msg":"x y z"}', Response(None, False, "unknown_cmd", {"msg": "x y z"})),
    ('@QZ1 1 OK {"s":"has OK and spaces","n":null}', Response("1", True, None, {"s": "has OK and spaces", "n": None})),
    ('@QZ1 ! EVT {"evt":"ready","proto":1}', Event({"evt": "ready", "proto": 1})),
    ('@QZ1 ! EVT {"evt":"detach"}\r\n', Event({"evt": "detach"})),
    ('@QZ1 2 OK {"u":"café"}\r', Response("2", True, None, {"u": "café"})),
]

LOG_LINES = [
    "",
    "I (123) qz: boot",
    " @QZ1 1 OK {}",  # leading space: not a protocol line
    "W (9) x: saw @QZ1 1 OK {} inside",
    "@qz1 1 OK {}",
    "\x1b[0;32mI (5) color\x1b[0m",
]

PARSE_BAD = [
    "@QZ1",
    "@QZ1x 1 OK {}",
    "@QZ1 1 OK",
    "@QZ1 1 OK not-json",
    "@QZ1 1 OK [1,2]",
    "@QZ1 1 OK 5",
    "@QZ1 toolongid1 OK {}",
    "@QZ1 a-b OK {}",
    "@QZ1 ! OK {}",
    "@QZ1 1 EVT {}",
    "@QZ1 ! EVT {",
    "@QZ1 1 ERR {}",
    "@QZ1 1 ERR BAD {}",
    "@QZ1 1 MAYBE {}",
]


@pytest.mark.parametrize("line,expected", PARSE_OK)
def test_parse_valid(line, expected):
    assert parse_line(line) == expected


@pytest.mark.parametrize("line", LOG_LINES)
def test_log_lines_are_ignored(line):
    assert parse_line(line) is None


@pytest.mark.parametrize("line", PARSE_BAD)
def test_parse_malformed(line):
    with pytest.raises(ProtocolError):
        parse_line(line)


def test_event_name():
    assert Event({"evt": "ready"}).name == "ready"
    assert Event({"evt": 5}).name is None
    assert Event({}).name is None


@pytest.mark.parametrize("arg,quoted", [
    ("abc", "abc"), ("", '""'), ("a b", '"a b"'), ('say "hi"', '"say \\"hi\\""'),
    ("back\\slash", '"back\\\\slash"'), ("tab\tx", '"tab\tx"'),
])
def test_quote_arg(arg, quoted):
    assert quote_arg(arg) == quoted


def test_join_command():
    assert join_command(["wifi", "set", "My Net", "p\"w"]) == 'wifi set "My Net" "p\\"w"'


def test_build_request():
    assert build_request("status", "7") == "#7 status"
    assert build_request("status", None) == "status"


@pytest.mark.parametrize("command,rid", [
    ("", "1"), ("  ", "1"), ("a\nb", "1"), ("a\rb", "1"), ("status", "toolongid1"), ("status", "a b"), ("status", ""),
    ("x" * MAX_REQUEST_BYTES, "1"),
])
def test_build_request_rejects(command, rid):
    with pytest.raises(ProtocolError):
        build_request(command, rid)


def test_build_request_length_limit_is_on_the_whole_line_in_bytes():
    exact = "x" * (MAX_REQUEST_BYTES - len("#1 "))
    assert build_request(exact, "1").endswith(exact)
    with pytest.raises(ProtocolError):
        build_request("é" * (MAX_REQUEST_BYTES // 2), "1")  # 2 bytes per char
