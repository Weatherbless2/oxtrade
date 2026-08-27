"""离线可运行的 Mock 行情源，用于 PyTdx Bridge 不可用时的开发和页面验收。"""

from __future__ import annotations

from datetime import datetime
from threading import Event, RLock, Thread, current_thread
from typing import Any

from .event import MarketEvent, split_symbol
from .feed import EventCallback, FeedState, IMarketFeed, MarketFeedMetrics, MarketFeedStatus, StateCallback


_DEFAULT_QUOTES: dict[str, dict[str, Any]] = {
    "000001.SZ": {"name": "平安银行", "price": 10.28, "pre_close": 10.12},
    "600000.SH": {"name": "浦发银行", "price": 9.45, "pre_close": 9.39},
    "600519.SH": {"name": "贵州茅台", "price": 1462.50, "pre_close": 1450.00},
    "000300.SZ": {"name": "沪深300", "price": 3850.40, "pre_close": 3825.00},
    "510300.SH": {"name": "300ETF", "price": 3.852, "pre_close": 3.826},
    "159915.SZ": {"name": "创业板ETF", "price": 1.935, "pre_close": 1.921},
}


class MockMarketFeed(IMarketFeed):
    """生成小幅确定性波动的累计行情；仅用于演示，不能作为真实风控或交易依据。"""

    def __init__(self, *, interval_seconds: float = 0.8) -> None:
        self._interval_seconds = max(0.05, interval_seconds)
        self._lock = RLock()
        self._stop_event = Event()
        self._thread: Thread | None = None
        self._event_callback: EventCallback | None = None
        self._state_callback: StateCallback | None = None
        self._subscriptions: set[str] = set(_DEFAULT_QUOTES)
        self._tick_count: dict[str, int] = {symbol: 0 for symbol in _DEFAULT_QUOTES}
        self._status = MarketFeedStatus("mock", FeedState.STOPPED, "Mock market feed has not started", MarketFeedMetrics())

    @property
    def status(self) -> MarketFeedStatus:
        with self._lock:
            return self._status

    def start(self, event_callback: EventCallback, state_callback: StateCallback) -> None:
        with self._lock:
            if self._thread is not None and self._thread.is_alive():
                return
            self._event_callback, self._state_callback = event_callback, state_callback
            self._stop_event.clear()
            self._thread = Thread(target=self._run, name="oxapp-mock-market", daemon=True)
            thread = self._thread
        self._set_status(FeedState.CONNECTED, "Mock market feed is active")
        thread.start()

    def subscribe_symbols(self, symbols: list[str]) -> None:
        with self._lock:
            for symbol in symbols:
                normalized = symbol.strip().upper()
                try:
                    split_symbol(normalized)
                except ValueError:
                    continue  # Mock 与 PyTdx 当前只覆盖沪深六码标的，期货行情由正式 SDK 后续接入。
                self._subscriptions.add(normalized)
                self._tick_count.setdefault(normalized, 0)

    def stop(self) -> None:
        self._stop_event.set()
        with self._lock:
            thread = self._thread
        if thread is not None and thread is not current_thread() and thread.is_alive():
            thread.join(timeout=max(1.0, self._interval_seconds * 2))
        with self._lock:
            self._thread = None
        self._set_status(FeedState.STOPPED, "Mock market feed stopped")

    def emit_once(self) -> None:
        """测试和离线演示可手工生成一轮行情，不需要等待后台轮询周期。"""

        with self._lock:
            symbols = tuple(sorted(self._subscriptions))
        for symbol in symbols:
            event = self._build_event(symbol)
            callback = self._event_callback
            if callback is not None:
                callback(event)
            with self._lock:
                metrics = self._status.metrics
                self._status = MarketFeedStatus(
                    "mock", self._status.state, self._status.reason,
                    MarketFeedMetrics(metrics.delivered + 1, metrics.reconnects, metrics.bad_payloads),
                )

    def _run(self) -> None:
        while not self._stop_event.is_set():
            self.emit_once()
            self._stop_event.wait(self._interval_seconds)  # Event.wait 可被 stop 立即唤醒，避免退出时卡住完整轮询周期。

    def _build_event(self, symbol: str) -> MarketEvent:
        now = datetime.now()
        ticker, exchange = split_symbol(symbol)
        seed = _DEFAULT_QUOTES.get(symbol, {"name": symbol, "price": 10.0, "pre_close": 10.0})
        with self._lock:
            count = self._tick_count.get(symbol, 0) + 1
            self._tick_count[symbol] = count
        unit = 0.001 if float(seed["price"]) < 20 else 0.01
        offset = ((count % 9) - 4) * unit
        last = round(float(seed["price"]) + offset, 3)
        volume = 10_000 + count * 100
        payload = {
            "type": "stock_quote", "ticker": ticker, "name": seed["name"], "exchange": exchange,
            "update_time": now.strftime("%H:%M:%S"), "millisec": now.microsecond // 1_000,
            "last_price": last, "volume": volume, "turnover": round(last * volume, 2),
            "pre_close": seed["pre_close"], "open": seed["price"], "high": max(last, float(seed["price"]) + 0.02),
            "low": min(last, float(seed["price"]) - 0.02),
            "bid_prices": [round(last - unit * level, 3) for level in range(1, 6)],
            "bid_volumes": [100 * level for level in range(1, 6)],
            "ask_prices": [round(last + unit * level, 3) for level in range(1, 6)],
            "ask_volumes": [120 * level for level in range(1, 6)], "source": "mock",
        }
        return MarketEvent.from_bridge_payload(payload, received_at=now)

    def _set_status(self, state: FeedState, reason: str) -> None:
        with self._lock:
            metrics = self._status.metrics
            self._status = MarketFeedStatus("mock", state, reason, metrics)
            status = self._status
            callback = self._state_callback
        if callback is not None:
            callback(status)  # 锁外通知，页面状态回调不应阻塞 Mock 行情生成线程。
