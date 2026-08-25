"""统一的 Mock 业务服务路由。

服务语义与未来后端一致：页面只通过请求、响应和错误模型访问数据，不能直接修改 MockDataStore。
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Mapping

from .mock_data import MockDataStore, RESOURCE_IDENTITIES


RESOURCE_MAP = {
    "system": "menus", "dictionaries": "dictionaries", "dictionary_details": "dictionary_details",
    "system_logs": "system_logs", "service_status": "service_status", "role_rights": "role_rights", "users": "users", "roles": "roles",
    "sites": "sites", "cololist": "cololist", "user_menu_rights": "user_menu_rights", "fund_rights": "fund_rights",
    "project_rights": "project_rights", "funds": "products", "projects": "projects", "accounts": "accounts",
    "fund_account_links": "fund_account_links", "risk": "risk_rules", "risk_parameters": "risk_parameters",
    "risk_events": "risk_events",
}

NON_DELETABLE_RESOURCES = {"system_logs", "service_status", "risk_events"}


class MockServiceError(RuntimeError):
    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code  # Transport 用稳定 code 映射页面可理解的 ErrorCode，不暴露内部异常类型。


@dataclass(frozen=True, slots=True)
class PageResult:
    items: tuple[dict[str, Any], ...]
    page: int
    page_size: int
    total: int

    def as_payload(self) -> dict[str, Any]:
        return {
            "items": [dict(item) for item in self.items], "page": self.page,
            "page_size": self.page_size, "total": self.total,
        }


class MockServiceRouter:
    """按 service + operation 处理 Mock 查询和 CRUD，后续 C++ Mock LBM 复用相同的操作语义。"""

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
            return {"status": "ok", "server_time": "2026-08-24T09:00:00+08:00", "source": "mock"}
        if operation == "login":
            return self._login(payload)
        if operation == "create":
            return self._create(service, payload)
        if operation == "update":
            return self._update(service, payload)
        if operation == "transition":
            return self._transition(service, payload)
        if operation == "delete":
            return self._delete(service, payload)
        raise MockServiceError("not_supported", f"unsupported operation: {service}.{operation}")

    def _resource_name(self, service: str) -> str:
        try:
            return RESOURCE_MAP[service]
        except KeyError as error:
            raise MockServiceError("not_supported", f"unknown mock service: {service}") from error

    def _page(self, service: str, payload: Mapping[str, Any]) -> PageResult:
        resource = self._resource_name(service)
        rows = list(self.store.resource(resource))
        status = payload.get("status")
        if status:
            rows = [
                row for row in rows
                if row.get("status") == status or row.get("level") == status
                or ("enabled" in row and row["enabled"] == (status == "enabled"))
            ]
        query = str(payload.get("query", "")).strip().lower()
        if query:
            rows = [row for row in rows if query in str(row).lower()]
        page = max(1, int(payload.get("page", 1)))
        page_size = min(20, max(1, int(payload.get("page_size", 10))))  # Mock 单页受 IPC 演示帧上限约束，页面可用翻页读取完整数据。
        start = (page - 1) * page_size
        return PageResult(tuple(rows[start : start + page_size]), page, page_size, len(rows))

    def _login(self, payload: Mapping[str, Any]) -> dict[str, Any]:
        user_name = str(payload.get("user_name", "yangjh")).strip()
        password = str(payload.get("password", ""))
        if password not in ("", "mock-password"):
            raise MockServiceError("unauthorized", "mock password is invalid")
        user = next((row for row in self.store.resource("users") if row["user_name"] == user_name), None)
        if user is None or user.get("status") != "active":
            raise MockServiceError("unauthorized", "mock user is not allowed to log in")
        self._append_audit(user_name, "INFO", "Mock 登录成功")
        return {"session_id": f"mock-session-{user['user_id']}", "user": user, "source": "mock"}

    def _create(self, service: str, payload: Mapping[str, Any]) -> dict[str, Any]:
        resource = self._resource_name(service)
        record = payload.get("record")
        if not isinstance(record, Mapping):
            raise MockServiceError("validation_error", "create requires an object record")
        primary_key, _ = RESOURCE_IDENTITIES[resource]
        created = dict(record)
        created[primary_key] = self.store.next_identifier(resource)  # Mock 主键也由服务端生成，页面不能伪造。
        for key, value in self._default_record(resource).items():
            created.setdefault(key, value)
        self.store.mutable_resource(resource).append(created)
        self._append_audit("yangjh", "INFO", f"Mock 新增 {service}: {created[primary_key]}")
        return {"item": dict(created), "message": "mock record created", "source": "mock"}

    def _update(self, service: str, payload: Mapping[str, Any]) -> dict[str, Any]:
        resource = self._resource_name(service)
        changes = payload.get("changes")
        if not isinstance(changes, Mapping):
            raise MockServiceError("validation_error", "update requires an object changes")
        row = self._find_row(resource, payload)
        primary_key, _ = RESOURCE_IDENTITIES[resource]
        row.update({key: value for key, value in changes.items() if key != primary_key})  # 编辑不能更改主键。
        self._append_audit("yangjh", "INFO", f"Mock 更新 {service}: {row[primary_key]}")
        return {"item": dict(row), "message": "mock record updated", "source": "mock"}

    def _transition(self, service: str, payload: Mapping[str, Any]) -> dict[str, Any]:
        resource = self._resource_name(service)
        state = str(payload.get("state", "")).strip()
        if not state:
            raise MockServiceError("validation_error", "transition requires a non-empty state")
        row = self._find_row(resource, payload)
        if "enabled" in row:
            row["enabled"] = state == "enabled"
        elif "handled" in row:
            row["handled"] = state == "handled"
            row["handled_by"] = "yangjh" if row["handled"] else ""
        else:
            row["status"] = state
        primary_key, _ = RESOURCE_IDENTITIES[resource]
        self._append_audit("yangjh", "INFO", f"Mock 状态变更 {service}: {row[primary_key]} -> {state}")
        return {"item": dict(row), "message": "mock state transitioned", "source": "mock"}

    def _delete(self, service: str, payload: Mapping[str, Any]) -> dict[str, Any]:
        resource = self._resource_name(service)
        if resource in NON_DELETABLE_RESOURCES:
            raise MockServiceError("validation_error", f"mock resource cannot be deleted: {resource}")
        primary_key, _ = RESOURCE_IDENTITIES[resource]
        record_id = str(payload.get("record_id", "")).strip()
        if not record_id:
            raise MockServiceError("validation_error", f"{primary_key} is required")
        rows = self.store.mutable_resource(resource)
        for index, row in enumerate(rows):
            if str(row.get(primary_key)) == record_id:
                deleted = dict(rows.pop(index))  # 先复制再移除，响应与审计不依赖已失效的列表引用。
                self._append_audit("yangjh", "INFO", f"Mock 删除 {service}: {record_id}")
                return {"item": deleted, "message": "mock record deleted", "source": "mock"}
        raise MockServiceError("not_found", f"mock {resource} record was not found")

    def _find_row(self, resource: str, payload: Mapping[str, Any]) -> dict[str, Any]:
        primary_key, _ = RESOURCE_IDENTITIES[resource]
        record_id = str(payload.get("record_id", "")).strip()
        if not record_id:
            raise MockServiceError("validation_error", f"{primary_key} is required")
        for row in self.store.mutable_resource(resource):
            if str(row.get(primary_key)) == record_id:
                return row
        raise MockServiceError("not_found", f"mock {resource} record was not found")

    @staticmethod
    def _default_record(resource: str) -> dict[str, Any]:
        if resource in {"menus", "dictionaries", "dictionary_details", "role_rights", "risk_rules"}:
            return {"enabled": True}
        if resource in {"users", "roles", "sites", "products", "projects", "accounts", "cololist", "fund_account_links"}:
            return {"status": "active"}
        if resource == "risk_events":
            return {"handled": False}
        return {}

    def _append_audit(self, operator: str, level: str, event: str) -> None:
        logs = self.store.mutable_resource("system_logs")
        logs.insert(0, {
            "log_id": f"log-{len(logs) + 1:04}", "event_time": "2026-08-24 10:00:00",
            "operator": operator, "level": level, "event": event,
            "trace_id": f"mock-mutation-{len(logs) + 1:04}",
        })
