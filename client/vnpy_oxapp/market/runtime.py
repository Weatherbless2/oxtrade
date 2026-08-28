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
                return  # 已有监控线程时重复 start 不创建第二套主备状态机，避免两边同时启停 Mock。
            self._event_callback, self._state_callback = event_callback, state_callback  # 最终只向同一个 Hub 发布当前有效源的事件和状态。
            self._active = self.primary  # 每次启动都先选择 PyTdx；Mock 只有在规定时间无有效 Tick 后才接管。
            self._stop_event.clear()  # 允许同一组合 Feed 在页面生命周期内重新启动。
            self._primary_received.clear()  # 清掉上次运行信号，本轮必须收到新行情才能证明主源可用。
            self.primary.start(self._on_primary_event, self._on_primary_state)  # 主源持续运行和重连，即使 Mock 已经接管也不停止它。
            self._monitor = Thread(target=self._watch_primary, name="oxapp-market-fallback", daemon=True)  # 独立等待主源活性，不能阻塞 Socket 或 Qt 线程。
            self._monitor.start()  # start 立即返回，页面可先展示 connecting 状态。

    def subscribe_symbols(self, symbols: list[str]) -> None:
        self.primary.subscribe_symbols(symbols)  # PyTdx 当前在线或以后重连时都需要这份订阅集合。
        self.fallback.subscribe_symbols(symbols)  # 提前同步订阅集，发生回退后无需再等待页面点击订阅。

    def stop(self) -> None:
        self._stop_event.set()  # 先唤醒 fallback_after_seconds 等待，避免退出还要等完整观察窗口。
        self.primary.stop()  # 主源拥有 TCP Socket/接收线程，由它自己 shutdown、close 和 join。
        self.fallback.stop()  # Mock 可能从未启动，stop 仍须幂等，统一释放可能存在的生成线程。
        with self._lock:
            monitor = self._monitor
        if monitor is not None and monitor is not current_thread():
            monitor.join(timeout=1.0)  # 等监控线程离开状态机；当前线程若正是 monitor 则不能自我等待。
        with self._lock:
            self._monitor = None

    def _watch_primary(self) -> None:
        while not self._stop_event.is_set():
            self._primary_received.clear()  # 每个观察窗口都要求一条新行情，连接状态本身不足以证明数据还在流动。
            if self._primary_received.wait(self.fallback_after_seconds):
                continue  # 在窗口内收到有效 PyTdx 快照，继续监控下一段间隔而不是启动 Mock。
            with self._lock:
                already_fallback = self._active is self.fallback  # 连续静默窗口不能反复 start 同一 Mock 线程。
                self._active = self.fallback  # 先原子切换状态，再在锁外启动回退源，status 查询立即看到新的责任源。
                callback = self._state_callback  # 只复制回调，实际用户代码不能在内部锁内执行。
            if not already_fallback:
                self.fallback.start(self._on_fallback_event, self._on_fallback_state)  # Mock 只兜底显示；primary 仍按自身退避重连，恢复后可自动切回。
                if callback is not None:
                    callback(self.fallback.status)
            # 下一轮仍等待完整的有效行情间隔；若 primary 已恢复，_on_primary_event 会立即切换 active 源并停止 Mock。

    def _on_primary_event(self, event: MarketEvent) -> None:
        self._primary_received.set()  # 只有成功解析成 MarketEvent 的包才续期主源活性，坏 JSON 不会阻止 Mock 回退。
        with self._lock:
            previous = self._active  # 记录是否真的发生 Mock -> PyTdx 切换，避免每条真实 Tick 都 stop Mock。
            self._active = self.primary  # 首条有效真实行情到达即恢复主源，后续事件全部从主源进入 Hub。
            state_callback = self._state_callback
        if previous is self.fallback:
            self.fallback.stop()  # 首条真实快照已证明 PyTdx 可用，停止 Mock 防止两种来源交叉覆盖同一标的。
            if state_callback is not None:
                state_callback(self.primary.status)
        callback = self._event_callback
        if callback is not None:
            callback(event)  # 切换完成后再交付首条恢复行情，Hub 不会先看到旧的 active 状态。

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

    mock = MockMarketFeed(interval_seconds=config.mock_interval_ms / 1_000)  # 配置毫秒换成线程 wait 使用的秒；Mock 始终可作为离线源。
    if config.source == "mock":
        return mock  # 安全默认模式完全不创建 TCP 连接，也不会探测 QuantFabric Bridge。
    pytdx = PyTdxJsonLineFeed(PyTdxFeedConfig(
        host=config.pytdx_host,
        port=config.pytdx_port,
        reconnect_seconds=config.reconnect_seconds,
    ))
    return FallbackMarketFeed(pytdx, mock, fallback_after_seconds=config.fallback_after_seconds)  # PyTdx 静默时兜底，但主源重连线程继续存活以支持自动恢复。
