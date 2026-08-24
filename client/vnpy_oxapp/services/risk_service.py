"""风控参数和规则服务接口。"""

from __future__ import annotations

from .base import BaseService


class RiskService(BaseService):
    def __init__(self, client, session_id: str = "mock-session") -> None:
        super().__init__(client, "risk", session_id)  # 风控页面的默认服务名固定为 risk。

    def list_rules(self, **filters):
        return self.page(payload=filters)  # 规则分页与其他管理页面共享统一请求链路。

    def list_parameters(self, **filters):
        return self.client.request(session_id=self.session_id, msg_type="risk_parameters", func_id="page", payload=filters)

    def list_events(self, **filters):
        return self.client.request(session_id=self.session_id, msg_type="risk_events", func_id="page", payload=filters)
