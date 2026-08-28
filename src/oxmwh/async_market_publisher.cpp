#include "oxmwh/async_market_publisher.h"

#include <algorithm>

namespace oxmwh {

AsyncMarketPublisher::AsyncMarketPublisher(IMarketPublisher& downstream,
                                           AsyncMarketPublisherOptions options)
    : downstream_(downstream), options_(options) {
    options_.capacity = std::max<std::size_t>(1, options_.capacity);
}

AsyncMarketPublisher::~AsyncMarketPublisher() {
    stop();
}

bool AsyncMarketPublisher::start(std::string& error) {
    std::lock_guard lock(mutex_);
    if (accepting_) {
        return true;
    }
    if (worker_.joinable()) {
        error = "AsyncMarketPublisher is stopping";
        return false;
    }
    accepting_ = true;
    thread_running_ = true;
    worker_ = std::thread(&AsyncMarketPublisher::run, this);  // 发布线程独立承受慢网络，源回调只操作有界内存队列。
    return true;
}

void AsyncMarketPublisher::stop() noexcept {
    {
        std::lock_guard lock(mutex_);
        accepting_ = false;  // 先关闭生产入口，再唤醒线程；已经入队的事件仍会被排空。
    }
    condition_.notify_all();
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
        worker_.join();  // 所有权由 publisher 自己管理，析构前确保 downstream 不再被访问。
    }
    std::lock_guard lock(mutex_);
    thread_running_ = false;
    stats_.queue_depth = queue_.size();
}

bool AsyncMarketPublisher::publish(const MarketEvent& event, std::string& error) {
    {
        std::lock_guard lock(mutex_);
        if (!accepting_) {
            error = "AsyncMarketPublisher is not started";
            return false;
        }
        if (queue_.size() >= options_.capacity) {
            if (options_.full_policy == MarketQueueFullPolicy::CoalesceSameSymbol) {
                const auto found = std::find_if(queue_.rbegin(), queue_.rend(), [&](const MarketEvent& queued) {
                    return queued.symbol == event.symbol;
                });
                if (found != queue_.rend()) {
                    *found = event;  // 快照只保留最新值；避免发布线程追赶已经过时的同标的 Tick。
                    ++stats_.coalesced;
                    return true;
                }
            }
            ++stats_.dropped;
            error = "AsyncMarketPublisher queue is full";
            return false;  // 满队列绝不等待，行情源线程不会被慢下游反向拖住。
        }
        queue_.push_back(event);  // Event 按值复制，发布线程不持有源回调的临时 buffer。
        ++stats_.accepted;
        stats_.queue_depth = queue_.size();
        stats_.queue_high_watermark = std::max(stats_.queue_high_watermark, queue_.size());
    }
    condition_.notify_one();
    return true;
}

bool AsyncMarketPublisher::running() const noexcept {
    std::lock_guard lock(mutex_);
    return accepting_ && thread_running_;
}

AsyncMarketPublisherStats AsyncMarketPublisher::stats() const noexcept {
    std::lock_guard lock(mutex_);
    AsyncMarketPublisherStats snapshot = stats_;
    snapshot.queue_depth = queue_.size();
    return snapshot;
}

void AsyncMarketPublisher::run() {
    for (;;) {
        MarketEvent event;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [&] { return !queue_.empty() || !accepting_; });
            if (queue_.empty() && !accepting_) {
                thread_running_ = false;
                return;  // stop() 已关闭入口且队列排空，线程可以安全结束。
            }
            event = std::move(queue_.front());
            queue_.pop_front();
            stats_.queue_depth = queue_.size();
        }

        std::string error;
        if (downstream_.publish(event, error)) {
            std::lock_guard lock(mutex_);
            ++stats_.published;
        } else {
            std::lock_guard lock(mutex_);
            ++stats_.publish_failed;  // 下游失败不在此处重试，避免阻塞所有标的；由上层指标和连接管理决定恢复。
        }
    }
}

}  // namespace oxmwh
