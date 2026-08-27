"""#14 开发版行情接入：标准化事件、可替换数据源和本地缓存中心。"""

from .config import MarketConfig, load_market_config
from .event import MarketEvent, MarketPayloadError
from .feed import FeedState, IMarketFeed, MarketFeedStatus
from .hub import MarketHub
from .mock_feed import MockMarketFeed
from .pytdx_feed import PyTdxJsonLineFeed
from .runtime import create_market_feed

__all__ = (
    "FeedState",
    "IMarketFeed",
    "MarketConfig",
    "MarketEvent",
    "MarketFeedStatus",
    "MarketHub",
    "MarketPayloadError",
    "MockMarketFeed",
    "PyTdxJsonLineFeed",
    "create_market_feed",
    "load_market_config",
)
