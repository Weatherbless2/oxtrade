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
        self.config = config or MockTransportConfig()  # 场景配置是确定性故障注入入口，不读取真实网络参数。
        self.service_router = service_router or MockServiceRouter()  # 默认使用内存数据，页面测试不依赖数据库。
        self._state = ConnectionState.DISCONNECTED  # 初始状态明确为未连接，send 会据此拒绝。
        self._lock = Lock()  # 状态和 Timer 列表可能被主线程、Timer 线程同时访问。
        self._response_callback: ResponseCallback | None = None  # 完整响应只交给客户端关联层。
        self._state_callback: StateCallback | None = None  # 状态变化交给 Gateway/Qt bridge。
        self._error_callback: ErrorCallback | None = None  # 无法关联请求的连接错误走独立通道。
        self._timers: list[Timer] = []  # 保存 Timer 所有权，disconnect 时逐个取消。

    @property
    def state(self) -> ConnectionState:
        with self._lock:
            return self._state  # 在锁内读取，避免观察到半更新状态。

    def set_response_callback(self, callback: ResponseCallback) -> None:
        self._response_callback = callback  # 注入回调而不是硬编码上层，便于替换真实 Transport。

    def set_state_callback(self, callback: StateCallback) -> None:
        self._state_callback = callback  # 状态通知不直接依赖 UI 或 vn.py。

    def set_error_callback(self, callback: ErrorCallback) -> None:
        self._error_callback = callback  # 连接失败等没有 request_id 的错误由客户端记录。

    def connect(self) -> bool:
        self._set_state(ConnectionState.CONNECTING, "mock connect started")  # 先广播连接中，让 UI 不误显示已连接。
        if self.config.scenario is MockScenario.CONNECT_FAILURE:
            self._set_state(ConnectionState.FAILED, self.config.connect_error_message)  # 注入失败终态，后续 send 不会被允许。
            self._emit_error(ErrorCode.CONNECTION_FAILED, self.config.connect_error_message)  # 把底层拒绝映射为稳定业务错误。
            return False  # 调用方可立即停止启动流程，不创建页面请求。
        self._set_state(ConnectionState.CONNECTED, "mock connection established")  # 正常连接完成，才开放 send。
        return True  # 连接成功只用 bool 表示，真实 Socket 可复用同一接口。

    def reconnect(self) -> bool:
        self._set_state(ConnectionState.RECONNECTING, "mock reconnect started")  # 重连先显式进入过渡态。
        return self.connect()  # 复用连接成功/失败和错误回调逻辑，避免两套状态机漂移。

    def disconnect(self) -> None:
        self._cancel_timers()  # 先取消尚未触发的后台回调，防止断开后又投递旧响应。
        self._set_state(ConnectionState.DISCONNECTED, "mock connection closed")  # 最后广播资源已关闭。

    def send(self, request: ClientRequest) -> bool:
        if self.state is not ConnectionState.CONNECTED:  # 未连接请求没有进入传输层，不能伪造成功回包。
            self._emit_error(ErrorCode.DISCONNECTED, "send rejected while transport is disconnected")  # 无 request_id 的传输错误走错误回调。
            return False  # ClientProtocolClient 会把 False 转成该请求的失败 Future。
        if self.config.scenario is MockScenario.DISCONNECT:
            self.disconnect()  # 故障注入模拟发送期间链路被对端关闭，并清理已有 Timer。
            return False  # 请求未发出，不能等待一个永远不会来的应答。
        if self.config.scenario is MockScenario.TIMEOUT:
            return True  # 故意不回调，交给 RequestTracker 的 deadline 验收，模拟网络黑洞。
        response = self._build_response(request)  # 服务路由同步生成业务结果，再异步模拟网络返回。
        if response is None:
            return True  # 空响应场景仍算发送成功，真正的空列表由 _build_response 交付。
        delay_ms = self.config.response_delay_ms
        if self.config.scenario in (MockScenario.DELAYED, MockScenario.LATE):
            delay_ms = max(delay_ms, 100)
        timer = Timer(delay_ms / 1_000, self._deliver, args=(response,))  # Timer 代表后台网络回调线程。
        timer.daemon = True  # 不让测试/进程退出被未完成的 Mock 回调阻塞。
        with self._lock:
            self._timers.append(timer)  # 保存取消责任，disconnect 可回收尚未触发的 Timer。
        timer.start()  # 真正异步触发，验证客户端不能假设 send 同步返回结果。
        if self.config.scenario is MockScenario.DUPLICATE:
            duplicate = Timer((delay_ms + 1) / 1_000, self._deliver, args=(response,))  # 第二次回包专门模拟网络重试/重复投递。
            duplicate.daemon = True  # 重复回调同样不拥有进程退出权。
            with self._lock:
                self._timers.append(duplicate)  # 与首个 Timer 一样纳入统一清理集合。
            duplicate.start()  # 由 RequestTracker 拒绝第二次完成。
        return True

    def _build_response(self, request: ClientRequest) -> ClientResponse | None:
        try:
            if self.config.scenario is MockScenario.EMPTY:  # 空数据是成功响应，不应被页面误判为服务错误。
                payload = {"items": [], "page": 1, "page_size": 20, "total": 0}
            elif self.config.scenario is MockScenario.UNAUTHORIZED:  # 主动制造权限失败，验证错误分类。
                raise MockServiceError("unauthorized", "mock permission denied")
            elif self.config.scenario is MockScenario.SERVICE_ERROR:  # 主动制造服务异常，验证失败状态而非成功空包。
                raise MockServiceError("service_error", "mock service unavailable")
            else:
                payload = self.service_router.handle(  # 正常场景进入统一服务路由，保持页面看到的字段形状稳定。
                    request.msg_type,
                    request.func_id,
                    request.payload,
                )
            return ClientResponse(  # 成功路径原样带回 request/session/业务字段，供关联层三重校验。
                request_id=request.request_id,
                session_id=request.session_id,
                msg_type=request.msg_type,
                func_id=request.func_id,
                status=MessageStatus.SUCCEEDED,
                payload=payload,
            )
        except MockServiceError as error:
            status = MessageStatus.UNAUTHORIZED if error.code == "unauthorized" else MessageStatus.FAILED  # 服务错误映射为页面终态。
            code = ErrorCode.UNAUTHORIZED if error.code == "unauthorized" else ErrorCode.SERVICE_ERROR  # 内部 code 转稳定枚举。
            return ClientResponse.failure(request, code, str(error), status=status)  # 失败也保留关联字段，Future 能正常收口。

    def _deliver(self, response: ClientResponse) -> None:
        callback = self._response_callback  # 读取回调引用后再执行，避免回调内部重入锁。
        if callback is not None:
            callback(response)  # 这里只交付数据；主线程更新由 Qt bridge 负责。

    def _set_state(self, state: ConnectionState, reason: str) -> None:
        with self._lock:
            self._state = state  # 状态写入在锁内完成，再在锁外通知，避免回调反向取锁死锁。
        if self._state_callback:
            self._state_callback(state, reason)  # 上层收到完整状态和可读原因。

    def _emit_error(self, code: ErrorCode, message: str) -> None:
        if self._error_callback:
            self._error_callback(code, message)  # 连接级错误没有业务响应，只能通过此通道上报。

    def _cancel_timers(self) -> None:
        with self._lock:
            timers, self._timers = self._timers, []  # 一次性交出所有权，后续不会重复取消同一批 Timer。
        for timer in timers:
            timer.cancel()  # 取消尚未执行的回调；已开始的回调仍由关联层判断是否过期/重复。
