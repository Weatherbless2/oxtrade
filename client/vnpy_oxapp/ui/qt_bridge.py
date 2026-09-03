"""将后台 Transport/行情回调安全转发给 Qt 主线程。

Transport、行情源和定时器可能运行在工作线程，不能直接操作 QWidget。
``AsyncQtBridge`` 是它们与 UI 的唯一事件边界：后台代码只调用 publish
方法，Qt 再根据接收对象线程投递信号，页面在主线程更新控件。
"""

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
        """跨线程事件桥。

        响应/连接状态和行情事件分别使用独立信号，防止高频行情刷新
        把普通请求状态混在一起，也防止行情源断线被误报成业务 Transport
        断线。接收者应在 Qt 主线程连接这些信号。
        """

        response_received = Signal(object)  # Qt queued signal 承载完整响应，不在后台线程触摸控件。
        state_changed = Signal(str, str)  # 状态和值班原因一起投递，状态栏可直接展示。
        market_event_received = Signal(object)  # 行情线程只提交标准事件，具体表格刷新必须回到 Qt 主线程。
        market_state_changed = Signal(str, str)  # 行情源状态独立于后台请求 Transport，避免把 PyTdx 断线误报成 HP-Socket 断线。

        def publish_response(self, response: object) -> None:
            """发布完整请求响应，由 Qt 负责排队到接收对象线程。"""
            self.response_received.emit(response)  # emit 由 Qt 决定排队到接收对象线程。

        def publish_state(self, state: str, reason: str) -> None:
            """发布 Transport 状态及可读原因。"""
            self.state_changed.emit(state, reason)  # 后台 Timer 只发布事件，GUI 主线程负责消费。

        def publish_market_event(self, event: object) -> None:
            """发布标准化行情事件，禁止在后台线程直接刷新表格。"""
            self.market_event_received.emit(event)  # Socket/Mock 行情线程不得直接调用 QWidget 或 QTableWidget。

        def publish_market_state(self, state: str, reason: str) -> None:
            """发布行情源状态，供行情页单独展示和重连提示。"""
            self.market_state_changed.emit(state, reason)  # 状态与行情事件分离，页面可独立展示当前源和重连原因。
else:
    class AsyncQtBridge:  # type: ignore[no-redef]
        """无 PySide6 时的明确失败边界。"""

        def __init__(self) -> None:
            raise RuntimeError("Qt UI requires PySide6; install client/requirements.txt first")
