from __future__ import annotations

from io import StringIO
import time
import unittest

from vnpy_oxapp.connection import ClientProtocolClient
from vnpy_oxapp.logging import ClientLogger
from vnpy_oxapp.mock.mock_transport import MockTransport
from vnpy_oxapp.ui.qt_bridge import AsyncQtBridge, PYSIDE_AVAILABLE


@unittest.skipUnless(PYSIDE_AVAILABLE, "PySide6 is required for layout tests")
class TableLayoutFillTest(unittest.TestCase):
    @staticmethod
    def _process_events(app, iterations: int = 50) -> None:
        for _ in range(iterations):
            app.processEvents()
            time.sleep(0.01)

    def test_tabs_expand_and_page_rows_fill_the_table_viewport(self) -> None:
        from PySide6.QtWidgets import QApplication
        from vnpy_oxapp.ui.main_window import OxAppMainWindow

        app = QApplication.instance() or QApplication([])
        bridge = AsyncQtBridge()
        client = ClientProtocolClient(MockTransport())
        client.set_response_callback(bridge.publish_response)
        client.set_state_callback(lambda state, reason: bridge.publish_state(state.value, reason))
        self.assertTrue(client.connect())
        window = OxAppMainWindow(bridge, client=client, logger=ClientLogger(stdout=StringIO()))
        window.resize(1440, 900)
        window._session_stack.setCurrentIndex(1)  # 页面布局测试显式进入工作台，生产默认仍从登录页开始。
        window.show()
        window._navigation.setCurrentRow(2)
        self._process_events(app)

        page = window._pages.widget(2)
        tab_bar = page._tabs.tabBar()
        self.assertTrue(tab_bar.expanding())  # 页签占满可用宽度，不保留右侧空白带。
        self.assertAlmostEqual(
            tab_bar.tabRect(0).width(), tab_bar.tabRect(tab_bar.count() - 1).width(), delta=1,
        )  # 等宽页签使多模块工作台保持稳定扫描节奏。

        panel = page._tabs.widget(0)._content
        table = panel._table
        self.assertAlmostEqual(table.viewport().height(), sum(table.rowHeight(row) for row in range(table.rowCount())), delta=1)
        # 垂直 Stretch 让当前页数据行填满 viewport，避免最后一条记录下方留下未分隔的大块黑色区域。
        window.close()
        client.disconnect()
        app.processEvents()

    def test_empty_filter_keeps_controls_visible_and_can_return_to_all_rows(self) -> None:
        from PySide6.QtWidgets import QApplication
        from vnpy_oxapp.ui.main_window import OxAppMainWindow

        app = QApplication.instance() or QApplication([])
        bridge = AsyncQtBridge()
        client = ClientProtocolClient(MockTransport())
        client.set_response_callback(bridge.publish_response)
        client.set_state_callback(lambda state, reason: bridge.publish_state(state.value, reason))
        self.assertTrue(client.connect())
        window = OxAppMainWindow(bridge, client=client, logger=ClientLogger(stdout=StringIO()))
        window.resize(1440, 900)
        window._session_stack.setCurrentIndex(1)  # 页面布局测试显式进入工作台，生产默认仍从登录页开始。
        window.show()
        window._navigation.setCurrentRow(2)
        self._process_events(app)

        page = window._pages.widget(2)
        container = page._tabs.widget(0)
        panel = container._content
        pending_index = panel._status.findData("pending")
        self.assertGreaterEqual(pending_index, 0)
        panel._status.setCurrentIndex(pending_index)
        page._load(0)
        self._process_events(app)
        self.assertGreater(panel._table.rowCount(), 0)  # Mock 数据中的待审核用户可以展示，筛选值不是无效占位符。

        panel._query.setText("__no_such_mock_user__")
        page._load(0)
        self._process_events(app)
        self.assertIs(container._stack.currentWidget(), panel)  # 空结果不能把筛选栏替换为全页“暂无数据”。
        self.assertTrue(panel._status.isVisible())
        self.assertTrue(panel._status.isEnabled())
        self.assertEqual(0, panel._table.rowCount())
        self.assertIn("暂无数据", panel._summary.text())

        panel._query.clear()
        panel._status.setCurrentIndex(0)
        page._load(0)
        self._process_events(app)
        self.assertIs(container._stack.currentWidget(), panel)
        self.assertGreater(panel._table.rowCount(), 0)  # 切回全部后立即恢复，不需要重建页面或重启客户端。
        window.close()
        client.disconnect()
        app.processEvents()
