"""Mock/正式交易与查询核心的页面服务接口。"""

from __future__ import annotations

from concurrent.futures import Future
from typing import Any

from .base import BaseService
from ..message import ClientResponse


class TradeService(BaseService):
    """交易命令走 trade，资金/持仓/历史查询走 query，保持最终架构中的核心隔离。"""

    def __init__(self, client, session_id="mock-session") -> None:
        super().__init__(client, "trade", session_id)

    def list_accounts(self, **filters: Any) -> Future[ClientResponse]:
        return self.client.request(session_id=self.current_session_id, msg_type="query", func_id="page", payload={"resource": "accounts", **filters})

    def list_positions(self, **filters: Any) -> Future[ClientResponse]:
        return self.client.request(session_id=self.current_session_id, msg_type="query", func_id="page", payload={"resource": "positions", **filters})

    def list_orders(self, **filters: Any) -> Future[ClientResponse]:
        return self.client.request(session_id=self.current_session_id, msg_type="query", func_id="page", payload={"resource": "orders", **filters})

    def list_trades(self, **filters: Any) -> Future[ClientResponse]:
        return self.client.request(session_id=self.current_session_id, msg_type="query", func_id="page", payload={"resource": "trades", **filters})

    def place_order(self, *, account_id: str, symbol: str, name: str, side: str, price: float, volume: int, order_type: str = "限价") -> Future[ClientResponse]:
        return self.page(operation="place_order", payload={
            "account_id": account_id, "symbol": symbol, "name": name, "side": side, "price": price, "volume": volume, "order_type": order_type,
        })  # 下单只返回 Mock 接受/风控拒绝结果，绝不表示请求已发送至任何真实柜台。

    def cancel_order(self, order_id: str, _state: str = "") -> Future[ClientResponse]:
        return self.page(operation="cancel_order", payload={"order_id": order_id})

    def fill_order(self, order_id: str) -> Future[ClientResponse]:
        return self.page(operation="fill_order", payload={"order_id": order_id})  # 演示撮合回报，正式环境由柜台异步成交回报替换。
