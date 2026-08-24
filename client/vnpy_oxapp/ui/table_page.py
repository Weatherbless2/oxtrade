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
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import (
        QDialog, QDialogButtonBox, QFileDialog, QComboBox, QFormLayout, QFrame,
        QHBoxLayout, QHeaderView, QLabel, QLineEdit, QMenu, QPushButton, QTabWidget,
        QTableWidget, QTableWidgetItem, QToolButton, QVBoxLayout, QWidget,
    )
    PYSIDE_AVAILABLE = True
except ImportError:
    PYSIDE_AVAILABLE = False


Column = tuple[str, str]
Loader = Callable[[dict[str, Any]], Any]
Creator = Callable[[dict[str, Any]], Any]
Updater = Callable[[str, dict[str, Any]], Any]
Transitioner = Callable[[str, str], Any]


@dataclass(frozen=True, slots=True)
class TableSpec:
    title: str
    columns: tuple[Column, ...]
    loader: Loader
    statuses: tuple[tuple[str, str], ...] = (("全部", ""),)
    primary_key: str = ""
    editable_fields: tuple[Column, ...] = ()
    creator: Creator | None = None
    updater: Updater | None = None
    transitioner: Transitioner | None = None


@dataclass(frozen=True, slots=True)
class PendingTableRequest:
    container: PageContainer
    panel: "DataTablePanel"
    title: str
    spec_index: int
    action: str
    load_generation: int = 0


if PYSIDE_AVAILABLE:
    class DataTablePanel(QWidget):
        """只负责控件和表格数据渲染，不直接访问服务或 Mock 数据。"""

        refresh_requested = Signal()
        create_requested = Signal(dict)
        update_requested = Signal(str, dict)
        transition_requested = Signal(str, str)

        def __init__(self, spec: TableSpec, parent: QWidget | None = None) -> None:
            super().__init__(parent)
            self.columns = spec.columns
            self._primary_key = spec.primary_key
            self._editable_fields = spec.editable_fields
            self._rows: list[dict[str, Any]] = []  # 表格只保存最近一次后端快照，用于定位选中行，不能在这里直接修改业务数据。
            self._page = 1
            self._page_size = 20
            self._filter_signature = ("", "")
            self._query = QLineEdit()
            self._query.setPlaceholderText("输入关键字筛选")
            self._query.setClearButtonEnabled(True)
            self._status = QComboBox()
            for label, value in spec.statuses:
                self._status.addItem(label, value)
            refresh = QPushButton("刷新")
            refresh.setIcon(self.style().standardIcon(self.style().StandardPixmap.SP_BrowserReload))
            refresh.clicked.connect(self.refresh_requested.emit)
            self._create = QToolButton()
            self._create.setIcon(self.style().standardIcon(self.style().StandardPixmap.SP_FileDialogNewFolder))
            self._create.setToolTip("新增记录")
            self._create.clicked.connect(self._request_create)
            self._edit = QToolButton()
            self._edit.setIcon(self.style().standardIcon(self.style().StandardPixmap.SP_FileDialogDetailedView))
            self._edit.setToolTip("编辑选中记录")
            self._edit.clicked.connect(self._request_update)
            self._transition = QToolButton()
            self._transition.setIcon(self.style().standardIcon(self.style().StandardPixmap.SP_BrowserReload))
            self._transition.setToolTip("切换选中记录状态")
            self._transition.clicked.connect(self._request_transition)
            self._previous = QToolButton()
            self._previous.setIcon(self.style().standardIcon(self.style().StandardPixmap.SP_ArrowBack))
            self._previous.setToolTip("上一页")
            self._previous.clicked.connect(self._previous_page)
            self._next = QToolButton()
            self._next.setIcon(self.style().standardIcon(self.style().StandardPixmap.SP_ArrowForward))
            self._next.setToolTip("下一页")
            self._next.clicked.connect(self._next_page)
            has_crud = bool(self._primary_key and self._editable_fields)
            self._create.setVisible(has_crud and spec.creator is not None)
            self._edit.setVisible(has_crud and spec.updater is not None)
            self._transition.setVisible(bool(self._primary_key and spec.transitioner is not None))
            self._summary = QLabel("未加载")
            self._summary.setObjectName("tableSummary")
            self._table = QTableWidget(0, len(self.columns))
            self._table.setHorizontalHeaderLabels([title for _, title in self.columns])
            self._table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
            self._table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
            self._table.setSelectionMode(QTableWidget.SelectionMode.SingleSelection)
            self._table.setAlternatingRowColors(True)
            self._table.setSortingEnabled(True)
            self._table.setWordWrap(False)
            self._table.verticalHeader().setVisible(False)
            self._table.verticalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)  # 有限的当前页记录均匀填满表格高度，避免数据行下方留下大块空白。
            self._table.horizontalHeader().setStretchLastSection(True)
            self._table.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
            self._table.customContextMenuRequested.connect(self._show_context_menu)
            header = QHBoxLayout()
            header.setContentsMargins(0, 0, 0, 0)
            header.addWidget(self._query, 1)
            header.addWidget(self._status)
            header.addWidget(self._create)
            header.addWidget(self._edit)
            header.addWidget(self._transition)
            header.addWidget(self._previous)
            header.addWidget(self._next)
            header.addWidget(refresh)
            toolbar = QFrame()
            toolbar.setObjectName("summaryBar")
            toolbar.setLayout(header)
            layout = QVBoxLayout(self)
            layout.setContentsMargins(10, 10, 10, 10)
            layout.setSpacing(8)
            layout.addWidget(toolbar)
            layout.addWidget(self._summary)
            layout.addWidget(self._table, 1)

        def filters(self) -> dict[str, Any]:
            signature = (self._query.text().strip(), self._status.currentData() or "")
            if signature != self._filter_signature:
                self._page = 1  # 改筛选条件后必须回到第一页，避免在旧页码上把有效结果误显示为空。
                self._filter_signature = signature
            return {
                "query": signature[0],
                "status": signature[1],
                "page": self._page,
                "page_size": self._page_size,
            }

        def set_rows(self, items: list[dict[str, Any]], total: int, page: int, page_size: int) -> None:
            self._rows = [dict(item) for item in items]  # 深拷贝响应快照，防止上层后续复用 payload 时改变已显示的选中记录。
            self._page = max(1, page)
            self._page_size = max(1, page_size)
            sorting_enabled = self._table.isSortingEnabled()
            self._table.setSortingEnabled(False)  # 填充期间禁止自动重排，否则逐行写入会把字段写到错误的行。
            self._table.setRowCount(len(items))
            for row_index, row in enumerate(items):
                for column_index, (key, _) in enumerate(self.columns):
                    value = row.get(key, "")
                    if isinstance(value, (tuple, list)):
                        value = ", ".join(str(part) for part in value)
                    item = QTableWidgetItem(str(value))
                    item.setTextAlignment(Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft)
                    if column_index == 0:
                        item.setData(Qt.ItemDataRole.UserRole, dict(row))  # 排序会改变可见行序，主键快照绑定在首列才能避免 CRUD 取错原始 _rows 下标。
                    self._table.setItem(row_index, column_index, item)
            self._table.resizeColumnsToContents()
            for index in range(self._table.columnCount()):
                width = self._table.columnWidth(index)
                self._table.setColumnWidth(index, min(max(width + 18, 90), 260))  # 限制列宽，避免长关联字段挤压终端主表。
            self._table.setSortingEnabled(sorting_enabled)  # 数据完整写入后恢复用户可用的表头排序。
            self._previous.setEnabled(self._page > 1)
            self._next.setEnabled(self._page * self._page_size < total)
            self._summary.setText(f"共 {total} 条，第 {self._page} 页，当前显示 {len(items)} 条")

        def set_loading(self) -> None:
            self._summary.setText("正在加载... 可继续调整筛选条件或点击刷新")  # 加载态只更新页内提示，筛选控件必须始终保留以便用户修正条件。

        def set_empty(self) -> None:
            self._clear_rows("暂无数据，可调整筛选条件或点击刷新")  # 空结果清空旧快照，仍保留筛选、分页和刷新入口，避免页面被锁死。

        def set_unauthorized(self, message: str) -> None:
            detail = message or "当前会话没有访问权限"
            self._clear_rows(f"无访问权限：{detail}。可调整筛选条件或点击刷新")  # 权限失败不能继续保留可能敏感的旧数据，但操作入口仍必须可见。

        def set_failure(self, message: str) -> None:
            self._summary.setText(f"请求失败：{message or '未知错误'}。可调整筛选条件或点击刷新")

        def _clear_rows(self, summary: str) -> None:
            self._rows.clear()
            self._table.setRowCount(0)  # 结果为空或无权时删除当前渲染快照，避免用户继续对过期行执行 CRUD。
            self._previous.setEnabled(False)
            self._next.setEnabled(False)
            self._summary.setText(summary)

        def _show_context_menu(self, position) -> None:
            menu = QMenu(self)  # 表格右键菜单复用 vn.py Monitor 的操作习惯。
            copy_action = menu.addAction("复制选中行")
            export_action = menu.addAction("导出当前表格")
            action = menu.exec(self._table.viewport().mapToGlobal(position))
            if action is copy_action:
                self._copy_selected_row()
            elif action is export_action:
                self._export_csv()

        def _copy_selected_row(self) -> None:
            row = self._table.currentRow()
            if row < 0:
                return
            values = [self._table.item(row, column).text() for column in range(self._table.columnCount())]
            clipboard = QGuiApplication.clipboard()  # 使用 Qt 全局剪贴板，兼容 Linux、Windows 和 offscreen 测试环境。
            if clipboard is not None:
                clipboard.setText("\t".join(values))

        def _export_csv(self) -> None:
            path, _ = QFileDialog.getSaveFileName(self, "导出表格", "oxapp-table.csv", "CSV 文件 (*.csv)")
            if not path:
                return
            import csv
            with open(path, "w", newline="", encoding="utf-8-sig") as stream:  # 导出使用 UTF-8 BOM，Windows Excel 可直接识别中文。
                writer = csv.writer(stream)
                writer.writerow([title for _, title in self.columns])
                for row in range(self._table.rowCount()):
                    writer.writerow([self._table.item(row, column).text() for column in range(self._table.columnCount())])

        def _selected_record(self) -> dict[str, Any] | None:
            row = self._table.currentRow()
            first_item = self._table.item(row, 0) if row >= 0 else None
            record = first_item.data(Qt.ItemDataRole.UserRole) if first_item is not None else None
            if not isinstance(record, dict):
                self.set_failure("请先选择一条记录")
                return None
            return dict(record)

        def _request_create(self) -> None:
            values = self._edit_values("新增记录", {})
            if values is not None:
                self.create_requested.emit(values)  # 面板只发出用户输入，实际 Service 请求仍由页面控制器创建。

        def _previous_page(self) -> None:
            if self._page > 1:
                self._page -= 1
                self.refresh_requested.emit()

        def _next_page(self) -> None:
            self._page += 1
            self.refresh_requested.emit()

        def _request_update(self) -> None:
            record = self._selected_record()
            if record is None:
                return
            record_id = str(record.get(self._primary_key, ""))
            if not record_id:
                self.set_failure("选中记录缺少主键")
                return
            values = self._edit_values("编辑记录", record)
            if values is not None:
                self.update_requested.emit(record_id, values)

        def _request_transition(self) -> None:
            record = self._selected_record()
            if record is None:
                return
            record_id = str(record.get(self._primary_key, ""))
            if not record_id:
                self.set_failure("选中记录缺少主键")
                return
            if "enabled" in record:
                enabled = str(record["enabled"]).lower() in {"true", "1", "yes"}
                state = "disabled" if enabled else "enabled"
            elif "handled" in record:
                handled = str(record["handled"]).lower() in {"true", "1", "yes"}
                state = "unhandled" if handled else "handled"
            else:
                state = "frozen" if str(record.get("status", "active")) == "active" else "active"
            self.transition_requested.emit(record_id, state)  # 状态切换也必须走后端审计和权限校验，不能只改当前单元格文本。

        def _edit_values(self, title: str, record: dict[str, Any]) -> dict[str, str] | None:
            dialog = QDialog(self)
            dialog.setWindowTitle(title)
            form = QFormLayout(dialog)
            inputs: dict[str, QLineEdit] = {}
            for key, label in self._editable_fields:
                input_widget = QLineEdit(str(record.get(key, "")))
                input_widget.setClearButtonEnabled(True)
                form.addRow(label, input_widget)
                inputs[key] = input_widget
            buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Save | QDialogButtonBox.StandardButton.Cancel)
            buttons.accepted.connect(dialog.accept)
            buttons.rejected.connect(dialog.reject)
            form.addRow(buttons)
            if dialog.exec() != QDialog.DialogCode.Accepted:
                return None
            values = {key: input_widget.text().strip() for key, input_widget in inputs.items()}
            if not any(values.values()):
                self.set_failure("至少填写一个字段")
                return None
            return values


    class ServiceTablePage(QWidget):
        """把多个 DataTablePanel 和 ClientProtocolClient 的异步回包关联起来。"""

        def __init__(self, client: ClientProtocolClient, bridge: AsyncQtBridge, logger: ClientLogger, specs: tuple[TableSpec, ...], parent: QWidget | None = None) -> None:
            super().__init__(parent)
            self.client = client
            self.bridge = bridge
            self.logger = logger
            self._pending: dict[str, PendingTableRequest] = {}
            self._load_generations: dict[int, int] = {}  # 每个页签独立计数，迟到的旧查询响应不能覆盖用户最新筛选结果。
            self._panels: dict[int, tuple[PageContainer, DataTablePanel, str]] = {}
            self._tabs = QTabWidget()
            self._tabs.setDocumentMode(True)  # 文档模式更接近 vn.py 的监控器标签页，减少厚重边框。
            self._tabs.setMovable(True)
            self._tabs.tabBar().setExpanding(True)  # 页签按工作区宽度均匀展开，右侧不再出现未使用的空白带。
            self._tabs.currentChanged.connect(self._load_current)
            layout = QVBoxLayout(self)
            layout.setContentsMargins(0, 0, 0, 0)
            layout.addWidget(self._tabs)
            self._specs = specs
            for index, spec in enumerate(specs):
                panel = DataTablePanel(spec)
                container = PageContainer(panel)
                panel.refresh_requested.connect(lambda index=index: self._load(index))
                panel.create_requested.connect(lambda values, index=index: self._create(index, values))
                panel.update_requested.connect(lambda record_id, values, index=index: self._update(index, record_id, values))
                panel.transition_requested.connect(lambda record_id, state, index=index: self._transition(index, record_id, state))
                self._panels[index] = (container, panel, spec.title)
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
            container, panel, title = self._panels[index]
            generation = self._load_generations.get(index, 0) + 1
            self._load_generations[index] = generation
            container.show_state(PageState.CONTENT)  # 表格页始终显示真实面板，不能用全页状态标签覆盖筛选工具栏。
            panel.set_loading()
            future = spec.loader(panel.filters())
            request_id = getattr(future, "request_id", "")
            if not request_id:
                panel.set_failure("请求未生成关联编号")
                return
            self._pending[request_id] = PendingTableRequest(container, panel, title, index, "page", generation)
            future.add_done_callback(self._publish_future)

        def _create(self, index: int, values: dict[str, Any]) -> None:
            spec = self._specs[index]
            if spec.creator is not None:
                self._submit_mutation(index, "create", spec.creator(values))

        def _update(self, index: int, record_id: str, values: dict[str, Any]) -> None:
            spec = self._specs[index]
            if spec.updater is not None:
                self._submit_mutation(index, "update", spec.updater(record_id, values))

        def _transition(self, index: int, record_id: str, state: str) -> None:
            spec = self._specs[index]
            if spec.transitioner is not None:
                self._submit_mutation(index, "transition", spec.transitioner(record_id, state))

        def _submit_mutation(self, index: int, action: str, future: Any) -> None:
            request_id = getattr(future, "request_id", "")
            if not request_id:
                self._panels[index][1].set_failure("操作未生成关联编号")
                return
            container, panel, title = self._panels[index]
            self._pending[request_id] = PendingTableRequest(container, panel, title, index, action)
            future.add_done_callback(self._publish_future)  # 回包和页面刷新仍走 Qt bridge，后台 Transport 绝不触碰 QWidget。

        def _publish_future(self, future: Any) -> None:
            try:
                self.bridge.publish_response(future.result())  # Future 在后台线程完成，只发布 Qt signal，不触碰控件。
            except Exception as error:
                self.logger.error(f"page_response_publish_failed error={error!r}", module="oxApp.ui")

        def _on_response(self, response: ClientResponse) -> None:
            binding = self._pending.pop(response.request_id, None)
            if binding is None:
                return  # 其他页面或重复回包不属于当前页面，不能误更新表格。
            container, panel, title = binding.container, binding.panel, binding.title
            if binding.action == "page" and binding.load_generation != self._load_generations.get(binding.spec_index):
                self.logger.info(f"page_response_ignored page={title} reason=stale_filter", module="oxApp.ui")
                return  # 用户已触发新的筛选查询时，旧请求即使后到也不能回写表格，防止筛选结果闪回。
            container.show_state(PageState.CONTENT)  # 成功、空、错误和权限结果都以内联方式提示，始终保留恢复操作。
            if response.status is MessageStatus.UNAUTHORIZED:
                panel.set_unauthorized(response.error_message)
            elif not response.is_success:
                panel.set_failure(response.error_message)
            elif binding.action != "page":
                self.logger.info(f"page_mutation page={title} action={binding.action} status=ok", module="oxApp.ui")
                self._load(binding.spec_index)  # 成功 mutation 后重新请求快照，禁止直接篡改 QTableWidget 内存行。
            else:
                payload = response.payload
                items = payload.get("items", [])
                panel.set_rows(
                    items,
                    int(payload.get("total", len(items))),
                    int(payload.get("page", 1)),
                    int(payload.get("page_size", max(1, len(items)))),
                )
                if not items:
                    panel.set_empty()
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
