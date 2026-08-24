"""用户、角色、站点和权限关系的服务接口。"""

from __future__ import annotations

from concurrent.futures import Future

from .base import BaseService
from ..message import ClientResponse


class UserService(BaseService):
    def __init__(self, client, session_id="mock-session") -> None:
        super().__init__(client, "users", session_id)  # 用户页面的基础资源名。

    def list_users(self, **filters) -> Future[ClientResponse]:
        return self.page(payload=filters)  # 用户列表复用公共分页链路。

    def list_roles(self, **filters) -> Future[ClientResponse]:
        return self.client.request(session_id=self.current_session_id, msg_type="roles", func_id="page", payload=filters)  # 角色独立路由，读取动态会话而不是保存旧 session_id。

    def list_sites(self, **filters) -> Future[ClientResponse]:
        return self.client.request(session_id=self.current_session_id, msg_type="sites", func_id="page", payload=filters)  # 站点列表由 sites 资源映射到 Mock 快照。

    def list_role_rights(self, **filters) -> Future[ClientResponse]:
        return self.client.request(session_id=self.current_session_id, msg_type="role_rights", func_id="page", payload=filters)

    def list_cololist(self, **filters) -> Future[ClientResponse]:
        return self.client.request(session_id=self.current_session_id, msg_type="cololist", func_id="page", payload=filters)  # 托管服务配置属于权限域，Mock 仅提供配置/心跳快照。

    def list_user_menu_rights(self, **filters) -> Future[ClientResponse]:
        return self.client.request(session_id=self.current_session_id, msg_type="user_menu_rights", func_id="page", payload=filters)

    def list_fund_rights(self, **filters) -> Future[ClientResponse]:
        return self.client.request(session_id=self.current_session_id, msg_type="fund_rights", func_id="page", payload=filters)

    def list_project_rights(self, **filters) -> Future[ClientResponse]:
        return self.client.request(session_id=self.current_session_id, msg_type="project_rights", func_id="page", payload=filters)

    def login(self, user_name: str, password: str = "mock-password") -> Future[ClientResponse]:
        return self.request_resource("users", "login", {"user_name": user_name, "password": password})  # Mock 登录也经过请求关联，不直接修改客户端会话。

    def logout(self, user_name: str) -> Future[ClientResponse]:
        return self.request_resource("users", "logout", {"user_name": user_name})  # 注销也经由后端审计，成功后页面再清除本地临时会话。
