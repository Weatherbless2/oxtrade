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
        first = json.dumps(quote_payload()).encode("utf-8") + b"\n"  # 换行是 JSON-lines 唯一帧边界，TCP recv 长度不具备报文语义。
        second = json.dumps(quote_payload(last_price=1463.0, millisec=124)).encode("utf-8") + b"\n"  # 第二条紧跟首包剩余部分，模拟现实中的半包和粘包同时发生。
        feed.consume_bytes(first[:17])  # 故意只交前 17 字节，没有换行时解析器必须保留半包而不能提前回调。
        self.assertEqual([], events)  # 若这里已有事件，说明不完整 JSON 被错误交给 Hub。
        feed.consume_bytes(first[17:] + second)  # 一次补齐第一行并携带完整第二行，循环应连续剥离两帧。
        self.assertEqual(2, len(events))  # 两条均交付说明没有因 TCP 分块方式丢包或合并行情。
        self.assertEqual(1463.0, events[-1].last_price)

    def test_bad_json_and_wrong_event_type_only_increment_metrics(self) -> None:
        feed = PyTdxJsonLineFeed(PyTdxFeedConfig(max_line_bytes=4096))
        feed.consume_bytes(b"not-json\n")  # 故意制造语法坏包，现实中可能来自协议失步或对端异常输出。
        feed.consume_bytes(json.dumps({"type": "subscription_added"}).encode("utf-8") + b"\n")  # 合法 JSON 但不是行情，同样只能丢弃。
        self.assertEqual(2, feed.status.metrics.bad_payloads)  # 坏包应可观测但不能终止接收线程；计数不对可定位到解码/类型闸门。

    def test_subscription_command_keeps_bridge_exchange_names(self) -> None:
        feed = PyTdxJsonLineFeed()
        feed.subscribe_symbols(["000001.SZ", "600519.SH", "IF2609.CFX"])
        self.assertEqual({"000001.SZ", "600519.SH"}, feed._subscriptions)  # 两只股票保留且期货被排除，验证产品边界而非简单数量。

    def test_local_tcp_bridge_receives_subscription_and_delivers_split_lines(self) -> None:
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", 0))  # 让内核分配临时空闲端口，测试不依赖开发机固定 19001 是否被占用。
        listener.listen(1)  # 建立真实本机 TCP 服务，覆盖 create_connection/sendall/recv/close 而不访问外网。
        host, port = listener.getsockname()
        command_lines: list[bytes] = []
        release = Event()

        def serve() -> None:
            connection, _ = listener.accept()  # 测试线程扮演 QuantFabric Bridge，接收 Feed 发来的真实订阅命令。
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
            self.assertTrue(self.wait_until(lambda: len(events) == 2))  # 有限等待后台线程交付，超时失败说明连接、拆帧或回调链路中断。
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
        probe.close()  # 故意释放端口且不 listen，模拟 Bridge 未启动，让 connect 获得真实 ECONNREFUSED。
        feed = PyTdxJsonLineFeed(PyTdxFeedConfig(port=port, connect_timeout_seconds=0.05, reconnect_seconds=0.05))
        feed.start(lambda event: None, lambda status: None)
        try:
            self.assertTrue(self.wait_until(lambda: feed.status.metrics.reconnects >= 1))  # 至少一次带退避重试证明网络失败没有杀死线程。
            self.assertEqual("reconnecting", feed.status.state.value)  # 稳定状态要能被页面展示，而不是把拒绝连接误报为已连接。
        finally:
            feed.stop()
