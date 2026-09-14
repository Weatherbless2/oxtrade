from __future__ import annotations

from collections import Counter
import socket
import threading
import time
import unittest

from vnpy_oxapp.hpsocket_transport import HpSocketTransport, HpSocketTransportConfig
from vnpy_oxapp.message import ClientRequest, ErrorCode, unix_time_ms
from vnpy_oxapp.pack_message import PackFrame, PackMessageCodec, PackMessageKind, PackMessageStreamDecoder


class LoopbackPackServer:
    """只用于测试的 PackMessage Echo Server，模拟分片回包和一次断线。"""

    def __init__(self, *, close_first_connection: bool = False) -> None:
        self.close_first_connection = close_first_connection
        self.received = []
        self.connection_count = 0
        self.heartbeat_count = 0
        self._stop = threading.Event()
        self._ready = threading.Event()
        self._listener: socket.socket | None = None
        self._clients: list[socket.socket] = []
        self._thread = threading.Thread(target=self._run, name="test-pack-server", daemon=True)

    @property
    def port(self) -> int:
        assert self._listener is not None
        return self._listener.getsockname()[1]

    def start(self) -> None:
        self._thread.start()
        self._ready.wait(1)

    def stop(self) -> None:
        self._stop.set()
        if self._listener is not None:
            self._listener.close()
        for client in list(self._clients):
            try:
                client.close()
            except OSError:
                pass
        self._thread.join(1)

    def _run(self) -> None:
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", 0))
        listener.listen(8)
        listener.settimeout(0.1)
        self._listener = listener
        self._ready.set()
        try:
            while not self._stop.is_set():
                try:
                    client, _ = listener.accept()
                except socket.timeout:
                    continue
                except OSError:
                    return
                self.connection_count += 1
                self._clients.append(client)
                threading.Thread(
                    target=self._serve_client,
                    args=(client, self.connection_count),
                    name="test-pack-client",
                    daemon=True,
                ).start()
        finally:
            listener.close()

    def _serve_client(self, client: socket.socket, connection_number: int) -> None:
        decoder = PackMessageStreamDecoder()
        client.settimeout(0.1)
        try:
            while not self._stop.is_set():
                try:
                    data = client.recv(65536)
                except socket.timeout:
                    continue
                except OSError:
                    return
                if not data:
                    return
                for frame in decoder.feed(data):
                    self.received.append(frame)
                    if frame.kind is PackMessageKind.HEARTBEAT:
                        self.heartbeat_count += 1
                        response = PackFrame(
                            kind=PackMessageKind.HEARTBEAT,
                            request_id=frame.request_id,
                            session_id=frame.session_id,
                            message_type=frame.message_type,
                            function_id=0,
                            sequence=frame.sequence,
                            timestamp_ms=unix_time_ms(),
                        )
                        client.sendall(PackMessageCodec.encode(response))
                        continue
                    if frame.kind is not PackMessageKind.REQUEST:
                        continue
                    response = PackFrame(
                        kind=PackMessageKind.RESPONSE,
                        request_id=frame.request_id,
                        session_id=frame.session_id,
                        message_type=frame.message_type,
                        function_id=frame.function_id,
                        timestamp_ms=unix_time_ms(),
                        payload=PackMessageCodec.encode_json({"data": {"ok": True, "echo": frame.payload.decode("utf-8")}}),
                    )
                    encoded = PackMessageCodec.encode(response)
                    client.sendall(encoded[:17])
                    client.sendall(encoded[17:])
                    if self.close_first_connection and connection_number == 1:
                        client.close()
                        return
                    if connection_number == 1:
                        push = PackFrame(
                            kind=PackMessageKind.PUSH,
                            request_id=0,
                            session_id=frame.session_id,
                            message_type=3,
                            function_id=3,
                            sequence=7,
                            timestamp_ms=unix_time_ms(),
                            payload=PackMessageCodec.encode_json({"data": {"symbol": "600000"}}),
                        )
                        client.sendall(PackMessageCodec.encode(push))
        finally:
            try:
                client.close()
            except OSError:
                pass


class PackMessageCodecTest(unittest.TestCase):
    def test_fixed_vector_and_stream_boundaries_match_protocol(self) -> None:
        payload = b'{"schema_version":1,"client_nonce":"00112233445566778899aabbccddeeff"}'
        frame = PackMessageCodec.encode(
            PackFrame(
                kind=PackMessageKind.REQUEST,
                request_id=1,
                session_id=0,
                message_type=1,
                function_id=0,
                timestamp_ms=404,
                payload=payload,
            )
        )
        self.assertEqual(134, len(frame))
        self.assertEqual("52122af4", frame[56:60].hex())
        decoder = PackMessageStreamDecoder()
        self.assertEqual([], decoder.feed(frame[:11]))
        messages = decoder.feed(frame[11:] + frame)
        self.assertEqual([1, 1], [item.request_id for item in messages])
        self.assertEqual(payload, messages[0].payload)


class HpSocketTransportTest(unittest.TestCase):
    def test_round_trip_split_response_push_and_heartbeat(self) -> None:
        server = LoopbackPackServer()
        server.start()
        transport = HpSocketTransport(
            HpSocketTransportConfig(
                port=server.port,
                heartbeat_interval_seconds=0.05,
                heartbeat_timeout_seconds=0.3,
                reconnect_enabled=False,
            )
        )
        responses = []
        pushes = []
        errors = []
        response_ready = threading.Event()
        push_ready = threading.Event()
        transport.set_response_callback(lambda response: (responses.append(response), response_ready.set()))
        transport.set_push_callback(lambda response: (pushes.append(response), push_ready.set()))
        transport.set_error_callback(lambda code, message: errors.append((code, message)))
        self.assertTrue(transport.connect())
        request = ClientRequest(
            request_id="request-1",
            session_id="session-1",
            msg_type="auth",
            func_id="challenge",
            payload={"client_nonce": "abc"},
            deadline_unix_ms=unix_time_ms() + 2_000,
        )
        self.assertTrue(transport.send(request))
        self.assertTrue(response_ready.wait(1))
        self.assertTrue(push_ready.wait(1))
        time.sleep(0.12)
        self.assertEqual("request-1", responses[0].request_id)
        self.assertEqual("session-1", responses[0].session_id)
        self.assertTrue(responses[0].is_success)
        self.assertEqual("600000", pushes[0].payload["symbol"])
        self.assertGreaterEqual(server.heartbeat_count, 1)
        self.assertEqual([], errors)
        transport.disconnect()
        server.stop()

    def test_unexpected_close_reconnects_and_keeps_transport_usable(self) -> None:
        server = LoopbackPackServer(close_first_connection=True)
        server.start()
        transport = HpSocketTransport(
            HpSocketTransportConfig(
                port=server.port,
                heartbeat_interval_seconds=0.2,
                heartbeat_timeout_seconds=0.8,
                reconnect_initial_delay_seconds=0.02,
                reconnect_max_delay_seconds=0.05,
            )
        )
        states = []
        responses = []
        ready = threading.Event()
        transport.set_state_callback(lambda state, _reason: states.append(state))
        transport.set_response_callback(lambda response: (responses.append(response), ready.set()))
        self.assertTrue(transport.connect())
        first = ClientRequest("first", "1", "auth", "challenge", {}, unix_time_ms() + 2_000)
        self.assertTrue(transport.send(first))
        self.assertTrue(ready.wait(1))
        deadline = time.monotonic() + 1
        while time.monotonic() < deadline and sum(state.value == "connected" for state in states) < 2:
            time.sleep(0.01)
        self.assertGreaterEqual(sum(state.value == "connected" for state in states), 2)
        ready.clear()
        second = ClientRequest("second", "1", "auth", "challenge", {}, unix_time_ms() + 2_000)
        self.assertTrue(transport.send(second))
        self.assertTrue(ready.wait(1))
        self.assertEqual(["first", "second"], [item.request_id for item in responses])
        self.assertIn("reconnecting", [state.value for state in states])
        transport.disconnect()
        server.stop()


if __name__ == "__main__":
    unittest.main()
