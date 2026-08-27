from __future__ import annotations

from threading import Event
from time import monotonic, sleep
import unittest

from vnpy_oxapp.market.event import MarketEvent
from vnpy_oxapp.market.feed import FeedState, IMarketFeed, MarketFeedMetrics, MarketFeedStatus
from vnpy_oxapp.market.runtime import FallbackMarketFeed
from tests.python.test_market_hub import event


class ControlledFeed(IMarketFeed):
    def __init__(self, source: str) -> None:
        self.source = source
        self.started = Event()
        self.stopped = Event()
        self._status = MarketFeedStatus(source, FeedState.STOPPED, "test", MarketFeedMetrics())
        self._event_callback = None
        self._state_callback = None
        self.subscriptions: list[str] = []

    @property
    def status(self) -> MarketFeedStatus:
        return self._status

    def start(self, event_callback, state_callback) -> None:
        self._event_callback, self._state_callback = event_callback, state_callback
        self._status = MarketFeedStatus(self.source, FeedState.CONNECTED, "test connected", MarketFeedMetrics())
        self.started.set()
        state_callback(self._status)

    def subscribe_symbols(self, symbols: list[str]) -> None:
        self.subscriptions.extend(symbols)

    def stop(self) -> None:
        self.stopped.set()
        self._status = MarketFeedStatus(self.source, FeedState.STOPPED, "test stopped", MarketFeedMetrics())

    def emit(self, value: MarketEvent) -> None:
        assert self._event_callback is not None
        self._event_callback(value)


class FallbackMarketFeedTest(unittest.TestCase):
    def wait_until(self, predicate, timeout: float = 1.0) -> bool:
        deadline = monotonic() + timeout
        while monotonic() < deadline:
            if predicate():
                return True
            sleep(0.01)
        return predicate()

    def test_mock_starts_after_primary_silence_then_stops_when_primary_recovers(self) -> None:
        primary = ControlledFeed("pytdx")
        fallback = ControlledFeed("mock")
        feed = FallbackMarketFeed(primary, fallback, fallback_after_seconds=0.05)
        events = []
        feed.subscribe_symbols(["000001.SZ"])
        feed.start(events.append, lambda status: None)
        try:
            self.assertTrue(fallback.started.wait(1.0))
            self.assertEqual(["000001.SZ"], primary.subscriptions)
            self.assertEqual(["000001.SZ"], fallback.subscriptions)
            fallback.emit(event(second=1, millisec=0, price=10.01, volume=100, turnover=1001))
            primary.emit(event(second=2, millisec=0, price=10.02, volume=150, turnover=1502))
            self.assertTrue(self.wait_until(fallback.stopped.is_set))
            self.assertEqual([10.01, 10.02], [market_event.last_price for market_event in events])
            self.assertEqual("pytdx", feed.status.source)
        finally:
            feed.stop()

    def test_primary_stall_after_a_live_event_restarts_mock_fallback(self) -> None:
        primary = ControlledFeed("pytdx")
        fallback = ControlledFeed("mock")
        feed = FallbackMarketFeed(primary, fallback, fallback_after_seconds=0.05)
        feed.start(lambda market_event: None, lambda status: None)
        try:
            primary.emit(event(second=1, millisec=0, price=10.01, volume=100, turnover=1001))
            self.assertTrue(self.wait_until(fallback.started.is_set))
            self.assertEqual("mock", feed.status.source)
        finally:
            feed.stop()
