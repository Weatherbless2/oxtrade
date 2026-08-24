"""统一的 Mock 业务服务路由。

服务返回与未来页面一致的分页、空结果、权限拒绝和服务异常语义，但不宣称真实落库。
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Mapping

from .mock_data import MockDataStore


class MockServiceError(RuntimeError):
    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)  # 异常文本供日志展示，不能替代结构化 code。
        self.code = code  # 传输层据此把 unauthorized/service_error 映射为稳定 ErrorCode。


@dataclass(frozen=True, slots=True)
class PageResult:
    items: tuple[dict[str, Any], ...]
    page: int
    page_size: int
    total: int

    def as_payload(self) -> dict[str, Any]:
        return {"items": [dict(item) for item in self.items], "page": self.page, "page_size": self.page_size, "total": self.total}  # tuple 转 JSON 友好的 list，并复制记录避免泄漏内部数据。


class MockServiceRouter:
    """按 service + operation 处理 Mock 请求，页面不直接持有 MockDataStore。"""

    def __init__(self, store: MockDataStore | None = None) -> None:
        self.store = store or MockDataStore()  # 注入自定义快照可做确定性测试，默认不连接外部存储。

    def handle(self, service: str, operation: str, payload: Mapping[str, Any]) -> dict[str, Any]:
        if payload.get("force_error"):  # 测试/演示可主动模拟服务异常，验证错误路径而非吞掉异常。
            raise MockServiceError("service_error", "mock service was asked to fail")
        if payload.get("force_unauthorized"):  # 主动模拟权限拒绝，页面应显示无权限而非空数据。
            raise MockServiceError("unauthorized", "current mock session has no permission")
        if operation == "page":  # 所有管理页面共用分页、过滤和 total 语义。
            return self._page(service, payload).as_payload()  # PageResult 在这里转为可序列化 payload。
        if operation == "heartbeat":  # 心跳只返回服务存活状态，不读取业务数据。
            return {"status": "ok", "server_time": "2026-08-21T09:00:00+08:00"}  # 固定时间让测试可重复。
        raise MockServiceError("not_supported", f"unsupported operation: {service}.{operation}")

    def _page(self, service: str, payload: Mapping[str, Any]) -> PageResult:
        resource_map = {  # 将外部服务名映射到内部快照字段，页面不直接依赖存储结构。
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
            rows = list(self.store.resource(resource_map[service]))  # 复制为可筛选列表，不能修改 frozen store 的 tuple。
        except KeyError as error:
            raise MockServiceError("not_supported", f"unknown mock service: {service}") from error
        status = payload.get("status")
        if status:  # status 同时兼容记录状态字符串和 enabled 布尔字段。
            rows = [row for row in rows if row.get("status") == status or row.get("enabled") == (status == "enabled")]  # 过滤后 total 反映真实匹配数。
        query = str(payload.get("query", "")).strip().lower()  # 统一大小写和空白，模拟页面搜索框输入。
        if query:
            rows = [row for row in rows if query in str(row).lower()]  # Mock 用整行文本匹配，保持实现简单可审计。
        page = max(1, int(payload.get("page", 1)))  # 页码下限为 1，避免负切片产生反直觉结果。
        page_size = min(100, max(1, int(payload.get("page_size", 20))))  # 限制单页最大 100，防止一次取空快照。
        start = (page - 1) * page_size  # 计算本页在过滤结果中的起点。
        return PageResult(tuple(rows[start : start + page_size]), page, page_size, len(rows))  # 只返回本页 items，同时保留 total。
