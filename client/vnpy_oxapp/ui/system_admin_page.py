"""工单 #18 系统管理 Mock 页面。"""

from __future__ import annotations

from ..connection import ClientProtocolClient
from ..logging import ClientLogger
from ..services.system_service import SystemService
from .qt_bridge import AsyncQtBridge
from .table_page import ServiceTablePage, TableSpec


class SystemAdminPage(ServiceTablePage):
    """系统菜单、数据字典、字典明细和在线日志四个可交互标签页。"""

    def __init__(self, client: ClientProtocolClient, bridge: AsyncQtBridge, logger: ClientLogger, parent=None) -> None:
        service = SystemService(client)
        specs = (
            TableSpec(
                "系统菜单",
                (("menu_id", "菜单编号"), ("menu_name", "菜单名称"), ("parent_id", "父菜单"), ("enabled", "启用")),
                lambda filters: service.list_menus(**filters),
                (("全部", ""), ("启用", "enabled"), ("停用", "disabled")),
            ),
            TableSpec(
                "数据字典",
                (("dict_id", "字典编号"), ("dict_name", "字典名称"), ("category", "分类"), ("enabled", "启用"), ("detail_count", "明细数")),
                lambda filters: service.list_dictionaries(**filters),
                (("全部", ""), ("启用", "enabled"), ("停用", "disabled")),
            ),
            TableSpec(
                "字典明细",
                (("detail_id", "明细编号"), ("dict_id", "字典编号"), ("value", "值"), ("label", "显示名称"), ("sort", "排序"), ("enabled", "启用")),
                lambda filters: service.list_dictionary_details(**filters),
                (("全部", ""), ("启用", "enabled"), ("停用", "disabled")),
            ),
            TableSpec(
                "在线日志",
                (("event_time", "时间"), ("operator", "操作员"), ("level", "级别"), ("event", "事件")),
                lambda filters: service.list_logs(**filters),
                (("全部", ""), ("信息", "INFO"), ("警告", "WARN")),
            ),
        )
        super().__init__(client, bridge, logger, specs, parent)
