"""Mock/正式行情适配层的页面服务接口。"""

from __future__ import annotations

from concurrent.futures import Future
from typing import Any
from uuid import uuid4

from .base import BaseService
from ..hpsocket_transport import HpSocketTransport
from ..market.event import split_symbol
from ..market.hub import MarketHub
from ..message import ClientResponse, ErrorCode, MessageStatus


class MarketService(BaseService):
    """页面只调用行情语义；MockGateway 和未来 oxMWH/HP-Socket 可在传输层替换。"""

    def __init__(self, client, session_id="mock-session", market_hub: MarketHub | None = None) -> None:
        super().__init__(client, "market", session_id)
        self.market_hub = market_hub  # 有实时快照时查询本机 Hub，尚未收到行情时继续走 #24 Mock Gateway 回退。
        self._formal_qmt = isinstance(getattr(client, "transport", None), HpSocketTransport)  # 正式模式只允许 QMT/PG，不再静默回退 Mock。

    def subscribe(self, symbols: list[str]) -> Future[ClientResponse]:
        if self._formal_qmt:
            return self._unsupported("subscribe", "QMT/PG 查询链路已接通，实时 quote.subscribe/Push 尚未进入 OX-P1-01")
        if self.market_hub is not None and all(self._is_pytdx_symbol(symbol) for symbol in symbols):
            self.market_hub.subscribe_symbols(symbols)  # 沪深股票直接登记到实时 Feed；重复订阅由 Feed 的 set 幂等去重。
            status = self.market_hub.feed_status  # 返回当前实际主源或 Mock 回退源，让页面明确显示行情来自哪里。
            return self._cache_response("subscribe", {"status": "subscribed", "symbols": symbols, "source": status.source})  # 即时 Future 保持页面原有异步协议，不绕过 request_id 关联。
        return self.page(operation="subscribe", payload={"symbols": symbols})  # Mock 订阅返回状态快照；正式版本改为注册推送流。

    def list_quotes(self, **filters: Any) -> Future[ClientResponse]:
        if self._formal_qmt:
            market, stkcode = self._parse_qmt_selection(filters.get("query", ""))
            page = max(1, int(filters.get("page", 1) or 1))
            page_size = max(1, min(50, int(filters.get("page_size", 20) or 20)))
            page_token = str(filters.get("page_token", "") or "")
            if not page_token and page > 1:
                page_token = f"v1:{(page - 1) * page_size}"
            return self.client.request(
                session_id=self.current_session_id,
                msg_type="quote",
                func_id="latest",
                payload={
                    "request_id": f"qmt-ui-{uuid4().hex}",
                    "schema_version": 1,
                    "data": {
                        "market": market,
                        "stkcode": stkcode,
                        "freshness_max_age_ms": 300_000,
                    },
                    "page": {
                        "page_size": page_size,
                        "page_token": page_token,
                        "sort": str(filters.get("sort", "receive_time") or "receive_time"),
                        "descending": bool(filters.get("descending", True)),
                    },
                },
            )  # 真实页面查询直接走正式 quote.latest，回包中的 source=QMT/PG 原样保留。
        if self.market_hub is not None and self.market_hub.has_data:
            return self._cache_response("quotes", self.market_hub.quote_page(filters))  # Hub 已有实时股票快照时本机读取，不再把行情查询发往低频 Mock 后端。
        return self.page(operation="quotes", payload=filters)

    def list_ticks(self, **filters: Any) -> Future[ClientResponse]:
        if self._formal_qmt:
            return self._unsupported("ticks", "当前 ta_data 只提供最新快照，没有历史 Tick 表")
        if self._can_read_hub(filters):
            return self._cache_response("ticks", self.market_hub.tick_page(filters))  # 只在请求标的确实存在于 Hub 时使用实时 Tick，避免股票缓存遮蔽期货 Mock。
        return self.page(operation="ticks", payload=filters)

    def list_bars(self, **filters: Any) -> Future[ClientResponse]:
        if self._formal_qmt:
            return self._unsupported("bars", "当前 ta_data 只提供最新快照，没有历史 K 线表")
        if self._can_read_hub(filters):
            return self._cache_response("bars", self.market_hub.bar_page(filters))
        return self.page(operation="bars", payload=filters)

    def list_depth(self, **filters: Any) -> Future[ClientResponse]:
        if self._formal_qmt:
            return self._unsupported("depth", "当前 ta_data 只提供最新快照，没有五档盘口明细表")
        if self._can_read_hub(filters):
            return self._cache_response("depth", self.market_hub.depth_page(filters))
        return self.page(operation="depth", payload=filters)

    @staticmethod
    def _parse_qmt_selection(value: Any) -> tuple[str, str]:
        """把页面筛选转换为 QMT/PG 的内部市场码和证券代码。"""

        query = str(value or "").strip().upper()
        if not query:
            return "", ""  # 空筛选表示全市场分页，服务端从 ta_data.stkprice/ft_stkprice 读取真实快照。
        if ":" in query:
            market, separator, stkcode = query.partition(":")
            return market.strip(), stkcode.strip()
        if "." in query:
            stkcode, separator, suffix = query.partition(".")
            market = {"SH": "1", "SSE": "1", "SZ": "0", "SZSE": "0", "BJ": "2"}.get(suffix, suffix)
            return market, stkcode.strip()
        return "1", query

    def _unsupported(self, operation: str, message: str) -> Future[ClientResponse]:
        """在正式模式明确报告未实现边界，禁止悄悄回退到 Mock 数据。"""

        request_id = f"qmt-unsupported-{uuid4().hex}"
        future: Future[ClientResponse] = Future()
        future.request_id = request_id
        future.set_result(ClientResponse(
            request_id=request_id,
            session_id=self.current_session_id,
            msg_type="quote",
            func_id=operation,
            status=MessageStatus.FAILED,
            error_code=ErrorCode.SERVICE_ERROR,
            error_message=message,
        ))
        return future

    def _cache_response(self, operation: str, payload: dict[str, Any]) -> Future[ClientResponse]:
        """把本地快照适配为既有 Future/ClientResponse 形状，页面无需区分 Socket 与缓存来源。"""

        request_id = f"market-cache-{uuid4().hex}"  # 每次本机快照查询也生成唯一编号，多个并发页面请求不会互相认错回包。
        future: Future[ClientResponse] = Future()  # 创建已完成 Future 复用页面消费接口，同时不额外创建线程或网络请求。
        future.request_id = request_id  # ServiceTablePage 仍按统一 request_id 消费，不绕过异步页面关联模型。
        future.set_result(ClientResponse(  # 快照已准备好，立即完成 Future，Qt 层仍经既有 done callback/Signal 更新。
            request_id=request_id, session_id=self.current_session_id, msg_type="market", func_id=operation,
            status=MessageStatus.SUCCEEDED, payload=payload,
        ))
        return future

    def _can_read_hub(self, filters: dict[str, Any]) -> bool:
        if self.market_hub is None or not self.market_hub.has_data:
            return False  # Hub 尚无数据时保留 #24 MockGateway 体验，页面不会因实时源冷启动而永久空白。
        symbol = str(filters.get("symbol") or filters.get("query") or "").strip().upper()
        return not symbol or self.market_hub.has_symbol_data(symbol)  # 无标的读取全市场快照；指定标的必须真实存在才切到 Hub。

    @staticmethod
    def _is_pytdx_symbol(symbol: str) -> bool:
        try:
            split_symbol(symbol)
        except ValueError:
            return False  # IF/IC 等期货不属于 PyTdx 股票 Bridge，继续由原 Mock Gateway/未来正式 SDK 处理。
        return True
