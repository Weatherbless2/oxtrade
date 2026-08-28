#include "oxmbs/market/market_wire_broadcaster.h"

#include <algorithm>
#include <utility>

namespace oxmbs::market {

MarketWireBroadcaster::MarketWireBroadcaster(IMarketClientSink& sink,
                                             std::size_t max_clients)
    : sink_(sink), max_clients_(std::max<std::size_t>(1, max_clients)) {
}

bool MarketWireBroadcaster::subscribe(const std::string& client_id,
                                      const std::vector<std::string>& symbols,
                                      std::string& error) {
    if (client_id.empty()) {
        error = "client_id is required";
        return false;
    }
    std::vector<std::string> normalized;
    normalized.reserve(symbols.size());
    for (const std::string& symbol : symbols) {
        const std::string value = oxmwh::normalize_symbol(symbol);
        if (value.empty()) {
            error = "invalid market subscription symbol: " + symbol;
            return false;
        }
        if (std::find(normalized.begin(), normalized.end(), value) == normalized.end()) {
            normalized.push_back(value);
        }
    }
    std::lock_guard lock(mutex_);
    if (subscriptions_.find(client_id) == subscriptions_.end() && subscriptions_.size() >= max_clients_) {
        error = "market client limit exceeded";
        return false;
    }
    auto& client_subscriptions = subscriptions_[client_id];
    client_subscriptions.insert(normalized.begin(), normalized.end());
    return true;
}

bool MarketWireBroadcaster::unsubscribe(const std::string& client_id,
                                        const std::vector<std::string>& symbols,
                                        std::string& error) {
    std::vector<std::string> normalized;
    normalized.reserve(symbols.size());
    for (const std::string& symbol : symbols) {
        const std::string value = oxmwh::normalize_symbol(symbol);
        if (value.empty()) {
            error = "invalid market unsubscription symbol: " + symbol;
            return false;
        }
        normalized.push_back(value);
    }
    std::lock_guard lock(mutex_);
    const auto client = subscriptions_.find(client_id);
    if (client == subscriptions_.end()) {
        return true;
    }
    for (const std::string& symbol : normalized) {
        client->second.erase(symbol);
    }
    if (client->second.empty()) {
        subscriptions_.erase(client);
    }
    return true;
}

void MarketWireBroadcaster::remove_client(const std::string& client_id) noexcept {
    std::lock_guard lock(mutex_);
    subscriptions_.erase(client_id);
}

bool MarketWireBroadcaster::on_source_bytes(const std::string& source_id,
                                            std::string_view bytes,
                                            std::string& error) {
    if (source_id.empty()) {
        error = "source_id is required";
        return false;
    }
    std::vector<PendingFrame> pending_frames;
    std::vector<PendingGap> pending_gaps;
    SnapshotRecoveryCallback recovery_callback;
    {
        std::lock_guard lock(mutex_);
        stats_.received_bytes += bytes.size();
        auto& decoder = decoders_[source_id];
        if (!decoder.append(bytes, error)) {
            ++stats_.invalid_frames;
            return false;
        }
        for (;;) {
            auto decoded = decoder.next();
            if (decoded.status == oxmwh::wire::DecodeStatus::Incomplete) {
                break;  // 半包留在 source_id 专属 decoder，下一次回调继续拼接。
            }
            if (!decoded) {
                ++stats_.invalid_frames;
                error = decoded.error.empty() ? "invalid market wire frame" : decoded.error;
                return false;  // 协议边界损坏后不能猜测偏移，调用方应断开并重新建流。
            }
            ++stats_.decoded_frames;
            if (decoded.message.message_type != oxmwh::wire::MessageType::Quote) {
                ++stats_.invalid_frames;
                error = "market broadcaster only accepts Quote frames";
                return false;
            }
            oxmwh::MarketEvent event;
            if (!oxmwh::wire::decode_market_event(decoded.message, event, error)) {
                ++stats_.invalid_frames;
                return false;
            }
            const std::string key = sequence_key(source_id, event.symbol);
            if (event.source_sequence != 0) {
                const auto previous = last_sequences_.find(key);
                if (previous != last_sequences_.end()) {
                    if (event.source_sequence <= previous->second) {
                        ++stats_.duplicate_or_out_of_order;
                        continue;  // 旧行情不能覆盖新行情，也不能重复广播给多个客户端。
                    }
                    if (event.source_sequence > previous->second + 1) {
                        ++stats_.sequence_gaps;
                        pending_gaps.push_back({event.symbol, previous->second + 1, event.source_sequence});
                    }
                }
                last_sequences_[key] = event.source_sequence;
            }

            bool delivered_to_client = false;
            for (const auto& [client_id, subscribed_symbols] : subscriptions_) {
                if (subscribed_symbols.find(event.symbol) == subscribed_symbols.end()) {
                    continue;
                }
                pending_frames.push_back({client_id, decoded.frame});  // 复用已校验原帧，不重复编码破坏协议字段。
                delivered_to_client = true;
            }
            if (!delivered_to_client) {
                ++stats_.filtered_frames;
            }
        }
        recovery_callback = snapshot_recovery_callback_;
    }

    for (const auto& gap : pending_gaps) {
        if (recovery_callback) {
            recovery_callback(gap.symbol, gap.expected_sequence, gap.actual_sequence);  // 慢快照请求在锁外触发，不阻塞源接收。
        }
    }
    bool all_sent = true;
    for (const auto& pending : pending_frames) {
        std::string send_error;
        if (sink_.send(pending.client_id, pending.frame, send_error)) {
            std::lock_guard lock(mutex_);
            ++stats_.delivered_frames;
        } else {
            std::lock_guard lock(mutex_);
            ++stats_.send_failed;
            all_sent = false;
            if (error.empty()) {
                error = "market client " + pending.client_id + " send failed: " + send_error;
            }
        }
    }
    return all_sent;
}

void MarketWireBroadcaster::set_snapshot_recovery_callback(SnapshotRecoveryCallback callback) {
    std::lock_guard lock(mutex_);
    snapshot_recovery_callback_ = std::move(callback);
}

MarketBroadcastStats MarketWireBroadcaster::stats() const noexcept {
    std::lock_guard lock(mutex_);
    MarketBroadcastStats snapshot = stats_;
    snapshot.clients = subscriptions_.size();
    return snapshot;
}

std::string MarketWireBroadcaster::sequence_key(const std::string& source_id,
                                                const std::string& symbol) {
    return source_id + '\x1f' + symbol;  // 分隔符不允许出现在标准代码中，避免 source/symbol 拼接碰撞。
}

}  // namespace oxmbs::market
