"""正式 C++ HP-Socket Server 的 Python/Qt 客户端传输实现。

该模块使用 Python 标准库 TCP Socket 连接 C++ HP-Socket Server；HP-Socket
只存在于服务端，客户端不依赖未确认的 Python HP-Socket 封装。网络线程只
解码并发布事件，Qt 控件更新仍由 ``AsyncQtBridge`` 完成。
"""

from __future__ import annotations

from dataclasses import dataclass
from hashlib import blake2b
import json
from threading import Event, Lock, RLock, Thread, current_thread
import socket
import time
from typing import Any, Callable

from .message import ClientRequest, ClientResponse, ErrorCode, MessageStatus, unix_time_ms
from .pack_message import PackFrame, PackMessageCodec, PackMessageKind, PackMessageStreamDecoder, PackProtocolError
from .transport import ConnectionState, ErrorCallback, IClientTransport, PushCallback, ResponseCallback, StateCallback


@dataclass(frozen=True, slots=True)
class HpSocketTransportConfig:
    """连接、心跳、重连和帧大小均显式配置，避免生产环境依赖隐含默认值。"""

    host: str = "127.0.0.1"
    port: int = 18080
    connect_timeout_seconds: float = 3.0
    read_timeout_seconds: float = 0.5
    heartbeat_interval_seconds: float = 15.0
    heartbeat_timeout_seconds: float = 60.0
    reconnect_enabled: bool = True
    reconnect_initial_delay_seconds: float = 0.5
    reconnect_max_delay_seconds: float = 5.0
    max_frame_bytes: int = PackMessageCodec.MAX_FRAME_BYTES

    def validate(self) -> None:
        if not self.host or not 1 <= self.port <= 65535:
            raise ValueError("host and port are invalid")
        if self.connect_timeout_seconds <= 0 or self.read_timeout_seconds <= 0:
            raise ValueError("socket timeouts must be positive")
        if self.heartbeat_interval_seconds <= 0 or self.heartbeat_timeout_seconds <= 0:
            raise ValueError("heartbeat intervals must be positive")
        if self.heartbeat_timeout_seconds <= self.heartbeat_interval_seconds:
            raise ValueError("heartbeat timeout must exceed heartbeat interval")
        if self.reconnect_initial_delay_seconds <= 0 or self.reconnect_max_delay_seconds < self.reconnect_initial_delay_seconds:
            raise ValueError("reconnect delays are invalid")
        if not PackMessageCodec.HEADER_LENGTH <= self.max_frame_bytes <= PackMessageCodec.MAX_FRAME_BYTES:
            raise ValueError("max_frame_bytes must be between 64 and 4 MiB")


# 这里只登记已经由 OX-P0-02 冻结的业务函数；未确认的旧 Mock 名称不会被冒充成正式协议。
_OPERATION_CODES: dict[tuple[str, str], tuple[int, int]] = {
    ("auth", "challenge"): (1, 0),
    ("auth", "login"): (1, 1),
    ("auth", "logout"): (1, 2),
    ("auth", "refresh"): (1, 3),
    ("instrument", "search"): (2, 1),
    ("calendar", "page"): (2, 2),
    ("quote", "latest"): (2, 3),
    ("quote", "subscribe"): (3, 1),
    ("quote", "unsubscribe"): (3, 2),
    ("quote", "push"): (3, 3),
    ("system", "health"): (4, 1),
    ("system", "capabilities"): (4, 2),
    ("bar", "page"): (5, 1),
    ("tick", "page"): (5, 2),
    ("depth", "latest"): (5, 3),
}
_OPERATION_NAMES = {value: key for key, value in _OPERATION_CODES.items()}
_MASK_64 = (1 << 64) - 1


def _stable_u64(value: str, *, namespace: str) -> int:
    digest = blake2b(f"{namespace}:{value}".encode("utf-8"), digest_size=8).digest()
    result = int.from_bytes(digest, "big")
    return result or 1


def _wire_identifier(value: str, *, namespace: str) -> int:
    text = str(value).strip()
    if text.isdecimal():
        number = int(text)
        if 0 <= number <= _MASK_64:
            return number
    return _stable_u64(text, namespace=namespace)


class HpSocketTransport(IClientTransport):
    """线程安全的正式 PackMessage Transport。

    ``connect`` 只负责建立 TCP 和后台线程；``send`` 只组帧并写入 Socket，
    不等待业务响应。响应与 Push 在 reader 线程解码后交给上层回调。
    """

    def __init__(self, config: HpSocketTransportConfig | None = None) -> None:
        self.config = config or HpSocketTransportConfig()
        self.config.validate()
        self._state = ConnectionState.DISCONNECTED
        self._lock = RLock()
        self._send_lock = Lock()  # 多个页面线程不能交叉写入同一 TCP 字节流。
        self._connect_lock = Lock()  # connect/disconnect/reconnect 的 Socket 所有权串行化。
        self._socket: socket.socket | None = None
        self._reader_thread: Thread | None = None
        self._heartbeat_thread: Thread | None = None
        self._reconnect_thread: Thread | None = None
        self._stop_event = Event()
        self._manual_disconnect = True
        self._decoder = PackMessageStreamDecoder(self.config.max_frame_bytes)
        self._last_receive_monotonic = time.monotonic()
        self._heartbeat_session_wire_id = 0
        self._pending: dict[int, tuple[ClientRequest, int]] = {}
        self._session_wire_ids: dict[str, int] = {}
        self._response_callback: ResponseCallback | None = None
        self._push_callback: PushCallback | None = None
        self._state_callback: StateCallback | None = None
        self._error_callback: ErrorCallback | None = None

    @property
    def state(self) -> ConnectionState:
        with self._lock:
            return self._state

    def set_response_callback(self, callback: ResponseCallback) -> None:
        self._response_callback = callback

    def set_push_callback(self, callback: PushCallback) -> None:
        self._push_callback = callback

    def set_state_callback(self, callback: StateCallback) -> None:
        self._state_callback = callback

    def set_error_callback(self, callback: ErrorCallback) -> None:
        self._error_callback = callback

    def connect(self) -> bool:
        with self._connect_lock:
            with self._lock:
                if self._state is ConnectionState.CONNECTED and self._socket is not None:
                    return True
                self._manual_disconnect = False
                self._stop_event.clear()
            self._set_state(ConnectionState.CONNECTING, "connecting to HP-Socket server")
            try:
                sock = self._open_socket()
            except OSError as error:
                self._manual_disconnect = True
                self._stop_event.set()
                reason = f"HP-Socket connect failed: {error}"
                self._set_state(ConnectionState.FAILED, reason)
                self._emit_error(ErrorCode.CONNECTION_FAILED, reason)
                return False
            self._install_socket(sock)
            self._start_reader(sock)
            self._start_heartbeat()
            self._set_state(ConnectionState.CONNECTED, "HP-Socket connected")
            return True

    def disconnect(self) -> None:
        with self._connect_lock:
            with self._lock:
                self._manual_disconnect = True
                self._stop_event.set()
                sock = self._socket
                self._socket = None
                reader = self._reader_thread
                heartbeat = self._heartbeat_thread
                reconnect = self._reconnect_thread
            self._close_socket(sock)
            for thread in (reader, heartbeat, reconnect):
                if thread is not None and thread is not current_thread():
                    thread.join(timeout=self.config.connect_timeout_seconds)
            with self._lock:
                self._reader_thread = None
                self._heartbeat_thread = None
                self._reconnect_thread = None
                self._decoder.clear()
                self._session_wire_ids.clear()
                self._heartbeat_session_wire_id = 0
            self._set_state(ConnectionState.DISCONNECTED, "HP-Socket disconnected")

    def send(self, request: ClientRequest) -> bool:
        try:
            message_type, function_id = self._operation_codes(request.msg_type, request.func_id)
            request_wire_id = self._request_wire_id(request.request_id)
            session_wire_id = self._session_wire_id(request.session_id)
            if (request.msg_type, request.func_id) in (("auth", "challenge"), ("auth", "login")):
                session_wire_id = 0  # 匿名认证请求固定使用 session_id=0，避免沿用页面的临时 Mock 会话。
            payload = dict(request.payload)
            payload.setdefault("schema_version", 1)
            payload_bytes = PackMessageCodec.encode_json(payload)
            frame = PackFrame(
                kind=PackMessageKind.REQUEST,
                request_id=request_wire_id,
                session_id=session_wire_id,
                message_type=message_type,
                function_id=function_id,
                timestamp_ms=unix_time_ms(),
                payload=payload_bytes,
            )
        except (PackProtocolError, TypeError, ValueError) as error:
            self._emit_error(ErrorCode.PROTOCOL_ERROR, f"request cannot be encoded: {error}")
            return False

        with self._lock:
            if self._state is not ConnectionState.CONNECTED or self._socket is None:
                self._emit_error(ErrorCode.DISCONNECTED, "HP-Socket is not connected")
                return False
            if request_wire_id in self._pending:
                self._emit_error(ErrorCode.PROTOCOL_ERROR, "request_id is already pending")
                return False
            self._pending[request_wire_id] = (request, session_wire_id)  # 先登记再发送，避免极快回包找不到原请求。
            self._heartbeat_session_wire_id = session_wire_id
        if not self._send_frame(frame):
            with self._lock:
                self._pending.pop(request_wire_id, None)
            return False
        return True

    def _open_socket(self) -> socket.socket:
        sock = socket.create_connection(
            (self.config.host, self.config.port),
            timeout=self.config.connect_timeout_seconds,
        )
        sock.settimeout(self.config.read_timeout_seconds)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        return sock

    def _install_socket(self, sock: socket.socket) -> None:
        with self._lock:
            old = self._socket
            self._socket = sock
            self._decoder.clear()
            self._last_receive_monotonic = time.monotonic()
        self._close_socket(old)

    def _start_reader(self, sock: socket.socket) -> None:
        reader = Thread(target=self._reader_loop, args=(sock,), name="oxapp-hpsocket-reader", daemon=True)
        with self._lock:
            self._reader_thread = reader
        reader.start()

    def _start_heartbeat(self) -> None:
        with self._lock:
            if self._heartbeat_thread is not None and self._heartbeat_thread.is_alive():
                return
            heartbeat = Thread(target=self._heartbeat_loop, name="oxapp-hpsocket-heartbeat", daemon=True)
            self._heartbeat_thread = heartbeat
        heartbeat.start()

    def _reader_loop(self, sock: socket.socket) -> None:
        while not self._stop_event.is_set():
            try:
                data = sock.recv(64 * 1024)
            except socket.timeout:
                continue
            except OSError as error:
                self._handle_disconnect(sock, f"HP-Socket receive failed: {error}")
                return
            if not data:
                self._handle_disconnect(sock, "HP-Socket server closed the connection")
                return
            with self._lock:
                self._last_receive_monotonic = time.monotonic()
            try:
                frames = self._decoder.feed(data)
            except PackProtocolError as error:
                self._emit_error(ErrorCode.PROTOCOL_ERROR, f"invalid PackMessage from server: {error}")
                self._handle_disconnect(sock, f"invalid PackMessage from server: {error}")
                return
            for frame in frames:
                self._dispatch_frame(frame)

    def _heartbeat_loop(self) -> None:
        while not self._stop_event.wait(self.config.heartbeat_interval_seconds):
            with self._lock:
                state = self._state
                sock = self._socket
                last_receive = self._last_receive_monotonic
            if state is not ConnectionState.CONNECTED or sock is None:
                continue
            if time.monotonic() - last_receive >= self.config.heartbeat_timeout_seconds:
                self._handle_disconnect(sock, "HP-Socket heartbeat timeout")
                continue
            heartbeat = PackFrame(
                kind=PackMessageKind.HEARTBEAT,
                request_id=0,
                session_id=self._heartbeat_session_wire_id,
                message_type=0,
                function_id=0,
                timestamp_ms=unix_time_ms(),
            )
            self._send_frame(heartbeat)

    def _send_frame(self, frame: PackFrame) -> bool:
        try:
            encoded = PackMessageCodec.encode(frame)
        except PackProtocolError as error:
            self._emit_error(ErrorCode.PROTOCOL_ERROR, f"frame cannot be encoded: {error}")
            return False
        with self._send_lock:
            with self._lock:
                sock = self._socket
                connected = self._state is ConnectionState.CONNECTED and sock is not None
            if not connected or sock is None:
                self._emit_error(ErrorCode.DISCONNECTED, "HP-Socket send rejected while disconnected")
                return False
            try:
                sock.sendall(encoded)  # sendall 只保证字节写入 Socket，不等待业务 Response。
                return True
            except OSError as error:
                self._handle_disconnect(sock, f"HP-Socket send failed: {error}")
                return False

    def _handle_disconnect(self, sock: socket.socket, reason: str) -> None:
        with self._lock:
            if self._socket is not sock:
                return
            self._socket = None
            self._decoder.clear()
            self._session_wire_ids.clear()  # 断线后的新连接不得复用旧连接的 Session 映射。
            self._heartbeat_session_wire_id = 0
            should_reconnect = (
                not self._manual_disconnect
                and not self._stop_event.is_set()
                and self.config.reconnect_enabled
            )
        self._close_socket(sock)
        self._set_state(ConnectionState.RECONNECTING if should_reconnect else ConnectionState.DISCONNECTED, reason)
        self._emit_error(ErrorCode.DISCONNECTED, reason)
        if should_reconnect:
            self._start_reconnect()

    def _start_reconnect(self) -> None:
        with self._lock:
            if self._reconnect_thread is not None and self._reconnect_thread.is_alive():
                return
            reconnect = Thread(target=self._reconnect_loop, name="oxapp-hpsocket-reconnect", daemon=True)
            self._reconnect_thread = reconnect
        reconnect.start()

    def _reconnect_loop(self) -> None:
        delay = self.config.reconnect_initial_delay_seconds
        while not self._stop_event.wait(delay):
            with self._lock:
                if self._manual_disconnect or self._state is not ConnectionState.RECONNECTING:
                    return
            try:
                sock = self._open_socket()
            except OSError as error:
                self._emit_error(ErrorCode.CONNECTION_FAILED, f"HP-Socket reconnect failed: {error}")
                delay = min(delay * 2, self.config.reconnect_max_delay_seconds)
                continue
            with self._lock:
                if self._manual_disconnect or self._stop_event.is_set():
                    self._close_socket(sock)
                    return
                self._socket = sock
                self._decoder.clear()
                self._last_receive_monotonic = time.monotonic()
            self._start_reader(sock)
            self._set_state(ConnectionState.CONNECTED, "HP-Socket reconnected")
            return

    def _dispatch_frame(self, frame: PackFrame) -> None:
        if frame.kind is PackMessageKind.HEARTBEAT:
            return  # 心跳只更新 last_receive，不进入请求关联表。
        if frame.kind is PackMessageKind.PUSH:
            self._dispatch_push(frame)
            return
        if frame.kind is not PackMessageKind.RESPONSE:
            self._emit_error(ErrorCode.PROTOCOL_ERROR, "server sent an unexpected Request frame")
            return
        with self._lock:
            pending = self._pending.pop(frame.request_id, None)
        if pending is None:
            self._emit_error(ErrorCode.PROTOCOL_ERROR, f"response request_id is unknown: {frame.request_id}")
            return
        request, session_wire_id = pending
        # auth.login 的成功响应可以在头部绑定新 Session；RequestTracker 仍按原始匿名会话完成该请求。
        if frame.session_id != session_wire_id and not (request.msg_type, request.func_id) == ("auth", "login"):
            self._emit_error(ErrorCode.PROTOCOL_ERROR, "response session_id does not match request")
            return
        try:
            document = PackMessageCodec.decode_json(frame.payload)
        except PackProtocolError as error:
            self._emit_error(ErrorCode.PROTOCOL_ERROR, f"response payload is invalid: {error}")
            return
        payload = document.get("data", document)
        if not isinstance(payload, dict):
            self._emit_error(ErrorCode.PROTOCOL_ERROR, "response data must be a JSON object")
            return
        status, error_code = self._status_from_error(frame.error_code)
        if request.func_id == "login":
            returned_session = payload.get("session_id")
            if isinstance(returned_session, (str, int)):
                self._heartbeat_session_wire_id = self._session_wire_id(str(returned_session))
        response = ClientResponse(
            request_id=request.request_id,
            session_id=request.session_id,
            msg_type=request.msg_type,
            func_id=request.func_id,
            status=status,
            payload=payload,
            error_code=error_code,
            error_message=str(document.get("message", document.get("error_message", ""))),
        )
        callback = self._response_callback
        if callback is not None:
            self._safe_call(callback, response)

    def _dispatch_push(self, frame: PackFrame) -> None:
        try:
            document = PackMessageCodec.decode_json(frame.payload)
        except PackProtocolError as error:
            self._emit_error(ErrorCode.PROTOCOL_ERROR, f"Push payload is invalid: {error}")
            return
        payload = document.get("data", document)
        if not isinstance(payload, dict):
            self._emit_error(ErrorCode.PROTOCOL_ERROR, "Push data must be a JSON object")
            return
        msg_type, func_id = _OPERATION_NAMES.get((frame.message_type, frame.function_id), (str(frame.message_type), str(frame.function_id)))
        session_id = self._session_name(frame.session_id)
        callback = self._push_callback
        if callback is not None:
            self._safe_call(
                callback,
                ClientResponse(
                    request_id="0",
                    session_id=session_id,
                    msg_type=msg_type,
                    func_id=func_id,
                    status=MessageStatus.SUCCEEDED,
                    payload={**payload, "sequence": frame.sequence},
                ),
            )

    def _operation_codes(self, msg_type: str, func_id: str) -> tuple[int, int]:
        key = (str(msg_type), str(func_id))
        if key in _OPERATION_CODES:
            return _OPERATION_CODES[key]
        if str(msg_type).isdecimal() and str(func_id).isdecimal():
            message_type, function_id = int(msg_type), int(func_id)
            if 0 <= message_type <= 0xFFFF and 0 <= function_id <= 0xFFFF:
                return message_type, function_id
        raise PackProtocolError(f"operation is not in the approved PackMessage registry: {msg_type}.{func_id}")

    def _request_wire_id(self, request_id: str) -> int:
        candidate = _wire_identifier(request_id, namespace="request")
        with self._lock:
            while candidate in self._pending and self._pending[candidate][0].request_id != request_id:
                candidate = (candidate + 1) & _MASK_64 or 1
        return candidate

    def _session_wire_id(self, session_id: str) -> int:
        text = str(session_id)
        with self._lock:
            existing = self._session_wire_ids.get(text)
            if existing is not None:
                return existing
            value = _wire_identifier(text, namespace="session")
            self._session_wire_ids[text] = value
            return value

    def _session_name(self, wire_id: int) -> str:
        with self._lock:
            for name, value in self._session_wire_ids.items():
                if value == wire_id:
                    return name
        return str(wire_id)

    @staticmethod
    def _status_from_error(error_code: int) -> tuple[MessageStatus, ErrorCode]:
        if error_code in (0, 2002):
            return MessageStatus.SUCCEEDED, ErrorCode.NONE
        if error_code in (1101, 1102, 1103, 1104, 1105):
            return MessageStatus.UNAUTHORIZED, ErrorCode.UNAUTHORIZED
        if error_code == 1202:
            return MessageStatus.TIMED_OUT, ErrorCode.TIMEOUT
        return MessageStatus.FAILED, ErrorCode.SERVICE_ERROR

    def _set_state(self, state: ConnectionState, reason: str) -> None:
        with self._lock:
            self._state = state
        callback = self._state_callback
        if callback is not None:
            self._safe_call(callback, state, reason)

    def _emit_error(self, code: ErrorCode, message: str) -> None:
        callback = self._error_callback
        if callback is not None:
            self._safe_call(callback, code, message)

    @staticmethod
    def _safe_call(callback: Callable[..., None], *args: Any) -> None:
        try:
            callback(*args)
        except Exception:
            pass  # 回调属于上层扩展点，不能让网络 reader/heartbeat 线程因 UI 异常退出。

    @staticmethod
    def _close_socket(sock: socket.socket | None) -> None:
        if sock is None:
            return
        try:
            sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            sock.close()
        except OSError:
            pass
