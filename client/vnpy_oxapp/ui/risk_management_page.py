"""工单 #21 风控参数、规则和异常事件 Mock 页面。"""

from __future__ import annotations

from ..connection import ClientProtocolClient
from ..logging import ClientLogger
from ..services.risk_service import RiskService
from .qt_bridge import AsyncQtBridge
from .table_page import ServiceTablePage, TableSpec


class RiskManagementPage(ServiceTablePage):
    """风控配置和事件展示共用异步请求、筛选、状态和日志链路。"""

    def __init__(self, client: ClientProtocolClient, bridge: AsyncQtBridge, logger: ClientLogger, session="mock-session", parent=None) -> None:
        service = RiskService(client, session)  # 风控配置查询与交易终端共用当前用户的会话边界。
        specs = (
            TableSpec(
                "风控参数",
                (("parameter_id", "参数编号"), ("parameter_name", "参数名称"), ("value", "值"), ("unit", "单位"), ("scope", "范围"), ("updated_by", "更新人")),
                lambda filters: service.list_parameters(**filters),
                primary_key="parameter_id",
                editable_fields=(("parameter_name", "参数名称"), ("value", "值"), ("unit", "单位"), ("scope", "范围")),
                creator=lambda record: service.create("risk_parameters", record),
                updater=lambda record_id, changes: service.update("risk_parameters", record_id, changes),
            ),
            TableSpec(
                "风控规则",
                (("rule_id", "规则编号"), ("rule_name", "规则名称"), ("enabled", "启用"), ("limit", "阈值"), ("unit", "单位"), ("scope", "范围")),
                lambda filters: service.list_rules(**filters),
                (("全部", ""), ("启用", "enabled"), ("停用", "disabled")),
                primary_key="rule_id",
                editable_fields=(("rule_name", "规则名称"), ("limit", "阈值"), ("unit", "单位"), ("scope", "范围")),
                creator=lambda record: service.create("risk", record),
                updater=lambda record_id, changes: service.update("risk", record_id, changes),
                transitioner=lambda record_id, state: service.transition("risk", record_id, state),
            ),
            TableSpec(
                "异常事件",
                (("event_time", "时间"), ("rule_id", "规则编号"), ("level", "级别"), ("message", "事件"), ("handled", "已处理")),
                lambda filters: service.list_events(**filters),
                (("全部", ""), ("信息", "INFO"), ("警告", "WARN"), ("错误", "ERROR")),
                primary_key="event_id",
                transitioner=lambda record_id, state: service.transition("risk_events", record_id, state),
            ),
        )
        super().__init__(client, bridge, logger, specs, parent)
