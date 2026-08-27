from __future__ import annotations

from datetime import datetime
import unittest

from vnpy_oxapp.market.event import MarketEvent, MarketPayloadError, split_symbol


def quote_payload(**changes):
    payload = {
        "type": "stock_quote", "ticker": "600519", "name": "贵州茅台", "exchange": "SSE",
        "update_time": "09:30:01", "millisec": 123, "last_price": 1462.5, "volume": 120000,
        "turnover": 175500000, "pre_close": 1450, "open": 1451, "high": 1465, "low": 1448,
        "bid_prices": [1462.49, 1462.48], "bid_volumes": [100, 200],
        "ask_prices": [1462.50, 1462.51], "ask_volumes": [300, 400], "source": "pytdx",
    }
    payload.update(changes)
    return payload


class MarketEventTest(unittest.TestCase):
    def test_bridge_payload_is_normalized_to_internal_symbol_and_five_levels(self) -> None:
        event = MarketEvent.from_bridge_payload(quote_payload(), received_at=datetime(2026, 8, 27, 10, 0, 0))
        self.assertEqual("600519.SH", event.symbol)
        self.assertEqual("09:30:01.123", event.update_time)
        self.assertEqual((1462.49, 1462.48, 0.0, 0.0, 0.0), event.bid_prices)
        self.assertEqual((300, 400, 0, 0, 0), event.ask_volumes)

    def test_invalid_type_and_negative_values_are_rejected_before_reaching_hub(self) -> None:
        with self.assertRaises(MarketPayloadError):
            MarketEvent.from_bridge_payload(quote_payload(type="subscription_added"))
        with self.assertRaises(MarketPayloadError):
            MarketEvent.from_bridge_payload(quote_payload(volume=-1))

    def test_bridge_subscription_symbol_conversion_only_accepts_supported_a_shares(self) -> None:
        self.assertEqual(("000001", "SZSE"), split_symbol("000001.SZ"))
        self.assertEqual(("600519", "SSE"), split_symbol("600519.SH"))
        with self.assertRaises(ValueError):
            split_symbol("IF2609.CFX")
