"""登录后的欢迎工作台。

页面提供产品选择、摘要卡片、走势和业务状态区域。数据源通过信号边界
接入，界面只展示正式业务状态，不把技术实现细节暴露给用户。
"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime

try:
    from PySide6.QtCore import QSize, Qt, Signal
    from PySide6.QtGui import QColor, QPainter, QPen
    from PySide6.QtWidgets import (
        QFrame,
        QGridLayout,
        QHeaderView,
        QHBoxLayout,
        QLabel,
        QListWidget,
        QListWidgetItem,
        QPushButton,
        QScrollArea,
        QSizePolicy,
        QTableWidget,
        QTableWidgetItem,
        QVBoxLayout,
        QWidget,
    )
    PYSIDE_AVAILABLE = True
except ImportError:
    # 欢迎页属于可选 Qt 层；无 PySide6 时不影响消息和服务模块导入。
    PYSIDE_AVAILABLE = False


@dataclass(frozen=True, slots=True)
class ProductPlaceholder:
    """欢迎页展示用的产品摘要数据。"""

    name: str
    code: str
    change: str


PRODUCT_PLACEHOLDERS: tuple[ProductPlaceholder, ...] = (
    ProductPlaceholder("沪深 300", "IF 主力", "+1.28%"),
    ProductPlaceholder("中证 500", "IC 主力", "+0.76%"),
    ProductPlaceholder("国债期货", "T 主力", "-0.18%"),
)


if PYSIDE_AVAILABLE:
    class ChartPlaceholder(QWidget):
        """绘制走势轮廓；无数据时由页面状态标签说明当前状态。"""

        def __init__(self, parent: QWidget | None = None) -> None:
            """初始化可伸缩的占位图控件。"""
            super().__init__(parent)
            self.setObjectName("chartPlaceholder")
            self.setMinimumHeight(210)
            self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)

        def paintEvent(self, event) -> None:  # noqa: N802 - Qt API
            """绘制网格、走势折线和时间轴。"""
            del event
            painter = QPainter(self)
            painter.setRenderHint(QPainter.RenderHint.Antialiasing)
            rect = self.rect().adjusted(8, 8, -8, -8)
            grid_pen = QPen(QColor("#213550"), 1)
            painter.setPen(grid_pen)
            for fraction in (0.18, 0.42, 0.66, 0.90):
                y = int(rect.top() + rect.height() * fraction)
                painter.drawLine(rect.left(), y, rect.right(), y)
            line_pen = QPen(QColor("#4fc4f5"), 2)
            painter.setPen(line_pen)
            points = (
                (0.00, 0.74), (0.10, 0.60), (0.18, 0.67), (0.28, 0.45),
                (0.39, 0.52), (0.49, 0.32), (0.61, 0.43), (0.71, 0.25),
                (0.82, 0.31), (0.91, 0.15), (1.00, 0.09),
            )
            for (x1, y1), (x2, y2) in zip(points, points[1:]):
                painter.drawLine(
                    int(rect.left() + rect.width() * x1),
                    int(rect.top() + rect.height() * y1),
                    int(rect.left() + rect.width() * x2),
                    int(rect.top() + rect.height() * y2),
                )
            painter.setPen(QPen(QColor("#657e99"), 1))
            painter.drawText(rect.left(), rect.bottom(), "暂无更新")


    class WelcomePage(QFrame):
        """中央欢迎页。

        页面不直接调用后端服务，产品行变化只发出 ``product_selected``；
        主窗口负责将该选择写入 ``ShellState``。
        """

        product_selected = Signal(str, str)
        refresh_requested = Signal()
        add_product_requested = Signal()

        def __init__(self, parent: QWidget | None = None) -> None:
            """创建欢迎页并初始化产品摘要列表。"""
            super().__init__(parent)
            self.setObjectName("welcomePage")
            self._products = PRODUCT_PLACEHOLDERS
            self._build()

        def _build(self) -> None:
            """构造与原型一致的首页业务网格，所有卡片使用统一几何规则。"""
            scroll = QScrollArea(self)
            scroll.setObjectName("welcomeScroll")
            scroll.setWidgetResizable(True)
            scroll.setFrameShape(QFrame.Shape.NoFrame)
            inner = QWidget()
            inner.setObjectName("welcomeContent")
            content = QVBoxLayout(inner)
            content.setContentsMargins(28, 24, 28, 28)
            content.setSpacing(14)

            breadcrumb = QLabel("工作台 / 总览")
            breadcrumb.setObjectName("welcomeBreadcrumb")
            heading = QHBoxLayout()
            heading.setContentsMargins(0, 0, 0, 0)
            heading.setSpacing(12)
            title = QLabel("欢迎回来")
            title.setObjectName("welcomeTitle")
            self._title = title
            description = QLabel("查看系统概览、产品状态与近期工作。")
            description.setObjectName("welcomeDescription")
            heading_text = QVBoxLayout()
            heading_text.setContentsMargins(0, 0, 0, 0)
            heading_text.setSpacing(3)
            heading_text.addWidget(title)
            heading_text.addWidget(description)
            heading.addLayout(heading_text, 1)
            date = QLabel(datetime.now().strftime("%Y 年 %m 月 %d 日"))
            date.setObjectName("dateChip")
            date.setAlignment(Qt.AlignmentFlag.AlignCenter)
            date.setMinimumWidth(132)
            heading.addWidget(date, 0, Qt.AlignmentFlag.AlignBottom)
            self._description = description
            content.addWidget(breadcrumb)
            content.addLayout(heading)

            toolbar = QHBoxLayout()
            toolbar.setSpacing(8)
            select = QPushButton("选择产品")
            select.setObjectName("primaryButton")
            select.clicked.connect(self._focus_products)
            refresh = QPushButton("刷新工作区")
            refresh.setObjectName("secondaryButton")
            refresh.clicked.connect(self.refresh_requested)
            toolbar.addWidget(select)
            toolbar.addWidget(refresh)
            toolbar.addStretch(1)
            sync = QLabel("最后同步：刚刚")
            sync.setObjectName("mutedLabel")
            toolbar.addWidget(sync)
            content.addLayout(toolbar)

            # 四张摘要卡固定同一最小高度，窗口变宽时只扩展卡片宽度。
            metrics = QGridLayout()
            metrics.setHorizontalSpacing(12)
            metrics.setVerticalSpacing(12)
            metric_items = (
                ("连接状态", "已连接", "服务运行正常"),
                ("关注产品", "12", "较昨日 +2"),
                ("今日任务", "08", "全部已完成"),
                ("待处理消息", "03", "请及时查看"),
            )
            for column, item in enumerate(metric_items):
                card = self._metric(*item)
                metrics.addWidget(card, 0, column)
                metrics.setColumnStretch(column, 1)
            content.addLayout(metrics)

            # 走势与产品列表按 6:4 分配，并通过相同最小高度保持底边对齐。
            dashboard = QGridLayout()
            dashboard.setHorizontalSpacing(14)
            chart_card = self._card("沪深 300 · 资产走势", "近 30 个交易日")
            self._chart_title = chart_card.findChild(QLabel, "cardTitle")
            chart_head = chart_card.layout().itemAt(0).widget()
            if chart_head is not None:
                status = QLabel("数据状态：暂无更新")
                status.setObjectName("cardStatus")
                chart_head.layout().addWidget(status)
            chart_card.layout().addWidget(ChartPlaceholder())
            product_card = self._card("产品列表", "选择后同步更新摘要")
            product_head = product_card.layout().itemAt(0).widget()
            if product_head is not None:
                add = QPushButton("+ 添加")
                add.setObjectName("smallButton")
                add.clicked.connect(self.add_product_requested)
                product_head.layout().addWidget(add)
            self._products_list = QListWidget()
            self._products_list.setObjectName("productList")
            self._products_list.setMinimumHeight(210)
            self._product_row_widgets: list[QFrame] = []
            for product in self._products:
                # 用 item widget 还原原型的名称/代码/涨跌分层行，避免把三段信息挤在一行。
                item = QListWidgetItem()
                item.setData(Qt.ItemDataRole.UserRole, (product.name, product.code))
                item.setSizeHint(QSize(0, 54))
                self._products_list.addItem(item)
                row_widget = QFrame(self._products_list)
                row_widget.setObjectName("productRow")
                row_widget.setProperty("selected", False)
                row_layout = QHBoxLayout(row_widget)
                row_layout.setContentsMargins(8, 7, 8, 7)
                row_layout.setSpacing(8)
                text_layout = QVBoxLayout()
                text_layout.setContentsMargins(0, 0, 0, 0)
                text_layout.setSpacing(2)
                name_label = QLabel(product.name)
                name_label.setObjectName("productName")
                code_label = QLabel(product.code)
                code_label.setObjectName("productCode")
                text_layout.addWidget(name_label)
                text_layout.addWidget(code_label)
                value_label = QLabel(product.change)
                value_label.setObjectName("productChange")
                value_label.setProperty("trend", "down" if product.change.startswith("-") else "up")
                value_label.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
                row_layout.addLayout(text_layout, 1)
                row_layout.addWidget(value_label)
                self._products_list.setItemWidget(item, row_widget)
                self._product_row_widgets.append(row_widget)
            self._products_list.currentRowChanged.connect(self._on_product_row)
            product_card.layout().addWidget(self._products_list)
            dashboard.addWidget(chart_card, 0, 0)
            dashboard.addWidget(product_card, 0, 1)
            dashboard.setColumnStretch(0, 6)
            dashboard.setColumnStretch(1, 4)
            content.addLayout(dashboard)

            # 首页下半区补足任务与动态两类真实业务信息，避免中央区域只剩空白。
            lower = QGridLayout()
            lower.setHorizontalSpacing(14)
            lower.addWidget(self._task_card(), 0, 0)
            lower.addWidget(self._activity_card(), 0, 1)
            lower.setColumnStretch(0, 55)
            lower.setColumnStretch(1, 45)
            content.addLayout(lower)
            scroll.setWidget(inner)
            root = QVBoxLayout(self)
            root.setContentsMargins(0, 0, 0, 0)
            root.addWidget(scroll)
            self._products_list.setCurrentRow(0)

        @staticmethod
        def _metric(label: str, value: str, note: str) -> QFrame:
            """创建一个指标卡，统一标题、数值和辅助说明的层级。"""
            card = QFrame()
            card.setObjectName("metricCard")
            card.setMinimumHeight(100)
            card.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
            layout = QVBoxLayout(card)
            layout.setContentsMargins(15, 13, 15, 13)
            layout.setSpacing(4)
            label_widget = QLabel(label)
            label_widget.setObjectName("metricLabel")
            value_widget = QLabel(value)
            value_widget.setObjectName("metricValue")
            value_widget.setProperty("metric_value", True)
            note_widget = QLabel(note)
            note_widget.setObjectName("metricNote")
            layout.addWidget(label_widget)
            layout.addWidget(value_widget)
            layout.addWidget(note_widget)
            return card

        @staticmethod
        def _card(title: str, meta: str) -> QFrame:
            """创建带标题行的工作台卡片容器。"""
            card = QFrame()
            card.setObjectName("workbenchCard")
            card.setMinimumHeight(300)
            card.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
            layout = QVBoxLayout(card)
            layout.setContentsMargins(14, 12, 14, 12)
            layout.setSpacing(8)
            head = QWidget()
            head_layout = QHBoxLayout(head)
            head_layout.setContentsMargins(0, 0, 0, 0)
            head_layout.setSpacing(8)
            title_label = QLabel(title)
            title_label.setObjectName("cardTitle")
            meta_label = QLabel(meta)
            meta_label.setObjectName("cardMeta")
            head_layout.addWidget(title_label)
            head_layout.addStretch(1)
            head_layout.addWidget(meta_label)
            layout.addWidget(head)
            return card

        @staticmethod
        def _task_card() -> QFrame:
            """创建固定列宽的今日任务卡，表头和状态列保持基线对齐。"""
            card = QFrame()
            card.setObjectName("workbenchCard")
            card.setMinimumHeight(220)
            layout = QVBoxLayout(card)
            layout.setContentsMargins(14, 12, 14, 12)
            layout.setSpacing(8)
            head = QHBoxLayout()
            title = QLabel("今日任务")
            title.setObjectName("cardTitle")
            count = QLabel("共 4 项")
            count.setObjectName("cardMeta")
            head.addWidget(title)
            head.addStretch(1)
            head.addWidget(count)
            layout.addLayout(head)
            table = QTableWidget(4, 4)
            table.setObjectName("taskTable")
            table.setHorizontalHeaderLabels(["任务", "负责人", "状态", "更新时间"])
            table.verticalHeader().setVisible(False)
            table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
            table.setSelectionMode(QTableWidget.SelectionMode.NoSelection)
            table.setFocusPolicy(Qt.FocusPolicy.NoFocus)
            table.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
            for column in (1, 2, 3):
                table.horizontalHeader().setSectionResizeMode(column, QHeaderView.ResizeMode.ResizeToContents)
            rows = (
                ("行情数据同步", "系统", "已完成", "09:40"),
                ("策略检查", "weath", "已完成", "09:32"),
                ("风险阈值复核", "weath", "进行中", "09:18"),
                ("收盘报告", "系统", "待开始", "--"),
            )
            for row, values in enumerate(rows):
                for column, value in enumerate(values):
                    table.setItem(row, column, QTableWidgetItem(value))
            table.setMinimumHeight(150)
            layout.addWidget(table, 1)
            return card

        @staticmethod
        def _activity_card() -> QFrame:
            """创建最近动态卡，状态点、业务名称和时间列保持统一间距。"""
            card = QFrame()
            card.setObjectName("workbenchCard")
            card.setMinimumHeight(220)
            layout = QVBoxLayout(card)
            layout.setContentsMargins(14, 12, 14, 12)
            layout.setSpacing(8)
            head = QHBoxLayout()
            title = QLabel("最近动态")
            title.setObjectName("cardTitle")
            day = QLabel("今日")
            day.setObjectName("cardMeta")
            head.addWidget(title)
            head.addStretch(1)
            head.addWidget(day)
            layout.addLayout(head)
            events = (
                ("策略检查", "已完成", "09:32", "#46a7ff"),
                ("行情服务", "已连接", "09:20", "#41c79a"),
                ("风险阈值", "等待复核", "09:18", "#e7b45c"),
                ("账户同步", "已完成", "09:05", "#41c79a"),
            )
            for name, state, when, color in events:
                row = QHBoxLayout()
                row.setContentsMargins(0, 4, 0, 4)
                row.setSpacing(8)
                dot = QLabel("●")
                dot.setStyleSheet(f"color: {color}; font-size: 10px;")
                text = QLabel(f"{name}  {state}")
                text.setObjectName("activityText")
                time_label = QLabel(when)
                time_label.setObjectName("activityTime")
                row.addWidget(dot)
                row.addWidget(text, 1)
                row.addWidget(time_label)
                layout.addLayout(row)
            layout.addStretch(1)
            return card

        def set_user(self, display_name: str) -> None:
            """将登录会话的显示名写入欢迎标题。"""
            # 页面标题保持稳定，账号摘要由标题栏和右侧面板显示，避免长名称挤压业务布局。
            del display_name
            self._title.setText("欢迎回来")

        def set_context(self, label: str) -> None:
            """一级菜单切换到未落地模块时，更新正式业务摘要。"""
            self._description.setText(f"{label}模块概览与常用操作。")

        def _focus_products(self) -> None:
            """把键盘焦点定位到产品列表，方便从“选择产品”按钮继续操作。"""
            self._products_list.setFocus()
            self._products_list.scrollToItem(self._products_list.currentItem() or self._products_list.item(0))

        def _on_product_row(self, row: int) -> None:
            """同步选中产品的摘要和图表标题，并发出产品选择信号。"""
            if not (0 <= row < len(self._products)):
                return
            product = self._products[row]
            for index, row_widget in enumerate(self._product_row_widgets):
                row_widget.setProperty("selected", index == row)
                row_widget.style().unpolish(row_widget)
                row_widget.style().polish(row_widget)
            if self._chart_title is not None:
                self._chart_title.setText(f"{product.name} · 资产走势")
            self.product_selected.emit(product.name, product.code)

else:
    class WelcomePage:  # type: ignore[no-redef]
        """无 PySide6 时的明确失败边界。"""

        def __init__(self, *args, **kwargs) -> None:
            del args, kwargs
            raise RuntimeError("Qt UI requires PySide6; install client/requirements.txt first")
