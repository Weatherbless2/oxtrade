"""根据开发配置选择 PyTdx 或 Mock 行情源，并在本机 Bridge 不可用时自动回退。"""

from __future__ import annotations

from threading import Event, RLock, Thread, current_thread

from .config import MarketConfig
from .event import MarketEvent
from .feed import EventCallback, IMarketFeed, MarketFeedStatus, StateCallback
from .mock_feed import MockMarketFeed
from .pytdx_feed import PyTdxFeedConfig, PyTdxJsonLineFeed


class FallbackMarketFeed(IMarketFeed):
    """优先尝试 PyTdx Bridge；超时未收到有效行情才启用 Mock，避免离线时页面完全空白。"""

    def __init__(self, primary: IMarketFeed, fallback: IMarketFeed, *, fallback_after_seconds: float) -> None:
        self.primary = primary
        self.fallback = fallback
        self.fallback_after_seconds = max(0.2, fallback_after_seconds)
        self._lock = RLock()
        self._stop_event = Event()
        self._primary_received = Event()
        self._monitor: Thread | None = None
        self._active: IMarketFeed = primary
        self._event_callback: EventCallback | None = None
        self._state_callback: StateCallback | None = None

    @property
    def status(self) -> MarketFeedStatus:
        with self._lock:
            return self._active.status

    def start(self, event_callback: EventCallback, state_callback: StateCallback) -> None:
        with self._lock:
            if self._monitor is not None and self._monitor.is_alive():
                return
            self._event_callback, self._state_callback = event_callback, state_callback
            self._active = self.primary
            self._stop_event.clear()
            self._primary_received.clear()
            self.primary.start(self._on_primary_event, self._on_primary_state)
            self._monitor = Thread(target=self._watch_primary, name="oxapp-market-fallback", daemon=True)
            self._monitor.start()

    def subscribe_symbols(self, symbols: list[str]) -> None:
        self.primary.subscribe_symbols(symbols)
        self.fallback.subscribe_symbols(symbols)  # 提前同步订阅集，发生回退后无需再等待页面点击订阅。

    def stop(self) -> None:
        self._stop_event.set()
        self.primary.stop()
        self.fallback.stop()
        with self._lock:
            monitor = self._monitor
        if monitor is not None and monitor is not current_thread():
            monitor.join(timeout=1.0)
        with self._lock:
            self._monitor = None

    def _watch_primary(self) -> None:
        while not self._stop_event.is_set():
            self._primary_received.clear()
            if self._primary_received.wait(self.fallback_after_seconds):
                continue  # 在窗口内收到有效 PyTdx 快照，继续监控下一段间隔而不是启动 Mock。
            with self._lock:
                already_fallback = self._active is self.fallback
                self._active = self.fallback
                callback = self._state_callback
            if not already_fallback:
                self.fallback.start(self._on_fallback_event, self._on_fallback_state)  # Mock 只兜底显示；primary 仍按自身退避重连，恢复后可自动切回。
                if callback is not None:
                    callback(self.fallback.status)
            # 下一轮仍等待完整的有效行情间隔；若 primary 已恢复，_on_primary_event 会立即切换 active 源并停止 Mock。

    def _on_primary_event(self, event: MarketEvent) -> None:
        self._primary_received.set()
        with self._lock:
            previous = self._active
            self._active = self.primary
            state_callback = self._state_callback
        if previous is self.fallback:
            self.fallback.stop()  # 首条真实快照已证明 PyTdx 可用，停止 Mock 防止两种来源交叉覆盖同一标的。
            if state_callback is not None:
                state_callback(self.primary.status)
        callback = self._event_callback
        if callback is not None:
            callback(event)

    def _on_fallback_event(self, event: MarketEvent) -> None:
        callback = self._event_callback
        if callback is not None:
            callback(event)

    def _on_primary_state(self, status: MarketFeedStatus) -> None:
        with self._lock:
            active = self._active
            callback = self._state_callback
        if active is self.primary and callback is not None:
            callback(status)

    def _on_fallback_state(self, status: MarketFeedStatus) -> None:
        with self._lock:
            active = self._active
            callback = self._state_callback
        if active is self.fallback and callback is not None:
            callback(status)


def create_market_feed(config: MarketConfig) -> IMarketFeed:
    """工厂只决定开发数据源，正式 oxMWH/券商 C++ SDK 后续从这里替换，不改页面。"""

    mock = MockMarketFeed(interval_seconds=config.mock_interval_ms / 1_000)
    if config.source == "mock":
        return mock
    pytdx = PyTdxJsonLineFeed(PyTdxFeedConfig(
        host=config.pytdx_host,
        port=config.pytdx_port,
        reconnect_seconds=config.reconnect_seconds,
    ))
    return FallbackMarketFeed(pytdx, mock, fallback_after_seconds=config.fallback_after_seconds)
