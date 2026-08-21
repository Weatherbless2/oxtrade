"""用户、角色、站点和权限关系的服务接口。"""

from __future__ import annotations

from concurrent.futures import Future

from .base import BaseService
from ..message import ClientResponse


class UserService(BaseService):
    def __init__(self, client, session_id: str = "mock-session") -> None:
        super().__init__(client, "users", session_id)

    def list_users(self, **filters) -> Future[ClientResponse]:
        return self.page(payload=filters)

    def list_roles(self, **filters) -> Future[ClientResponse]:
        return self.client.request(session_id=self.session_id, msg_type="roles", func_id="page", payload=filters)

    def list_sites(self, **filters) -> Future[ClientResponse]:
        return self.client.request(session_id=self.session_id, msg_type="sites", func_id="page", payload=filters)
