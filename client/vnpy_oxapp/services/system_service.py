"""系统管理服务接口（数据字典、系统菜单、心跳）。"""

from __future__ import annotations

from concurrent.futures import Future

from .base import BaseService
from ..message import ClientResponse


class SystemService(BaseService):
    def __init__(self, client, session_id: str = "mock-session") -> None:
        super().__init__(client, "system", session_id)  # 系统管理资源由 system 路由统一处理。

    def list_menus(self, **filters) -> Future[ClientResponse]:
        return self.page(payload=filters)  # 菜单查询只改变过滤参数，不复制通信实现。

    def heartbeat(self) -> Future[ClientResponse]:
        return self.page(operation="heartbeat")  # 心跳沿用同一 Future/超时机制，但路由到 heartbeat 操作。

    def list_dictionaries(self, **filters) -> Future[ClientResponse]:
        return self.client.request(session_id=self.session_id, msg_type="dictionaries", func_id="page", payload=filters)

    def list_dictionary_details(self, **filters) -> Future[ClientResponse]:
        return self.client.request(session_id=self.session_id, msg_type="dictionary_details", func_id="page", payload=filters)

    def list_logs(self, **filters) -> Future[ClientResponse]:
        return self.client.request(session_id=self.session_id, msg_type="system_logs", func_id="page", payload=filters)
