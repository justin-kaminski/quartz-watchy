"""Exception hierarchy. Everything qzctl raises on purpose derives from QzError."""


class QzError(Exception):
    """Base class for qzctl failures (exit status 3 in the CLI)."""


class ProtocolError(QzError):
    """A line starting with @QZ1 that is not a valid protocol line, or a bad request."""


class TransportClosed(QzError):
    """The link went away (USB port disappeared, subprocess exited)."""


class ConnectTimeout(QzError):
    """The device did not (re)appear in time."""


class RequestTimeout(QzError):
    """The device did not answer / emit the expected event in time."""
