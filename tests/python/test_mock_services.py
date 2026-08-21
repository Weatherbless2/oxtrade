from __future__ import annotations

import unittest

from vnpy_oxapp.mock.mock_services import MockServiceError, MockServiceRouter


class MockServiceRouterTest(unittest.TestCase):
    def setUp(self) -> None:
        self.router = MockServiceRouter()

    def test_paging_filtering_and_boundaries(self) -> None:
        result = self.router.handle("users", "page", {"status": "active", "page": 1, "page_size": 1})
        self.assertEqual(2, result["total"])
        self.assertEqual(1, len(result["items"]))
        empty = self.router.handle("users", "page", {"query": "not-found"})
        self.assertEqual([], empty["items"])

    def test_explicit_failures_are_not_reported_as_success(self) -> None:
        with self.assertRaises(MockServiceError) as unauthorized:
            self.router.handle("users", "page", {"force_unauthorized": True})
        self.assertEqual("unauthorized", unauthorized.exception.code)
        with self.assertRaises(MockServiceError):
            self.router.handle("unknown", "page", {})
