"""风控参数和规则服务接口。"""

from __future__ import annotations

from .base import BaseService


class RiskService(BaseService):
    def __init__(self, client, session_id: str = "mock-session") -> None:
        super().__init__(client, "risk", session_id)

    def list_rules(self, **filters):
        return self.page(payload=filters)
