from __future__ import annotations

import json
import socket
from threading import Event, Thread
from time import monotonic, sleep
import unittest

from vnpy_oxapp.market.pytdx_feed import PyTdxFeedConfig, PyTdxJsonLineFeed
from tests.python.test_market_event import quote_payload


class PyTdxJsonLineFeedTest(unittest.TestCase):
    def wait_until(self, predicate, timeout: float = 2.0) -> bool:
        deadline = monotonic() + timeout
        while monotonic() < deadline:
            if predicate():
                return True
            sleep(0.01)
        return predicate()

    def test_tcp_half_packet_and_multiple_lines_are_framed_without_losing_quotes(self) -> None:
        feed = PyTdxJsonLineFeed(PyTdxFeedConfig(max_line_bytes=4096))
        events = []
        feed._event_callback = events.append  # 不启动真实网络，只向协议边界注入已分片的 TCP 字节流。
        first = json.dumps(quote_payload()).encode("utf-8") + b"\n"
        second = json.dumps(quote_payload(last_price=1463.0, millisec=124)).encode("utf-8") + b"\n"
        feed.consume_bytes(first[:17])
        self.assertEqual([], events)
        feed.consume_bytes(first[17:] + second)
        self.assertEqual(2, len(events))
        self.assertEqual(1463.0, events[-1].last_price)

    def test_bad_json_and_wrong_event_type_only_increment_metrics(self) -> None:
        feed = PyTdxJsonLineFeed(PyTdxFeedConfig(max_line_bytes=4096))
        feed.consume_bytes(b"not-json\n")
        feed.consume_bytes(json.dumps({"type": "subscription_added"}).encode("utf-8") + b"\n")
        self.assertEqual(2, feed.status.metrics.bad_payloads)

    def test_subscription_command_keeps_bridge_exchange_names(self) -> None:
        feed = PyTdxJsonLineFeed()
        feed.subscribe_symbols(["000001.SZ", "600519.SH", "IF2609.CFX"])
        self.assertEqual({"000001.SZ", "600519.SH"}, feed._subscriptions)

    def test_local_tcp_bridge_receives_subscription_and_delivers_split_lines(self) -> None:
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        host, port = listener.getsockname()
        command_lines: list[bytes] = []
        release = Event()

        def serve() -> None:
            connection, _ = listener.accept()
            with connection:
                connection.settimeout(0.1)
                pending = b""
                while b"\n" not in pending:
                    try:
                        pending += connection.recv(4096)
                    except socket.timeout:
                        continue
                command_lines.extend(line for line in pending.splitlines() if line)
                first = json.dumps(quote_payload()).encode("utf-8") + b"\n"
                second = json.dumps(quote_payload(last_price=1463.0, millisec=124)).encode("utf-8") + b"\n"
                connection.sendall(first[:23])
                connection.sendall(first[23:] + second)  # 同一次 recv 中既补齐首行又附带第二行，验证拆包/粘包。
                release.wait(2.0)
            listener.close()

        server = Thread(target=serve, name="pytdx-test-bridge", daemon=True)
        server.start()
        events = []
        feed = PyTdxJsonLineFeed(PyTdxFeedConfig(host=host, port=port, connect_timeout_seconds=0.2, reconnect_seconds=0.05))
        feed.subscribe_symbols(["600519.SH"])
        feed.start(events.append, lambda status: None)
        try:
            self.assertTrue(self.wait_until(lambda: len(events) == 2))
            self.assertEqual(1463.0, events[-1].last_price)
            self.assertEqual({"type": "subscribe", "ticker": "600519", "exchange": "SSE"}, json.loads(command_lines[0]))
        finally:
            release.set()
            feed.stop()
            server.join(timeout=1.0)

    def test_connection_refusal_enters_reconnect_state_without_busy_loop(self) -> None:
        probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        probe.bind(("127.0.0.1", 0))
        _, port = probe.getsockname()
        probe.close()  # 释放端口但不监听，下一次连接会立即被拒绝。
        feed = PyTdxJsonLineFeed(PyTdxFeedConfig(port=port, connect_timeout_seconds=0.05, reconnect_seconds=0.05))
        feed.start(lambda event: None, lambda status: None)
        try:
            self.assertTrue(self.wait_until(lambda: feed.status.metrics.reconnects >= 1))
            self.assertEqual("reconnecting", feed.status.state.value)
        finally:
            feed.stop()
