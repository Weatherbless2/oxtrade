from __future__ import annotations

import struct
import unittest
from datetime import datetime

from vnpy_oxapp.market.market_wire_feed import HEADER_SIZE, MAGIC, MarketWireFeed


def frame(sequence: int, symbol: str = "600519.SH", price: float = 1450.0) -> bytes:
    payload = bytearray(struct.pack("<dQd", price, 1000, price * 1000))
    for index in range(5):
        payload.extend(struct.pack("<dQ", price - index * 0.1, 100 + index))
    for index in range(5):
        payload.extend(struct.pack("<dQ", price + index * 0.1, 200 + index))
    instrument = symbol.encode("ascii")
    exchange = b"SSE"
    header = struct.pack(
        "<IHHIHHQQQQIHHI",
        MAGIC, 1, HEADER_SIZE, HEADER_SIZE + len(payload), 1, 0,
        0, 0, sequence, int(datetime.now().timestamp() * 1_000_000_000), len(payload),
        len(instrument), len(exchange), 0,
    )
    return header + instrument.ljust(32, b"\0") + exchange.ljust(8, b"\0") + payload


class MarketWireFeedTest(unittest.TestCase):
    def test_half_sticky_subscribed_and_sequence_guard(self) -> None:
        feed = MarketWireFeed()
        events = []
        feed.start(events.append, lambda _: None)
        feed.subscribe_symbols(["600519.SH"])
        first = frame(1)
        self.assertTrue(feed.feed_bytes(first[:9]))
        self.assertEqual([], events)
        self.assertTrue(feed.feed_bytes(first[9:] + frame(1) + frame(2, price=1451.0)))
        self.assertEqual([1450.0, 1451.0], [event.last_price for event in events])
        self.assertEqual(2, feed.status.metrics.delivered)
        self.assertEqual(1, feed.status.metrics.bad_payloads)

    def test_bad_magic_and_unsubscribed_are_rejected(self) -> None:
        feed = MarketWireFeed()
        events = []
        feed.start(events.append, lambda _: None)
        feed.subscribe_symbols(["000001.SZ"])
        bad = bytearray(frame(1))
        bad[0] ^= 1
        self.assertFalse(feed.feed_bytes(bad))
        self.assertEqual(1, feed.status.metrics.bad_payloads)
        self.assertTrue(feed.feed_bytes(frame(2)))
        self.assertEqual([], events)


if __name__ == "__main__":
    unittest.main()
