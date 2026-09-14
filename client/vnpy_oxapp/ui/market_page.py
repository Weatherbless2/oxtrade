"""行情中心：展示行情列表、Tick、分钟 K 线和五档盘口。"""

from __future__ import annotations

from typing import Any

from ..connection import ClientProtocolClient
from ..hpsocket_transport import HpSocketTransport
from ..logging import ClientLogger
from ..market.hub import MarketHub
from ..message import ClientResponse
from ..services.market_service import MarketService
from .qt_bridge import AsyncQtBridge
from .table_page import ServiceTablePage, TableSpec

try:
    from PySide6.QtCore import QTimer, Signal
    from PySide6.QtWidgets import QComboBox, QFrame, QHBoxLayout, QLabel, QPushButton, QVBoxLayout, QWidget
    PYSIDE_AVAILABLE = True
except ImportError:
    PYSIDE_AVAILABLE = False


if PYSIDE_AVAILABLE:
    class MarketPage(QWidget):
        """行情页面通过 MarketService 读取快照，不直接读取 Socket 或行情线程中的可变数据。"""

        subscription_changed = Signal(str)

        def __init__(self, client: ClientProtocolClient, bridge: AsyncQtBridge, logger: ClientLogger, session="mock-session", market_hub: MarketHub | None = None, parent=None) -> None:
            super().__init__(parent)
            self.bridge = bridge
            self.logger = logger
            self.market_hub = market_hub
            self._formal_qmt = isinstance(client.transport, HpSocketTransport)  # 正式模式的表格只显示 QMT/PG 回包，不混入开发行情源。
            self.service = MarketService(client, session, market_hub)
            self._pending: set[str] = set()
            self._hub_unsubscribers: list[Any] = []
            self._refresh_timer = QTimer(self)
            self._refresh_timer.setSingleShot(True)
            self._refresh_timer.timeout.connect(self._refresh_from_market)  # 高频事件先合并，单个 Tick 不应逐条触发表格重绘。
            self._symbols = QComboBox()
            symbols = (
                (("688693.SH", "锴威特"), ("110075.SH", "南航转债"))
                if self._formal_qmt else
                (("000001.SZ", "平安银行"), ("600000.SH", "浦发银行"), ("600519.SH", "贵州茅台"),
                 ("000300.SZ", "沪深300"), ("510300.SH", "300ETF"), ("IF2609.CFX", "IF2609"),
                 ("IC2609.CFX", "IC2609"), ("159915.SZ", "创业板ETF"))
            )
            for symbol, name in symbols:
                self._symbols.addItem(f"{symbol}  {name}", symbol)
            subscribe = QPushButton("订阅行情")
            subscribe.clicked.connect(self._subscribe)
            self._status = QLabel("行情源：正在初始化")
            self._status.setObjectName("tableSummary")
            toolbar_layout = QHBoxLayout()
            toolbar_layout.setContentsMargins(0, 0, 0, 0)
            toolbar_layout.addWidget(QLabel("标的"))
            toolbar_layout.addWidget(self._symbols)
            toolbar_layout.addWidget(subscribe)
            toolbar_layout.addWidget(self._status, 1)
            toolbar = QFrame()
            toolbar.setObjectName("summaryBar")
            toolbar.setLayout(toolbar_layout)

            specs = (
                TableSpec(
                    "行情列表",
                    (("instrument_id", "标的"), ("name", "名称"), ("market", "市场"), ("last_price", "最新"),
                     ("pre_close", "昨收"), ("volume", "成交量"), ("amount", "成交额"),
                     ("quote_time", "行情时间"), ("receive_time", "入库时间"), ("source", "来源"))
                    if self._formal_qmt else
                    (("symbol", "代码"), ("name", "名称"), ("market", "市场"), ("last", "最新"),
                     ("change_percent", "涨跌幅"), ("volume", "成交量"), ("amount", "成交额"), ("update_time", "更新时间")),
                    lambda filters: self.service.list_quotes(**filters),
                ),
                TableSpec(
                    "Tick 明细",
                    (("datetime", "时间"), ("symbol", "代码"), ("name", "名称"), ("last", "最新价"),
                     ("volume", "成交量"), ("turnover", "成交额"), ("bid1", "买一"), ("ask1", "卖一"), ("open_interest", "持仓量")),
                    lambda filters: self.service.list_ticks(**filters),
                ),
                TableSpec(
                    "一分钟 K 线",
                    (("datetime", "时间"), ("symbol", "代码"), ("open", "开"), ("high", "高"),
                     ("low", "低"), ("close", "收"), ("volume", "成交量"), ("amount", "成交额")),
                    lambda filters: self.service.list_bars(**filters),
                ),
                TableSpec(
                    "五档盘口",
                    (("symbol", "代码"), ("name", "名称"), ("bid1_price", "买一价"), ("bid1_volume", "买一量"),
                     ("bid2_price", "买二价"), ("bid2_volume", "买二量"), ("ask1_price", "卖一价"),
                     ("ask1_volume", "卖一量"), ("ask2_price", "卖二价"), ("ask2_volume", "卖二量")),
                    lambda filters: self.service.list_depth(**filters),
                ),
            )
            self._tables = ServiceTablePage(client, bridge, logger, specs)
            self._tabs = self._tables._tabs  # 主窗口刷新按钮沿用 ServiceTablePage 的当前页刷新语义。
            layout = QVBoxLayout(self)
            layout.setContentsMargins(0, 0, 0, 0)
            layout.setSpacing(8)
            layout.addWidget(toolbar)
            layout.addWidget(self._tables, 1)
            bridge.response_received.connect(self._on_response)
            bridge.market_event_received.connect(self._on_market_event)
            bridge.market_state_changed.connect(self._on_market_state)
            if self.market_hub is not None:
                self._hub_unsubscribers.append(self.market_hub.add_event_listener(bridge.publish_market_event))  # 保存取消函数，页面销毁后 Hub 不再向旧 Qt 对象发信号。
                self._hub_unsubscribers.append(self.market_hub.add_state_listener(
                    lambda status: bridge.publish_market_state(status.state.value, f"{status.source} | {status.reason}"),
                ))  # Hub 在后台线程通知 Qt bridge，页面自身绝不注册跨线程 QWidget 回调。
            else:
                self._status.setText("行情源：未订阅")
            self._symbols.currentIndexChanged.connect(self._refresh_selected_symbol)  # 表格对象已就绪后再监听选择变化，避免构造期信号访问未创建成员。
            self._set_symbol_filter()
            self._tables._load(0)  # ServiceTablePage 构造时已请求过未筛选快照；此处发起当前标的筛选的正式初始请求。

        def _load_current(self, index: int) -> None:
            self._set_symbol_filter()
            self._tables._load_current(index)

        def _set_symbol_filter(self) -> None:
            symbol = str(self._symbols.currentData() or "")
            indices = (0, 1, 2, 3) if self._formal_qmt else (1, 2, 3)
            for index in indices:
                self._tables._panels[index][1].set_query_text(symbol)  # Tick/K线/盘口按当前标的请求，行情总表仍保留全市场扫描视图。

        def _refresh_selected_symbol(self) -> None:
            self._set_symbol_filter()
            self._load_current(self._tabs.currentIndex())  # 切换标的立即重新请求当前页，不能只改输入框而显示旧数据。

        def _subscribe(self) -> None:
            symbol = str(self._symbols.currentData() or "")  # 下拉框保存内部标准代码，不把带中文名称的展示文本发给 Feed。
            future = self.service.subscribe([symbol])  # 沪深股票进入 Hub Feed，期货继续走既有 MockGateway/未来正式 SDK。
            request_id = getattr(future, "request_id", "")
            if not request_id:
                self._status.setText("订阅请求未生成关联编号")
                return
            self._pending.add(request_id)  # 页面只消费自己发出的订阅响应，其他表格 Future 不会误改行情状态栏。
            future.add_done_callback(self._publish_future)  # 本机缓存和远端 Mock 都统一走 Future 完成回调。
            self._status.setText(f"正在订阅 {symbol}...")

        def _publish_future(self, future: Any) -> None:
            try:
                self.bridge.publish_response(future.result())  # Future 可能在后台线程完成，只转发 Qt signal。
            except Exception as error:
                self.logger.error(f"market_response_publish_failed error={error!r}", module="oxApp.market")

        def _on_response(self, response: ClientResponse) -> None:
            if response.request_id not in self._pending:
                return
            self._pending.discard(response.request_id)
            if response.is_success:
                source = str(response.payload.get("source", "行情服务"))
                if "mock" in source.lower():
                    source = "行情服务"
                self._status.setText(f"行情源：{source} | 已订阅 {response.payload.get('symbols', '当前标的')}")
                self.subscription_changed.emit(str(response.payload.get("symbols", "")))
                self._load_current(self._tabs.currentIndex())
            else:
                self._status.setText(f"订阅失败：{response.error_message}")

        def _on_market_event(self, event: object) -> None:
            if self.market_hub is None or not hasattr(event, "symbol"):
                return
            selected = str(self._symbols.currentData() or "")
            current_tab = self._tabs.currentIndex()
            if current_tab != 0 and getattr(event, "symbol", "") != selected:
                return  # Tick/K线/盘口只因当前标的更新；总行情表才响应任意订阅标的。
            if not self._refresh_timer.isActive():
                self._refresh_timer.start(self.market_hub.ui_refresh_ms)  # 例如 200ms 内多个 Tick 合并为一次快照查询和表格刷新。

        def _on_market_state(self, state: str, reason: str) -> None:
            if self.market_hub is None:
                return
            metrics = self.market_hub.feed_status.metrics
            del reason
            self._status.setText(f"行情源：行情服务 | 状态：{state} | 重连 {metrics.reconnects}")

        def _refresh_from_market(self) -> None:
            self._load_current(self._tabs.currentIndex())  # 定时器运行在 Qt 主线程，Service 从 Hub 快照读取而不接触行情 Socket。

        def closeEvent(self, event) -> None:
            for unsubscribe in self._hub_unsubscribers:
                unsubscribe()  # 从 Hub 锁保护的监听表移除当前页面，行情 Feed 和其他订阅者继续运行。
            self._hub_unsubscribers.clear()  # 清空所有权记录，重复关闭不会重复调用已经移除的监听项。
            super().closeEvent(event)  # 最后交还 Qt 执行 QWidget 正常关闭；全局 Hub 由 app.finally 统一 stop。
else:
    class MarketPage:  # type: ignore[no-redef]
        def __init__(self, *args, **kwargs) -> None:
            del args, kwargs
            raise RuntimeError("Qt UI requires PySide6; install client/requirements.txt first")
