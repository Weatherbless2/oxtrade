"""产品、项目和资金账号服务接口。"""

from __future__ import annotations

from .base import BaseService


class FundService(BaseService):
    def __init__(self, client, session_id: str = "mock-session") -> None:
        super().__init__(client, "funds", session_id)

    def list_products(self, **filters):
        return self.page(payload=filters)

    def list_projects(self, **filters):
        return self.client.request(session_id=self.session_id, msg_type="projects", func_id="page", payload=filters)

    def list_accounts(self, **filters):
        return self.client.request(session_id=self.session_id, msg_type="accounts", func_id="page", payload=filters)
