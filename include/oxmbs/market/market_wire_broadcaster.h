#pragma once

#include "oxmwh/market_wire.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace oxmbs::market {

class IMarketClientSink {
public:
    virtual ~IMarketClientSink() = default;

    virtual bool send(const std::string& client_id,
                      const std::vector<std::uint8_t>& frame,
                      std::string& error) = 0;
};

using SnapshotRecoveryCallback = std::function<void(const std::string& symbol,
                                                    std::uint64_t expected_sequence,
                                                    std::uint64_t actual_sequence)>;

struct MarketBroadcastStats {
    std::uint64_t received_bytes{0};
    std::uint64_t decoded_frames{0};
    std::uint64_t invalid_frames{0};
    std::uint64_t filtered_frames{0};
    std::uint64_t duplicate_or_out_of_order{0};
    std::uint64_t sequence_gaps{0};
    std::uint64_t delivered_frames{0};
    std::uint64_t send_failed{0};
    std::size_t clients{0};
};

// oxMBS 行情边界：接收已成帧的源字节，按订阅复制广播给 oxApp 客户端。
// 它只负责协议、订阅和生命周期，不拥有 Socket 的连接线程，也不触碰低频 IPC。
class MarketWireBroadcaster final {
public:
    explicit MarketWireBroadcaster(IMarketClientSink& sink,
                                    std::size_t max_clients = 4096);

    bool subscribe(const std::string& client_id,
                   const std::vector<std::string>& symbols,
                   std::string& error);
    bool unsubscribe(const std::string& client_id,
                     const std::vector<std::string>& symbols,
                     std::string& error);
    void remove_client(const std::string& client_id) noexcept;

    // source_id 维度隔离序号；输入可以是半包或粘包，方法会尽可能处理完整帧。
    bool on_source_bytes(const std::string& source_id,
                         std::string_view bytes,
                         std::string& error);

    void set_snapshot_recovery_callback(SnapshotRecoveryCallback callback);
    MarketBroadcastStats stats() const noexcept;

private:
    struct PendingFrame {
        std::string client_id;
        std::vector<std::uint8_t> frame;
    };

    struct PendingGap {
        std::string symbol;
        std::uint64_t expected_sequence{0};
        std::uint64_t actual_sequence{0};
    };

    static std::string sequence_key(const std::string& source_id,
                                    const std::string& symbol);

    IMarketClientSink& sink_;
    const std::size_t max_clients_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unordered_set<std::string>> subscriptions_;
    std::unordered_map<std::string, oxmwh::wire::StreamDecoder> decoders_;
    std::unordered_map<std::string, std::uint64_t> last_sequences_;
    SnapshotRecoveryCallback snapshot_recovery_callback_;
    MarketBroadcastStats stats_;
};

}  // namespace oxmbs::market
