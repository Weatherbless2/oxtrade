"""Mock/正式行情适配层的页面服务接口。"""

from __future__ import annotations

from concurrent.futures import Future
from typing import Any
from uuid import uuid4

from .base import BaseService
from ..market.event import split_symbol
from ..market.hub import MarketHub
from ..message import ClientResponse, MessageStatus


class MarketService(BaseService):
    """页面只调用行情语义；MockGateway 和未来 oxMWH/HP-Socket 可在传输层替换。"""

    def __init__(self, client, session_id="mock-session", market_hub: MarketHub | None = None) -> None:
        super().__init__(client, "market", session_id)
        self.market_hub = market_hub  # 有实时快照时查询本机 Hub，尚未收到行情时继续走 #24 Mock Gateway 回退。

    def subscribe(self, symbols: list[str]) -> Future[ClientResponse]:
        if self.market_hub is not None and all(self._is_pytdx_symbol(symbol) for symbol in symbols):
            self.market_hub.subscribe_symbols(symbols)
            status = self.market_hub.feed_status
            return self._cache_response("subscribe", {"status": "subscribed", "symbols": symbols, "source": status.source})
        return self.page(operation="subscribe", payload={"symbols": symbols})  # Mock 订阅返回状态快照；正式版本改为注册推送流。

    def list_quotes(self, **filters: Any) -> Future[ClientResponse]:
        if self.market_hub is not None and self.market_hub.has_data:
            return self._cache_response("quotes", self.market_hub.quote_page(filters))
        return self.page(operation="quotes", payload=filters)

    def list_ticks(self, **filters: Any) -> Future[ClientResponse]:
        if self._can_read_hub(filters):
            return self._cache_response("ticks", self.market_hub.tick_page(filters))
        return self.page(operation="ticks", payload=filters)

    def list_bars(self, **filters: Any) -> Future[ClientResponse]:
        if self._can_read_hub(filters):
            return self._cache_response("bars", self.market_hub.bar_page(filters))
        return self.page(operation="bars", payload=filters)

    def list_depth(self, **filters: Any) -> Future[ClientResponse]:
        if self._can_read_hub(filters):
            return self._cache_response("depth", self.market_hub.depth_page(filters))
        return self.page(operation="depth", payload=filters)

    def _cache_response(self, operation: str, payload: dict[str, Any]) -> Future[ClientResponse]:
        """把本地快照适配为既有 Future/ClientResponse 形状，页面无需区分 Socket 与缓存来源。"""

        request_id = f"market-cache-{uuid4().hex}"
        future: Future[ClientResponse] = Future()
        future.request_id = request_id  # ServiceTablePage 仍按统一 request_id 消费，不绕过异步页面关联模型。
        future.set_result(ClientResponse(
            request_id=request_id, session_id=self.current_session_id, msg_type="market", func_id=operation,
            status=MessageStatus.SUCCEEDED, payload=payload,
        ))
        return future

    def _can_read_hub(self, filters: dict[str, Any]) -> bool:
        if self.market_hub is None or not self.market_hub.has_data:
            return False
        symbol = str(filters.get("symbol") or filters.get("query") or "").strip().upper()
        return not symbol or self.market_hub.has_symbol_data(symbol)

    @staticmethod
    def _is_pytdx_symbol(symbol: str) -> bool:
        try:
            split_symbol(symbol)
        except ValueError:
            return False  # IF/IC 等期货不属于 PyTdx 股票 Bridge，继续由原 Mock Gateway/未来正式 SDK 处理。
        return True
