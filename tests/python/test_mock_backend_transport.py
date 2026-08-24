from __future__ import annotations

from pathlib import Path
import unittest

from vnpy_oxapp.connection import ClientProtocolClient
from vnpy_oxapp.message import ErrorCode, MessageStatus
from vnpy_oxapp.mock.mock_backend_transport import MockBackendTransport, MockBackendTransportConfig


def backend_path() -> Path:
    return Path(__file__).resolve().parents[2] / "build" / "bin" / "oxMbsMockBackend"


@unittest.skipUnless(backend_path().is_file(), "build oxMbsMockBackend before running subprocess integration tests")
class MockBackendTransportTest(unittest.TestCase):
    def setUp(self) -> None:
        self.transport = MockBackendTransport(MockBackendTransportConfig(backend_path=backend_path()))
        self.client = ClientProtocolClient(self.transport)
        self.assertTrue(self.client.connect())

    def tearDown(self) -> None:
        self.client.disconnect()

    def test_query_login_and_crud_cross_python_cpp_ipc_lbm_boundary(self) -> None:
        users = self.client.request(
            session_id="mock-session", msg_type="users", func_id="page", payload={"page": 1, "page_size": 2},
        ).result(timeout=2)
        self.assertTrue(users.is_success)
        self.assertEqual(40, users.payload["total"])
        self.assertEqual("mock-lbm", users.payload["source"])
        self.assertEqual("yangjh", users.payload["items"][0]["user_name"])

        login = self.client.request(
            session_id="mock-session", msg_type="users", func_id="login",
            payload={"user_name": "yangjh", "password": "mock-password"},
        ).result(timeout=2)
        self.assertTrue(login.is_success)
        self.assertEqual("yangjh", login.payload["user"]["user_name"])

        created = self.client.request(
            session_id="mock-session", msg_type="users", func_id="create",
            payload={"record": {"user_name": "subprocess-user", "display_name": "Subprocess User"}},
        ).result(timeout=2)
        self.assertTrue(created.is_success)
        user_id = created.payload["item"]["user_id"]
        self.assertEqual("user-041", user_id)  # 由 C++ Mock LBM 分配，Python 页面没有生成或直接修改主键。

        updated = self.client.request(
            session_id="mock-session", msg_type="users", func_id="update",
            payload={"record_id": user_id, "changes": {"display_name": "Updated Through IPC"}},
        ).result(timeout=2)
        self.assertEqual("Updated Through IPC", updated.payload["item"]["display_name"])
        frozen = self.client.request(
            session_id="mock-session", msg_type="users", func_id="transition",
            payload={"record_id": user_id, "state": "frozen"},
        ).result(timeout=2)
        self.assertEqual("frozen", frozen.payload["item"]["status"])

    def test_lbm_permission_error_preserves_request_correlation(self) -> None:
        response = self.client.request(
            session_id="mock-session", msg_type="risk", func_id="page", payload={"force_unauthorized": True},
        ).result(timeout=2)
        self.assertEqual(MessageStatus.UNAUTHORIZED, response.status)
        self.assertEqual(ErrorCode.UNAUTHORIZED, response.error_code)
