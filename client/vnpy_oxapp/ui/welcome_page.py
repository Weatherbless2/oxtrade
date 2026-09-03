"""登录后的欢迎工作台。

这里提供壳层验收所需的产品选择、摘要卡片、走势占位和业务接入提示。
页面明确标记 Mock/占位状态，不把静态演示值伪装成实时资产；真实数据
接入时只需替换数据源和刷新逻辑，布局与信号接口可以保持不变。
"""

from __future__ import annotations

from dataclasses import dataclass

try:
    from PySide6.QtCore import Qt, Signal
    from PySide6.QtGui import QColor, QPainter, QPen
    from PySide6.QtWidgets import (
        QFrame,
        QGridLayout,
        QHBoxLayout,
        QLabel,
        QListWidget,
        QListWidgetItem,
        QPushButton,
        QScrollArea,
        QSizePolicy,
        QVBoxLayout,
        QWidget,
    )
    PYSIDE_AVAILABLE = True
except ImportError:
    # 欢迎页属于可选 Qt 层；无 PySide6 时不影响消息和服务模块导入。
    PYSIDE_AVAILABLE = False


@dataclass(frozen=True, slots=True)
class ProductPlaceholder:
    """欢迎页展示用的产品占位数据，不代表真实合约行情。"""

    name: str
    code: str
    change: str


PRODUCT_PLACEHOLDERS: tuple[ProductPlaceholder, ...] = (
    ProductPlaceholder("沪深 300", "IF 主力 · 模拟数据", "+1.28%"),
    ProductPlaceholder("中证 500", "IC 主力 · 模拟数据", "+0.76%"),
    ProductPlaceholder("国债期货", "T 主力 · 模拟数据", "-0.18%"),
)


if PYSIDE_AVAILABLE:
    class ChartPlaceholder(QWidget):
        """绘制中性的走势轮廓，明确声明不代表实时账户资产。"""

        def __init__(self, parent: QWidget | None = None) -> None:
            """初始化可伸缩的占位图控件。"""
            super().__init__(parent)
            self.setObjectName("chartPlaceholder")
            self.setMinimumHeight(210)
            self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)

        def paintEvent(self, event) -> None:  # noqa: N802 - Qt API
            """绘制网格、示意折线和占位说明文字。"""
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
            painter.drawText(rect.left(), rect.bottom(), "占位走势 · 不代表实时资产")


    class WelcomePage(QFrame):
        """基础壳层阶段的中央欢迎页。

        页面不直接调用后端服务，产品行变化只发出 ``product_selected``；
        主窗口负责将该选择写入 ``ShellState``，未来再由业务控制器接管。
        """

        product_selected = Signal(str, str)
        refresh_requested = Signal()
        add_product_requested = Signal()

        def __init__(self, parent: QWidget | None = None) -> None:
            """创建欢迎页，并使用固定的产品占位列表初始化内容。"""
            super().__init__(parent)
            self.setObjectName("welcomePage")
            self._products = PRODUCT_PLACEHOLDERS
            self._build()

        def _build(self) -> None:
            """构造欢迎文案、指标卡、走势图和产品列表。"""
            scroll = QScrollArea(self)
            scroll.setObjectName("welcomeScroll")
            scroll.setWidgetResizable(True)
            scroll.setFrameShape(QFrame.Shape.NoFrame)
            inner = QWidget()
            inner.setObjectName("welcomeContent")
            content = QVBoxLayout(inner)
            content.setContentsMargins(28, 24, 28, 28)
            content.setSpacing(12)

            eyebrow = QLabel("WELCOME BACK")
            eyebrow.setObjectName("eyebrow")
            title = QLabel("欢迎回来，未登录")
            title.setObjectName("welcomeTitle")
            self._title = title
            description = QLabel("这里是你的量化工作台。先从产品选择或左侧模块开始。")
            description.setObjectName("welcomeDescription")
            self._description = description
            content.addWidget(eyebrow)
            content.addWidget(title)
            content.addWidget(description)

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
            sync = QLabel("最后同步：占位")
            sync.setObjectName("mutedLabel")
            toolbar.addWidget(sync)
            content.addLayout(toolbar)

            # 指标卡先提供稳定的视觉骨架，暂无实时值时使用“—”和说明文字。
            metrics = QGridLayout()
            metrics.setHorizontalSpacing(12)
            metrics.setVerticalSpacing(12)
            self._selected_product = self._metric("当前产品", "沪深 300", "IF 主力 · 模拟数据")
            self._daily_return = self._metric("今日收益", "—", "暂无实时数据")
            self._running_strategies = self._metric("运行策略", "—", "等待业务接入")
            metrics.addWidget(self._selected_product, 0, 0)
            metrics.addWidget(self._daily_return, 0, 1)
            metrics.addWidget(self._running_strategies, 0, 2)
            content.addLayout(metrics)

            # 左侧走势、右侧产品列表按 6:4 分配，产品选中后同步更新标题。
            dashboard = QGridLayout()
            dashboard.setHorizontalSpacing(14)
            chart_card = self._card("沪深 300 · 资产走势", "近 30 个交易日 · 占位图表")
            self._chart_title = chart_card.findChild(QLabel, "cardTitle")
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
            for product in self._products:
                item = QListWidgetItem(f"{product.name}    {product.code}    {product.change}")
                item.setData(Qt.ItemDataRole.UserRole, (product.name, product.code))
                self._products_list.addItem(item)
            self._products_list.currentRowChanged.connect(self._on_product_row)
            product_card.layout().addWidget(self._products_list)
            dashboard.addWidget(chart_card, 0, 0)
            dashboard.addWidget(product_card, 0, 1)
            dashboard.setColumnStretch(0, 6)
            dashboard.setColumnStretch(1, 4)
            content.addLayout(dashboard)

            # 业务页暂不在欢迎页内伪造数据，保留明确的后续接入区域。
            empty = QFrame()
            empty.setObjectName("emptyBusinessCard")
            empty_layout = QVBoxLayout(empty)
            empty_layout.setContentsMargins(16, 14, 16, 14)
            empty_title = QLabel("业务工作区占位")
            empty_title.setObjectName("cardTitle")
            empty_text = QLabel("后续可在这里接入行情、策略、交易、风控和管理页面。当前只验证壳层布局与状态反馈。")
            empty_text.setObjectName("mutedLabel")
            empty_text.setWordWrap(True)
            empty_layout.addWidget(empty_title)
            empty_layout.addWidget(empty_text)
            content.addWidget(empty)
            content.addStretch(1)
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

        def set_user(self, display_name: str) -> None:
            """将登录会话的显示名写入欢迎标题。"""
            name = display_name or "用户"
            self._title.setText(f"欢迎回来，{name}")

        def set_context(self, label: str) -> None:
            """一级菜单切换到未落地模块时，更新中性的占位说明。"""
            self._description.setText(f"这里是 {label} 模块的基础占位区域，后续接入具体业务页面。")

        def _focus_products(self) -> None:
            """把键盘焦点定位到产品列表，方便从“选择产品”按钮继续操作。"""
            self._products_list.setFocus()
            self._products_list.scrollToItem(self._products_list.currentItem() or self._products_list.item(0))

        def _on_product_row(self, row: int) -> None:
            """同步选中产品的摘要和图表标题，并发出产品选择信号。"""
            if not (0 <= row < len(self._products)):
                return
            product = self._products[row]
            data = self._selected_product.findChildren(QLabel)
            if len(data) >= 3:
                data[1].setText(product.name)
                data[2].setText(product.code)
            if self._chart_title is not None:
                self._chart_title.setText(f"{product.name} · 资产走势")
            self.product_selected.emit(product.name, product.code)

else:
    class WelcomePage:  # type: ignore[no-redef]
        """无 PySide6 时的明确失败边界。"""

        def __init__(self, *args, **kwargs) -> None:
            del args, kwargs
            raise RuntimeError("Qt UI requires PySide6; install client/requirements.txt first")
