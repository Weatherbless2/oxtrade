"""服务层公共请求方法。"""

from __future__ import annotations

from concurrent.futures import Future
from typing import Any

from ..connection import ClientProtocolClient
from ..message import ClientResponse


class BaseService:
    """页面只看到业务方法；通信细节统一由 ClientProtocolClient 处理。"""

    def __init__(self, client: ClientProtocolClient, service_name: str, session_id: str = "mock-session") -> None:
        self.client = client
        self.service_name = service_name
        self.session_id = session_id

    def page(self, *, operation: str = "page", payload: dict[str, Any] | None = None, timeout_ms: int = 3_000) -> Future[ClientResponse]:
        return self.client.request(
            session_id=self.session_id,
            msg_type=self.service_name,
            func_id=operation,
            payload=payload or {},
            timeout_ms=timeout_ms,
        )
