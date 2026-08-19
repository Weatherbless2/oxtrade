#pragma once

#include "oxmbs/ipc/ipc_result.h"

#include <cstddef>
#include <string>

namespace oxmbs::ipc {

struct QueueOptions {
    std::size_t max_bytes{0};
    int permissions{0600};
};

class SystemVMessageQueue {
public:
    SystemVMessageQueue() = default;
    ~SystemVMessageQueue();

    SystemVMessageQueue(const SystemVMessageQueue&) = delete;
    SystemVMessageQueue& operator=(const SystemVMessageQueue&) = delete;

    SystemVMessageQueue(SystemVMessageQueue&& other) noexcept;
    SystemVMessageQueue& operator=(SystemVMessageQueue&& other) noexcept;

    IpcResult create(const QueueOptions& options, std::string& error);
    IpcResult send(long message_type,
                   const void* data,
                   std::size_t size,
                   bool non_blocking,
                   std::string& error) const;
    ReceiveResult receive(long message_type,
                          void* buffer,
                          std::size_t capacity,
                          bool non_blocking) const;
    IpcResult remove(std::string& error) noexcept;

    bool is_open() const noexcept;
    int id() const noexcept;
    std::size_t max_bytes() const noexcept;

private:
    void reset() noexcept;

    int id_{-1};
    std::size_t max_bytes_{0};
};

}  // namespace oxmbs::ipc
