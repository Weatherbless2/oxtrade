"""将后台 Transport 回调安全转发给 Qt 主线程。"""

from __future__ import annotations

try:
    from PySide6.QtCore import QObject, Signal
    PYSIDE_AVAILABLE = True
except ImportError:  # 可选 UI 依赖，避免阻塞核心通信层测试。
    QObject = object  # type: ignore[misc,assignment]  # 无 PySide6 时保留可导入边界，让核心通信测试继续运行。
    PYSIDE_AVAILABLE = False

    class Signal:  # type: ignore[no-redef]
        def __init__(self, *args) -> None:
            del args


if PYSIDE_AVAILABLE:
    class AsyncQtBridge(QObject):
        """跨线程 signal 会由 Qt 排队到接收 QObject 所在线程，禁止 Timer 线程直接写控件。"""

        response_received = Signal(object)  # Qt queued signal 承载完整响应，不在后台线程触摸控件。
        state_changed = Signal(str, str)  # 状态和值班原因一起投递，状态栏可直接展示。
        market_event_received = Signal(object)  # 行情线程只提交标准事件，具体表格刷新必须回到 Qt 主线程。
        market_state_changed = Signal(str, str)  # 行情源状态独立于后台请求 Transport，避免把 PyTdx 断线误报成 HP-Socket 断线。

        def publish_response(self, response: object) -> None:
            self.response_received.emit(response)  # emit 由 Qt 决定排队到接收对象线程。

        def publish_state(self, state: str, reason: str) -> None:
            self.state_changed.emit(state, reason)  # 后台 Timer 只发布事件，GUI 主线程负责消费。

        def publish_market_event(self, event: object) -> None:
            self.market_event_received.emit(event)  # Socket/Mock 行情线程不得直接调用 QWidget 或 QTableWidget。

        def publish_market_state(self, state: str, reason: str) -> None:
            self.market_state_changed.emit(state, reason)  # 状态与行情事件分离，页面可独立展示当前源和重连原因。
else:
    class AsyncQtBridge:  # type: ignore[no-redef]
        def __init__(self) -> None:
            raise RuntimeError("Qt UI requires PySide6; install client/requirements.txt first")
