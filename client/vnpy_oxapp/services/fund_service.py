"""产品、项目和资金账号服务接口。"""

from __future__ import annotations

from .base import BaseService


class FundService(BaseService):
    def __init__(self, client, session_id="mock-session") -> None:
        super().__init__(client, "funds", session_id)  # 所有资金页面请求统一走 funds 路由。

    def list_products(self, **filters):
        return self.page(payload=filters)  # 产品分页复用 BaseService 的关联、超时和 Future 逻辑。

    def list_projects(self, **filters):
        return self.client.request(session_id=self.current_session_id, msg_type="projects", func_id="page", payload=filters)  # 项目是独立资源，仍共享同一会话。

    def list_accounts(self, **filters):
        return self.client.request(session_id=self.current_session_id, msg_type="accounts", func_id="page", payload=filters)  # 资金账号列表交给 accounts 路由处理。

    def list_account_links(self, **filters):
        return self.client.request(session_id=self.current_session_id, msg_type="fund_account_links", func_id="page", payload=filters)  # 资产-资金账号关联由独立资源返回，不能用产品-资产展示代替正式 fundacctlink。
