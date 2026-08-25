"""工单 #19 用户、角色、站点和权限 Mock 页面。"""

from __future__ import annotations

from ..connection import ClientProtocolClient
from ..logging import ClientLogger
from ..services.user_service import UserService
from .qt_bridge import AsyncQtBridge
from .table_page import ServiceTablePage, TableSpec


class UserRightsPage(ServiceTablePage):
    """把用户主表和角色/站点/菜单权限关联字段直接展示出来。"""

    def __init__(self, client: ClientProtocolClient, bridge: AsyncQtBridge, logger: ClientLogger, session="mock-session", parent=None) -> None:
        service = UserService(client, session)  # Service 保持同一对象但每次请求动态读取登录后的 session_id。
        specs = (
            TableSpec(
                "用户管理",
                (("user_id", "用户编号"), ("user_name", "用户名"), ("status", "状态"), ("role_ids", "角色"), ("site_ids", "站点")),
                lambda filters: service.list_users(**filters),
                (("全部", ""), ("启用", "active"), ("冻结", "frozen"), ("待审核", "pending")),
                primary_key="user_id",
                editable_fields=(("user_name", "用户名"), ("display_name", "显示名称")),
                creator=lambda record: service.create("users", record),
                updater=lambda record_id, changes: service.update("users", record_id, changes),
                transitioner=lambda record_id, state: service.transition("users", record_id, state),
                deleter=lambda record_id: service.delete("users", record_id),
            ),
            TableSpec(
                "角色管理",
                (("role_id", "角色编号"), ("role_name", "角色名称"), ("status", "状态"), ("permission_count", "权限数量")),
                lambda filters: service.list_roles(**filters),
                (("全部", ""), ("启用", "active"), ("冻结", "frozen")),
                primary_key="role_id",
                editable_fields=(("role_name", "角色名称"),),
                creator=lambda record: service.create("roles", record),
                updater=lambda record_id, changes: service.update("roles", record_id, changes),
                transitioner=lambda record_id, state: service.transition("roles", record_id, state),
                deleter=lambda record_id: service.delete("roles", record_id),
            ),
            TableSpec(
                "站点管理",
                (("site_id", "站点编号"), ("site_name", "站点名称"), ("ip_policy", "IP 策略"), ("multi_login", "允许多点登录"), ("status", "状态")),
                lambda filters: service.list_sites(**filters),
                (("全部", ""), ("启用", "active"), ("冻结", "frozen")),
                primary_key="site_id",
                editable_fields=(("site_name", "站点名称"), ("ip_policy", "IP 策略")),
                creator=lambda record: service.create("sites", record),
                updater=lambda record_id, changes: service.update("sites", record_id, changes),
                transitioner=lambda record_id, state: service.transition("sites", record_id, state),
                deleter=lambda record_id: service.delete("sites", record_id),
            ),
            TableSpec(
                "角色菜单权限",
                (("right_id", "权限编号"), ("role_id", "角色编号"), ("menu_id", "菜单编号"), ("permission", "权限"), ("enabled", "启用")),
                lambda filters: service.list_role_rights(**filters),
                (("全部", ""), ("启用", "enabled"), ("停用", "disabled")),
                primary_key="right_id",
                editable_fields=(("role_id", "角色编号"), ("menu_id", "菜单编号"), ("permission", "权限")),
                creator=lambda record: service.create("role_rights", record),
                updater=lambda record_id, changes: service.update("role_rights", record_id, changes),
                transitioner=lambda record_id, state: service.transition("role_rights", record_id, state),
                deleter=lambda record_id: service.delete("role_rights", record_id),
            ),
            TableSpec(
                "托管服务",
                (("colo_id", "托管编号"), ("colo_name", "服务名称"), ("endpoint", "地址"),
                 ("ip_policy", "IP 策略"), ("status", "状态"), ("last_heartbeat", "最近心跳")),
                lambda filters: service.list_cololist(**filters),
                (("全部", ""), ("启用", "active"), ("维护", "maintenance")),
                primary_key="colo_id",
                editable_fields=(("colo_name", "服务名称"), ("endpoint", "地址"), ("ip_policy", "IP 策略")),
                creator=lambda record: service.create("cololist", record),
                updater=lambda record_id, changes: service.update("cololist", record_id, changes),
                transitioner=lambda record_id, state: service.transition("cololist", record_id, state),
                deleter=lambda record_id: service.delete("cololist", record_id),
            ),
            TableSpec(
                "用户菜单权限",
                (("right_id", "权限编号"), ("user_id", "用户编号"), ("menu_id", "菜单编号"), ("permission", "权限"), ("enabled", "启用")),
                lambda filters: service.list_user_menu_rights(**filters),
                (("全部", ""), ("启用", "enabled"), ("停用", "disabled")),
                primary_key="right_id",
                editable_fields=(("user_id", "用户编号"), ("menu_id", "菜单编号"), ("permission", "权限")),
                creator=lambda record: service.create("user_menu_rights", record),
                updater=lambda record_id, changes: service.update("user_menu_rights", record_id, changes),
                transitioner=lambda record_id, state: service.transition("user_menu_rights", record_id, state),
                deleter=lambda record_id: service.delete("user_menu_rights", record_id),
            ),
            TableSpec(
                "产品权限",
                (("right_id", "权限编号"), ("user_id", "用户编号"), ("fund_id", "产品编号"), ("permission", "权限"), ("enabled", "启用")),
                lambda filters: service.list_fund_rights(**filters),
                (("全部", ""), ("启用", "enabled"), ("停用", "disabled")),
                primary_key="right_id",
                editable_fields=(("user_id", "用户编号"), ("fund_id", "产品编号"), ("permission", "权限")),
                creator=lambda record: service.create("fund_rights", record),
                updater=lambda record_id, changes: service.update("fund_rights", record_id, changes),
                transitioner=lambda record_id, state: service.transition("fund_rights", record_id, state),
                deleter=lambda record_id: service.delete("fund_rights", record_id),
            ),
            TableSpec(
                "资产权限",
                (("right_id", "权限编号"), ("user_id", "用户编号"), ("project_id", "资产编号"), ("permission", "权限"), ("enabled", "启用")),
                lambda filters: service.list_project_rights(**filters),
                (("全部", ""), ("启用", "enabled"), ("停用", "disabled")),
                primary_key="right_id",
                editable_fields=(("user_id", "用户编号"), ("project_id", "资产编号"), ("permission", "权限")),
                creator=lambda record: service.create("project_rights", record),
                updater=lambda record_id, changes: service.update("project_rights", record_id, changes),
                transitioner=lambda record_id, state: service.transition("project_rights", record_id, state),
                deleter=lambda record_id: service.delete("project_rights", record_id),
            ),
        )
        super().__init__(client, bridge, logger, specs, parent)
