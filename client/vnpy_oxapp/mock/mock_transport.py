"""可控延迟与故障的异步 MockTransport。"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from threading import Lock, Timer
from typing import Callable

from ..message import ClientRequest, ClientResponse, ErrorCode, MessageStatus
from ..transport import ConnectionState, ErrorCallback, IClientTransport, ResponseCallback, StateCallback
from .mock_services import MockServiceError, MockServiceRouter


class MockScenario(str, Enum):
    NORMAL = "normal"
    CONNECT_FAILURE = "connect_failure"
    DELAYED = "delayed"
    TIMEOUT = "timeout"
    DISCONNECT = "disconnect"
    EMPTY = "empty"
    UNAUTHORIZED = "unauthorized"
    SERVICE_ERROR = "service_error"
    DUPLICATE = "duplicate"
    LATE = "late"


@dataclass(frozen=True, slots=True)
class MockTransportConfig:
    scenario: MockScenario = MockScenario.NORMAL
    response_delay_ms: int = 0
    connect_error_message: str = "mock connection refused"


class MockTransport(IClientTransport):
    """线程安全地模拟网络传输；Timer 代表后台回调，绝不直接改 Qt 控件。"""

    def __init__(self, config: MockTransportConfig | None = None, service_router: MockServiceRouter | None = None) -> None:
        self.config = config or MockTransportConfig()
        self.service_router = service_router or MockServiceRouter()
        self._state = ConnectionState.DISCONNECTED
        self._lock = Lock()
        self._response_callback: ResponseCallback | None = None
        self._state_callback: StateCallback | None = None
        self._error_callback: ErrorCallback | None = None
        self._timers: list[Timer] = []

    @property
    def state(self) -> ConnectionState:
        with self._lock:
            return self._state

    def set_response_callback(self, callback: ResponseCallback) -> None:
        self._response_callback = callback

    def set_state_callback(self, callback: StateCallback) -> None:
        self._state_callback = callback

    def set_error_callback(self, callback: ErrorCallback) -> None:
        self._error_callback = callback

    def connect(self) -> bool:
        self._set_state(ConnectionState.CONNECTING, "mock connect started")
        if self.config.scenario is MockScenario.CONNECT_FAILURE:
            self._set_state(ConnectionState.FAILED, self.config.connect_error_message)
            self._emit_error(ErrorCode.CONNECTION_FAILED, self.config.connect_error_message)
            return False
        self._set_state(ConnectionState.CONNECTED, "mock connection established")
        return True

    def reconnect(self) -> bool:
        self._set_state(ConnectionState.RECONNECTING, "mock reconnect started")
        return self.connect()

    def disconnect(self) -> None:
        self._cancel_timers()
        self._set_state(ConnectionState.DISCONNECTED, "mock connection closed")

    def send(self, request: ClientRequest) -> bool:
        if self.state is not ConnectionState.CONNECTED:
            self._emit_error(ErrorCode.DISCONNECTED, "send rejected while transport is disconnected")
            return False
        if self.config.scenario is MockScenario.DISCONNECT:
            self.disconnect()
            return False
        if self.config.scenario is MockScenario.TIMEOUT:
            return True  # 故意不回调，交给 RequestTracker 的 deadline 验收。
        response = self._build_response(request)
        if response is None:
            return True
        delay_ms = self.config.response_delay_ms
        if self.config.scenario in (MockScenario.DELAYED, MockScenario.LATE):
            delay_ms = max(delay_ms, 100)
        timer = Timer(delay_ms / 1_000, self._deliver, args=(response,))
        timer.daemon = True
        with self._lock:
            self._timers.append(timer)
        timer.start()
        if self.config.scenario is MockScenario.DUPLICATE:
            duplicate = Timer((delay_ms + 1) / 1_000, self._deliver, args=(response,))
            duplicate.daemon = True
            with self._lock:
                self._timers.append(duplicate)
            duplicate.start()
        return True

    def _build_response(self, request: ClientRequest) -> ClientResponse | None:
        try:
            if self.config.scenario is MockScenario.EMPTY:
                payload = {"items": [], "page": 1, "page_size": 20, "total": 0}
            elif self.config.scenario is MockScenario.UNAUTHORIZED:
                raise MockServiceError("unauthorized", "mock permission denied")
            elif self.config.scenario is MockScenario.SERVICE_ERROR:
                raise MockServiceError("service_error", "mock service unavailable")
            else:
                payload = self.service_router.handle(
                    request.msg_type,
                    request.func_id,
                    request.payload,
                )
            return ClientResponse(
                request_id=request.request_id,
                session_id=request.session_id,
                msg_type=request.msg_type,
                func_id=request.func_id,
                status=MessageStatus.SUCCEEDED,
                payload=payload,
            )
        except MockServiceError as error:
            status = MessageStatus.UNAUTHORIZED if error.code == "unauthorized" else MessageStatus.FAILED
            code = ErrorCode.UNAUTHORIZED if error.code == "unauthorized" else ErrorCode.SERVICE_ERROR
            return ClientResponse.failure(request, code, str(error), status=status)

    def _deliver(self, response: ClientResponse) -> None:
        callback = self._response_callback
        if callback is not None:
            callback(response)  # 回调只交付数据；主线程更新由 Qt bridge 负责。

    def _set_state(self, state: ConnectionState, reason: str) -> None:
        with self._lock:
            self._state = state
        if self._state_callback:
            self._state_callback(state, reason)

    def _emit_error(self, code: ErrorCode, message: str) -> None:
        if self._error_callback:
            self._error_callback(code, message)

    def _cancel_timers(self) -> None:
        with self._lock:
            timers, self._timers = self._timers, []
        for timer in timers:
            timer.cancel()
