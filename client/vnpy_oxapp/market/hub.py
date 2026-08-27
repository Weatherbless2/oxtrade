"""行情缓存、去重、乱序保护与分钟 K 线聚合。"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass
from datetime import datetime
from threading import RLock
from typing import Any, Callable

from .event import MarketEvent
from .feed import IMarketFeed, MarketFeedStatus


@dataclass(slots=True)
class _MinuteBar:
    minute: datetime
    symbol: str
    open_price: float
    high_price: float
    low_price: float
    close_price: float
    volume: int
    amount: float


HubEventCallback = Callable[[MarketEvent], None]
HubStateCallback = Callable[[MarketFeedStatus], None]


class MarketHub:
    """将高频事件归并为线程安全快照；Qt 页面只能通过 Service 读取这些快照。"""

    def __init__(self, feed: IMarketFeed, *, tick_capacity: int = 500, bar_capacity: int = 480, ui_refresh_ms: int = 200) -> None:
        self.feed = feed
        self.tick_capacity = max(10, tick_capacity)
        self.bar_capacity = max(10, bar_capacity)
        self.ui_refresh_ms = max(50, ui_refresh_ms)
        self._lock = RLock()
        self._latest: dict[str, MarketEvent] = {}
        self._ticks: dict[str, deque[MarketEvent]] = {}
        self._bars: dict[str, deque[_MinuteBar]] = {}
        self._cumulative: dict[str, tuple[int, float]] = {}
        self._event_callbacks: dict[int, HubEventCallback] = {}
        self._state_callbacks: dict[int, HubStateCallback] = {}
        self._callback_id = 0
        self._sequence = 0
        self._accepted = 0
        self._duplicates = 0
        self._out_of_order = 0
        self._feed_status = feed.status

    @property
    def has_data(self) -> bool:
        with self._lock:
            return bool(self._latest)

    def has_symbol_data(self, symbol: str) -> bool:
        """判断某个标的是否已有本机实时快照，股票缓存不能覆盖期货等其他 Mock 视图。"""

        with self._lock:
            return symbol.strip().upper() in self._latest

    @property
    def feed_status(self) -> MarketFeedStatus:
        with self._lock:
            return self._feed_status

    @property
    def metrics(self) -> dict[str, int]:
        with self._lock:
            return {"accepted": self._accepted, "duplicates": self._duplicates, "out_of_order": self._out_of_order}

    def start(self) -> None:
        self.feed.start(self.publish, self._on_feed_state)  # Feed 线程仅调用 Hub，不知道 Qt，便于替换成 C++ SDK/行情商实现。

    def stop(self) -> None:
        self.feed.stop()

    def subscribe_symbols(self, symbols: list[str]) -> None:
        self.feed.subscribe_symbols(symbols)  # 订阅语义由 Feed 处理，Hub 只管理接收后的标准化快照。

    def add_event_listener(self, callback: HubEventCallback) -> Callable[[], None]:
        with self._lock:
            self._callback_id += 1
            callback_id = self._callback_id
            self._event_callbacks[callback_id] = callback

        def unsubscribe() -> None:
            with self._lock:
                self._event_callbacks.pop(callback_id, None)

        return unsubscribe

    def add_state_listener(self, callback: HubStateCallback) -> Callable[[], None]:
        with self._lock:
            self._callback_id += 1
            callback_id = self._callback_id
            self._state_callbacks[callback_id] = callback
            status = self._feed_status
        callback(status)  # 立即交付当前状态，避免页面晚订阅后永久显示“未知”。

        def unsubscribe() -> None:
            with self._lock:
                self._state_callbacks.pop(callback_id, None)

        return unsubscribe

    def publish(self, event: MarketEvent) -> bool:
        """接收一个标准事件；重复/乱序事件不覆盖最新快照也不触发 UI 刷新。"""

        with self._lock:
            previous = self._latest.get(event.symbol)
            if previous is not None:
                if event.source_time.date() == previous.source_time.date() and event.source_timestamp_ms < previous.source_timestamp_ms:
                    self._out_of_order += 1
                    return False  # 同交易日更早的数据到达时直接丢弃，防止盘口和 K 线“倒退”。
                if event.fingerprint == previous.fingerprint:
                    self._duplicates += 1
                    return False  # 完全重复的网络重发不能占满 Tick 缓冲或导致 UI 高频无效重绘。
            self._sequence += 1
            accepted = event.with_sequence(self._sequence)
            self._latest[accepted.symbol] = accepted
            self._ticks.setdefault(accepted.symbol, deque(maxlen=self.tick_capacity)).append(accepted)  # deque 的 maxlen 提供硬上限，长时间运行不会无界占用内存。
            self._update_bar(accepted)
            self._accepted += 1
            callbacks = tuple(self._event_callbacks.values())
        for callback in callbacks:
            callback(accepted)  # 锁外发布，慢 UI/日志订阅者不能阻塞下一条行情入缓存。
        return True

    def quote_page(self, filters: dict[str, Any]) -> dict[str, Any]:
        with self._lock:
            rows = [self._quote_row(event) for event in self._latest.values()]
        return self._page(rows, filters, sort_key="symbol")

    def tick_page(self, filters: dict[str, Any]) -> dict[str, Any]:
        symbol = str(filters.get("symbol") or filters.get("query") or "").upper()
        with self._lock:
            events = list(self._ticks.get(symbol, ())) if symbol else [event for ticks in self._ticks.values() for event in ticks]
            rows = [self._tick_row(event) for event in reversed(events)]
        return self._page(rows, filters, sort_key="datetime", reverse=True)

    def bar_page(self, filters: dict[str, Any]) -> dict[str, Any]:
        symbol = str(filters.get("symbol") or filters.get("query") or "").upper()
        with self._lock:
            bars = list(self._bars.get(symbol, ())) if symbol else [bar for records in self._bars.values() for bar in records]
            rows = [self._bar_row(bar) for bar in reversed(bars)]
        return self._page(rows, filters, sort_key="datetime", reverse=True)

    def depth_page(self, filters: dict[str, Any]) -> dict[str, Any]:
        symbol = str(filters.get("symbol") or filters.get("query") or "").upper()
        with self._lock:
            events = [self._latest[symbol]] if symbol in self._latest else list(self._latest.values())
            rows = [self._depth_row(event) for event in events]
        return self._page(rows, filters, sort_key="symbol")

    def _on_feed_state(self, status: MarketFeedStatus) -> None:
        with self._lock:
            self._feed_status = status
            callbacks = tuple(self._state_callbacks.values())
        for callback in callbacks:
            callback(status)  # 状态监听者和行情监听者都在锁外执行，状态变化不能卡住行情接收。

    def _update_bar(self, event: MarketEvent) -> None:
        minute = event.source_time.replace(second=0, microsecond=0)
        last_volume, last_turnover = self._cumulative.get(event.symbol, (event.volume, event.turnover))
        volume_delta = event.volume - last_volume if event.volume >= last_volume else event.volume
        turnover_delta = event.turnover - last_turnover if event.turnover >= last_turnover else event.turnover
        # 累计值回退通常是跨交易日/源重启，按新累计值作为首段增量，不能用 0 抹掉新时段真实成交。
        self._cumulative[event.symbol] = (event.volume, event.turnover)
        bars = self._bars.setdefault(event.symbol, deque(maxlen=self.bar_capacity))
        if not bars or bars[-1].minute != minute:
            bars.append(_MinuteBar(minute, event.symbol, event.last_price, event.last_price, event.last_price, event.last_price, volume_delta, turnover_delta))
            return
        current = bars[-1]
        current.high_price = max(current.high_price, event.last_price)
        current.low_price = min(current.low_price, event.last_price)
        current.close_price = event.last_price
        current.volume += volume_delta
        current.amount += turnover_delta

    @staticmethod
    def _quote_row(event: MarketEvent) -> dict[str, Any]:
        change_percent = 0.0 if event.pre_close <= 0 else (event.last_price - event.pre_close) / event.pre_close * 100
        return {
            "symbol": event.symbol, "name": event.name, "market": "上交所" if event.exchange == "SSE" else "深交所",
            "last": f"{event.last_price:.3f}", "change_percent": f"{change_percent:.2f}%", "volume": event.volume,
            "amount": f"{event.turnover:.2f}", "update_time": event.update_time, "source": event.source,
        }

    @staticmethod
    def _tick_row(event: MarketEvent) -> dict[str, Any]:
        return {
            "datetime": event.source_time.strftime("%Y-%m-%d %H:%M:%S.%f")[:-3], "symbol": event.symbol, "name": event.name,
            "last": f"{event.last_price:.3f}", "volume": event.volume, "turnover": f"{event.turnover:.2f}",
            "bid1": f"{event.bid_prices[0]:.3f}", "ask1": f"{event.ask_prices[0]:.3f}", "open_interest": "--",
        }

    @staticmethod
    def _bar_row(bar: _MinuteBar) -> dict[str, Any]:
        return {
            "datetime": bar.minute.strftime("%Y-%m-%d %H:%M"), "symbol": bar.symbol,
            "open": f"{bar.open_price:.3f}", "high": f"{bar.high_price:.3f}", "low": f"{bar.low_price:.3f}",
            "close": f"{bar.close_price:.3f}", "volume": bar.volume, "amount": f"{bar.amount:.2f}",
        }

    @staticmethod
    def _depth_row(event: MarketEvent) -> dict[str, Any]:
        row: dict[str, Any] = {"symbol": event.symbol, "name": event.name}
        for index in range(2):
            row[f"bid{index + 1}_price"] = f"{event.bid_prices[index]:.3f}"
            row[f"bid{index + 1}_volume"] = event.bid_volumes[index]
            row[f"ask{index + 1}_price"] = f"{event.ask_prices[index]:.3f}"
            row[f"ask{index + 1}_volume"] = event.ask_volumes[index]
        return row

    @staticmethod
    def _page(rows: list[dict[str, Any]], filters: dict[str, Any], *, sort_key: str, reverse: bool = False) -> dict[str, Any]:
        query = str(filters.get("query", "")).strip().lower()
        if query:
            rows = [row for row in rows if query in " ".join(str(value) for value in row.values()).lower()]
        rows.sort(key=lambda row: str(row.get(sort_key, "")), reverse=reverse)
        total = len(rows)
        page_size = max(1, int(filters.get("page_size", 20) or 20))
        page = max(1, int(filters.get("page", 1) or 1))
        start = (page - 1) * page_size
        return {"items": rows[start:start + page_size], "page": page, "page_size": page_size, "total": total}
