#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <libpq-fe.h>

namespace oxmbs::storage {

enum class PostgresStatus {
    Ok,
    InvalidArgument,
    ConnectionUnavailable,
    QueryFailed,
    Timeout,
    TransactionFailed,
    PoolClosed,
};

const char* postgres_status_name(PostgresStatus status) noexcept;

struct PostgresFailure {
    PostgresStatus status{PostgresStatus::QueryFailed};
    std::string message;
    std::string sqlstate;
};

struct PostgresConnectionOptions {
    // 空 conninfo 时由 libpq 使用 DATABASE_URL/PG* 环境变量；不在源码保存账号密码。
    std::string conninfo;
    std::chrono::seconds connect_timeout{5};
    std::chrono::milliseconds statement_timeout{3000};
    std::chrono::milliseconds lock_timeout{1000};
    std::chrono::milliseconds idle_transaction_timeout{10000};
};

class PostgresResult {
public:
    PostgresResult() = default;
    explicit PostgresResult(PGresult* result) noexcept;
    ~PostgresResult();

    PostgresResult(const PostgresResult&) = delete;
    PostgresResult& operator=(const PostgresResult&) = delete;
    PostgresResult(PostgresResult&& other) noexcept;
    PostgresResult& operator=(PostgresResult&& other) noexcept;

    bool ok() const noexcept;
    ExecStatusType status() const noexcept;
    int row_count() const noexcept;
    int column_count() const noexcept;
    bool is_null(int row, int column) const noexcept;
    std::string value(int row, int column) const;
    std::string error_message() const;
    std::string sqlstate() const;

private:
    PGresult* result_{nullptr};
};

class PostgresConnection {
public:
    explicit PostgresConnection(PostgresConnectionOptions options = {});
    ~PostgresConnection();

    PostgresConnection(const PostgresConnection&) = delete;
    PostgresConnection& operator=(const PostgresConnection&) = delete;
    PostgresConnection(PostgresConnection&& other) noexcept;
    PostgresConnection& operator=(PostgresConnection&& other) noexcept;

    bool connect(PostgresFailure& failure);
    void close() noexcept;
    bool healthy() const noexcept;

    PostgresResult exec_command(std::string_view sql, PostgresFailure& failure);
    PostgresResult exec_prepared(
        std::string_view statement_name,
        std::string_view sql,
        const std::vector<std::optional<std::string>>& parameters,
        PostgresFailure& failure);

    const PostgresConnectionOptions& options() const noexcept { return options_; }

private:
    bool prepare(std::string_view statement_name,
                 std::string_view sql,
                 std::size_t parameter_count,
                 PostgresFailure& failure);
    void move_from(PostgresConnection&& other) noexcept;

    PostgresConnectionOptions options_;
    PGconn* connection_{nullptr};
    std::unordered_set<std::string> prepared_statements_;
};

class PostgresTransaction {
public:
    static std::unique_ptr<PostgresTransaction> begin(PostgresConnection& connection,
                                                       PostgresFailure& failure);

    ~PostgresTransaction();
    PostgresTransaction(const PostgresTransaction&) = delete;
    PostgresTransaction& operator=(const PostgresTransaction&) = delete;

    bool commit(PostgresFailure& failure);
    void rollback() noexcept;

private:
    explicit PostgresTransaction(PostgresConnection& connection) noexcept;

    PostgresConnection* connection_{nullptr};
    bool active_{false};
};

struct PostgresPoolOptions {
    PostgresConnectionOptions connection;
    std::size_t min_size{1};
    std::size_t max_size{8};
    std::chrono::milliseconds acquire_timeout{1000};
};

class PostgresConnectionPool {
public:
    class Lease;

    explicit PostgresConnectionPool(PostgresPoolOptions options = {});
    ~PostgresConnectionPool();

    PostgresConnectionPool(const PostgresConnectionPool&) = delete;
    PostgresConnectionPool& operator=(const PostgresConnectionPool&) = delete;

    bool open(PostgresFailure& failure);
    void close() noexcept;
    std::unique_ptr<Lease> acquire(PostgresFailure& failure);

    std::size_t total_size() const noexcept;
    std::size_t idle_size() const noexcept;
    const PostgresPoolOptions& options() const noexcept { return options_; }

private:
    friend class Lease;

    void release(std::unique_ptr<PostgresConnection> connection) noexcept;

    PostgresPoolOptions options_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<std::unique_ptr<PostgresConnection>> idle_connections_;
    std::size_t total_connections_{0};
    bool opened_{false};
};

class PostgresConnectionPool::Lease {
public:
    Lease(PostgresConnectionPool& owner,
          std::unique_ptr<PostgresConnection> connection) noexcept;
    ~Lease();

    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    Lease(Lease&& other) noexcept;
    Lease& operator=(Lease&& other) noexcept;

    PostgresConnection& connection() noexcept { return *connection_; }
    const PostgresConnection& connection() const noexcept { return *connection_; }
    explicit operator bool() const noexcept { return connection_ != nullptr; }

private:
    void release() noexcept;

    PostgresConnectionPool* owner_{nullptr};
    std::unique_ptr<PostgresConnection> connection_;
};

}  // namespace oxmbs::storage
