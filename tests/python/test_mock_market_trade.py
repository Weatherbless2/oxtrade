from __future__ import annotations

from pathlib import Path
import unittest

from vnpy_oxapp.connection import ClientProtocolClient
from vnpy_oxapp.mock.mock_backend_transport import MockBackendTransport, MockBackendTransportConfig


def backend_path() -> Path:
    return Path(__file__).resolve().parents[2] / "build" / "bin" / "oxMbsMockBackend"


@unittest.skipUnless(backend_path().is_file(), "build oxMbsMockBackend before running Mock market/trade tests")
class MockMarketTradeTest(unittest.TestCase):
    def setUp(self) -> None:
        self.transport = MockBackendTransport(MockBackendTransportConfig(backend_path=backend_path()))
        self.client = ClientProtocolClient(self.transport)
        self.assertTrue(self.client.connect())

    def tearDown(self) -> None:
        self.client.disconnect()

    def request(self, service: str, operation: str, payload: dict | None = None):
        return self.client.request(
            session_id="mock-session", msg_type=service, func_id=operation, payload=payload or {}, timeout_ms=3000,
        ).result(timeout=2)

    def test_market_views_and_subscription_use_mock_gateway(self) -> None:
        quotes = self.request("market", "quotes", {"page": 1, "page_size": 20})
        self.assertTrue(quotes.is_success)
        self.assertGreaterEqual(quotes.payload["total"], 8)
        self.assertEqual("000001.SZ", quotes.payload["items"][0]["symbol"])

        ticks = self.request("market", "ticks", {"symbol": "000001.SZ", "page": 1, "page_size": 5})
        bars = self.request("market", "bars", {"symbol": "000001.SZ", "page": 1, "page_size": 5})
        depth = self.request("market", "depth", {"symbol": "000001.SZ", "page": 1, "page_size": 5})
        self.assertEqual(5, len(ticks.payload["items"]))
        self.assertEqual(5, len(bars.payload["items"]))
        self.assertIn("bid1_price", depth.payload["items"][0])
        subscribed = self.request("market", "subscribe", {"symbols": ["000001.SZ"]})
        self.assertEqual("subscribed", subscribed.payload["status"])

    def test_order_cancel_releases_frozen_cash(self) -> None:
        before = self.request("query", "page", {"resource": "accounts", "query": "MOCK-ACCT-001"})
        available_before = float(before.payload["items"][0]["available"])
        placed = self.request("trade", "place_order", {
            "account_id": "MOCK-ACCT-001", "symbol": "000001.SZ", "side": "buy", "price": 10.28, "volume": 100,
        })
        self.assertTrue(placed.is_success)
        order_id = placed.payload["order"]["order_id"]
        during = self.request("query", "page", {"resource": "accounts", "query": "MOCK-ACCT-001"})
        self.assertLess(float(during.payload["items"][0]["available"]), available_before)
        cancelled = self.request("trade", "cancel_order", {"order_id": order_id})
        self.assertTrue(cancelled.is_success)
        after = self.request("query", "page", {"resource": "accounts", "query": "MOCK-ACCT-001"})
        self.assertAlmostEqual(available_before, float(after.payload["items"][0]["available"]), places=2)

    def test_fill_creates_trade_and_updates_position(self) -> None:
        placed = self.request("trade", "place_order", {
            "account_id": "MOCK-ACCT-001", "symbol": "000001.SZ", "side": "buy", "price": 10.28, "volume": 100,
        })
        order_id = placed.payload["order"]["order_id"]
        filled = self.request("trade", "fill_order", {"order_id": order_id})
        self.assertTrue(filled.is_success)
        self.assertEqual("filled", filled.payload["order"]["status"])
        self.assertEqual(order_id, filled.payload["trade"]["order_id"])
        trades = self.request("query", "page", {"resource": "trades", "query": order_id})
        positions = self.request("query", "page", {"resource": "positions", "query": "000001.SZ"})
        self.assertEqual(1, trades.payload["total"])
        self.assertEqual("12100", positions.payload["items"][0]["total_volume"])
        self.assertEqual("100", positions.payload["items"][0]["today_volume"])

    def test_mock_risk_rejects_bad_volume(self) -> None:
        response = self.request("trade", "place_order", {
            "account_id": "MOCK-ACCT-001", "symbol": "000001.SZ", "side": "buy", "price": 10.28, "volume": 101,
        })
        self.assertFalse(response.is_success)
        self.assertEqual("validation_error", response.error_code.value)
