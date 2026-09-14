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
from .transport import ConnectionState, IClientTransport, PushCallback


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
        self._lock = RLock()  # 请求可能由页面线程创建、Timer 线程回包，所有表访问必须同一把锁保护。
        self._pending: dict[str, PendingRequest] = {}  # 尚未完成的 request_id -> 请求和单调截止时间。
        self._completed: dict[str, RequestResolution] = {}  # 已结束结果短暂留存，用于拒绝重复/迟到包。
        self._completed_limit = completed_limit  # 限制历史缓存，避免长期运行因迟到包记录无限增长。

    def create_request(
        self,
        *,
        session_id: str,
        msg_type: str,
        func_id: str,
        payload: dict | None = None,
        timeout_ms: int = 3_000,
    ) -> ClientRequest:
        if timeout_ms <= 0:  # 没有正超时就无法决定何时回收 pending，先在入口拒绝。
            raise ValueError("timeout_ms must be positive")
        request = ClientRequest(  # 同时携带绝对 deadline，给跨进程协议和日志使用。
            request_id=uuid4().hex,
            session_id=session_id,
            msg_type=msg_type,
            func_id=func_id,
            payload=payload or {},
            deadline_unix_ms=unix_time_ms() + timeout_ms,
        )
        with self._lock:  # 先登记 pending 再交给传输层，避免极快回包找不到请求。
            self._pending[request.request_id] = PendingRequest(
                request=request,
                deadline_monotonic=time.monotonic() + timeout_ms / 1_000,
            )  # 单调时钟不受本机校时影响，用于本进程超时判定。
        return request  # 调用方随后把同一对象交给 Transport，request_id 保持不变。

    def resolve(self, response: ClientResponse) -> RequestResolution:
        with self._lock:
            pending = self._pending.get(response.request_id)  # 先按关联键找原请求，未知包不进入页面。
            if pending is None:
                completed = self._completed.get(response.request_id)
                if completed is RequestResolution.LATE:  # 已超时的迟到回包永远不能重新打开 Future。
                    return RequestResolution.LATE
                if completed is not None:
                    return RequestResolution.DUPLICATE  # 已完成请求的再次回包不能再次驱动页面。
                return RequestResolution.UNKNOWN
            if pending.request.session_id != response.session_id:  # 三重边界中的会话校验失败，保留 pending 等可信回包。
                return RequestResolution.SESSION_MISMATCH  # 会话不匹配时保留 pending，等待可信回包。
            if time.monotonic() >= pending.deadline_monotonic:  # 单调时钟只判断本机经过时长，不受系统校时影响。
                del self._pending[response.request_id]  # 超时后立即释放等待项，避免内存和页面状态一直挂起。
                self._remember(response.request_id, RequestResolution.LATE)  # 记住迟到结论，后续包会被静默拒绝。
                return RequestResolution.LATE
            del self._pending[response.request_id]  # 成功消费只允许一次，删除后重复包无法再次完成 Future。
            self._remember(response.request_id, RequestResolution.ACCEPTED)  # 保存已接受标记，便于识别重复回包。
            return RequestResolution.ACCEPTED

    def expire(self) -> list[ClientRequest]:
        now = time.monotonic()  # 一次扫描使用同一个时间点，避免同批请求因微小时差产生不一致。
        expired: list[ClientRequest] = []
        with self._lock:
            for request_id, pending in list(self._pending.items()):  # 拷贝键值遍历，允许循环中删除原 pending。
                if now >= pending.deadline_monotonic:
                    expired.append(pending.request)  # 返回原请求，调用层据此构造带关联字段的超时应答。
                    del self._pending[request_id]  # 超时请求从活动集合移除，后续迟到包只能走 LATE。
                    self._remember(request_id, RequestResolution.LATE)  # 为迟到包建立终态挡板。
        return expired

    @property
    def pending_count(self) -> int:
        with self._lock:
            return len(self._pending)

    def _remember(self, request_id: str, resolution: RequestResolution) -> None:
        self._completed[request_id] = resolution
        while len(self._completed) > self._completed_limit:
            self._completed.pop(next(iter(self._completed)))  # FIFO 淘汰最旧终态，限制迟到包缓存内存。


class ClientProtocolClient:
    """将 Transport 回调转为可关联的客户端响应，页面无需直接管理 request_id。"""

    def __init__(self, transport: IClientTransport, tracker: RequestTracker | None = None) -> None:
        self.transport = transport
        self.tracker = tracker or RequestTracker()
        self._response_callback: Callable[[ClientResponse], None] | None = None
        self._push_callback: PushCallback | None = None
        self._state_callback: Callable[[ConnectionState, str], None] | None = None
        self._future_lock = RLock()
        self._futures: dict[str, Future[ClientResponse]] = {}
        transport.set_response_callback(self._on_transport_response)  # Transport 只交付数据，关联判断集中在本类。
        transport.set_push_callback(self._on_transport_push)  # Push 不进入 RequestTracker，单独交给行情/状态页面。
        transport.set_state_callback(self._on_transport_state)  # 连接状态沿同一回调链交给页面/Gateway。

    def set_response_callback(self, callback: Callable[[ClientResponse], None]) -> None:
        self._response_callback = callback

    def set_push_callback(self, callback: PushCallback) -> None:
        """设置行情/状态 Push 回调；普通请求仍由 response_callback 处理。"""
        self._push_callback = callback

    def set_state_callback(self, callback: Callable[[ConnectionState, str], None]) -> None:
        self._state_callback = callback

    def connect(self) -> bool:
        return self.transport.connect()  # 具体连接实现可替换，客户端只依赖 bool 成功语义。

    def disconnect(self) -> None:
        self.transport.disconnect()  # 由传输实现取消后台资源并发布 DISCONNECTED。

    def request(
        self,
        *,
        session_id: str,
        msg_type: str,
        func_id: str,
        payload: dict | None = None,
        timeout_ms: int = 3_000,
    ) -> Future[ClientResponse]:
        request = self.tracker.create_request(  # 先生成 request_id、双时钟 deadline 并登记 pending。
            session_id=session_id,
            msg_type=msg_type,
            func_id=func_id,
            payload=payload,
            timeout_ms=timeout_ms,
        )
        future: Future[ClientResponse] = Future()  # 页面通过 Future 等待结果，不阻塞发起请求的线程。
        future.request_id = request.request_id  # 页面用稳定关联键匹配 Qt 回调，不依赖 Future 对象身份。
        with self._future_lock:
            self._futures[request.request_id] = future  # 发送前保存 Future，处理同步失败也能准确收口。
        if not self.transport.send(request):  # False 表示请求未进入传输层，必须补一条稳定的断线结果。
            failed = ClientResponse.failure(request, ErrorCode.DISCONNECTED, "transport is not connected")
            self._deliver_if_current(failed)  # 统一经过 tracker，清除 pending/Future，避免泄漏。
        return future  # 调用方可 result(timeout=...) 或注册回调，不需要接触内部表。

    def expire_requests(self) -> list[ClientResponse]:
        responses = [  # 把 tracker 找到的过期请求转换成页面可识别的 TIMED_OUT 结果。
            ClientResponse.failure(
                request,
                ErrorCode.TIMEOUT,
                "response did not arrive before the request deadline",
                status=MessageStatus.TIMED_OUT,
            )
            for request in self.tracker.expire()
        ]
        for response in responses:  # 每个超时都走与正常回包相同的 Future/回调收口。
            self._finish(response)
        return responses

    def _on_transport_response(self, response: ClientResponse) -> None:
        self._deliver_if_current(response)

    def _on_transport_push(self, response: ClientResponse) -> None:
        if self._push_callback:
            self._push_callback(response)  # Push 没有 request_id 关联，不得送进 tracker 误判为未知响应。

    def _deliver_if_current(self, response: ClientResponse) -> None:
        resolution = self.tracker.resolve(response)  # 同时检查 request_id、session_id 和 deadline。
        if resolution is RequestResolution.ACCEPTED:
            self._finish(response)  # 只有 ACCEPTED 才能完成一次 Future 并通知页面。

    def _finish(self, response: ClientResponse) -> None:
        with self._future_lock:
            future = self._futures.pop(response.request_id, None)  # 弹出所有权，确保同一请求只找到一个 Future。
        if future is not None and not future.done():
            future.set_result(response)  # Future 从 pending 变为终态，等待方被唤醒。
        if self._response_callback:
            self._response_callback(response)  # UI 必须经 Qt bridge 接收，不能在后台 Timer 线程直接改控件。

    def _on_transport_state(self, state: ConnectionState, reason: str) -> None:
        if self._state_callback:
            self._state_callback(state, reason)
