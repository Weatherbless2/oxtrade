from __future__ import annotations

import time
import unittest
from io import StringIO

from vnpy_oxapp.connection import ClientProtocolClient
from vnpy_oxapp.logging import ClientLogger
from vnpy_oxapp.mock.mock_transport import MockTransport
from vnpy_oxapp.ui.qt_bridge import AsyncQtBridge, PYSIDE_AVAILABLE


@unittest.skipUnless(PYSIDE_AVAILABLE, "PySide6 is required for page tests")
class FundManagementPageTest(unittest.TestCase):
    def test_fund_page_loads_product_project_and_account_data(self) -> None:
        from PySide6.QtWidgets import QApplication
        from vnpy_oxapp.ui.main_window import OxAppMainWindow

        app = QApplication.instance() or QApplication([])
        bridge = AsyncQtBridge()
        client = ClientProtocolClient(MockTransport())
        client.set_response_callback(bridge.publish_response)
        client.set_state_callback(lambda state, reason: bridge.publish_state(state.value, reason))
        client.connect()
        window = OxAppMainWindow(bridge, client=client, logger=ClientLogger(stdout=StringIO()))
        for _ in range(40):
            app.processEvents()
            time.sleep(0.01)
        page = window._pages.widget(3)
        self.assertEqual(4, page._tabs.count())
        self.assertEqual(2, page._tabs.widget(0)._content._table.rowCount())
        page._tabs.setCurrentIndex(1)
        for _ in range(30):
            app.processEvents()
            time.sleep(0.01)
        self.assertEqual(3, page._tabs.widget(1)._content._table.rowCount())
        page._tabs.setCurrentIndex(2)
        for _ in range(30):
            app.processEvents()
            time.sleep(0.01)
        self.assertEqual(2, page._tabs.widget(2)._content._table.rowCount())
        window.close()
        app.processEvents()
