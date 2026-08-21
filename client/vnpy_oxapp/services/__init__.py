"""页面依赖的服务接口。"""

from .fund_service import FundService
from .risk_service import RiskService
from .system_service import SystemService
from .user_service import UserService

__all__ = ["FundService", "RiskService", "SystemService", "UserService"]
