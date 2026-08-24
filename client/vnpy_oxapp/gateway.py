"""vn.py BaseGateway 适配层。

这个文件不修改 vn.py；只把 oxApp 的 Transport 事件翻译成 vn.py EventEngine 可消费事件。
"""

from __future__ import annotations

from typing import Any

from .connection import ClientProtocolClient
from .message import ClientResponse, ErrorCode
from .transport import ConnectionState, IClientTransport

try:
    from vnpy.event import Event
    from vnpy.trader.gateway import BaseGateway
    from vnpy.trader.object import LogData

    VNPY_AVAILABLE = True
except ImportError:  # 让纯 Python Mock 测试无需安装重量级 GUI/交易依赖，真实 Gateway 仍会明确拒绝启动。
    Event = Any  # type: ignore[misc,assignment]
    BaseGateway = object  # type: ignore[misc,assignment]
    LogData = Any  # type: ignore[misc,assignment]
    VNPY_AVAILABLE = False


EVENT_OXAPP_CONNECTION = "eOxAppConnection"
EVENT_OXAPP_RESPONSE = "eOxAppResponse"


class OxAppGateway(BaseGateway):
    """将连接状态和查询响应发布给 vn.py EventEngine 的轻量 Gateway。"""

    default_name = "OXAPP"
    exchanges: list = []

    def __init__(self, event_engine: Any, gateway_name: str = default_name, transport: IClientTransport | None = None) -> None:
        if not VNPY_AVAILABLE:  # 缺少 vn.py 时不伪造 Gateway，避免调用方误以为事件已接入。
            raise RuntimeError("OxAppGateway requires vn.py; install client/requirements.txt first")
        super().__init__(event_engine, gateway_name)  # BaseGateway 保存 EventEngine，后续事件统一投递到它。
        if transport is None:  # 强制依赖注入，工单 16 不猜测真实地址或网络实现。
            raise ValueError("transport must be injected; #16 does not select a real network transport")
        self.client = ClientProtocolClient(transport)  # Gateway 只看稳定客户端接口，不感知 Mock/Socket 细节。
        self.client.set_response_callback(self._on_response)  # 完整响应转换为 vn.py Event。
        self.client.set_state_callback(self._on_state)  # 连接变化同时更新 EventEngine 和日志。

    def connect(self, setting: dict) -> None:
        del setting  # 当前使用注入的 MockTransport；真实地址配置由未来 HpSocketTransport 解释。
        self.client.connect()  # 连接结果通过 state callback 异步通知，Gateway 不直接操作 UI。

    def close(self) -> None:
        self.client.disconnect()  # 由 Transport 取消后台资源，再发布 DISCONNECTED。

    def query_account(self) -> None:
        self._send_mock_query("accounts", "账户查询")

    def query_position(self) -> None:
        self._send_mock_query("positions", "持仓查询")

    def subscribe(self, req: Any) -> None:
        symbol = str(getattr(req, "vt_symbol", getattr(req, "symbol", "")))
        future = self.client.request(session_id="mock-session", msg_type="market", func_id="subscribe", payload={"symbols": [symbol]})
        self.on_log(LogData(msg=f"Mock 行情订阅已提交 request_id={future.request_id} symbol={symbol}", gateway_name=self.gateway_name))

    def send_order(self, req: Any) -> str:
        symbol = str(getattr(req, "vt_symbol", getattr(req, "symbol", "")))
        side = str(getattr(req, "direction", "buy")).lower()
        side = "sell" if "sell" in side or "short" in side or "卖" in side else "buy"
        future = self.client.request(
            session_id="mock-session", msg_type="trade", func_id="place_order",
            payload={"account_id": "MOCK-ACCT-001", "symbol": symbol, "name": symbol, "side": side,
                     "price": float(getattr(req, "price", 0.0)), "volume": int(getattr(req, "volume", 0)), "order_type": "限价"},
        )
        self.on_log(LogData(msg=f"Mock 委托已提交 request_id={future.request_id}，不会发送到真实柜台", gateway_name=self.gateway_name))
        return future.request_id  # 返回请求关联号，而不是伪造真实柜台 orderid；成交/拒绝由异步响应事件交付。

    def cancel_order(self, req: Any) -> None:
        order_id = str(getattr(req, "orderid", getattr(req, "order_id", "")))
        future = self.client.request(session_id="mock-session", msg_type="trade", func_id="cancel_order", payload={"order_id": order_id})
        self.on_log(LogData(msg=f"Mock 撤单已提交 request_id={future.request_id} order_id={order_id}", gateway_name=self.gateway_name))

    def _send_mock_query(self, resource: str, label: str) -> None:
        future = self.client.request(session_id="mock-session", msg_type="query", func_id="page", payload={"resource": resource})
        self.on_log(LogData(msg=f"Mock {label}已提交 request_id={future.request_id}，结果通过 eOxAppResponse 交付", gateway_name=self.gateway_name))

    def _on_state(self, state: ConnectionState, reason: str) -> None:
        self.event_engine.put(Event(EVENT_OXAPP_CONNECTION, {"state": state.value, "reason": reason, "gateway": self.gateway_name}))  # 连接状态事件交给 vn.py 消费者。
        self.on_log(LogData(msg=f"oxApp transport={state.value}: {reason}", gateway_name=self.gateway_name))  # 同时留下可审计日志。

    def _on_response(self, response: ClientResponse) -> None:
        # 将完整响应作为 Event data，Monitor 可按 request_id/session_id 安全过滤并更新界面。
        self.event_engine.put(Event(EVENT_OXAPP_RESPONSE, response))  # 原始 ClientResponse 保留关联字段，Monitor 自行过滤。
        if not response.is_success:
            self.on_log(LogData(msg=f"oxApp request failed: {response.error_code.value} {response.error_message}", gateway_name=self.gateway_name))  # 失败回包记录结构化错误，成功不制造噪声。

    def _log_not_available(self, message: str) -> None:
        self.on_log(LogData(msg=message, gateway_name=self.gateway_name))
