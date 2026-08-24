"""Mock-first oxApp 客户端启动入口。"""

from __future__ import annotations

import sys

from .connection import ClientProtocolClient
from .mock.mock_transport import MockTransport
from .ui.main_window import OxAppMainWindow
from .ui.qt_bridge import AsyncQtBridge


def main() -> int:
    try:
        from PySide6.QtWidgets import QApplication  # 启动入口只在真正运行 GUI 时加载可选依赖。
    except ImportError:
        print("PySide6 is required; run: python -m pip install -r client/requirements.txt", file=sys.stderr)  # 明确依赖缺失和安装方式。
        return 2  # 非零退出码让脚本/验收知道 GUI 根本没有启动。
    app = QApplication(sys.argv)  # 创建 Qt 主线程事件循环，所有 QWidget 必须归它管理。
    bridge = AsyncQtBridge()  # 建立后台 Transport 到 GUI 主线程的信号边界。
    client = ClientProtocolClient(MockTransport())  # #16 只接入确定性 Mock，不连接真实后台。
    client.set_response_callback(bridge.publish_response)  # 响应先进入 Qt queued signal，再由页面消费。
    client.set_state_callback(lambda state, reason: bridge.publish_state(state.value, reason))  # 状态同样禁止后台线程直接碰控件。
    client.connect()  # 连接状态通过 bridge 更新状态栏。
    window = OxAppMainWindow(bridge)  # 构造公共导航和页面插槽，#18-#21 后续填充。
    window.show()  # 将窗口交给 Qt 事件循环显示。
    return app.exec()  # 阻塞在 GUI 事件循环，退出码由 Qt 返回给操作系统。


if __name__ == "__main__":
    raise SystemExit(main())
