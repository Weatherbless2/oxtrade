"""oxApp 客户端启动入口。

启动顺序固定为：创建日志和 QApplication → 应用主题 → 建立 Qt Bridge
与客户端 Transport → 启动行情 Hub → 构造主窗口 → 进入 Qt 事件循环。
运行模式默认是 Mock；显式启用 ``hpsocket`` 时，业务页面通过正式
PackMessage/HP-Socket 读取 oxMBS 数据。退出时按相反顺序停止行情、断开 Transport 并关闭日志，确保后台线程
不会在 QWidget 销毁后继续回调。
"""

from __future__ import annotations

import sys

from .connection import ClientProtocolClient
from .logging import create_startup_logger
from .market import MarketHub, create_market_feed, load_market_config
from .runtime_config import create_client_transport, load_client_runtime_config
from .ui.main_window import OxAppMainWindow
from .ui.qt_bridge import AsyncQtBridge
from .ui.theme import apply_workbench_theme


def main() -> int:
    """启动 oxApp GUI 并在退出时释放客户端、行情和日志资源。"""
    logger = create_startup_logger()  # 每次进程启动新建 oxTrader 日志，记录启动和异常边界。
    client: ClientProtocolClient | None = None
    market_hub: MarketHub | None = None
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
        runtime_config = load_client_runtime_config()  # 运行模式由配置/环境决定，页面不自行选择数据源。
        transport = create_client_transport(runtime_config)
        client = ClientProtocolClient(transport)
        logger.info(
            f"client_transport kind={runtime_config.transport_kind} "
            f"endpoint={runtime_config.host}:{runtime_config.port}",
            module="oxApp",
        )
        client.set_response_callback(bridge.publish_response)  # 响应先进入 Qt queued signal，再由页面消费。
        client.set_state_callback(lambda state, reason: bridge.publish_state(state.value, reason))  # 状态同样禁止后台线程直接碰控件。
        client.connect()  # 连接状态通过 bridge 更新状态栏。
        market_hub = None
        if runtime_config.transport_kind == "mock":
            # Mock 模式保留原有开发行情 Hub；正式 Socket 模式直接由 MarketService 请求 QMT/PG。
            market_config = load_market_config()
            market_hub = MarketHub(
                create_market_feed(market_config),
                tick_capacity=market_config.tick_buffer_size,
                bar_capacity=market_config.bar_buffer_size,
                ui_refresh_ms=market_config.ui_refresh_ms,
            )
            market_hub.start()  # 先启动缓存与数据源，再构建页面；首批行情即使先到也会保存在 Hub 中。
        window = OxAppMainWindow(
            bridge,
            client=client,
            logger=logger,
            market_hub=market_hub,
            session_id=runtime_config.session_id,
        )  # 构造公共导航和页面插槽，并记录控件加载耗时。
        window.show()  # 将窗口交给 Qt 事件循环显示。
        logger.info("client_ready", module="oxApp")
        return app.exec()  # 阻塞在 GUI 事件循环，退出码由 Qt 返回给操作系统。
    except Exception as error:
        logger.error(f"client_start_failed error={error!r}", module="oxApp")
        raise
    finally:
        if market_hub is not None:
            market_hub.stop()  # Socket/Mock 线程属于 Hub，GUI 退出时必须先停止，避免回调访问已销毁的 Qt 对象。
        if client is not None:
            client.disconnect()  # 本地 MockBackendTransport 通过 EOF 清理子进程及它拥有的 IPC 资源。
        logger.info("client_stopping", module="oxApp")
        logger.close()


if __name__ == "__main__":
    raise SystemExit(main())
