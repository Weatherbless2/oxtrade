from __future__ import annotations

from concurrent.futures import Future
import unittest

from vnpy_oxapp.market.hub import MarketHub
from vnpy_oxapp.services.market_service import MarketService
from tests.python.test_market_hub import ManualFeed, event


class FallbackClient:
    def __init__(self) -> None:
        self.calls: list[dict] = []

    def request(self, **kwargs):
        self.calls.append(kwargs)
        future = Future()
        future.request_id = "fallback-request"
        future.set_result(None)
        return future


class MarketServiceTest(unittest.TestCase):
    def test_live_hub_uses_existing_future_response_shape_without_transport_request(self) -> None:
        client = FallbackClient()
        hub = MarketHub(ManualFeed())
        hub.publish(event(second=1, millisec=0, price=10.01, volume=100, turnover=1001))
        service = MarketService(client, market_hub=hub)
        response = service.list_quotes(page=1, page_size=20).result()
        self.assertTrue(response.is_success)
        self.assertEqual("000001.SZ", response.payload["items"][0]["symbol"])
        self.assertEqual([], client.calls)  # Hub 有数据时不得再穿过 MockBackendTransport，证明高频行情与低频 IPC 隔离。

    def test_missing_live_snapshot_falls_back_to_existing_backend_service(self) -> None:
        client = FallbackClient()
        service = MarketService(client, market_hub=MarketHub(ManualFeed()))
        future = service.list_quotes(page=1, page_size=20)
        self.assertEqual("fallback-request", future.request_id)
        self.assertEqual("market", client.calls[0]["msg_type"])  # 冷启动没有实时快照时仍回到既有 Gateway，页面不会永久空白。
        self.assertEqual("quotes", client.calls[0]["func_id"])

    def test_future_symbol_keeps_using_existing_gateway_when_stock_hub_is_live(self) -> None:
        client = FallbackClient()
        hub = MarketHub(ManualFeed())
        hub.publish(event(second=1, millisec=0, price=10.01, volume=100, turnover=1001))
        service = MarketService(client, market_hub=hub)
        future = service.list_ticks(query="IF2609.CFX", page=1, page_size=20)  # 股票 Hub 已有数据但故意查询期货，验证不同产品源不会互相遮蔽。
        self.assertEqual("fallback-request", future.request_id)
        self.assertEqual("ticks", client.calls[0]["func_id"])
