"""工单 #20 产品、资产和资金账号 Mock 页面。"""

from __future__ import annotations

from ..connection import ClientProtocolClient
from ..logging import ClientLogger
from ..services.fund_service import FundService
from .qt_bridge import AsyncQtBridge
from .table_page import ServiceTablePage, TableSpec


class FundManagementPage(ServiceTablePage):
    """产品、项目、账号和关联关系使用同一异步表格交互。"""

    def __init__(self, client: ClientProtocolClient, bridge: AsyncQtBridge, logger: ClientLogger, parent=None) -> None:
        service = FundService(client)
        specs = (
            TableSpec(
                "产品管理",
                (("fund_id", "产品编号"), ("fund_name", "产品名称"), ("status", "状态"), ("project_ids", "关联项目")),
                lambda filters: service.list_products(**filters),
                (("全部", ""), ("启用", "active"), ("暂停", "paused")),
            ),
            TableSpec(
                "项目/资产",
                (("project_id", "项目编号"), ("project_name", "项目名称"), ("fund_id", "产品编号"), ("status", "状态")),
                lambda filters: service.list_projects(**filters),
                (("全部", ""), ("启用", "active"), ("暂停", "paused")),
            ),
            TableSpec(
                "资金账号",
                (("account_id", "账号编号"), ("account_name", "账号名称"), ("fund_id", "产品编号"), ("available", "可用资金"), ("currency", "币种")),
                lambda filters: service.list_accounts(**filters),
            ),
            TableSpec(
                "产品项目关联",
                (("link_id", "关联编号"), ("fund_id", "产品编号"), ("project_id", "项目编号"), ("allocation", "配置比例"), ("status", "状态")),
                lambda filters: service.list_project_links(**filters),
                (("全部", ""), ("启用", "active"), ("暂停", "paused")),
            ),
        )
        super().__init__(client, bridge, logger, specs, parent)
