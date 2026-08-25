"""工单 #20 产品、资产和资金账号 Mock 页面。"""

from __future__ import annotations

from ..connection import ClientProtocolClient
from ..logging import ClientLogger
from ..services.fund_service import FundService
from .qt_bridge import AsyncQtBridge
from .table_page import ServiceTablePage, TableSpec


class FundManagementPage(ServiceTablePage):
    """产品、项目、账号和关联关系使用同一异步表格交互。"""

    def __init__(self, client: ClientProtocolClient, bridge: AsyncQtBridge, logger: ClientLogger, session="mock-session", parent=None) -> None:
        service = FundService(client, session)  # 后续切换真实登录令牌时，产品资金页无需重建或重写。
        specs = (
            TableSpec(
                "产品管理",
                (("fund_id", "产品编号"), ("fund_name", "产品名称"), ("status", "状态"), ("project_ids", "关联项目")),
                lambda filters: service.list_products(**filters),
                (("全部", ""), ("启用", "active"), ("暂停", "paused")),
                primary_key="fund_id",
                editable_fields=(("fund_name", "产品名称"), ("project_ids", "关联项目")),
                creator=lambda record: service.create("funds", record),
                updater=lambda record_id, changes: service.update("funds", record_id, changes),
                transitioner=lambda record_id, state: service.transition("funds", record_id, state),
                deleter=lambda record_id: service.delete("funds", record_id),
            ),
            TableSpec(
                "项目/资产",
                (("project_id", "项目编号"), ("project_name", "项目名称"), ("fund_id", "产品编号"), ("status", "状态")),
                lambda filters: service.list_projects(**filters),
                (("全部", ""), ("启用", "active"), ("暂停", "paused")),
                primary_key="project_id",
                editable_fields=(("project_name", "项目名称"), ("fund_id", "产品编号")),
                creator=lambda record: service.create("projects", record),
                updater=lambda record_id, changes: service.update("projects", record_id, changes),
                transitioner=lambda record_id, state: service.transition("projects", record_id, state),
                deleter=lambda record_id: service.delete("projects", record_id),
            ),
            TableSpec(
                "资金账号",
                (("account_id", "账号编号"), ("account_name", "账号名称"), ("fund_id", "产品编号"), ("available", "可用资金"), ("currency", "币种")),
                lambda filters: service.list_accounts(**filters),
                primary_key="account_id",
                editable_fields=(("account_name", "账号名称"), ("fund_id", "产品编号"), ("available", "可用资金")),
                creator=lambda record: service.create("accounts", record),
                updater=lambda record_id, changes: service.update("accounts", record_id, changes),
                transitioner=lambda record_id, state: service.transition("accounts", record_id, state),
                deleter=lambda record_id: service.delete("accounts", record_id),
            ),
            TableSpec(
                "资产账号关联",
                (("link_id", "关联编号"), ("project_id", "资产编号"), ("account_id", "资金账号"), ("allocation", "配置比例"), ("status", "状态")),
                lambda filters: service.list_account_links(**filters),
                (("全部", ""), ("启用", "active"), ("暂停", "paused")),
                primary_key="link_id",
                editable_fields=(("project_id", "资产编号"), ("account_id", "资金账号"), ("allocation", "配置比例")),
                creator=lambda record: service.create("fund_account_links", record),
                updater=lambda record_id, changes: service.update("fund_account_links", record_id, changes),
                transitioner=lambda record_id, state: service.transition("fund_account_links", record_id, state),
                deleter=lambda record_id: service.delete("fund_account_links", record_id),
            ),
        )
        super().__init__(client, bridge, logger, specs, parent)
