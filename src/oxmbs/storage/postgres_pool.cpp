#include "oxmbs/storage/postgres_pool.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace oxmbs::storage {
namespace {

bool positive(std::chrono::milliseconds value) noexcept {
    return value > std::chrono::milliseconds::zero();
}

PostgresFailure make_failure(PostgresStatus status,
                             std::string message,
                             std::string sqlstate = {}) {
    return PostgresFailure{status, std::move(message), std::move(sqlstate)};
}

bool is_timeout_state(std::string_view state) noexcept {
    return state == "57014" || state == "55P03";
}

std::string command_timeout_sql(const PostgresConnectionOptions& options) {
    // 数值来自已校验的进程配置，不接收用户输入；使用 SET 让每条池连接具备同样的服务端超时。
    std::ostringstream sql;
    sql << "SET statement_timeout = " << options.statement_timeout.count() << ";"
        << " SET lock_timeout = " << options.lock_timeout.count() << ";"
        << " SET idle_in_transaction_session_timeout = "
        << options.idle_transaction_timeout.count() << ";";
    return sql.str();
}

}  // namespace

const char* postgres_status_name(PostgresStatus status) noexcept {
    switch (status) {
    case PostgresStatus::Ok: return "Ok";
    case PostgresStatus::InvalidArgument: return "InvalidArgument";
    case PostgresStatus::ConnectionUnavailable: return "ConnectionUnavailable";
    case PostgresStatus::QueryFailed: return "QueryFailed";
    case PostgresStatus::Timeout: return "Timeout";
    case PostgresStatus::TransactionFailed: return "TransactionFailed";
    case PostgresStatus::PoolClosed: return "PoolClosed";
    }
    return "Unknown";
}

PostgresResult::PostgresResult(PGresult* result) noexcept : result_(result) {}

PostgresResult::~PostgresResult() {
    if (result_ != nullptr) {
        PQclear(result_);
    }
}

PostgresResult::PostgresResult(PostgresResult&& other) noexcept
    : result_(other.result_) {
    other.result_ = nullptr;
}

PostgresResult& PostgresResult::operator=(PostgresResult&& other) noexcept {
    if (this != &other) {
        if (result_ != nullptr) {
            PQclear(result_);
        }
        result_ = other.result_;
        other.result_ = nullptr;
    }
    return *this;
}

bool PostgresResult::ok() const noexcept {
    return result_ != nullptr &&
           (PQresultStatus(result_) == PGRES_TUPLES_OK ||
            PQresultStatus(result_) == PGRES_COMMAND_OK);
}

ExecStatusType PostgresResult::status() const noexcept {
    return result_ == nullptr ? PGRES_FATAL_ERROR : PQresultStatus(result_);
}

int PostgresResult::row_count() const noexcept {
    return result_ == nullptr ? 0 : PQntuples(result_);
}

int PostgresResult::column_count() const noexcept {
    return result_ == nullptr ? 0 : PQnfields(result_);
}

bool PostgresResult::is_null(int row, int column) const noexcept {
    return result_ == nullptr || row < 0 || row >= PQntuples(result_) ||
           column < 0 || column >= PQnfields(result_) || PQgetisnull(result_, row, column) != 0;
}

std::string PostgresResult::value(int row, int column) const {
    if (is_null(row, column)) {
        return {};
    }
    return PQgetvalue(result_, row, column);
}

std::string PostgresResult::error_message() const {
    if (result_ == nullptr) {
        return "PostgreSQL result is null";
    }
    const char* message = PQresultErrorMessage(result_);
    return message == nullptr ? "PostgreSQL query failed" : std::string(message);
}

std::string PostgresResult::sqlstate() const {
    if (result_ == nullptr) {
        return {};
    }
    const char* state = PQresultErrorField(result_, PG_DIAG_SQLSTATE);
    return state == nullptr ? std::string{} : std::string(state);
}

PostgresConnection::PostgresConnection(PostgresConnectionOptions options)
    : options_(std::move(options)) {}

PostgresConnection::~PostgresConnection() {
    close();
}

PostgresConnection::PostgresConnection(PostgresConnection&& other) noexcept {
    move_from(std::move(other));
}

PostgresConnection& PostgresConnection::operator=(PostgresConnection&& other) noexcept {
    if (this != &other) {
        close();
        move_from(std::move(other));
    }
    return *this;
}

void PostgresConnection::move_from(PostgresConnection&& other) noexcept {
    options_ = std::move(other.options_);
    connection_ = other.connection_;
    other.connection_ = nullptr;
    prepared_statements_ = std::move(other.prepared_statements_);
}

bool PostgresConnection::connect(PostgresFailure& failure) {
    if (options_.connect_timeout <= std::chrono::seconds::zero() ||
        !positive(options_.statement_timeout) || !positive(options_.lock_timeout) ||
        !positive(options_.idle_transaction_timeout)) {
        failure = make_failure(PostgresStatus::InvalidArgument,
                               "PostgreSQL connection timeout options must be positive");
        return false;
    }
    close();

    // PQconninfoParse 允许使用 URI 或 keyword=value conninfo；在不改变凭据的前提下补入连接超时。
    char* parse_error = nullptr;
    PQconninfoOption* parsed = PQconninfoParse(options_.conninfo.c_str(), &parse_error);
    if (parsed == nullptr) {
        const std::string message = parse_error == nullptr ? "invalid PostgreSQL conninfo"
                                                            : std::string(parse_error);
        if (parse_error != nullptr) {
            PQfreemem(parse_error);
        }
        failure = make_failure(PostgresStatus::InvalidArgument, message);
        return false;
    }

    std::vector<const char*> keywords;
    std::vector<const char*> parameters;
    keywords.reserve(32);
    parameters.reserve(32);
    bool has_connect_timeout = false;
    for (PQconninfoOption* item = parsed; item->keyword != nullptr; ++item) {
        if (item->val == nullptr) {
            continue;
        }
        if (std::strcmp(item->keyword, "connect_timeout") == 0) {
            has_connect_timeout = true;
        }
        keywords.push_back(item->keyword);
        parameters.push_back(item->val);
    }
    std::string timeout_value;
    if (!has_connect_timeout) {
        timeout_value = std::to_string(options_.connect_timeout.count());
        keywords.push_back("connect_timeout");
        parameters.push_back(timeout_value.c_str());
    }
    keywords.push_back(nullptr);
    parameters.push_back(nullptr);
    connection_ = PQconnectdbParams(keywords.data(), parameters.data(), 0);
    PQconninfoFree(parsed);
    if (connection_ == nullptr || PQstatus(connection_) != CONNECTION_OK) {
        const std::string message = connection_ == nullptr
                                        ? "PostgreSQL connection allocation failed"
                                        : std::string(PQerrorMessage(connection_));
        close();
        failure = make_failure(PostgresStatus::ConnectionUnavailable, message);
        return false;
    }

    PostgresResult timeout_result(PQexec(connection_, command_timeout_sql(options_).c_str()));
    if (!timeout_result.ok()) {
        failure = make_failure(is_timeout_state(timeout_result.sqlstate()) ? PostgresStatus::Timeout
                                                                            : PostgresStatus::QueryFailed,
                               timeout_result.error_message(), timeout_result.sqlstate());
        close();
        return false;
    }
    prepared_statements_.clear();
    failure = PostgresFailure{PostgresStatus::Ok, {}, {}};
    return true;
}

void PostgresConnection::close() noexcept {
    prepared_statements_.clear();
    if (connection_ != nullptr) {
        PQfinish(connection_);
        connection_ = nullptr;
    }
}

bool PostgresConnection::healthy() const noexcept {
    return connection_ != nullptr && PQstatus(connection_) == CONNECTION_OK;
}

PostgresResult PostgresConnection::exec_command(std::string_view sql,
                                                PostgresFailure& failure) {
    if (!healthy() || sql.empty()) {
        failure = make_failure(!healthy() ? PostgresStatus::ConnectionUnavailable
                                          : PostgresStatus::InvalidArgument,
                               !healthy() ? "PostgreSQL connection is unavailable"
                                          : "SQL command must not be empty");
        return {};
    }
    const std::string command(sql);
    PostgresResult result(PQexec(connection_, command.c_str()));
    if (!result.ok()) {
        const auto sqlstate = result.sqlstate();
        failure = make_failure(is_timeout_state(sqlstate) ? PostgresStatus::Timeout
                                                           : PostgresStatus::QueryFailed,
                               result.error_message(), sqlstate);
        return result;
    }
    failure = PostgresFailure{PostgresStatus::Ok, {}, {}};
    return result;
}

bool PostgresConnection::prepare(std::string_view statement_name,
                                 std::string_view sql,
                                 std::size_t parameter_count,
                                 PostgresFailure& failure) {
    if (statement_name.empty() || sql.empty() || parameter_count > static_cast<std::size_t>(INT_MAX)) {
        failure = make_failure(PostgresStatus::InvalidArgument, "invalid prepared statement arguments");
        return false;
    }
    const std::string name(statement_name);
    if (prepared_statements_.find(name) != prepared_statements_.end()) {
        return true;
    }
    const std::string query(sql);
    PostgresResult result(PQprepare(connection_, name.c_str(), query.c_str(),
                                    static_cast<int>(parameter_count), nullptr));
    if (!result.ok()) {
        const auto sqlstate = result.sqlstate();
        failure = make_failure(is_timeout_state(sqlstate) ? PostgresStatus::Timeout
                                                           : PostgresStatus::QueryFailed,
                               result.error_message(), sqlstate);
        return false;
    }
    prepared_statements_.insert(name);
    return true;
}

PostgresResult PostgresConnection::exec_prepared(
    std::string_view statement_name,
    std::string_view sql,
    const std::vector<std::optional<std::string>>& parameters,
    PostgresFailure& failure) {
    if (!healthy()) {
        failure = make_failure(PostgresStatus::ConnectionUnavailable,
                               "PostgreSQL connection is unavailable");
        return {};
    }
    if (!prepare(statement_name, sql, parameters.size(), failure)) {
        return {};
    }

    std::vector<const char*> values;
    values.reserve(parameters.size());
    for (const auto& parameter : parameters) {
        values.push_back(parameter ? parameter->c_str() : nullptr);
    }
    const std::string name(statement_name);
    PostgresResult result(PQexecPrepared(connection_, name.c_str(),
                                          static_cast<int>(values.size()), values.data(),
                                          nullptr, nullptr, 0));
    if (!result.ok()) {
        const auto sqlstate = result.sqlstate();
        failure = make_failure(is_timeout_state(sqlstate) ? PostgresStatus::Timeout
                                                           : PostgresStatus::QueryFailed,
                               result.error_message(), sqlstate);
        return result;
    }
    failure = PostgresFailure{PostgresStatus::Ok, {}, {}};
    return result;
}

std::unique_ptr<PostgresTransaction> PostgresTransaction::begin(PostgresConnection& connection,
                                                                PostgresFailure& failure) {
    auto transaction = std::unique_ptr<PostgresTransaction>(new PostgresTransaction(connection));
    auto result = connection.exec_command("BEGIN", failure);
    if (!result.ok()) {
        failure.status = PostgresStatus::TransactionFailed;
        transaction.reset();
        return nullptr;
    }
    transaction->active_ = true;
    return transaction;
}

PostgresTransaction::PostgresTransaction(PostgresConnection& connection) noexcept
    : connection_(&connection) {}

PostgresTransaction::~PostgresTransaction() {
    rollback();
}

bool PostgresTransaction::commit(PostgresFailure& failure) {
    if (!active_ || connection_ == nullptr) {
        failure = make_failure(PostgresStatus::TransactionFailed, "transaction is not active");
        return false;
    }
    auto result = connection_->exec_command("COMMIT", failure);
    if (!result.ok()) {
        failure.status = PostgresStatus::TransactionFailed;
        return false;
    }
    active_ = false;
    return true;
}

void PostgresTransaction::rollback() noexcept {
    if (!active_ || connection_ == nullptr || !connection_->healthy()) {
        active_ = false;
        return;
    }
    PostgresFailure ignored;
    connection_->exec_command("ROLLBACK", ignored);
    active_ = false;
}

PostgresConnectionPool::PostgresConnectionPool(PostgresPoolOptions options)
    : options_(std::move(options)) {
    if (options_.min_size == 0 || options_.min_size > options_.max_size ||
        options_.max_size == 0 || !positive(options_.acquire_timeout)) {
        throw std::invalid_argument("PostgreSQL pool sizes and acquire timeout are invalid");
    }
}

PostgresConnectionPool::~PostgresConnectionPool() {
    close();
}

bool PostgresConnectionPool::open(PostgresFailure& failure) {
    std::vector<std::unique_ptr<PostgresConnection>> created;
    created.reserve(options_.min_size);
    for (std::size_t index = 0; index < options_.min_size; ++index) {
        auto connection = std::make_unique<PostgresConnection>(options_.connection);
        if (!connection->connect(failure)) {
            return false;
        }
        created.push_back(std::move(connection));
    }
    {
        std::lock_guard lock(mutex_);
        if (opened_) {
            failure = make_failure(PostgresStatus::InvalidArgument, "PostgreSQL pool is already open");
            return false;
        }
        idle_connections_ = std::move(created);
        total_connections_ = idle_connections_.size();
        opened_ = true;
    }
    condition_.notify_all();
    failure = PostgresFailure{PostgresStatus::Ok, {}, {}};
    return true;
}

void PostgresConnectionPool::close() noexcept {
    std::vector<std::unique_ptr<PostgresConnection>> connections;
    {
        std::lock_guard lock(mutex_);
        opened_ = false;
        connections.swap(idle_connections_);
        total_connections_ -= connections.size();
    }
    // Lease 仍在使用的连接会在归还时发现 opened_=false 并销毁；这里先唤醒所有等待者。
    condition_.notify_all();
}

std::unique_ptr<PostgresConnectionPool::Lease> PostgresConnectionPool::acquire(
    PostgresFailure& failure) {
    std::unique_ptr<PostgresConnection> connection;
    bool reserve_new = false;
    const auto deadline = std::chrono::steady_clock::now() + options_.acquire_timeout;
    {
        std::unique_lock lock(mutex_);
        while (opened_ && idle_connections_.empty() && total_connections_ >= options_.max_size) {
            if (condition_.wait_until(lock, deadline) == std::cv_status::timeout) {
                failure = make_failure(PostgresStatus::Timeout, "PostgreSQL pool acquire timed out");
                return nullptr;
            }
        }
        if (!opened_) {
            failure = make_failure(PostgresStatus::PoolClosed, "PostgreSQL pool is closed");
            return nullptr;
        }
        if (!idle_connections_.empty()) {
            connection = std::move(idle_connections_.back());
            idle_connections_.pop_back();
        } else {
            ++total_connections_;  // 先预留槽位，连接创建在锁外进行，避免阻塞其他归还者。
            reserve_new = true;
        }
    }

    if (reserve_new) {
        connection = std::make_unique<PostgresConnection>(options_.connection);
        if (!connection->connect(failure)) {
            std::lock_guard lock(mutex_);
            --total_connections_;
            condition_.notify_one();
            return nullptr;
        }
    } else if (!connection->healthy()) {
        connection->close();
        connection = std::make_unique<PostgresConnection>(options_.connection);
        if (!connection->connect(failure)) {
            std::lock_guard lock(mutex_);
            --total_connections_;
            condition_.notify_one();
            return nullptr;
        }
    }
    failure = PostgresFailure{PostgresStatus::Ok, {}, {}};
    return std::make_unique<Lease>(*this, std::move(connection));
}

void PostgresConnectionPool::release(std::unique_ptr<PostgresConnection> connection) noexcept {
    if (!connection) {
        return;
    }
    bool keep = false;
    {
        std::lock_guard lock(mutex_);
        keep = opened_ && connection->healthy() && idle_connections_.size() < options_.max_size;
        if (keep) {
            idle_connections_.push_back(std::move(connection));
        } else if (total_connections_ > 0) {
            --total_connections_;
        }
    }
    if (!keep) {
        connection.reset();
    }
    condition_.notify_one();
}

std::size_t PostgresConnectionPool::total_size() const noexcept {
    std::lock_guard lock(mutex_);
    return total_connections_;
}

std::size_t PostgresConnectionPool::idle_size() const noexcept {
    std::lock_guard lock(mutex_);
    return idle_connections_.size();
}

PostgresConnectionPool::Lease::Lease(PostgresConnectionPool& owner,
                                     std::unique_ptr<PostgresConnection> connection) noexcept
    : owner_(&owner), connection_(std::move(connection)) {}

PostgresConnectionPool::Lease::~Lease() {
    release();
}

PostgresConnectionPool::Lease::Lease(Lease&& other) noexcept
    : owner_(other.owner_), connection_(std::move(other.connection_)) {
    other.owner_ = nullptr;
}

PostgresConnectionPool::Lease& PostgresConnectionPool::Lease::operator=(Lease&& other) noexcept {
    if (this != &other) {
        release();
        owner_ = other.owner_;
        connection_ = std::move(other.connection_);
        other.owner_ = nullptr;
    }
    return *this;
}

void PostgresConnectionPool::Lease::release() noexcept {
    if (owner_ != nullptr && connection_ != nullptr) {
        owner_->release(std::move(connection_));
    }
    owner_ = nullptr;
}

}  // namespace oxmbs::storage
