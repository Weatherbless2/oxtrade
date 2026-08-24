"""Mock 管理页面共用的表格、筛选和异步状态处理。"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Callable

from ..connection import ClientProtocolClient
from ..logging import ClientLogger
from ..message import ClientResponse, MessageStatus
from .page_container import PageContainer, PageState
from .qt_bridge import AsyncQtBridge

try:
    from PySide6.QtCore import Qt, Signal
    from PySide6.QtWidgets import (
        QComboBox, QHBoxLayout, QLabel, QLineEdit, QPushButton, QTabWidget,
        QTableWidget, QTableWidgetItem, QVBoxLayout, QWidget,
    )
    PYSIDE_AVAILABLE = True
except ImportError:
    PYSIDE_AVAILABLE = False


Column = tuple[str, str]
Loader = Callable[[dict[str, Any]], Any]


@dataclass(frozen=True, slots=True)
class TableSpec:
    title: str
    columns: tuple[Column, ...]
    loader: Loader
    statuses: tuple[tuple[str, str], ...] = (("全部", ""),)


if PYSIDE_AVAILABLE:
    class DataTablePanel(QWidget):
        """只负责控件和表格数据渲染，不直接访问服务或 Mock 数据。"""

        refresh_requested = Signal()

        def __init__(self, columns: tuple[Column, ...], statuses: tuple[tuple[str, str], ...], parent: QWidget | None = None) -> None:
            super().__init__(parent)
            self.columns = columns
            self._query = QLineEdit()
            self._query.setPlaceholderText("输入关键字筛选")
            self._status = QComboBox()
            for label, value in statuses:
                self._status.addItem(label, value)
            refresh = QPushButton("刷新")
            refresh.clicked.connect(self.refresh_requested.emit)
            self._summary = QLabel("未加载")
            self._table = QTableWidget(0, len(columns))
            self._table.setHorizontalHeaderLabels([title for _, title in columns])
            self._table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
            self._table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
            self._table.setAlternatingRowColors(True)
            header = QHBoxLayout()
            header.addWidget(self._query, 1)
            header.addWidget(self._status)
            header.addWidget(refresh)
            layout = QVBoxLayout(self)
            layout.addLayout(header)
            layout.addWidget(self._summary)
            layout.addWidget(self._table, 1)

        def filters(self) -> dict[str, Any]:
            return {
                "query": self._query.text().strip(),
                "status": self._status.currentData() or "",
                "page": 1,
                "page_size": 20,
            }

        def set_rows(self, items: list[dict[str, Any]], total: int) -> None:
            self._table.setRowCount(len(items))
            for row_index, row in enumerate(items):
                for column_index, (key, _) in enumerate(self.columns):
                    value = row.get(key, "")
                    if isinstance(value, (tuple, list)):
                        value = ", ".join(str(part) for part in value)
                    item = QTableWidgetItem(str(value))
                    item.setTextAlignment(Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft)
                    self._table.setItem(row_index, column_index, item)
            self._table.resizeColumnsToContents()
            self._summary.setText(f"共 {total} 条，当前显示 {len(items)} 条")

        def set_failure(self, message: str) -> None:
            self._summary.setText(message)


    class ServiceTablePage(QWidget):
        """把多个 DataTablePanel 和 ClientProtocolClient 的异步回包关联起来。"""

        def __init__(self, client: ClientProtocolClient, bridge: AsyncQtBridge, logger: ClientLogger, specs: tuple[TableSpec, ...], parent: QWidget | None = None) -> None:
            super().__init__(parent)
            self.client = client
            self.bridge = bridge
            self.logger = logger
            self._pending: dict[str, tuple[PageContainer, DataTablePanel, str]] = {}
            self._tabs = QTabWidget()
            self._tabs.currentChanged.connect(self._load_current)
            layout = QVBoxLayout(self)
            layout.addWidget(self._tabs)
            self._specs = specs
            for index, spec in enumerate(specs):
                panel = DataTablePanel(spec.columns, spec.statuses)
                container = PageContainer(panel)
                panel.refresh_requested.connect(lambda index=index: self._load(index))
                self._pending[f"__spec_{index}"] = (container, panel, spec.title)
                self._tabs.addTab(container, spec.title)  # 先登记绑定再触发 Qt currentChanged，避免初始化竞态。
            bridge.response_received.connect(self._on_response)
            self._load(0)

        def _load_current(self, index: int) -> None:
            if index >= 0:
                self._load(index)

        def _load(self, index: int) -> None:
            if index < 0 or index >= len(self._specs):
                return
            spec = self._specs[index]
            container, panel, title = self._pending[f"__spec_{index}"]
            container.show_state(PageState.LOADING)
            future = spec.loader(panel.filters())
            request_id = getattr(future, "request_id", "")
            if not request_id:
                container.show_state(PageState.ERROR, "请求未生成关联编号")
                return
            self._pending[request_id] = (container, panel, title)
            future.add_done_callback(self._publish_future)

        def _publish_future(self, future: Any) -> None:
            try:
                self.bridge.publish_response(future.result())  # Future 在后台线程完成，只发布 Qt signal，不触碰控件。
            except Exception as error:
                self.logger.error(f"page_response_publish_failed error={error!r}", module="oxApp.ui")

        def _on_response(self, response: ClientResponse) -> None:
            binding = self._pending.pop(response.request_id, None)
            if binding is None:
                return  # 其他页面或重复回包不属于当前页面，不能误更新表格。
            container, panel, title = binding
            if response.status is MessageStatus.UNAUTHORIZED:
                container.show_state(PageState.UNAUTHORIZED, response.error_message)
            elif not response.is_success:
                panel.set_failure(response.error_message)
                container.show_state(PageState.ERROR, response.error_message)
            else:
                payload = response.payload
                items = payload.get("items", [])
                panel.set_rows(items, int(payload.get("total", len(items))))
                container.show_state(PageState.EMPTY if not items else PageState.CONTENT)
            self.logger.info(f"page_response page={title} status={response.status.value}", module="oxApp.ui")


else:
    class DataTablePanel:  # type: ignore[no-redef]
        def __init__(self, *args, **kwargs) -> None:
            del args, kwargs
            raise RuntimeError("Qt UI requires PySide6; install client/requirements.txt first")

    class ServiceTablePage:  # type: ignore[no-redef]
        def __init__(self, *args, **kwargs) -> None:
            del args, kwargs
            raise RuntimeError("Qt UI requires PySide6; install client/requirements.txt first")
