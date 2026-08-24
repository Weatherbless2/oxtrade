"""Mock/正式行情适配层的页面服务接口。"""

from __future__ import annotations

from concurrent.futures import Future
from typing import Any

from .base import BaseService
from ..message import ClientResponse


class MarketService(BaseService):
    """页面只调用行情语义；MockGateway 和未来 oxMWH/HP-Socket 可在传输层替换。"""

    def __init__(self, client, session_id="mock-session") -> None:
        super().__init__(client, "market", session_id)

    def subscribe(self, symbols: list[str]) -> Future[ClientResponse]:
        return self.page(operation="subscribe", payload={"symbols": symbols})  # Mock 订阅返回状态快照；正式版本改为注册推送流。

    def list_quotes(self, **filters: Any) -> Future[ClientResponse]:
        return self.page(operation="quotes", payload=filters)

    def list_ticks(self, **filters: Any) -> Future[ClientResponse]:
        return self.page(operation="ticks", payload=filters)

    def list_bars(self, **filters: Any) -> Future[ClientResponse]:
        return self.page(operation="bars", payload=filters)

    def list_depth(self, **filters: Any) -> Future[ClientResponse]:
        return self.page(operation="depth", payload=filters)
