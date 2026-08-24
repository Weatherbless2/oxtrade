from __future__ import annotations

import time
import unittest
from io import StringIO

from vnpy_oxapp.connection import ClientProtocolClient
from vnpy_oxapp.logging import ClientLogger
from vnpy_oxapp.mock.mock_transport import MockTransport
from vnpy_oxapp.ui.qt_bridge import AsyncQtBridge, PYSIDE_AVAILABLE


@unittest.skipUnless(PYSIDE_AVAILABLE, "PySide6 is required for page tests")
class SystemAdminPageTest(unittest.TestCase):
    def test_system_page_loads_mock_menu_and_switches_tabs(self) -> None:
        from PySide6.QtWidgets import QApplication
        from vnpy_oxapp.ui.main_window import OxAppMainWindow

        app = QApplication.instance() or QApplication([])
        bridge = AsyncQtBridge()
        client = ClientProtocolClient(MockTransport())
        client.set_response_callback(bridge.publish_response)
        client.set_state_callback(lambda state, reason: bridge.publish_state(state.value, reason))
        client.connect()
        logger = ClientLogger(stdout=StringIO())
        window = OxAppMainWindow(bridge, client=client, logger=logger)
        for _ in range(30):
            app.processEvents()
            time.sleep(0.01)
        system_page = window._pages.widget(1)
        self.assertEqual(5, system_page._tabs.count())  # 服务状态页签覆盖最终架构中的运行监控功能。
        panel = system_page._tabs.widget(0)._content
        self.assertEqual(20, panel._table.rowCount())  # 4 个一级菜单和 16 个子菜单覆盖了表格滚动与筛选场景。
        system_page._tabs.setCurrentIndex(1)
        for _ in range(30):
            app.processEvents()
            time.sleep(0.01)
        self.assertEqual(8, system_page._tabs.widget(1)._content._table.rowCount())
        system_page._tabs.setCurrentIndex(4)
        for _ in range(30):
            app.processEvents()
            time.sleep(0.01)
        self.assertEqual(8, system_page._tabs.widget(4)._content._table.rowCount())
        window.close()
        app.processEvents()
