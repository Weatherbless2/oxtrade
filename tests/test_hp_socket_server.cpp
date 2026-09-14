#include "oxmbs/network/hp_socket_server.h"

#include <arpa/inet.h>
#include <array>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using namespace std::chrono_literals;
using oxmbs::network::PackMessage;
using oxmbs::network::PackMessageKind;
using oxmbs::network::PackPayload;

struct SinkState {
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<PackMessage> messages;
    oxmbs::network::ConnectionId connection_id{0};
};

void collect_frame(oxmbs::network::ConnectionId connection_id,
                   const PackMessage& message,
                   void* context) {
    auto* state = static_cast<SinkState*>(context);
    std::lock_guard lock(state->mutex);
    state->connection_id = connection_id;
    state->messages.push_back(message);
    state->condition.notify_all();
}

std::uint16_t reserve_port() {
    const auto socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(socket_fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);
    assert(::bind(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    assert(::getsockname(socket_fd, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    const auto port = ntohs(address.sin_port);
    ::close(socket_fd);
    return port;
}

int connect_client(std::uint16_t port) {
    const auto socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(socket_fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    assert(::connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    return socket_fd;
}

void send_all(int socket_fd, const std::uint8_t* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const auto sent = ::send(socket_fd, data + offset, size - offset, 0);
        assert(sent > 0);
        offset += static_cast<std::size_t>(sent);
    }
}

bool receive_all(int socket_fd, std::uint8_t* data, std::size_t size, int timeout_ms = 2000) {
    std::size_t offset = 0;
    while (offset < size) {
        pollfd descriptor{socket_fd, POLLIN | POLLHUP | POLLERR, 0};
        const auto ready = ::poll(&descriptor, 1, timeout_ms);
        if (ready <= 0) {
            return false;
        }
        const auto received = ::recv(socket_fd, data + offset, size - offset, 0);
        if (received <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(received);
    }
    return true;
}

PackMessage make_request(std::uint64_t request_id, std::uint16_t function_id = 1) {
    PackMessage message;
    message.kind = PackMessageKind::Request;
    message.request_id = request_id;
    message.message_type = 2;
    message.function_id = function_id;
    message.timestamp_ms = 100;
    const char payload[] = "{\"schema_version\":1,\"keyword\":\"600000\"}";
    message.payload.assign(reinterpret_cast<const std::uint8_t*>(payload),
                           reinterpret_cast<const std::uint8_t*>(payload) + std::strlen(payload));
    return message;
}

void write_u32(std::vector<std::uint8_t>& frame, std::size_t offset, std::uint32_t value) {
    frame[offset] = static_cast<std::uint8_t>(value >> 24U);
    frame[offset + 1U] = static_cast<std::uint8_t>(value >> 16U);
    frame[offset + 2U] = static_cast<std::uint8_t>(value >> 8U);
    frame[offset + 3U] = static_cast<std::uint8_t>(value);
}

void test_codec_boundaries() {
    const std::string payload = "{\"schema_version\":1,\"client_nonce\":\"00112233445566778899aabbccddeeff\"}";
    PackMessage challenge;
    challenge.kind = PackMessageKind::Request;
    challenge.request_id = 1;
    challenge.message_type = 1;
    challenge.function_id = 0;
    challenge.payload.assign(payload.begin(), payload.end());

    PackPayload frame;
    std::string error;
    assert(oxmbs::network::encode_pack_message(challenge, frame, error) ==
           oxmbs::network::PackMessageStatus::Ok);
    assert(frame.size() == 134);
    assert(frame[56] == 0x52 && frame[57] == 0x12 && frame[58] == 0x2A && frame[59] == 0xF4);

    oxmbs::network::PackMessageFrameDecoder decoder;
    std::vector<PackMessage> messages;
    assert(decoder.append(frame.data(), 11, messages, error) ==
           oxmbs::network::PackMessageStatus::NeedMoreData);
    assert(messages.empty());
    assert(decoder.append(frame.data() + 11, frame.size() - 11, messages, error) ==
           oxmbs::network::PackMessageStatus::Ok);
    assert(messages.size() == 1 && messages.front().request_id == 1);

    PackPayload second;
    challenge.request_id = 2;
    assert(oxmbs::network::encode_pack_message(challenge, second, error) ==
           oxmbs::network::PackMessageStatus::Ok);
    PackPayload combined = frame;
    combined.insert(combined.end(), second.begin(), second.end());
    messages.clear();
    assert(decoder.append(combined.data(), combined.size(), messages, error) ==
           oxmbs::network::PackMessageStatus::Ok);
    assert(messages.size() == 2 && messages[0].request_id == 1 && messages[1].request_id == 2);

    frame[0] = 0;
    const auto bad_magic = oxmbs::network::decode_pack_message(frame.data(), frame.size());
    assert(!bad_magic);
    assert(bad_magic.status == oxmbs::network::PackMessageStatus::BadMagic);
    frame = second;
    frame[56] ^= 0x01;
    assert(oxmbs::network::decode_pack_message(frame.data(), frame.size()).status ==
           oxmbs::network::PackMessageStatus::InvalidPayload);

    frame = second;
    frame[5] = 2;
    assert(oxmbs::network::decode_pack_message(frame.data(), frame.size()).status ==
           oxmbs::network::PackMessageStatus::UnsupportedVersion);
    frame = second;
    frame[7] = 0;
    assert(oxmbs::network::decode_pack_message(frame.data(), frame.size()).status ==
           oxmbs::network::PackMessageStatus::InvalidHeader);
    frame = second;
    frame[14] = 1;
    assert(oxmbs::network::decode_pack_message(frame.data(), frame.size()).status ==
           oxmbs::network::PackMessageStatus::InvalidHeader);
    frame = second;
    frame[12] = 9;
    assert(oxmbs::network::decode_pack_message(frame.data(), frame.size()).status ==
           oxmbs::network::PackMessageStatus::InvalidMessageKind);

    PackPayload too_large = second;
    too_large.resize(oxmbs::network::PackMessageWireLayout::kHeaderBytes);
    write_u32(too_large, 8, 0xFFFFFFFFU);
    messages.clear();
    assert(decoder.append(too_large.data(), too_large.size(), messages, error) ==
           oxmbs::network::PackMessageStatus::FrameTooLarge);
}

void test_server_lifecycle_and_frames() {
    SinkState state;
    oxmbs::network::HpSocketServerOptions options;
    options.port = reserve_port();
    options.idle_timeout = 5s;
    options.idle_sweep_interval = 20ms;
    options.frame_sink = collect_frame;
    options.frame_sink_context = &state;
    oxmbs::network::HpSocketServer server(options);
    std::string error;
    assert(server.start(error));
    assert(server.is_running());

    const auto client = connect_client(options.port);
    PackPayload request_frame;
    auto request = make_request(10);
    assert(oxmbs::network::encode_pack_message(request, request_frame, error) ==
           oxmbs::network::PackMessageStatus::Ok);
    send_all(client, request_frame.data(), 13);
    send_all(client, request_frame.data() + 13, request_frame.size() - 13);
    {
        std::unique_lock lock(state.mutex);
        assert(state.condition.wait_for(lock, 2s, [&] { return state.messages.size() == 1; }));
        assert(state.messages.front().request_id == 10);
    }
    assert(server.connection_count() == 1);
    const auto connection_id = state.connection_id;
    assert(connection_id != 0);
    assert(server.bind_session(connection_id, 77, error));

    PackMessage response;
    response.kind = PackMessageKind::Response;
    response.request_id = 10;
    response.session_id = 77;
    response.message_type = 2;
    response.function_id = 1;
    response.error_code = static_cast<std::uint32_t>(oxmbs::network::PackMessageErrorCode::Ok);
    response.payload = {'{', '}', '\n'};
    assert(server.send_to_session(77, response, error));
    std::array<std::uint8_t, oxmbs::network::PackMessageWireLayout::kHeaderBytes> response_header{};
    assert(receive_all(client, response_header.data(), response_header.size()));
    const auto body_size = (static_cast<std::uint32_t>(response_header[8]) << 24U) |
                           (static_cast<std::uint32_t>(response_header[9]) << 16U) |
                           (static_cast<std::uint32_t>(response_header[10]) << 8U) |
                           static_cast<std::uint32_t>(response_header[11]);
    PackPayload response_frame(response_header.begin(), response_header.end());
    std::vector<std::uint8_t> body(body_size);
    assert(receive_all(client, body.data(), body.size()));
    response_frame.insert(response_frame.end(), body.begin(), body.end());
    const auto decoded_response = oxmbs::network::decode_pack_message(response_frame.data(), response_frame.size());
    assert(decoded_response && decoded_response.message.session_id == 77);

    const auto invalid_client = connect_client(options.port);
    for (int attempt = 0; attempt < 100 && server.connection_count() < 2; ++attempt) {
        std::this_thread::sleep_for(5ms);
    }
    assert(server.connection_count() == 2);
    auto invalid_frame = request_frame;
    invalid_frame[0] = 0;
    send_all(invalid_client, invalid_frame.data(), invalid_frame.size());
    for (int attempt = 0; attempt < 200 && server.connection_count() != 1; ++attempt) {
        std::this_thread::sleep_for(5ms);
    }
    assert(server.connection_count() == 1);  // 坏帧只隔离第二条连接，第一条连接的 Session 仍可继续使用。
    ::close(invalid_client);

    auto heartbeat = request;
    heartbeat.kind = PackMessageKind::Heartbeat;
    heartbeat.function_id = 0;
    heartbeat.request_id = 11;
    heartbeat.payload.clear();
    PackPayload heartbeat_frame;
    assert(oxmbs::network::encode_pack_message(heartbeat, heartbeat_frame, error) ==
           oxmbs::network::PackMessageStatus::Ok);
    send_all(client, heartbeat_frame.data(), heartbeat_frame.size());
    std::array<std::uint8_t, oxmbs::network::PackMessageWireLayout::kHeaderBytes> heartbeat_header{};
    assert(receive_all(client, heartbeat_header.data(), heartbeat_header.size()));
    assert(heartbeat_header[12] == static_cast<std::uint8_t>(PackMessageKind::Heartbeat));
    ::close(client);
    assert(server.stop(error));
    assert(!server.is_running());
    assert(server.connection_count() == 0);
}

void test_idle_disconnect() {
    oxmbs::network::HpSocketServerOptions options;
    options.port = reserve_port();
    options.idle_timeout = 100ms;
    options.idle_sweep_interval = 10ms;
    oxmbs::network::HpSocketServer server(options);
    std::string error;
    assert(server.start(error));
    const auto client = connect_client(options.port);
    for (int attempt = 0; attempt < 100 && server.connection_count() == 0; ++attempt) {
        std::this_thread::sleep_for(5ms);
    }
    assert(server.connection_count() == 1);
    for (int attempt = 0; attempt < 200 && server.connection_count() != 0; ++attempt) {
        std::this_thread::sleep_for(5ms);
    }
    assert(server.connection_count() == 0);
    ::close(client);
    assert(server.stop(error));
}

}  // namespace

int main() {
    test_codec_boundaries();
    test_server_lifecycle_and_frames();
    test_idle_disconnect();
    return 0;
}
