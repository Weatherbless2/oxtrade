#pragma once

#include <cstddef>
#include <string>

namespace oxmbs::ipc {

enum class IpcResult {
    Ok,
    InvalidArgument,
    NotOpen,
    Interrupted,
    WouldBlock,
    MessageTooLarge,
    QueueRemoved,
    PermissionDenied,
    ResourceUnavailable,
    SystemError,
};

const char* ipc_result_name(IpcResult result) noexcept;

struct ReceiveResult {
    IpcResult result{IpcResult::SystemError};
    std::size_t size{0};
    long message_type{0};
    std::string error;

    explicit operator bool() const noexcept {
        return result == IpcResult::Ok;
    }
};

}  // namespace oxmbs::ipc
