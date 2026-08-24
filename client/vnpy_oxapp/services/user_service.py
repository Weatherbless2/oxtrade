"""用户、角色、站点和权限关系的服务接口。"""

from __future__ import annotations

from concurrent.futures import Future

from .base import BaseService
from ..message import ClientResponse


class UserService(BaseService):
    def __init__(self, client, session_id: str = "mock-session") -> None:
        super().__init__(client, "users", session_id)  # 用户页面的基础资源名。

    def list_users(self, **filters) -> Future[ClientResponse]:
        return self.page(payload=filters)  # 用户列表复用公共分页链路。

    def list_roles(self, **filters) -> Future[ClientResponse]:
        return self.client.request(session_id=self.session_id, msg_type="roles", func_id="page", payload=filters)  # 角色独立路由，保留同一 session_id。

    def list_sites(self, **filters) -> Future[ClientResponse]:
        return self.client.request(session_id=self.session_id, msg_type="sites", func_id="page", payload=filters)  # 站点列表由 sites 资源映射到 Mock 快照。

    def list_role_rights(self, **filters) -> Future[ClientResponse]:
        return self.client.request(session_id=self.session_id, msg_type="role_rights", func_id="page", payload=filters)
