from __future__ import annotations

from io import StringIO
import time
import unittest

from vnpy_oxapp.connection import ClientProtocolClient
from vnpy_oxapp.logging import ClientLogger
from vnpy_oxapp.mock.mock_transport import MockTransport
from vnpy_oxapp.ui.qt_bridge import AsyncQtBridge, PYSIDE_AVAILABLE


@unittest.skipUnless(PYSIDE_AVAILABLE, "PySide6 is required for page tests")
class TableCrudInteractionTest(unittest.TestCase):
    def test_user_page_mutations_go_through_service_then_refresh_snapshot(self) -> None:
        from PySide6.QtWidgets import QApplication
        from vnpy_oxapp.ui.main_window import OxAppMainWindow

        app = QApplication.instance() or QApplication([])
        bridge = AsyncQtBridge()
        transport = MockTransport()
        client = ClientProtocolClient(transport)
        client.set_response_callback(bridge.publish_response)
        client.set_state_callback(lambda state, reason: bridge.publish_state(state.value, reason))
        self.assertTrue(client.connect())
        window = OxAppMainWindow(bridge, client=client, logger=ClientLogger(stdout=StringIO()))
        page = window._pages.widget(2)
        for _ in range(30):
            app.processEvents()
            time.sleep(0.01)

        page._create(0, {"user_name": "ui-created-user", "display_name": "UI Created User"})
        for _ in range(30):
            app.processEvents()
            time.sleep(0.01)
        users = transport.service_router.store.resource("users")
        created = next(row for row in users if row["user_name"] == "ui-created-user")
        self.assertEqual("UI Created User", created["display_name"])  # 页面并未直接改表格，记录由 Service/Transport 写入 Mock 存储后再刷新。

        page._update(0, created["user_id"], {"display_name": "UI Updated User"})
        page._transition(0, created["user_id"], "frozen")
        for _ in range(40):
            app.processEvents()
            time.sleep(0.01)
        changed = next(row for row in transport.service_router.store.resource("users") if row["user_id"] == created["user_id"])
        self.assertEqual("UI Updated User", changed["display_name"])
        self.assertEqual("frozen", changed["status"])
        page._delete(0, created["user_id"])
        for _ in range(30):
            app.processEvents()
            time.sleep(0.01)
        self.assertFalse(any(row["user_id"] == created["user_id"] for row in transport.service_router.store.resource("users")))
        page._tabs.widget(0)._content.set_failure("mock query failed")
        self.assertEqual(0, page._tabs.widget(0)._content._table.rowCount())
        window.close()
        client.disconnect()
        app.processEvents()
