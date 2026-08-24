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

    def test_main_window_keeps_vnpy_workbench_regions_separate(self) -> None:
        """验收 vn.py 风格的导航、中心页、运行状态和日志区域不会重新挤成单页。"""
        from PySide6.QtWidgets import QApplication, QDockWidget
        from vnpy_oxapp.ui.main_window import OxAppMainWindow
        from vnpy_oxapp.ui.qt_bridge import AsyncQtBridge

        app = QApplication.instance() or QApplication([])
        window = OxAppMainWindow(AsyncQtBridge())
        dock_names = {dock.objectName() for dock in window.findChildren(QDockWidget)}
        self.assertTrue({"navigationDock", "runtimeDock", "logDock"}.issubset(dock_names))
        self.assertEqual(5, window._navigation.count())
        self.assertEqual(5, window._pages.count())
        self.assertEqual("概览", window._navigation.item(0).text())
        window.close()
        app.processEvents()
