"""统一的 Mock 业务服务路由。

服务返回与未来页面一致的分页、空结果、权限拒绝和服务异常语义，但不宣称真实落库。
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Mapping

from .mock_data import MockDataStore


class MockServiceError(RuntimeError):
    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


@dataclass(frozen=True, slots=True)
class PageResult:
    items: tuple[dict[str, Any], ...]
    page: int
    page_size: int
    total: int

    def as_payload(self) -> dict[str, Any]:
        return {"items": [dict(item) for item in self.items], "page": self.page, "page_size": self.page_size, "total": self.total}


class MockServiceRouter:
    """按 service + operation 处理 Mock 请求，页面不直接持有 MockDataStore。"""

    def __init__(self, store: MockDataStore | None = None) -> None:
        self.store = store or MockDataStore()

    def handle(self, service: str, operation: str, payload: Mapping[str, Any]) -> dict[str, Any]:
        if payload.get("force_error"):
            raise MockServiceError("service_error", "mock service was asked to fail")
        if payload.get("force_unauthorized"):
            raise MockServiceError("unauthorized", "current mock session has no permission")
        if operation == "page":
            return self._page(service, payload).as_payload()
        if operation == "heartbeat":
            return {"status": "ok", "server_time": "2026-08-21T09:00:00+08:00"}
        raise MockServiceError("not_supported", f"unsupported operation: {service}.{operation}")

    def _page(self, service: str, payload: Mapping[str, Any]) -> PageResult:
        resource_map = {
            "system": "menus",
            "users": "users",
            "roles": "roles",
            "sites": "sites",
            "funds": "products",
            "projects": "projects",
            "accounts": "accounts",
            "risk": "risk_rules",
        }
        try:
            rows = list(self.store.resource(resource_map[service]))
        except KeyError as error:
            raise MockServiceError("not_supported", f"unknown mock service: {service}") from error
        status = payload.get("status")
        if status:
            rows = [row for row in rows if row.get("status") == status or row.get("enabled") == (status == "enabled")]
        query = str(payload.get("query", "")).strip().lower()
        if query:
            rows = [row for row in rows if query in str(row).lower()]
        page = max(1, int(payload.get("page", 1)))
        page_size = min(100, max(1, int(payload.get("page_size", 20))))
        start = (page - 1) * page_size
        return PageResult(tuple(rows[start : start + page_size]), page, page_size, len(rows))
