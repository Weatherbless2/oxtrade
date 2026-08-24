"""服务层公共请求方法。"""

from __future__ import annotations

from concurrent.futures import Future
from typing import Any

from ..connection import ClientProtocolClient
from ..message import ClientResponse


class BaseService:
    """页面只看到业务方法；通信细节统一由 ClientProtocolClient 处理。"""

    def __init__(self, client: ClientProtocolClient, service_name: str, session_id: Any = "mock-session") -> None:
        self.client = client  # 页面服务只持有稳定客户端接口，不直接接触 Transport/Timer。
        self.service_name = service_name  # 固定路由名由具体服务子类提供。
        self.session_id = session_id  # 可为字符串或 ClientSession；页面无需为登录后会话更新重建 Service。

    @property
    def current_session_id(self) -> str:
        value = getattr(self.session_id, "session_id", self.session_id)
        return str(value)

    def page(self, *, operation: str = "page", payload: dict[str, Any] | None = None, timeout_ms: int = 3_000) -> Future[ClientResponse]:
        return self.client.request(  # 统一生成 request_id、deadline 并返回 Future，页面无需复制通信模板。
            session_id=self.current_session_id,  # 登录后动态读取会话，输入仍交给关联层做隔离判断。
            msg_type=self.service_name,  # 当前服务名进入 Mock/未来后台路由。
            func_id=operation,  # 当前操作名决定 page/heartbeat 等分支。
            payload=payload or {},  # None 归一化为空对象，保持协议字段形状。
            timeout_ms=timeout_ms,  # 超时边界由客户端单调时钟统一管理。
        )

    def request_resource(self, resource: str, operation: str, payload: dict[str, Any] | None = None) -> Future[ClientResponse]:
        """发送资源级操作，CRUD 与查询共用同一关联、超时和 Transport 边界。"""
        return self.client.request(
            session_id=self.current_session_id,
            msg_type=resource,
            func_id=operation,
            payload=payload or {},
        )

    def create(self, resource: str, record: dict[str, Any]) -> Future[ClientResponse]:
        return self.request_resource(resource, "create", {"record": record})  # 服务端分配 Mock 主键，页面只提交可编辑字段。

    def update(self, resource: str, record_id: str, changes: dict[str, Any]) -> Future[ClientResponse]:
        return self.request_resource(resource, "update", {"record_id": record_id, "changes": changes})

    def transition(self, resource: str, record_id: str, state: str) -> Future[ClientResponse]:
        return self.request_resource(resource, "transition", {"record_id": record_id, "state": state})
