"""页面依赖的服务接口。"""

from .fund_service import FundService
from .market_service import MarketService
from .risk_service import RiskService
from .system_service import SystemService
from .trade_service import TradeService
from .user_service import UserService

__all__ = ["FundService", "MarketService", "RiskService", "SystemService", "TradeService", "UserService"]
