"""请求响应关联和超时处理。"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from concurrent.futures import Future
from threading import RLock
from typing import Callable
from uuid import uuid4
import time

from .message import ClientRequest, ClientResponse, ErrorCode, MessageStatus, unix_time_ms
from .transport import ConnectionState, IClientTransport


class RequestResolution(str, Enum):
    ACCEPTED = "accepted"
    UNKNOWN = "unknown"
    SESSION_MISMATCH = "session_mismatch"
    DUPLICATE = "duplicate"
    LATE = "late"


@dataclass(frozen=True, slots=True)
class PendingRequest:
    request: ClientRequest
    deadline_monotonic: float


class RequestTracker:
    """用 request_id + session_id 关联响应，并把已结束请求保留用于拒绝迟到包。"""

    def __init__(self, completed_limit: int = 1024) -> None:
        self._lock = RLock()
        self._pending: dict[str, PendingRequest] = {}
        self._completed: dict[str, RequestResolution] = {}
        self._completed_limit = completed_limit

    def create_request(
        self,
        *,
        session_id: str,
        msg_type: str,
        func_id: str,
        payload: dict | None = None,
        timeout_ms: int = 3_000,
    ) -> ClientRequest:
        if timeout_ms <= 0:
            raise ValueError("timeout_ms must be positive")
        request = ClientRequest(
            request_id=uuid4().hex,
            session_id=session_id,
            msg_type=msg_type,
            func_id=func_id,
            payload=payload or {},
            deadline_unix_ms=unix_time_ms() + timeout_ms,
        )
        with self._lock:
            self._pending[request.request_id] = PendingRequest(
                request=request,
                deadline_monotonic=time.monotonic() + timeout_ms / 1_000,
            )  # 单调时钟不受本机校时影响，用于本进程超时判定。
        return request

    def resolve(self, response: ClientResponse) -> RequestResolution:
        with self._lock:
            pending = self._pending.get(response.request_id)
            if pending is None:
                completed = self._completed.get(response.request_id)
                if completed is RequestResolution.LATE:
                    return RequestResolution.LATE
                if completed is not None:
                    return RequestResolution.DUPLICATE  # 已完成请求的再次回包不能再次驱动页面。
                return RequestResolution.UNKNOWN
            if pending.request.session_id != response.session_id:
                return RequestResolution.SESSION_MISMATCH  # 会话不匹配时保留 pending，等待可信回包。
            if time.monotonic() >= pending.deadline_monotonic:
                del self._pending[response.request_id]
                self._remember(response.request_id, RequestResolution.LATE)
                return RequestResolution.LATE
            del self._pending[response.request_id]
            self._remember(response.request_id, RequestResolution.ACCEPTED)
            return RequestResolution.ACCEPTED

    def expire(self) -> list[ClientRequest]:
        now = time.monotonic()
        expired: list[ClientRequest] = []
        with self._lock:
            for request_id, pending in list(self._pending.items()):
                if now >= pending.deadline_monotonic:
                    expired.append(pending.request)
                    del self._pending[request_id]
                    self._remember(request_id, RequestResolution.LATE)
        return expired

    @property
    def pending_count(self) -> int:
        with self._lock:
            return len(self._pending)

    def _remember(self, request_id: str, resolution: RequestResolution) -> None:
        self._completed[request_id] = resolution
        while len(self._completed) > self._completed_limit:
            self._completed.pop(next(iter(self._completed)))  # dict 保持插入顺序，限制迟到包缓存内存。


class ClientProtocolClient:
    """将 Transport 回调转为可关联的客户端响应，页面无需直接管理 request_id。"""

    def __init__(self, transport: IClientTransport, tracker: RequestTracker | None = None) -> None:
        self.transport = transport
        self.tracker = tracker or RequestTracker()
        self._response_callback: Callable[[ClientResponse], None] | None = None
        self._state_callback: Callable[[ConnectionState, str], None] | None = None
        self._future_lock = RLock()
        self._futures: dict[str, Future[ClientResponse]] = {}
        transport.set_response_callback(self._on_transport_response)
        transport.set_state_callback(self._on_transport_state)

    def set_response_callback(self, callback: Callable[[ClientResponse], None]) -> None:
        self._response_callback = callback

    def set_state_callback(self, callback: Callable[[ConnectionState, str], None]) -> None:
        self._state_callback = callback

    def connect(self) -> bool:
        return self.transport.connect()

    def disconnect(self) -> None:
        self.transport.disconnect()

    def request(
        self,
        *,
        session_id: str,
        msg_type: str,
        func_id: str,
        payload: dict | None = None,
        timeout_ms: int = 3_000,
    ) -> Future[ClientResponse]:
        request = self.tracker.create_request(
            session_id=session_id,
            msg_type=msg_type,
            func_id=func_id,
            payload=payload,
            timeout_ms=timeout_ms,
        )
        future: Future[ClientResponse] = Future()
        with self._future_lock:
            self._futures[request.request_id] = future
        if not self.transport.send(request):
            failed = ClientResponse.failure(request, ErrorCode.DISCONNECTED, "transport is not connected")
            self._deliver_if_current(failed)
        return future

    def expire_requests(self) -> list[ClientResponse]:
        responses = [
            ClientResponse.failure(
                request,
                ErrorCode.TIMEOUT,
                "response did not arrive before the request deadline",
                status=MessageStatus.TIMED_OUT,
            )
            for request in self.tracker.expire()
        ]
        for response in responses:
            self._finish(response)
        return responses

    def _on_transport_response(self, response: ClientResponse) -> None:
        self._deliver_if_current(response)

    def _deliver_if_current(self, response: ClientResponse) -> None:
        resolution = self.tracker.resolve(response)
        if resolution is RequestResolution.ACCEPTED:
            self._finish(response)

    def _finish(self, response: ClientResponse) -> None:
        with self._future_lock:
            future = self._futures.pop(response.request_id, None)
        if future is not None and not future.done():
            future.set_result(response)
        if self._response_callback:
            self._response_callback(response)  # UI 必须经 Qt bridge 接收，不能在此后台回调中直接改控件。

    def _on_transport_state(self, state: ConnectionState, reason: str) -> None:
        if self._state_callback:
            self._state_callback(state, reason)
