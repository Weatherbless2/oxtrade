from __future__ import annotations

import unittest

from vnpy_oxapp.mock.mock_services import MockServiceError, MockServiceRouter


class MockServiceRouterTest(unittest.TestCase):
    def setUp(self) -> None:
        self.router = MockServiceRouter()

    def test_paging_filtering_and_boundaries(self) -> None:
        result = self.router.handle("users", "page", {"status": "active", "page": 1, "page_size": 1})  # 同时验证状态过滤、分页和 total。
        self.assertGreater(result["total"], 10)  # 扩充后的 Mock 数据必须能够覆盖分页，total 不是当前页长度。
        self.assertEqual(1, len(result["items"]))  # page_size=1 只允许返回一条当前页记录。
        empty = self.router.handle("users", "page", {"query": "not-found"})  # 故意查询不存在文本，模拟合法空结果。
        self.assertEqual([], empty["items"])  # 空数据是成功业务结果，不应抛服务异常。

    def test_login_and_crud_change_only_service_owned_store(self) -> None:
        login = self.router.handle("users", "login", {"user_name": "yangjh", "password": "mock-password"})
        self.assertEqual("yangjh", login["user"]["user_name"])  # 登录返回用户快照和临时 session，不伪造真实鉴权。
        before = self.router.handle("users", "page", {"page": 1, "page_size": 20})["total"]
        created = self.router.handle("users", "create", {"record": {"user_name": "mock-new-user", "role_ids": ("r-001",)}})
        user_id = created["item"]["user_id"]
        self.assertEqual(before + 1, self.router.handle("users", "page", {"page": 1, "page_size": 20})["total"])
        updated = self.router.handle("users", "update", {"record_id": user_id, "changes": {"display_name": "Mock New User"}})
        self.assertEqual("Mock New User", updated["item"]["display_name"])  # 更新不能通过页面对象直接修改，必须经服务操作。
        frozen = self.router.handle("users", "transition", {"record_id": user_id, "state": "frozen"})
        self.assertEqual("frozen", frozen["item"]["status"])
        self.assertGreater(self.router.handle("system_logs", "page", {"page": 1, "page_size": 5})["total"], 60)  # 变更会留下 Mock 审计日志。

    def test_delete_removes_mutable_record_but_protects_audit_resources(self) -> None:
        created = self.router.handle("users", "create", {"record": {"user_name": "delete-me"}})
        user_id = created["item"]["user_id"]
        deleted = self.router.handle("users", "delete", {"record_id": user_id})
        self.assertEqual(user_id, deleted["item"]["user_id"])
        self.assertEqual(0, self.router.handle("users", "page", {"query": user_id})["total"])
        with self.assertRaises(MockServiceError) as context:
            self.router.handle("system_logs", "delete", {"record_id": "log-0001"})
        self.assertEqual("validation_error", context.exception.code)

    def test_explicit_failures_are_not_reported_as_success(self) -> None:
        with self.assertRaises(MockServiceError) as unauthorized:  # 主动制造权限拒绝，验证不能伪装为空列表。
            self.router.handle("users", "page", {"force_unauthorized": True})
        self.assertEqual("unauthorized", unauthorized.exception.code)  # 结构化 code 供传输层映射 ErrorCode.UNAUTHORIZED。
        with self.assertRaises(MockServiceError):  # 未知服务必须走 not_supported 异常路径。
            self.router.handle("unknown", "page", {})
