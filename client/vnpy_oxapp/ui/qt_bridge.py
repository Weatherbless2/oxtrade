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

        def publish_response(self, response: object) -> None:
            self.response_received.emit(response)  # emit 由 Qt 决定排队到接收对象线程。

        def publish_state(self, state: str, reason: str) -> None:
            self.state_changed.emit(state, reason)  # 后台 Timer 只发布事件，GUI 主线程负责消费。
else:
    class AsyncQtBridge:  # type: ignore[no-redef]
        def __init__(self) -> None:
            raise RuntimeError("Qt UI requires PySide6; install client/requirements.txt first")
