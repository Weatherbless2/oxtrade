"""oxApp 无边框窗口的自定义标题栏。

标题栏是独立的 Qt 控件，负责品牌、一级菜单、搜索入口、配置入口、
账号摘要和窗口控制。它只发出语义化信号，不直接切换业务页面；窗口
拖动也封装在这里，以便未来替换为原生标题栏或平台特化实现。
"""

from __future__ import annotations

try:
    from PySide6.QtCore import QPoint, Qt, Signal
    from PySide6.QtGui import QMouseEvent
    from PySide6.QtWidgets import (
        QHBoxLayout,
        QLabel,
        QLineEdit,
        QPushButton,
        QSizePolicy,
        QVBoxLayout,
        QWidget,
    )
    PYSIDE_AVAILABLE = True
except ImportError:
    # PySide6 缺失时保留导入边界，避免核心模块因 UI 依赖而无法测试。
    PYSIDE_AVAILABLE = False

from .menu_config import PRIMARY_MENUS


if PYSIDE_AVAILABLE:
    class TitleBar(QWidget):
        """与业务页面堆栈解耦的应用级标题栏。

        信号的接收者是 ``OxAppMainWindow``：

        * ``primary_menu_selected`` 路由一级菜单；
        * ``search_submitted`` 返回搜索字符串；
        * ``config_requested``/``theme_requested``/``account_requested`` 打开对应入口；
        * 窗口控制信号由主窗口执行最小化、最大化/还原和关闭。
        """

        primary_menu_selected = Signal(str)
        search_submitted = Signal(str)
        config_requested = Signal()
        navigation_requested = Signal()
        theme_requested = Signal()
        account_requested = Signal()
        minimize_requested = Signal()
        maximize_requested = Signal()
        close_requested = Signal()

        def __init__(self, parent: QWidget | None = None) -> None:
            """创建标题栏并建立默认的首页选中态。"""
            super().__init__(parent)
            self.setObjectName("titleBar")
            self.setFixedHeight(60)  # 与原型保持固定高度，内容区不会因标题文字变化而跳动。
            self._drag_offset: QPoint | None = None
            self._maximized = False
            self._menu_buttons: dict[str, QPushButton] = {}
            self._account_name = "未登录"
            self._build()

        def _build(self) -> None:
            """按从左到右的视觉顺序构造标题栏控件。"""
            root = QHBoxLayout(self)
            root.setContentsMargins(16, 0, 12, 0)
            root.setSpacing(10)

            # 品牌区保持最小宽度，为一级菜单预留稳定空间。
            brand = QWidget(self)
            brand.setObjectName("titleBrand")
            brand.setMinimumWidth(172)
            brand_layout = QHBoxLayout(brand)
            brand_layout.setContentsMargins(0, 0, 8, 0)
            brand_layout.setSpacing(9)
            mark = QLabel("OX")
            mark.setObjectName("brandMark")
            mark.setAlignment(Qt.AlignmentFlag.AlignCenter)
            mark.setFixedSize(32, 32)
            names = QVBoxLayout()
            names.setContentsMargins(0, 0, 0, 0)
            names.setSpacing(0)
            name = QLabel("oxApp")
            name.setObjectName("brandName")
            subtitle = QLabel("量化工作台")
            subtitle.setObjectName("brandSubtitle")
            names.addWidget(name)
            names.addWidget(subtitle)
            brand_layout.addWidget(mark)
            brand_layout.addLayout(names)
            root.addWidget(brand)

            # 一级菜单按钮使用配置驱动；lambda 捕获 item_id 的默认参数避免闭包晚绑定。
            nav = QWidget(self)
            nav.setObjectName("primaryNav")
            nav_layout = QHBoxLayout(nav)
            nav_layout.setContentsMargins(0, 0, 0, 0)
            nav_layout.setSpacing(2)
            for item in PRIMARY_MENUS:
                button = QPushButton(item.label)
                button.setObjectName("primaryNavButton")
                button.setProperty("menu_id", item.item_id)
                button.setCheckable(True)
                button.setCursor(Qt.CursorShape.PointingHandCursor)
                button.clicked.connect(lambda checked=False, item_id=item.item_id: self._select_menu(item_id))
                nav_layout.addWidget(button)
                self._menu_buttons[item.item_id] = button
            nav_layout.addStretch(1)
            root.addWidget(nav, 1)

            # 搜索当前只反馈状态栏，预留后续接入菜单/产品查询服务的边界。
            self._search = QLineEdit()
            self._search.setObjectName("globalSearch")
            self._search.setPlaceholderText("⌕  搜索菜单 / 产品")
            self._search.setFixedWidth(190)
            self._search.setClearButtonEnabled(True)
            self._search.returnPressed.connect(lambda: self.search_submitted.emit(self._search.text().strip()))
            root.addWidget(self._search)

            # 导航、配置、主题、账号入口不在标题栏内展开面板，由主窗口统一管理。
            self._navigation_button = self._icon_button("☰", "显示或隐藏系统菜单", "titleNavigationButton")
            self._navigation_button.clicked.connect(self.navigation_requested)
            root.addWidget(self._navigation_button)
            self._config_button = self._icon_button("⚙", "系统配置", "titleConfigButton")
            self._config_button.clicked.connect(self.config_requested)
            root.addWidget(self._config_button)
            self._theme_button = self._icon_button("◐", "主题", "titleThemeButton")
            self._theme_button.clicked.connect(self.theme_requested)
            root.addWidget(self._theme_button)
            self._account_button = QPushButton("Y  未登录  ▾")
            self._account_button.setObjectName("accountButton")
            self._account_button.setToolTip("账号与配置")
            self._account_button.clicked.connect(self.account_requested)
            root.addWidget(self._account_button)

            # 原生窗口控制被替换为三个按钮，保证无边框窗口仍具备基本操作。
            controls = QWidget(self)
            controls.setObjectName("windowControls")
            controls_layout = QHBoxLayout(controls)
            controls_layout.setContentsMargins(0, 0, 0, 0)
            controls_layout.setSpacing(2)
            self._minimize = self._icon_button("—", "最小化", "windowMinimizeButton")
            self._maximize = self._icon_button("□", "最大化", "windowMaximizeButton")
            self._close = self._icon_button("×", "关闭", "windowCloseButton")
            self._minimize.clicked.connect(self.minimize_requested)
            self._maximize.clicked.connect(self.maximize_requested)
            self._close.clicked.connect(self.close_requested)
            controls_layout.addWidget(self._minimize)
            controls_layout.addWidget(self._maximize)
            controls_layout.addWidget(self._close)
            root.addWidget(controls)

            self.set_active("home")

        @staticmethod
        def _icon_button(text: str, tooltip: str, object_name: str) -> QPushButton:
            """创建统一尺寸的图标按钮，减少标题栏重复 QSS 和布局代码。"""
            button = QPushButton(text)
            button.setObjectName(object_name)
            button.setToolTip(tooltip)
            button.setCursor(Qt.CursorShape.PointingHandCursor)
            button.setFixedSize(32, 32)
            return button

        @property
        def search_edit(self) -> QLineEdit:
            """返回全局搜索输入框，供测试或后续搜索控制器使用。"""
            return self._search

        @property
        def account_button(self) -> QPushButton:
            """返回账号入口按钮。"""
            return self._account_button

        @property
        def navigation_button(self) -> QPushButton:
            """返回系统菜单显示/隐藏入口。"""
            return self._navigation_button

        def _select_menu(self, item_id: str) -> None:
            """更新按钮选中态并向主窗口广播一级菜单 ID。"""
            self.set_active(item_id)
            self.primary_menu_selected.emit(item_id)

        def set_active(self, item_id: str) -> None:
            """按 ID 设置一级菜单互斥选中状态。"""
            for key, button in self._menu_buttons.items():
                button.setChecked(key == item_id)

        def set_account(self, user_name: str, display_name: str = "") -> None:
            """刷新账号摘要；只展示名称首字母和显示名，不保存密码。"""
            self._account_name = display_name or user_name or "未登录"
            initial = self._account_name[:1].upper() if self._account_name else "U"
            self._account_button.setText(f"{initial}  {self._account_name}  ▾")

        def set_theme(self, mode: str) -> None:
            """同步主题入口的图标提示，保留快捷切换语义。"""
            light = str(mode).lower() == "light"
            self._theme_button.setText("☼" if light else "◐")
            self._theme_button.setToolTip("切换为深蓝主题" if light else "切换为浅色主题")

        def set_maximized(self, maximized: bool) -> None:
            """同步窗口当前模式，并切换最大化按钮的图标与提示。"""
            self._maximized = maximized
            self._maximize.setText("❐" if maximized else "□")
            self._maximize.setToolTip("还原" if maximized else "最大化")

        def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt API
            """记录鼠标相对窗口左上角的偏移，用于拖动无边框窗口。"""
            if event.button() == Qt.MouseButton.LeftButton:
                window = self.window()
                self._drag_offset = event.globalPosition().toPoint() - window.frameGeometry().topLeft()
                event.accept()
                return
            super().mousePressEvent(event)

        def mouseMoveEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt API
            """拖动标题栏时移动顶层窗口；最大化状态下禁止移动。"""
            if self._drag_offset is not None and event.buttons() & Qt.MouseButton.LeftButton:
                if not self._maximized:
                    self.window().move(event.globalPosition().toPoint() - self._drag_offset)
                event.accept()
                return
            super().mouseMoveEvent(event)

        def mouseReleaseEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt API
            """结束一次标题栏拖动，清理临时偏移量。"""
            self._drag_offset = None
            super().mouseReleaseEvent(event)

else:
    class TitleBar:  # type: ignore[no-redef]
        """无 PySide6 时的明确失败边界。"""

        def __init__(self, *args, **kwargs) -> None:
            del args, kwargs
            raise RuntimeError("Qt UI requires PySide6; install client/requirements.txt first")
