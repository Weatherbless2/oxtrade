"""Mock-first oxApp 客户端启动入口。"""

from __future__ import annotations

import sys

from .connection import ClientProtocolClient
from .mock.mock_transport import MockTransport
from .ui.main_window import OxAppMainWindow
from .ui.qt_bridge import AsyncQtBridge


def main() -> int:
    try:
        from PySide6.QtWidgets import QApplication
    except ImportError:
        print("PySide6 is required; run: python -m pip install -r client/requirements.txt", file=sys.stderr)
        return 2
    app = QApplication(sys.argv)
    bridge = AsyncQtBridge()
    client = ClientProtocolClient(MockTransport())
    client.set_response_callback(bridge.publish_response)
    client.set_state_callback(lambda state, reason: bridge.publish_state(state.value, reason))
    client.connect()
    window = OxAppMainWindow(bridge)
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
