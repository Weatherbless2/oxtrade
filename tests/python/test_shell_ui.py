from __future__ import annotations

import time
import unittest
from io import StringIO

from vnpy_oxapp.connection import ClientProtocolClient
from vnpy_oxapp.logging import ClientLogger
from vnpy_oxapp.mock.mock_transport import MockTransport
from vnpy_oxapp.ui.qt_bridge import AsyncQtBridge, PYSIDE_AVAILABLE
from vnpy_oxapp.ui.shell_state import SessionState, ShellState


class ShellStateTest(unittest.TestCase):
    def test_default_state_is_safe_and_display_only(self) -> None:
        state = ShellState()
        self.assertEqual(SessionState.LOGGED_OUT, state.session)
        self.assertTrue(state.right_panel_visible)
        self.assertEqual("dark_navy", state.theme.value)
        self.assertNotIn("password", state.__dict__)


@unittest.skipUnless(PYSIDE_AVAILABLE, "PySide6 is required for shell UI tests")
class ShellUiTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        from PySide6.QtWidgets import QApplication

        cls.app = QApplication.instance() or QApplication([])

    def setUp(self) -> None:
        from vnpy_oxapp.ui.main_window import OxAppMainWindow

        self.bridge = AsyncQtBridge()
        self.client = ClientProtocolClient(MockTransport())
        self.client.set_response_callback(self.bridge.publish_response)
        self.client.set_state_callback(lambda state, reason: self.bridge.publish_state(state.value, reason))
        self.assertTrue(self.client.connect())
        self.window = OxAppMainWindow(self.bridge, client=self.client, logger=ClientLogger(stdout=StringIO()))

    def tearDown(self) -> None:
        self.window.close()
        self.client.disconnect()
        self.app.processEvents()

    def _pump(self, iterations: int = 35) -> None:
        for _ in range(iterations):
            self.app.processEvents()
            time.sleep(0.01)

    def _login(self) -> None:
        self.window.show()
        self.app.processEvents()
        page = self.window._login_page
        page.account_input.setText("yangjh")
        page.password_input.setText("mock-password")
        page.submit_button.click()
        self._pump(45)

    def test_frameless_window_and_fixed_three_columns(self) -> None:
        from PySide6.QtCore import Qt

        self.assertTrue(self.window.windowFlags() & Qt.WindowType.FramelessWindowHint)
        self.assertGreaterEqual(self.window.minimumWidth(), 1100)
        self.assertEqual(210, self.window._navigation_panel.width())
        self.assertEqual(282, self.window._config_panel.width())
        self.assertEqual(9, self.window._pages.count())
        self.assertEqual(0, self.window._session_stack.currentIndex())
        self.assertFalse(self.window._title_bar.isVisible())

    def test_login_validation_success_and_logout_route(self) -> None:
        page = self.window._login_page
        page.account_input.clear()
        page.password_input.clear()
        page.submit_button.click()
        self.assertEqual("请输入账号和密码", page.error_label.text())
        self.assertEqual(SessionState.ERROR, self.window.state.session)

        self._login()
        self.assertTrue(self.window.session.authenticated)
        self.assertEqual(SessionState.LOGGED_IN, self.window.state.session)
        self.assertEqual(1, self.window._session_stack.currentIndex())
        self.assertTrue(self.window._title_bar.isVisible())
        self.assertIn("yangjh", self.window._title_bar._account_name.lower())

        self.window._logout()
        self._pump(45)
        self.assertFalse(self.window.session.authenticated)
        self.assertEqual(SessionState.LOGGED_OUT, self.window.state.session)
        self.assertEqual(0, self.window._session_stack.currentIndex())

    def test_login_error_preserves_account_and_clears_password(self) -> None:
        self.window.show()
        page = self.window._login_page
        page.account_input.setText("yangjh")
        page.password_input.setText("wrong-password")
        page.submit_button.click()
        self._pump(45)
        self.assertIn("登录失败", page.error_label.text())
        self.assertEqual("yangjh", page.account_input.text())
        self.assertEqual("", page.password_input.text())
        self.assertEqual(SessionState.ERROR, self.window.state.session)

    def test_menu_product_and_config_panel_state(self) -> None:
        self.window._session_stack.setCurrentIndex(1)
        self.window.show()
        self.app.processEvents()
        self.window._title_bar._menu_buttons["admin"].click()
        self.assertEqual("admin", self.window.state.primary_menu)
        self.window._navigation_panel.select("market")
        self.assertEqual("market", self.window.state.system_menu)

        self.window._welcome_page._products_list.setCurrentRow(1)
        self.assertEqual("中证 500", self.window.state.selected_product)
        self.assertIn("IC 主力", self.window.state.selected_product_code)

        self.assertTrue(self.window._config_panel.isVisible())
        self.window._config_panel.panel_toggle_button.click()
        self.assertFalse(self.window._config_panel.isVisible())
        self.assertFalse(self.window.state.right_panel_visible)
        self.window._title_bar._config_button.click()
        self.assertTrue(self.window._config_panel.isVisible())
        self.assertTrue(self.window.state.right_panel_visible)
