from __future__ import annotations

import unittest
from threading import Thread, get_ident


class QtBridgeImportTest(unittest.TestCase):
    def test_optional_qt_bridge_has_clear_dependency_boundary(self) -> None:
        from vnpy_oxapp.ui.qt_bridge import AsyncQtBridge, PYSIDE_AVAILABLE

        if not PYSIDE_AVAILABLE:  # 核心环境可不安装 Qt，但边界必须给出明确错误。
            with self.assertRaisesRegex(RuntimeError, "PySide6"):
                AsyncQtBridge()  # 验证缺依赖不会静默创建不可用桥接对象。
        else:
            bridge = AsyncQtBridge()
            self.assertIsNotNone(bridge)

    def test_signal_delivers_background_publish_on_qt_main_thread(self) -> None:
        from vnpy_oxapp.ui.qt_bridge import AsyncQtBridge, PYSIDE_AVAILABLE

        if not PYSIDE_AVAILABLE:
            self.skipTest("PySide6 is not installed")
        from PySide6.QtCore import QCoreApplication

        app = QCoreApplication.instance() or QCoreApplication([])
        bridge = AsyncQtBridge()
        receiver_threads = []
        bridge.response_received.connect(lambda response: receiver_threads.append(get_ident()))
        publisher = Thread(target=lambda: bridge.publish_response({"request_id": "r-1"}))  # 从后台线程发布，模拟 Timer/网络回调。
        publisher.start()  # 后台线程只能 emit signal，不能访问 QWidget。
        publisher.join()  # 等后台发布动作结束，再由主线程排空事件队列。
        for _ in range(20):
            app.processEvents()  # 排空 queued signal，验证 Timer/网络线程不直接触碰 QWidget。
            if receiver_threads:
                break
        self.assertEqual([get_ident()], receiver_threads)  # 接收回调线程必须是 Qt 主线程。

    def test_market_signal_delivers_background_event_on_qt_main_thread(self) -> None:
        """#14 行情 Socket 线程与既有请求回包一样，必须经 Qt queued signal 进入页面。"""
        from vnpy_oxapp.ui.qt_bridge import AsyncQtBridge, PYSIDE_AVAILABLE

        if not PYSIDE_AVAILABLE:
            self.skipTest("PySide6 is not installed")
        from PySide6.QtCore import QCoreApplication

        app = QCoreApplication.instance() or QCoreApplication([])
        bridge = AsyncQtBridge()
        receiver_threads = []
        bridge.market_event_received.connect(lambda event: receiver_threads.append(get_ident()))
        publisher = Thread(target=lambda: bridge.publish_market_event({"symbol": "000001.SZ"}))  # 用普通后台线程模拟 PyTdx Socket 回调所在执行流。
        publisher.start()  # 后台线程只 emit Qt signal；若直接操作 QWidget，真实运行会出现跨线程崩溃或竞态。
        publisher.join()  # 先确认生产者结束，再由主线程主动处理 queued signal。
        for _ in range(20):
            app.processEvents()  # 测试没有 app.exec()，手工泵事件循环才能交付排队到主线程的行情信号。
            if receiver_threads:
                break
        self.assertEqual([get_ident()], receiver_threads)  # 接收线程必须等于测试/Qt 主线程，失败即定位跨线程边界错误。

    def test_main_window_records_layout_and_page_load_timings(self) -> None:
        from vnpy_oxapp.ui.qt_bridge import AsyncQtBridge, PYSIDE_AVAILABLE

        if not PYSIDE_AVAILABLE:
            self.skipTest("PySide6 is not installed")
        from PySide6.QtWidgets import QApplication
        from vnpy_oxapp.ui.main_window import OxAppMainWindow

        app = QApplication.instance() or QApplication([])
        window = OxAppMainWindow(AsyncQtBridge())
        names = [timing.component for timing in window.load_timings]
        self.assertEqual(["main_window_layout", "main_window_pages"], names)
        window.close()
        app.processEvents()

    def test_main_window_uses_fixed_navigation_and_config_panels(self) -> None:
        """验收新壳层使用固定三栏，运行日志由左侧导航进入。"""
        from PySide6.QtWidgets import QApplication, QDockWidget
        from vnpy_oxapp.ui.main_window import OxAppMainWindow
        from vnpy_oxapp.ui.qt_bridge import AsyncQtBridge

        app = QApplication.instance() or QApplication([])
        window = OxAppMainWindow(AsyncQtBridge())
        dock_names = {dock.objectName() for dock in window.findChildren(QDockWidget)}
        self.assertEqual(set(), dock_names)
        self.assertEqual(210, window._navigation_panel.width())
        self.assertEqual(282, window._config_panel.width())
        self.assertEqual(9, window._navigation.count())
        self.assertEqual(9, window._pages.count())
        self.assertEqual("⌂  工作台", window._navigation.item(0).text())
        self.assertEqual("⌁  行情中心", window._navigation.item(5).text())
        self.assertEqual("⇄  交易终端", window._navigation.item(6).text())
        self.assertEqual("●  登录会话", window._navigation.item(7).text())
        self.assertEqual("≡  运行日志", window._navigation.item(8).text())
        window.close()
        app.processEvents()
