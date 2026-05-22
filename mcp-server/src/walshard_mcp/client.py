"""Thin TCP client for the Walshard Studio InspectServer line protocol.

The editor listens on 127.0.0.1:9876 by default (override with
``OCS_INSPECT_PORT``). The protocol is one line per command, one or
more lines per reply, terminated by a sentinel blank line for
multi-line replies, or a single line for the common case.

This client opens a fresh connection per call. Cheap because the
editor is local; simple because we don't have to manage a long-lived
socket lifecycle from the MCP tool layer.
"""

from __future__ import annotations

import os
import socket
import time
from dataclasses import dataclass


@dataclass(frozen=True)
class WalshardClientConfig:
    host: str = "127.0.0.1"
    port: int = 9876
    timeout_s: float = 10.0
    token: str | None = None  # OCS_INSPECT_TOKEN passthrough

    @classmethod
    def from_env(cls) -> "WalshardClientConfig":
        return cls(
            host=os.environ.get("WALSHARD_HOST", "127.0.0.1"),
            port=int(os.environ.get("OCS_INSPECT_PORT",
                                    os.environ.get("WALSHARD_PORT", "9876"))),
            timeout_s=float(os.environ.get("WALSHARD_TIMEOUT_S", "10")),
            token=os.environ.get("OCS_INSPECT_TOKEN") or
                  os.environ.get("WALSHARD_TOKEN"),
        )


class WalshardError(RuntimeError):
    """Raised when the editor returns an ERR reply or the socket fails."""


class WalshardClient:
    """Single-call TCP client. Each ``send`` opens its own connection."""

    def __init__(self, config: WalshardClientConfig | None = None) -> None:
        self._cfg = config or WalshardClientConfig.from_env()

    def send(self, command: str) -> str:
        """Send one command, return the full reply (whitespace-trimmed).

        The editor uses two reply shapes:
          * single line: "ok ..." or "ERR ..." terminated by '\\n'
          * multi-line:  blank line marks end-of-reply

        We read until either pattern is satisfied. Lines starting with
        "ERR " raise WalshardError.
        """
        if "\n" in command:
            raise ValueError("command must not contain embedded newlines")

        sock = socket.create_connection((self._cfg.host, self._cfg.port),
                                        timeout=self._cfg.timeout_s)
        try:
            sock.settimeout(self._cfg.timeout_s)
            # Auth handshake — best-effort. If the editor doesn't
            # require auth, it returns "ok no-auth"; if it does and
            # we have no token, the command itself will be rejected.
            if self._cfg.token:
                sock.sendall(f"auth {self._cfg.token}\n".encode("utf-8"))
                self._read_one_line(sock)  # consume the ack
            sock.sendall((command + "\n").encode("utf-8"))
            reply = self._read_reply(sock)
        finally:
            try: sock.close()
            except Exception: pass

        if reply.startswith("ERR "):
            raise WalshardError(reply[4:].strip())
        return reply

    # --- internal helpers -------------------------------------------------

    @staticmethod
    def _read_one_line(sock: socket.socket) -> str:
        buf = bytearray()
        while b"\n" not in buf:
            chunk = sock.recv(4096)
            if not chunk:
                break
            buf += chunk
        line, _, _ = buf.partition(b"\n")
        return line.decode("utf-8", errors="replace")

    @staticmethod
    def _read_reply(sock: socket.socket) -> str:
        """Read until either:
          * a single non-empty line followed by EOF or sentinel,
          * a multi-line reply terminated by a blank line.
        Bounded to ~64 KB to avoid pathological replies.
        """
        buf = bytearray()
        deadline = time.monotonic() + 30.0  # hard ceiling
        while True:
            try:
                chunk = sock.recv(8192)
            except socket.timeout:
                break
            if not chunk:
                break
            buf += chunk
            if len(buf) > 64 * 1024:
                break
            # Multi-line sentinel: blank line after content.
            if buf.endswith(b"\n\n") or buf.endswith(b"\r\n\r\n"):
                break
            # Single-line: one newline + reasonable settle delay so
            # we don't truncate a deferred multi-line follow-up.
            if buf.count(b"\n") >= 1 and not _looks_continuable(buf):
                break
            if time.monotonic() > deadline:
                break
        return buf.decode("utf-8", errors="replace").rstrip("\r\n")


def _looks_continuable(buf: bytes) -> bool:
    """Heuristic: a reply that begins with 'ok ' or 'ERR ' and contains
    only one newline is almost certainly complete. Multi-line replies
    typically end with a sentinel blank line, which the recv loop
    catches separately."""
    head, _, _ = buf.partition(b"\n")
    s = head.decode("utf-8", errors="replace").strip()
    if s.startswith("ok ") or s == "ok" or s.startswith("ERR "):
        return False
    return True
