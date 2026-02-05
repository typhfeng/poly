#pragma once

// ============================================================================
// 宏配置
// ============================================================================
#define PARALLEL_PER_SOURCE 9999 // 每个 source 内最多并行 entity 数
#define PARALLEL_TOTAL 9999      // 全局最大并发请求数
#define GRAPHQL_BATCH_SIZE 1000  // 每次请求的 limit
#define DB_FLUSH_THRESHOLD 1000  // 累积多少条刷入 DB
#define PULL_RETRY_DELAY_MS 10   // 重试延迟(ms)

#include <algorithm>
#include <cassert>
#include <chrono>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "config.hpp"
#include "db.hpp"
#include "entities.hpp"
#include "entity_stats.hpp"
#include "https_pool.hpp"

using json = nlohmann::json;

// 前向声明
class Puller;
class SourceScheduler;

// ============================================================================
// GraphQL 查询构建
// ============================================================================
namespace graphql {

inline std::string escape_json(const std::string &s) {
  std::string result;
  result.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
    case '"':
      result += "\\\"";
      break;
    case '\\':
      result += "\\\\";
      break;
    case '\n':
      result += "\\n";
      break;
    default:
      result += c;
      break;
    }
  }
  return result;
}

inline std::string build_query(const char *plural, const char *fields,
                               const std::string &cursor, int limit) {
  std::ostringstream oss;
  oss << R"({"query":"query Q($limit:Int!){)" << plural;

  if (cursor.empty()) {
    // 首次查询：无 where 条件
    oss << R"((first:$limit,orderBy:id,orderDirection:asc){)";
  } else {
    // 增量查询：从 cursor 继续
    oss << R"((first:$limit,orderBy:id,orderDirection:asc,where:{id_gt:\")"
        << escape_json(cursor) << R"(\"}){)";
  }

  oss << fields << R"(}}","variables":{"limit":)" << limit << "}}";
  return oss.str();
}

inline std::string build_target(const std::string &subgraph_id) {
  return "/api/subgraphs/id/" + subgraph_id;
}

} // namespace graphql

// ============================================================================
// EntityPuller - 单个 entity 的拉取器
// ============================================================================
class EntityPuller {
public:
  EntityPuller(const std::string &subgraph_id, const std::string &source_name,
               const entities::EntityDef *entity, Database &db, HttpsPool &pool,
               SourceScheduler *scheduler)
      : source_name_(source_name), entity_(entity), db_(db), pool_(pool), scheduler_(scheduler), target_(graphql::build_target(subgraph_id)) {
    buffer_.reserve(DB_FLUSH_THRESHOLD);
    build_query_cache_();
  }

  void start(const std::string &api_key);
  void on_response(const std::string &body);

  bool is_done() const { return done_; }
  const char *name() const { return entity_->name; }
  int total_synced() const { return total_synced_; }

private:
  void send_request();
  void flush_buffer(bool force_save_cursor);

  // 配置
  std::string source_name_;
  const entities::EntityDef *entity_;
  Database &db_;
  HttpsPool &pool_;
  SourceScheduler *scheduler_;
  std::string target_;

  // 状态
  std::string api_key_;
  std::string cursor_;
  std::string last_cursor_;
  std::vector<std::string> buffer_;
  int total_synced_ = 0;
  bool done_ = false;

  // 请求计时
  std::chrono::steady_clock::time_point request_start_;

  // GraphQL query 缓存（避免每次 ostringstream 拼接 fields）
  std::string query_initial_;
  std::string query_with_cursor_prefix_;
  std::string query_with_cursor_suffix_;

  void build_query_cache_() {
    const int limit = GRAPHQL_BATCH_SIZE;
    const std::string limit_str = std::to_string(limit);

    // cursor 为空时的完整 query
    query_initial_.clear();
    query_initial_.reserve(128 + std::strlen(entity_->plural) + std::strlen(entity_->fields) + limit_str.size());
    query_initial_ += R"({"query":"query Q($limit:Int!){)";
    query_initial_ += entity_->plural;
    query_initial_ += R"((first:$limit,orderBy:id,orderDirection:asc){)";
    query_initial_ += entity_->fields;
    query_initial_ += R"(}}","variables":{"limit":)";
    query_initial_ += limit_str;
    query_initial_ += "}}";

    // cursor 非空时：prefix + escaped(cursor) + suffix
    query_with_cursor_prefix_.clear();
    query_with_cursor_prefix_.reserve(128 + std::strlen(entity_->plural));
    query_with_cursor_prefix_ += R"({"query":"query Q($limit:Int!){)";
    query_with_cursor_prefix_ += entity_->plural;
    query_with_cursor_prefix_ += R"((first:$limit,orderBy:id,orderDirection:asc,where:{id_gt:\")";

    query_with_cursor_suffix_.clear();
    query_with_cursor_suffix_.reserve(64 + std::strlen(entity_->fields) + limit_str.size());
    query_with_cursor_suffix_ += R"(\"}){)";
    query_with_cursor_suffix_ += entity_->fields;
    query_with_cursor_suffix_ += R"(}}","variables":{"limit":)";
    query_with_cursor_suffix_ += limit_str;
    query_with_cursor_suffix_ += "}}";
  }
};

// ============================================================================
// SourceScheduler - 单个 source 的调度器
// ============================================================================
class SourceScheduler {
public:
  SourceScheduler(const SourceConfig &config, Database &db, HttpsPool &pool,
                  Puller *puller, bool is_pnl)
      : source_name_(config.name), db_(db), pool_(pool), puller_(puller) {
    auto entity_list = is_pnl ? entities::PNL_ENTITIES : entities::MAIN_ENTITIES;
    auto entity_count = is_pnl ? entities::PNL_ENTITY_COUNT : entities::MAIN_ENTITY_COUNT;

    for (const auto &name : config.entities) {
      auto *e = entities::find_entity(name.c_str(), entity_list, entity_count);
      if (e) {
        pullers_.emplace_back(config.subgraph_id, source_name_, e, db_, pool_, this);
        db_.init_entity(e);

        // 初始化 entity stats（从 DB 获取当前 count）
        int64_t count = db_.get_table_count(e->table);
        int64_t row_size_bytes = entities::estimate_row_size_bytes(e);
        EntityStatsManager::instance().init(source_name_, e->name, count, row_size_bytes);
      } else {
        std::cerr << "[Scheduler] 未知 entity: " << name << std::endl;
      }
    }
  }

  void start(const std::string &api_key);
  void on_entity_done(EntityPuller *puller);

  bool all_done() const {
    return done_count_ == static_cast<int>(pullers_.size());
  }

  const std::string &name() const { return source_name_; }
  int active_count() const { return active_count_; }

private:
  void start_next();

  // 配置
  std::string source_name_;
  Database &db_;
  HttpsPool &pool_;
  Puller *puller_;

  // 状态
  std::string api_key_;
  std::vector<EntityPuller> pullers_;
  size_t next_idx_ = 0;
  int active_count_ = 0;
  int done_count_ = 0;
};

// ============================================================================
// Puller - 全局协调器
// ============================================================================
class Puller {
public:
  Puller(const Config &config, Database &db, HttpsPool &pool)
      : config_(config), db_(db), pool_(pool) {
    db_.init_sync_state();

    // 设置数据库连接到EntityStatsManager
    EntityStatsManager::instance().set_database(&db_);

    // 预分配空间，避免向量重新分配导致 scheduler_ 指针失效
    schedulers_.reserve(config.sources.size());
    for (const auto &src : config.sources) {
      // 根据 entity 内容判断是否为 PnL subgraph
      bool is_pnl = std::any_of(src.entities.begin(), src.entities.end(),
                                [](const std::string &e) { return e == "UserPosition"; });
      schedulers_.emplace_back(src, db_, pool_, this, is_pnl);
    }
  }

  void run(asio::io_context &ioc) {
    std::cout << "[Puller] 启动，共 " << schedulers_.size() << " 个 source" << std::endl;

    for (auto &s : schedulers_) {
      s.start(config_.api_key);
    }

    ioc.run();
    std::cout << "[Puller] 所有 source 同步完成" << std::endl;
  }

  bool try_acquire_slot() {
    if (total_active_ < PARALLEL_TOTAL) {
      ++total_active_;
      return true;
    }
    return false;
  }

  void release_slot() {
    --total_active_;
    for (auto &s : schedulers_) {
      if (!s.all_done() && s.active_count() < PARALLEL_PER_SOURCE) {
        s.on_entity_done(nullptr);
        break;
      }
    }
  }

private:
  const Config &config_;
  Database &db_;
  HttpsPool &pool_;
  std::vector<SourceScheduler> schedulers_;
  int total_active_ = 0;
};

// ============================================================================
// EntityPuller 实现
// ============================================================================

inline void EntityPuller::start(const std::string &api_key) {
  api_key_ = api_key;
  cursor_ = db_.get_cursor(source_name_, entity_->name);

  // 初始化统计
  EntityStatsManager::instance().start_sync(source_name_, entity_->name);

  std::cout << "[Pull] " << source_name_ << "/" << entity_->name
            << " 开始，cursor=" << (cursor_.empty() ? "(empty)" : cursor_.substr(0, 20) + "...")
            << std::endl;
  send_request();
}

inline void EntityPuller::send_request() {
  std::string query;
  if (cursor_.empty()) {
    query = query_initial_;
  } else {
    // prefix + escaped(cursor) + suffix
    const std::string esc = graphql::escape_json(cursor_);
    query.reserve(query_with_cursor_prefix_.size() + esc.size() + query_with_cursor_suffix_.size());
    query += query_with_cursor_prefix_;
    query += esc;
    query += query_with_cursor_suffix_;
  }

  request_start_ = std::chrono::steady_clock::now();

  // 设置API状态为CALLING
  EntityStatsManager::instance().set_api_state(source_name_, entity_->name, ApiState::CALLING);

  pool_.async_post(target_, query, [this](std::string body) {
    // 设置API状态为PROCESSING
    EntityStatsManager::instance().set_api_state(source_name_, entity_->name, ApiState::PROCESSING);
    on_response(body);
  });
}

inline void EntityPuller::flush_buffer(bool force_save_cursor) {
  if (buffer_.empty())
    return;

  db_.atomic_insert_and_save_cursor(
      entity_->table, entity_->columns, buffer_,
      source_name_, entity_->name, last_cursor_, force_save_cursor);

  std::cout << "[Pull] " << source_name_ << "/" << entity_->name
            << " flush " << buffer_.size() << " 条 (total=" << total_synced_ << ")" << std::endl;

  buffer_.clear();
}

inline void EntityPuller::on_response(const std::string &body) {
  auto now = std::chrono::steady_clock::now();
  auto latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - request_start_).count();

  auto &stats = EntityStatsManager::instance();

  // 请求失败 - 重试
  if (body.empty()) {
    stats.record_failure(source_name_, entity_->name, FailureKind::NETWORK, latency_ms);
    std::cerr << "[Pull] " << entity_->name << " 请求失败，重试" << std::endl;
    pool_.schedule_retry([this]() { send_request(); }, PULL_RETRY_DELAY_MS);
    return;
  }

  // JSON 解析
  json j;
  try {
    j = json::parse(body);
  } catch (...) {
    stats.record_failure(source_name_, entity_->name, FailureKind::JSON, latency_ms);
    std::cerr << "[Pull] " << entity_->name << " JSON 解析失败，重试" << std::endl;
    pool_.schedule_retry([this]() { send_request(); }, PULL_RETRY_DELAY_MS);
    return;
  }

  // GraphQL 错误 - 重试
  if (j.contains("errors")) {
    stats.record_failure(source_name_, entity_->name, FailureKind::GRAPHQL, latency_ms);
    // 尝试从错误里解析 bad indexers（只有失败能归因到具体 indexer）
    if (j["errors"].is_array()) {
      for (auto &err : j["errors"]) {
        if (!err.contains("message") || !err["message"].is_string())
          continue;
        const std::string msg = err["message"].get<std::string>();
        const std::string k = "bad indexers:";
        auto p = msg.find(k);
        if (p == std::string::npos)
          continue;
        auto lb = msg.find('{', p);
        auto rb = msg.find('}', lb == std::string::npos ? p : lb + 1);
        if (lb == std::string::npos || rb == std::string::npos || rb <= lb)
          continue;
        std::string inside = msg.substr(lb + 1, rb - lb - 1);

        auto trim = [](std::string s) {
          size_t b = 0;
          while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n' || s[b] == '\r'))
            ++b;
          size_t e = s.size();
          while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' || s[e - 1] == '\r'))
            --e;
          return s.substr(b, e - b);
        };

        size_t pos = 0;
        while (pos < inside.size()) {
          auto comma = inside.find(',', pos);
          std::string part = (comma == std::string::npos) ? inside.substr(pos) : inside.substr(pos, comma - pos);
          pos = (comma == std::string::npos) ? inside.size() : comma + 1;
          part = trim(std::move(part));
          if (part.empty())
            continue;
          auto colon = part.find(':');
          if (colon == std::string::npos)
            continue;
          std::string indexer = trim(part.substr(0, colon));
          std::string reason = trim(part.substr(colon + 1));
          // 不要多算：Unavailable 是正常情况；只统计 BadResponse
          if (!indexer.empty() && reason.starts_with("BadResponse")) {
            stats.record_indexer_fail(source_name_, entity_->name, indexer);
          }
        }
      }
    }
    std::cerr << "[Pull] " << entity_->name << " GraphQL 错误: " << j["errors"].dump() << std::endl;
    std::cerr << "[Pull] " << entity_->name << " 重试" << std::endl;
    pool_.schedule_retry([this]() { send_request(); }, PULL_RETRY_DELAY_MS);
    return;
  }

  // 响应格式错误 - 重试
  if (!j.contains("data") || !j["data"].contains(entity_->plural)) {
    stats.record_failure(source_name_, entity_->name, FailureKind::FORMAT, latency_ms);
    std::cerr << "[Pull] " << entity_->name << " 响应格式错误，重试" << std::endl;
    pool_.schedule_retry([this]() { send_request(); }, PULL_RETRY_DELAY_MS);
    return;
  }

  auto &items = j["data"][entity_->plural];

  // 没有更多数据
  if (items.empty()) {
    stats.record_success(source_name_, entity_->name, 0, latency_ms);
    flush_buffer(true);
    stats.end_sync(source_name_, entity_->name);
    std::cout << "[Pull] " << source_name_ << "/" << entity_->name
              << " 完成，共 " << total_synced_ << " 条" << std::endl;
    done_ = true;
    scheduler_->on_entity_done(this);
    return;
  }

  // 记录成功
  stats.record_success(source_name_, entity_->name, items.size(), latency_ms);

  // 处理数据
  buffer_.reserve(buffer_.size() + items.size());
  for (const auto &item : items) {
    std::string values = entity_->to_values(item);
    if (!values.empty()) {
      buffer_.push_back(std::move(values));
    }
  }

  last_cursor_ = items.back()["id"].get<std::string>();
  cursor_ = last_cursor_;
  total_synced_ += items.size();

  // 达到阈值则刷入
  if (buffer_.size() >= DB_FLUSH_THRESHOLD) {
    flush_buffer(false);
  }

  // 最后一批
  if (items.size() < GRAPHQL_BATCH_SIZE) {
    flush_buffer(true);
    stats.end_sync(source_name_, entity_->name);
    std::cout << "[Pull] " << source_name_ << "/" << entity_->name
              << " 完成，共 " << total_synced_ << " 条" << std::endl;
    done_ = true;
    scheduler_->on_entity_done(this);
    return;
  }

  // 继续请求
  send_request();
}

// ============================================================================
// SourceScheduler 实现
// ============================================================================

inline void SourceScheduler::start(const std::string &api_key) {
  api_key_ = api_key;
  std::cout << "[Scheduler] " << source_name_ << " 启动，共 "
            << pullers_.size() << " 个 entity" << std::endl;

  while (next_idx_ < pullers_.size() &&
         active_count_ < PARALLEL_PER_SOURCE &&
         puller_->try_acquire_slot()) {
    pullers_[next_idx_].start(api_key_);
    ++active_count_;
    ++next_idx_;
  }
}

inline void SourceScheduler::on_entity_done(EntityPuller *puller) {
  if (puller != nullptr) {
    assert(puller->is_done());
    ++done_count_;
    --active_count_;
    puller_->release_slot();
  }
  start_next();
}

inline void SourceScheduler::start_next() {
  while (next_idx_ < pullers_.size() &&
         active_count_ < PARALLEL_PER_SOURCE &&
         puller_->try_acquire_slot()) {
    pullers_[next_idx_].start(api_key_);
    ++active_count_;
    ++next_idx_;
  }
}
