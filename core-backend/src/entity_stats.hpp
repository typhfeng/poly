#pragma once

#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>

using json = nlohmann::json;

// 前向声明
class Database;

// ============================================================================
// API 状态枚举
// ============================================================================
enum class ApiState {
  IDLE,      // 空闲
  CALLING,   // API 调用中
  PROCESSING // 本地解析/backoff
};

// ============================================================================
// 单个 Entity 的实时统计
// ============================================================================
struct EntityStat {
  std::string source;
  std::string entity;

  // 记录数（从 DB 初始化，之后累加）
  int64_t count = 0;
  int64_t initial_count = 0; // 同步开始时的初始count

  // 估算：单条记录的"结构体大小"(字节)，用于展示 DB 规模
  int64_t row_size_bytes = 0;

  // 速度统计（历史累积，持久化到DB）
  int64_t total_rows_synced = 0; // 历史总同步行数
  int64_t total_api_time_ms = 0; // 历史总API调用时间（不含本地处理）

  // QoS 统计
  std::deque<int64_t> recent_latencies; // 最近20个请求的延时（不持久化）
  double success_rate = 100.0;          // 持久化到DB
  int64_t total_requests = 0;           // 历史总请求数（持久化）
  int64_t success_requests = 0;         // 历史成功请求数（持久化）

  // 状态
  bool is_syncing = false;
  bool sync_done = false;
  ApiState api_state = ApiState::IDLE;
  std::chrono::steady_clock::time_point last_update;
};

// ============================================================================
// 全局 Entity Stats 管理器
// ============================================================================
class EntityStatsManager {
public:
  static EntityStatsManager &instance() {
    static EntityStatsManager inst;
    return inst;
  }

  // 设置数据库连接（在启动时调用一次）
  void set_database(Database *db) {
    db_ = db;
  }

  // 初始化 entity（设置初始 count，并从DB加载历史统计）
  void init(const std::string &source, const std::string &entity, int64_t count, int64_t row_size_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto key = make_key(source, entity);
    auto &stat = stats_[key];
    stat.source = source;
    stat.entity = entity;
    stat.count = count;
    stat.initial_count = count;
    stat.row_size_bytes = row_size_bytes;
    stat.last_update = std::chrono::steady_clock::now();

    // 从数据库加载历史统计
    load_from_db(stat);
  }

  // 开始同步
  void start_sync(const std::string &source, const std::string &entity) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto key = make_key(source, entity);
    auto &stat = stats_[key];
    stat.is_syncing = true;
    stat.sync_done = false;
    stat.initial_count = stat.count;
    stat.api_state = ApiState::IDLE;
  }

  // 完成同步
  void end_sync(const std::string &source, const std::string &entity) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto key = make_key(source, entity);
    if (stats_.count(key)) {
      stats_[key].is_syncing = false;
      stats_[key].sync_done = true;
      stats_[key].api_state = ApiState::IDLE;
      save_to_db(stats_[key]); // 保存到数据库
    }
  }

  // 设置API状态
  void set_api_state(const std::string &source, const std::string &entity, ApiState state) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto key = make_key(source, entity);
    if (stats_.count(key)) {
      stats_[key].api_state = state;
    }
  }

  // 记录成功的请求（latency_ms是纯API调用时间，不含本地处理）
  void record_success(const std::string &source, const std::string &entity,
                      int64_t records, int64_t latency_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto key = make_key(source, entity);
    auto &stat = stats_[key];

    stat.count += records;
    stat.total_requests++;
    stat.success_requests++;
    stat.total_rows_synced += records;
    stat.total_api_time_ms += latency_ms; // 累加API调用时间
    stat.last_update = std::chrono::steady_clock::now();

    // 记录最近20个延时（不持久化）
    stat.recent_latencies.push_back(latency_ms);
    if (stat.recent_latencies.size() > 20) {
      stat.recent_latencies.pop_front();
    }

    // 更新成功率
    stat.success_rate = static_cast<double>(stat.success_requests) / stat.total_requests * 100.0;

    // 每10次请求保存一次到数据库
    if (stat.total_requests % 10 == 0) {
      save_to_db(stat);
    }
  }

  // 记录失败的请求（latency_ms是纯API调用时间，不含本地处理）
  void record_failure(const std::string &source, const std::string &entity, int64_t latency_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto key = make_key(source, entity);
    auto &stat = stats_[key];

    stat.total_requests++;
    stat.total_api_time_ms += latency_ms; // 累加API调用时间
    stat.last_update = std::chrono::steady_clock::now();

    // 记录最近20个延时（不持久化）
    stat.recent_latencies.push_back(latency_ms);
    if (stat.recent_latencies.size() > 20) {
      stat.recent_latencies.pop_front();
    }

    // 更新成功率
    stat.success_rate = static_cast<double>(stat.success_requests) / stat.total_requests * 100.0;

    // 每10次请求保存一次到数据库
    if (stat.total_requests % 10 == 0) {
      save_to_db(stat);
    }
  }

  // 获取所有统计（JSON 格式）
  json get_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    json result = json::object();

    for (auto &[key, stat] : stats_) {
      // 计算速度：基于历史累积的准确数据
      // speed = 总同步行数 / 总API调用时间（秒）
      double speed = 0.0;
      if (stat.total_rows_synced > 0 && stat.total_api_time_ms > 0) {
        speed = static_cast<double>(stat.total_rows_synced) / (static_cast<double>(stat.total_api_time_ms) / 1000.0);
      }

      // 计算平均延时：最近20个请求，done后显示0ms
      double avg_latency = 0.0;
      if (!stat.sync_done && !stat.recent_latencies.empty()) {
        int64_t sum = 0;
        for (auto latency : stat.recent_latencies) {
          sum += latency;
        }
        avg_latency = static_cast<double>(sum) / stat.recent_latencies.size();
      }

      // API状态字符串
      std::string api_state_str = "idle";
      if (stat.api_state == ApiState::CALLING) {
        api_state_str = "calling";
      } else if (stat.api_state == ApiState::PROCESSING) {
        api_state_str = "processing";
      }

      result[key] = {
          {"source", stat.source},
          {"entity", stat.entity},
          {"count", stat.count},
          {"row_size_bytes", stat.row_size_bytes},
          {"db_size_mb", (stat.row_size_bytes > 0) ? (static_cast<double>(stat.row_size_bytes) * static_cast<double>(stat.count) / (1024.0 * 1024.0)) : 0.0},
          {"speed", std::round(speed * 10) / 10},
          {"avg_latency_ms", std::round(avg_latency)},
          {"success_rate", std::round(stat.success_rate * 10) / 10},
          {"is_syncing", stat.is_syncing},
          {"sync_done", stat.sync_done},
          {"total_requests", stat.total_requests},
          {"total_rows_synced", stat.total_rows_synced},
          {"api_state", api_state_str},
      };
    }

    return result;
  }

private:
  EntityStatsManager() = default;

  std::string make_key(const std::string &source, const std::string &entity) {
    return source + "/" + entity;
  }

  // 从数据库加载历史统计
  void load_from_db(EntityStat &stat);

  // 保存统计到数据库
  void save_to_db(const EntityStat &stat);

  std::mutex mutex_;
  std::unordered_map<std::string, EntityStat> stats_;
  Database *db_ = nullptr;
};

// ============================================================================
// EntityStatsManager 数据库操作实现
// ============================================================================

#include "db.hpp"
#include "entities.hpp"

inline void EntityStatsManager::load_from_db(EntityStat &stat) {
  if (!db_)
    return;

  try {
    std::string sql =
        "SELECT total_requests, success_requests, total_rows_synced, total_api_time_ms, success_rate "
        "FROM entity_stats_meta "
        "WHERE source = " +
        entities::escape_sql(stat.source) +
        " AND entity = " + entities::escape_sql(stat.entity);
    auto result = db_->query_json(sql);

    if (!result.empty()) {
      auto &row = result[0];
      stat.total_requests = row["total_requests"].get<int64_t>();
      stat.success_requests = row["success_requests"].get<int64_t>();
      stat.total_rows_synced = row["total_rows_synced"].get<int64_t>();
      stat.total_api_time_ms = row["total_api_time_ms"].get<int64_t>();
      stat.success_rate = row["success_rate"].get<double>();
    }
  } catch (...) {
    // 忽略错误
  }
}

inline void EntityStatsManager::save_to_db(const EntityStat &stat) {
  if (!db_)
    return;

  try {
    std::string sql =
        "INSERT OR REPLACE INTO entity_stats_meta "
        "(source, entity, total_requests, success_requests, total_rows_synced, total_api_time_ms, success_rate, updated_at) "
        "VALUES (" +
        entities::escape_sql(stat.source) + ", " +
        entities::escape_sql(stat.entity) + ", " +
        std::to_string(stat.total_requests) + ", " +
        std::to_string(stat.success_requests) + ", " +
        std::to_string(stat.total_rows_synced) + ", " +
        std::to_string(stat.total_api_time_ms) + ", " +
        std::to_string(stat.success_rate) + ", CURRENT_TIMESTAMP)";
    db_->execute(sql);
  } catch (...) {
    // 忽略错误
  }
}
