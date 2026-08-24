from __future__ import annotations

import unittest

from vnpy_oxapp.mock.mock_services import MockServiceError, MockServiceRouter


class MockServiceRouterTest(unittest.TestCase):
    def setUp(self) -> None:
        self.router = MockServiceRouter()

    def test_paging_filtering_and_boundaries(self) -> None:
        result = self.router.handle("users", "page", {"status": "active", "page": 1, "page_size": 1})  # 同时验证状态过滤、分页和 total。
        self.assertEqual(2, result["total"])  # 两个 active 用户应被统计，total 不是当前页长度。
        self.assertEqual(1, len(result["items"]))  # page_size=1 只允许返回一条当前页记录。
        empty = self.router.handle("users", "page", {"query": "not-found"})  # 故意查询不存在文本，模拟合法空结果。
        self.assertEqual([], empty["items"])  # 空数据是成功业务结果，不应抛服务异常。

    def test_explicit_failures_are_not_reported_as_success(self) -> None:
        with self.assertRaises(MockServiceError) as unauthorized:  # 主动制造权限拒绝，验证不能伪装为空列表。
            self.router.handle("users", "page", {"force_unauthorized": True})
        self.assertEqual("unauthorized", unauthorized.exception.code)  # 结构化 code 供传输层映射 ErrorCode.UNAUTHORIZED。
        with self.assertRaises(MockServiceError):  # 未知服务必须走 not_supported 异常路径。
            self.router.handle("unknown", "page", {})
