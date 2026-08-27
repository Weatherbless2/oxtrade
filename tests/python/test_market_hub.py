from __future__ import annotations

from datetime import datetime
import unittest

from vnpy_oxapp.market.event import MarketEvent
from vnpy_oxapp.market.feed import FeedState, IMarketFeed, MarketFeedMetrics, MarketFeedStatus
from vnpy_oxapp.market.hub import MarketHub


class ManualFeed(IMarketFeed):
    """不创建网络线程的测试数据源，专注验证 Hub 的缓存语义。"""

    def __init__(self) -> None:
        self._status = MarketFeedStatus("manual", FeedState.STOPPED, "test", MarketFeedMetrics())
        self.subscriptions: list[str] = []

    @property
    def status(self) -> MarketFeedStatus:
        return self._status

    def start(self, event_callback, state_callback) -> None:
        self._status = MarketFeedStatus("manual", FeedState.CONNECTED, "test ready", MarketFeedMetrics())
        state_callback(self._status)

    def subscribe_symbols(self, symbols: list[str]) -> None:
        self.subscriptions.extend(symbols)

    def stop(self) -> None:
        self._status = MarketFeedStatus("manual", FeedState.STOPPED, "test stopped", MarketFeedMetrics())


def event(*, second: int, millisec: int, price: float, volume: int, turnover: float) -> MarketEvent:
    source_time = datetime(2026, 8, 27, 9, 30, second, millisec * 1_000)
    return MarketEvent(
        symbol="000001.SZ", ticker="000001", name="平安银行", exchange="SZSE", source="test",
        source_time=source_time, received_at=source_time, last_price=price, volume=volume, turnover=turnover,
        pre_close=10.0, open_price=10.0, high_price=price, low_price=price,
        bid_prices=(price - 0.01,) * 5, bid_volumes=(100,) * 5,
        ask_prices=(price + 0.01,) * 5, ask_volumes=(200,) * 5,
    )


class MarketHubTest(unittest.TestCase):
    def setUp(self) -> None:
        self.feed = ManualFeed()
        self.hub = MarketHub(self.feed, tick_capacity=10, bar_capacity=10)

    def test_duplicate_and_out_of_order_events_do_not_overwrite_new_snapshot(self) -> None:
        first = event(second=1, millisec=0, price=10.01, volume=100, turnover=1001)
        newest = event(second=2, millisec=0, price=10.02, volume=150, turnover=1502)
        self.assertTrue(self.hub.publish(first))
        self.assertFalse(self.hub.publish(first))  # 相同网络重发不重复入 Tick。
        self.assertTrue(self.hub.publish(newest))
        self.assertFalse(self.hub.publish(event(second=1, millisec=500, price=9.99, volume=140, turnover=1400)))
        quote = self.hub.quote_page({"page": 1, "page_size": 20})["items"][0]
        self.assertEqual("10.020", quote["last"])
        self.assertEqual({"accepted": 2, "duplicates": 1, "out_of_order": 1}, self.hub.metrics)

    def test_cumulative_turnover_is_differenced_into_one_minute_bar(self) -> None:
        self.hub.publish(event(second=1, millisec=0, price=10.01, volume=100, turnover=1001))
        self.hub.publish(event(second=2, millisec=0, price=10.03, volume=160, turnover=1603))
        bar = self.hub.bar_page({"query": "000001.SZ", "page": 1, "page_size": 20})["items"][0]
        self.assertEqual("10.010", bar["open"])
        self.assertEqual("10.030", bar["close"])
        self.assertEqual(60, bar["volume"])
        self.assertEqual("602.00", bar["amount"])

    def test_tick_buffer_has_a_fixed_upper_bound(self) -> None:
        for index in range(12):
            self.hub.publish(event(second=index + 1, millisec=0, price=10 + index / 100, volume=100 + index, turnover=1000 + index))
        ticks = self.hub.tick_page({"query": "000001.SZ", "page": 1, "page_size": 20})
        self.assertEqual(10, ticks["total"])
        self.assertEqual("10.110", ticks["items"][0]["last"])

    def test_cumulative_counter_reset_starts_a_new_increment_instead_of_going_negative(self) -> None:
        self.hub.publish(event(second=1, millisec=0, price=10.01, volume=500, turnover=5000))
        self.hub.publish(event(second=2, millisec=0, price=10.02, volume=30, turnover=301))
        bar = self.hub.bar_page({"query": "000001.SZ", "page": 1, "page_size": 20})["items"][0]
        self.assertEqual(30, bar["volume"])
        self.assertEqual("301.00", bar["amount"])
