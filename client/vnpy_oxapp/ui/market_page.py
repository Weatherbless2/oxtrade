"""Mock 行情中心：展示行情列表、Tick、分钟 K 线和五档盘口。"""

from __future__ import annotations

from typing import Any

from ..connection import ClientProtocolClient
from ..logging import ClientLogger
from ..message import ClientResponse
from ..services.market_service import MarketService
from .qt_bridge import AsyncQtBridge
from .table_page import ServiceTablePage, TableSpec

try:
    from PySide6.QtCore import Signal
    from PySide6.QtWidgets import QComboBox, QFrame, QHBoxLayout, QLabel, QPushButton, QVBoxLayout, QWidget
    PYSIDE_AVAILABLE = True
except ImportError:
    PYSIDE_AVAILABLE = False


if PYSIDE_AVAILABLE:
    class MarketPage(QWidget):
        """行情页面通过 MarketService 请求 Mock 高频网关，不直接读取本地行情数据。"""

        subscription_changed = Signal(str)

        def __init__(self, client: ClientProtocolClient, bridge: AsyncQtBridge, logger: ClientLogger, session="mock-session", parent=None) -> None:
            super().__init__(parent)
            self.bridge = bridge
            self.logger = logger
            self.service = MarketService(client, session)
            self._pending: set[str] = set()
            self._symbols = QComboBox()
            for symbol, name in (
                ("000001.SZ", "平安银行"), ("600000.SH", "浦发银行"), ("600519.SH", "贵州茅台"),
                ("000300.SH", "沪深300"), ("510300.SH", "300ETF"), ("IF2609.CFX", "IF2609"),
                ("IC2609.CFX", "IC2609"), ("159915.SZ", "创业板ETF"),
            ):
                self._symbols.addItem(f"{symbol}  {name}", symbol)
            subscribe = QPushButton("订阅 Mock 行情")
            subscribe.clicked.connect(self._subscribe)
            self._status = QLabel("行情源：MockMarketGateway | 未订阅")
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
            self._symbols.currentIndexChanged.connect(self._refresh_selected_symbol)  # 表格对象已就绪后再监听选择变化，避免构造期信号访问未创建成员。
            self._set_symbol_filter()
            self._tables._load(0)  # ServiceTablePage 构造时已请求过未筛选快照；此处发起当前标的筛选的正式初始请求。

        def _load_current(self, index: int) -> None:
            self._set_symbol_filter()
            self._tables._load_current(index)

        def _set_symbol_filter(self) -> None:
            symbol = str(self._symbols.currentData() or "")
            for index in (1, 2, 3):
                self._tables._panels[index][1].set_query_text(symbol)  # Tick/K线/盘口按当前标的请求，行情总表仍保留全市场扫描视图。

        def _refresh_selected_symbol(self) -> None:
            self._set_symbol_filter()
            self._load_current(self._tabs.currentIndex())  # 切换标的立即重新请求当前页，不能只改输入框而显示旧数据。

        def _subscribe(self) -> None:
            symbol = str(self._symbols.currentData() or "")
            future = self.service.subscribe([symbol])
            request_id = getattr(future, "request_id", "")
            if not request_id:
                self._status.setText("订阅请求未生成关联编号")
                return
            self._pending.add(request_id)
            future.add_done_callback(self._publish_future)
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
                self._status.setText(f"行情源：MockMarketGateway | 已订阅 {response.payload.get('symbols', '当前标的')}")
                self.subscription_changed.emit(str(response.payload.get("symbols", "")))
                self._load_current(self._tabs.currentIndex())
            else:
                self._status.setText(f"订阅失败：{response.error_message}")
else:
    class MarketPage:  # type: ignore[no-redef]
        def __init__(self, *args, **kwargs) -> None:
            del args, kwargs
            raise RuntimeError("Qt UI requires PySide6; install client/requirements.txt first")
