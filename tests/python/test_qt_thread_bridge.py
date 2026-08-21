from __future__ import annotations

import unittest
from threading import Thread, get_ident


class QtBridgeImportTest(unittest.TestCase):
    def test_optional_qt_bridge_has_clear_dependency_boundary(self) -> None:
        from vnpy_oxapp.ui.qt_bridge import AsyncQtBridge, PYSIDE_AVAILABLE

        if not PYSIDE_AVAILABLE:
            with self.assertRaisesRegex(RuntimeError, "PySide6"):
                AsyncQtBridge()
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
        publisher = Thread(target=lambda: bridge.publish_response({"request_id": "r-1"}))
        publisher.start()
        publisher.join()
        for _ in range(20):
            app.processEvents()  # 排空 queued signal，验证 Timer/网络线程不直接触碰 QWidget。
            if receiver_threads:
                break
        self.assertEqual([get_ident()], receiver_threads)
