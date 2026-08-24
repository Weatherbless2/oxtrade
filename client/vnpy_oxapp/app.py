"""Mock-first oxApp 客户端启动入口。"""

from __future__ import annotations

import sys

from .connection import ClientProtocolClient
from .logging import create_startup_logger
from .mock.mock_transport import MockTransport
from .ui.main_window import OxAppMainWindow
from .ui.qt_bridge import AsyncQtBridge
from .ui.theme import apply_workbench_theme


def main() -> int:
    logger = create_startup_logger()  # 每次进程启动新建 oxTrader 日志，记录启动和异常边界。
    logger.info("client_starting", module="oxApp")
    try:
        from PySide6.QtWidgets import QApplication  # 启动入口只在真正运行 GUI 时加载可选依赖。
    except ImportError:
        print("PySide6 is required; run: python -m pip install -r client/requirements.txt", file=sys.stderr)  # 明确依赖缺失和安装方式。
        logger.error("client_start_failed reason=missing_pyside6", module="oxApp")
        logger.close()
        return 2  # 非零退出码让脚本/验收知道 GUI 根本没有启动。
    try:
        app = QApplication(sys.argv)  # 创建 Qt 主线程事件循环，所有 QWidget 必须归它管理。
        apply_workbench_theme(app)  # 正式入口和测试构造窗口共用同一套 vn.py 风格工作台主题。
        bridge = AsyncQtBridge()  # 建立后台 Transport 到 GUI 主线程的信号边界。
        client = ClientProtocolClient(MockTransport())  # #16 只接入确定性 Mock，不连接真实后台。
        client.set_response_callback(bridge.publish_response)  # 响应先进入 Qt queued signal，再由页面消费。
        client.set_state_callback(lambda state, reason: bridge.publish_state(state.value, reason))  # 状态同样禁止后台线程直接碰控件。
        client.connect()  # 连接状态通过 bridge 更新状态栏。
        window = OxAppMainWindow(bridge, client=client, logger=logger)  # 构造公共导航和页面插槽，并记录控件加载耗时。
        window.show()  # 将窗口交给 Qt 事件循环显示。
        logger.info("client_ready", module="oxApp")
        return app.exec()  # 阻塞在 GUI 事件循环，退出码由 Qt 返回给操作系统。
    except Exception as error:
        logger.error(f"client_start_failed error={error!r}", module="oxApp")
        raise
    finally:
        logger.info("client_stopping", module="oxApp")
        logger.close()


if __name__ == "__main__":
    raise SystemExit(main())
