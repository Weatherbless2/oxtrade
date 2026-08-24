"""Mock 交易终端：下单、撤单、模拟成交和资金持仓查询使用同一异步链路。"""

from __future__ import annotations

from typing import Any

from ..connection import ClientProtocolClient
from ..logging import ClientLogger
from ..message import ClientResponse
from ..services.trade_service import TradeService
from .qt_bridge import AsyncQtBridge
from .table_page import ServiceTablePage, TableSpec

try:
    from PySide6.QtCore import Qt
    from PySide6.QtWidgets import (
        QComboBox, QDoubleSpinBox, QFormLayout, QFrame, QGroupBox, QHBoxLayout,
        QLabel, QPushButton, QSpinBox, QVBoxLayout, QWidget,
    )
    PYSIDE_AVAILABLE = True
except ImportError:
    PYSIDE_AVAILABLE = False


if PYSIDE_AVAILABLE:
    class TradingPage(QWidget):
        """交易页面只发业务请求；所有订单、成交、账户和仓位快照均由 MockGateway 返回。"""

        def __init__(self, client: ClientProtocolClient, bridge: AsyncQtBridge, logger: ClientLogger, session="mock-session", parent=None) -> None:
            super().__init__(parent)
            self.bridge = bridge
            self.logger = logger
            self.service = TradeService(client, session)
            self._pending: dict[str, str] = {}
            self._instrument = QComboBox()
            self._account = QComboBox()
            self._side = QComboBox()
            self._price = QDoubleSpinBox()
            self._volume = QSpinBox()
            self._state = QLabel("模拟交易：等待委托")
            self._state.setObjectName("tableSummary")
            self._build_order_entry()

            specs = (
                TableSpec(
                    "资金账户",
                    (("account_id", "资金账号"), ("account_name", "名称"), ("currency", "币种"), ("total_asset", "总资产"),
                     ("available", "可用资金"), ("frozen", "冻结资金"), ("market_value", "持仓市值"), ("daily_pnl", "当日盈亏"), ("status", "状态")),
                    lambda filters: self.service.list_accounts(**filters),
                ),
                TableSpec(
                    "持仓",
                    (("position_id", "持仓编号"), ("account_id", "资金账号"), ("symbol", "代码"), ("name", "名称"),
                     ("total_volume", "总持仓"), ("available_volume", "可用持仓"), ("today_volume", "今买"),
                     ("yesterday_volume", "昨仓"), ("frozen_volume", "冻结持仓"), ("cost_price", "成本价"),
                     ("last_price", "最新价"), ("market_value", "市值"), ("pnl", "盈亏")),
                    lambda filters: self.service.list_positions(**filters),
                ),
                TableSpec(
                    "委托",
                    (("order_id", "委托编号"), ("account_id", "资金账号"), ("symbol", "代码"), ("name", "名称"),
                     ("side", "方向"), ("order_type", "类型"), ("price", "委托价"), ("volume", "委托量"),
                     ("traded_volume", "成交量"), ("frozen_cash", "冻结资金"), ("status", "状态"), ("order_time", "委托时间")),
                    lambda filters: self.service.list_orders(**filters),
                    (("全部", ""), ("活动委托", "working"), ("部分成交", "partial"), ("已成交", "filled"), ("已撤销", "cancelled")),
                    primary_key="order_id",
                    transitioner=lambda order_id, _state: self.service.cancel_order(order_id),
                    transition_tooltip="撤销选中委托",
                ),
                TableSpec(
                    "成交",
                    (("trade_id", "成交编号"), ("order_id", "委托编号"), ("symbol", "代码"), ("name", "名称"),
                     ("side", "方向"), ("price", "成交价"), ("volume", "成交量"), ("trade_time", "成交时间"), ("gateway", "来源")),
                    lambda filters: self.service.list_trades(**filters),
                ),
            )
            self._tables = ServiceTablePage(client, bridge, logger, specs)
            self._tabs = self._tables._tabs  # 主窗口“刷新当前”沿用通用表格页的加载逻辑。
            layout = QVBoxLayout(self)
            layout.setContentsMargins(0, 0, 0, 0)
            layout.setSpacing(8)
            layout.addWidget(self._order_box)
            layout.addWidget(self._state)
            layout.addWidget(self._tables, 1)
            bridge.response_received.connect(self._on_response)

        def _build_order_entry(self) -> None:
            for symbol, name, account_id, price in (
                ("000001.SZ", "平安银行", "MOCK-ACCT-001", 10.28),
                ("600000.SH", "浦发银行", "MOCK-ACCT-001", 18.18),
                ("600519.SH", "贵州茅台", "MOCK-ACCT-001", 26.70),
                ("510300.SH", "300ETF", "MOCK-ACCT-001", 43.40),
                ("159915.SZ", "创业板ETF", "MOCK-ACCT-001", 68.02),
                ("IF2609.CFX", "IF2609", "MOCK-ACCT-002", 51.75),
                ("IC2609.CFX", "IC2609", "MOCK-ACCT-002", 60.10),
            ):
                self._instrument.addItem(f"{symbol}  {name}", (symbol, name, account_id, price))
            self._account.addItem("MOCK-ACCT-001  量化测试账户", "MOCK-ACCT-001")
            self._account.addItem("MOCK-ACCT-002  期货模拟账户", "MOCK-ACCT-002")
            self._side.addItem("买入", "buy")
            self._side.addItem("卖出", "sell")
            self._price.setDecimals(2)
            self._price.setRange(0.01, 999999.99)
            self._price.setSingleStep(0.01)
            self._volume.setRange(1, 500000)
            submit = QPushButton("提交模拟委托")
            cancel = QPushButton("撤销选中委托")
            fill = QPushButton("模拟成交选中委托")
            submit.clicked.connect(self._submit_order)
            cancel.clicked.connect(self._cancel_selected)
            fill.clicked.connect(self._fill_selected)
            self._instrument.currentIndexChanged.connect(self._sync_instrument_defaults)
            self._sync_instrument_defaults()

            form = QFormLayout()
            form.addRow("标的", self._instrument)
            form.addRow("资金账号", self._account)
            form.addRow("方向", self._side)
            form.addRow("委托价格", self._price)
            form.addRow("委托数量", self._volume)
            commands = QHBoxLayout()
            commands.addWidget(submit)
            commands.addWidget(cancel)
            commands.addWidget(fill)
            commands.addStretch(1)
            form.addRow("操作", commands)
            self._order_box = QGroupBox("委托录入")
            self._order_box.setLayout(form)

        def _sync_instrument_defaults(self) -> None:
            selected = self._instrument.currentData()
            if not isinstance(selected, tuple):
                return
            symbol, _name, account_id, price = selected
            account_index = self._account.findData(account_id)
            if account_index >= 0:
                self._account.setCurrentIndex(account_index)
            self._price.setValue(float(price))
            is_future = str(symbol).endswith(".CFX")
            self._volume.setMinimum(1 if is_future else 100)
            self._volume.setSingleStep(1 if is_future else 100)
            self._volume.setValue(1 if is_future else 100)  # 股票默认一手，期货默认一张，前端先减少不合法输入。

        def _load_current(self, index: int) -> None:
            self._tables._load_current(index)

        def _submit_order(self) -> None:
            selected = self._instrument.currentData()
            if not isinstance(selected, tuple):
                self._state.setText("模拟交易：未选择有效标的")
                return
            symbol, name, _default_account, _price = selected
            future = self.service.place_order(
                account_id=str(self._account.currentData() or ""), symbol=str(symbol), name=str(name),
                side=str(self._side.currentData() or ""), price=float(self._price.value()), volume=int(self._volume.value()),
            )
            self._track(future, "place")
            self._state.setText("模拟交易：正在进行订单校验和冻结")

        def _cancel_selected(self) -> None:
            record = self._selected_order()
            if record is None:
                return
            self._track(self.service.cancel_order(str(record["order_id"])), "cancel")
            self._state.setText(f"模拟交易：正在撤销 {record['order_id']}")

        def _fill_selected(self) -> None:
            record = self._selected_order()
            if record is None:
                return
            self._track(self.service.fill_order(str(record["order_id"])), "fill")
            self._state.setText(f"模拟交易：正在模拟 {record['order_id']} 成交")

        def _selected_order(self) -> dict[str, Any] | None:
            record = self._tables.selected_record(2)
            if record is None:
                self._state.setText("模拟交易：请先在委托页选择一条订单")
                return None
            return record

        def _track(self, future: Any, action: str) -> None:
            request_id = getattr(future, "request_id", "")
            if not request_id:
                self._state.setText("模拟交易：请求未生成关联编号")
                return
            self._pending[request_id] = action
            future.add_done_callback(self._publish_future)

        def _publish_future(self, future: Any) -> None:
            try:
                self.bridge.publish_response(future.result())  # 后端管道线程只发布 Qt signal，所有控件状态变化回到主线程执行。
            except Exception as error:
                self.logger.error(f"trade_response_publish_failed error={error!r}", module="oxApp.trade")

        def _on_response(self, response: ClientResponse) -> None:
            action = self._pending.pop(response.request_id, None)
            if action is None:
                return
            if response.is_success:
                message = str(response.payload.get("message", "mock request accepted"))
                self._state.setText(f"模拟交易：{message}")
                self._tables.refresh_all()  # 成功后由后端重新给出账户/持仓/委托/成交快照，页面绝不自己篡改任一表格行。
            else:
                self._state.setText(f"模拟交易失败：{response.error_message}")

else:
    class TradingPage:  # type: ignore[no-redef]
        def __init__(self, *args, **kwargs) -> None:
            del args, kwargs
            raise RuntimeError("Qt UI requires PySide6; install client/requirements.txt first")
