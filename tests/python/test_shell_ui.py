from __future__ import annotations

import time
import unittest
from io import StringIO

from vnpy_oxapp.connection import ClientProtocolClient
from vnpy_oxapp.logging import ClientLogger
from vnpy_oxapp.mock.mock_transport import MockTransport
from vnpy_oxapp.ui.qt_bridge import AsyncQtBridge, PYSIDE_AVAILABLE
from vnpy_oxapp.ui.shell_state import SessionState, ShellState, ThemeMode


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
        self.assertEqual(820, self.window.minimumWidth())
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

    def test_visible_navigation_secondary_nav_and_switch_controls(self) -> None:
        from vnpy_oxapp.ui.config_panel import SwitchControl
        from vnpy_oxapp.ui.theme import DARK_NAVY_QSS

        visible = self.window._navigation_panel.list_widget
        self.assertEqual(6, visible.count())
        self.assertEqual("⌂  工作台", visible.item(0).text())
        self.assertEqual("⌁  行情中心", visible.item(1).text())
        self.assertEqual("◇  策略研究", visible.item(2).text())
        self.assertEqual("⇄  交易终端", visible.item(3).text())
        self.assertEqual("▣  产品管理", visible.item(4).text())
        self.assertEqual("⚙  系统管理", visible.item(5).text())

        self.assertEqual(["overview", "watchlist", "recent"], list(self.window._secondary_nav.buttons))
        self.window._title_bar._menu_buttons["market"].click()
        self.assertEqual("market", self.window.state.primary_menu)
        self.assertEqual(["market_overview", "market_watchlist", "market_alerts"], list(self.window._secondary_nav.buttons))
        self.assertEqual("market_overview", self.window._secondary_nav.current_item_id)
        self.window._title_bar._menu_buttons["research"].click()
        self.assertEqual("research", self.window.state.primary_menu)
        self.assertEqual("research_overview", self.window._secondary_nav.current_item_id)
        self.assertEqual(0, visible.currentRow())
        self.assertIsInstance(self.window._config_panel._auto_refresh, SwitchControl)
        self.assertIn("QWidget#titleBar", DARK_NAVY_QSS)
        self.assertNotIn(chr(92) + "n# 固定三栏", DARK_NAVY_QSS)

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

    def test_central_workspace_expands_when_panels_collapsed_or_hidden(self) -> None:
        self._login()
        self._pump(6)
        body = self.window._workbench_body
        central = self.window._central_workspace
        nav = self.window._navigation_panel
        config = self.window._config_panel

        # 登录后进入宽屏工作台；标准状态下左栏 210 + 右栏 282 均可见。
        self.assertGreaterEqual(self.window.minimumWidth(), 1180)
        self.assertTrue(nav.isVisible())
        self.assertTrue(config.isVisible())
        base = central.width()
        self.assertGreater(base, 300)

        # 隐藏右侧配置面板后中央获得完整 282px，不保留空列。
        config.panel_toggle_button.click()
        self._pump(6)
        self.assertFalse(config.isVisible())
        self.assertFalse(self.window.state.right_panel_visible)
        self.assertEqual(base + 282, central.width())

        # 右栏隐藏后收起左栏：左右均隐藏，左栏不占宽度，中央铺满工作区。
        self.window._title_bar._navigation_button.click()
        self._pump(6)
        self.assertFalse(nav.isVisible())
        self.assertEqual(body.width(), central.width())

        # 通过标题栏“导航”恢复左栏：回到右栏隐藏、左栏展开(210)状态。
        self.window._title_bar._navigation_button.click()
        self._pump(6)
        self.assertTrue(nav.isVisible())
        self.assertTrue(self.window.state.left_navigation_expanded)
        self.assertEqual(210, nav.width())
        self.assertEqual(body.width() - 210, central.width())

    def test_theme_switch_updates_all_regions_and_preserves_selection(self) -> None:
        from vnpy_oxapp.ui.theme import LIGHT_QSS

        self._login()
        self.window._welcome_page._products_list.setCurrentRow(1)
        self.window._config_panel.panel_toggle_button.click()
        self.assertFalse(self.window.state.right_panel_visible)

        self.window._title_bar._theme_button.click()
        self.assertEqual(ThemeMode.LIGHT, self.window.state.theme)
        self.assertIn("#f3f6fa", self.app.styleSheet())
        self.assertIn("#f3f6fa", LIGHT_QSS)
        self.assertEqual("中证 500", self.window.state.selected_product)
        self.assertFalse(self.window.state.right_panel_visible)
        self.assertEqual("浅色", self.window._config_panel._theme.text())
        self.assertEqual("主题：浅色", self.window._theme_status.text())

        self.window._title_bar._theme_button.click()
        self.assertEqual(ThemeMode.DARK_NAVY, self.window.state.theme)
        self.assertEqual("深蓝", self.window._config_panel._theme.text())
        self.assertTrue(self.window._title_bar._theme_button.toolTip())

    def test_visible_shell_copy_uses_formal_business_wording(self) -> None:
        from PySide6.QtWidgets import QLabel

        self._login()
        visible_text = " ".join(
            widget.text()
            for widget in self.window.findChildren(QLabel)
            if widget.isVisible()
        )
        for forbidden in (
            "Mock", "模拟", "占位", "待接入", "不代表真实",
            "统一入口", "行情 · 交易 · 管理", "安全连接", "连接状态可见",
            "服务状态：可用", "专业工作台", "服务运行正常", "最后同步",
        ):
            self.assertNotIn(forbidden, visible_text)
