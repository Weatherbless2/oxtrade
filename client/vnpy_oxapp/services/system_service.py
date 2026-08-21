"""系统管理服务接口（数据字典、系统菜单、心跳）。"""

from __future__ import annotations

from concurrent.futures import Future

from .base import BaseService
from ..message import ClientResponse


class SystemService(BaseService):
    def __init__(self, client, session_id: str = "mock-session") -> None:
        super().__init__(client, "system", session_id)

    def list_menus(self, **filters) -> Future[ClientResponse]:
        return self.page(payload=filters)

    def heartbeat(self) -> Future[ClientResponse]:
        return self.page(operation="heartbeat")
