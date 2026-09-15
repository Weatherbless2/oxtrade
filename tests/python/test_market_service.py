from __future__ import annotations

from concurrent.futures import Future
import unittest

from vnpy_oxapp.market.hub import MarketHub
from vnpy_oxapp.hpsocket_transport import HpSocketTransport
from vnpy_oxapp.message import MessageStatus
from vnpy_oxapp.services.market_service import MarketService
from tests.python.test_market_hub import ManualFeed, event


class FallbackClient:
    def __init__(self) -> None:
        self.calls: list[dict] = []
        self.transport = object()

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

    def test_formal_transport_routes_quote_latest_to_qmt_protocol(self) -> None:
        client = FallbackClient()
        client.transport = HpSocketTransport()
        service = MarketService(client)
        future = service.list_quotes(query="688693.SH", page=3, page_size=20, sort="receive_time", descending=True)
        self.assertEqual("quote", client.calls[0]["msg_type"])
        self.assertEqual("latest", client.calls[0]["func_id"])
        self.assertEqual("1", client.calls[0]["payload"]["data"]["market"])
        self.assertEqual("688693", client.calls[0]["payload"]["data"]["stkcode"])
        self.assertEqual(1, client.calls[0]["payload"]["schema_version"])
        self.assertEqual(20, client.calls[0]["payload"]["page"]["page_size"])
        self.assertEqual("v1:40", client.calls[0]["payload"]["page"]["page_token"])
        del future

    def test_formal_transport_empty_query_requests_full_qmt_page(self) -> None:
        client = FallbackClient()
        client.transport = HpSocketTransport()
        service = MarketService(client)
        future = service.list_quotes(page=1, page_size=20)
        self.assertEqual("", client.calls[0]["payload"]["data"]["market"])
        self.assertEqual("", client.calls[0]["payload"]["data"]["stkcode"])
        self.assertEqual(20, client.calls[0]["payload"]["page"]["page_size"])
        self.assertEqual("receive_time", client.calls[0]["payload"]["page"]["sort"])
        del future

    def test_formal_transport_preserves_market_scope_and_keyword_for_real_workbench(self) -> None:
        client = FallbackClient()
        client.transport = HpSocketTransport()
        service = MarketService(client)
        future = service.list_quotes(query="F:IF2609", page=2, page_size=20)
        data = client.calls[0]["payload"]["data"]
        self.assertEqual("F", data["market"])
        self.assertEqual("IF2609", data["stkcode"])
        self.assertEqual("v1:20", client.calls[0]["payload"]["page"]["page_token"])
        del future

    def test_formal_transport_does_not_fallback_to_mock_for_unavailable_tables(self) -> None:
        client = FallbackClient()
        client.transport = HpSocketTransport()
        service = MarketService(client)
        response = service.list_ticks(query="688693.SH").result()
        self.assertIs(MessageStatus.FAILED, response.status)
        self.assertIn("没有历史 Tick 表", response.error_message)
        self.assertEqual([], client.calls)
