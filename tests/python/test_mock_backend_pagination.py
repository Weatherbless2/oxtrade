from __future__ import annotations

from io import StringIO
from pathlib import Path
import time
import unittest

from vnpy_oxapp.connection import ClientProtocolClient
from vnpy_oxapp.logging import ClientLogger
from vnpy_oxapp.mock.mock_backend_transport import MockBackendTransport, MockBackendTransportConfig
from vnpy_oxapp.ui.qt_bridge import AsyncQtBridge, PYSIDE_AVAILABLE


def backend_path() -> Path:
    return Path(__file__).resolve().parents[2] / "build" / "bin" / "oxMbsMockBackend"


@unittest.skipUnless(PYSIDE_AVAILABLE and backend_path().is_file(), "PySide6 and oxMbsMockBackend are required")
class MockBackendPaginationTest(unittest.TestCase):
    def test_user_table_requests_next_page_from_cpp_mock_backend(self) -> None:
        from PySide6.QtWidgets import QApplication
        from vnpy_oxapp.ui.main_window import OxAppMainWindow

        app = QApplication.instance() or QApplication([])
        bridge = AsyncQtBridge()
        client = ClientProtocolClient(MockBackendTransport(MockBackendTransportConfig(backend_path=backend_path())))
        client.set_response_callback(bridge.publish_response)
        client.set_state_callback(lambda state, reason: bridge.publish_state(state.value, reason))
        self.assertTrue(client.connect())
        window = OxAppMainWindow(bridge, client=client, logger=ClientLogger(stdout=StringIO()))
        page = window._pages.widget(2)
        for _ in range(50):
            app.processEvents()
            time.sleep(0.01)
        panel = page._tabs.widget(0)._content
        self.assertEqual(1, panel._page)
        self.assertEqual(10, panel._page_size)  # C++ LBM 在 IPC 帧边界下主动将页面大小收敛为 10。
        self.assertTrue(panel._next.isEnabled())

        panel._next_page()
        for _ in range(50):
            app.processEvents()
            time.sleep(0.01)
        self.assertEqual(2, panel._page)
        self.assertEqual("user-011", panel._rows[0]["user_id"])  # 第二页来自新后端查询，不是 Qt 本地切片。
        window.close()
        client.disconnect()
        app.processEvents()
