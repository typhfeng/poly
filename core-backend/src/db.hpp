#pragma once

#include "entities.hpp"
#include <cassert>
#include <chrono>
#include <duckdb.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

class Database {
public:
  explicit Database(const std::string &path) {
    db_ = std::make_unique<duckdb::DuckDB>(path);
    conn_ = std::make_unique<duckdb::Connection>(*db_);      // 写连接
    read_conn_ = std::make_unique<duckdb::Connection>(*db_); // 读连接
  }

  // 表初始化
  void init_sync_state() {
    execute(entities::SYNC_STATE_DDL);
    execute(entities::ENTITY_STATS_META_DDL);
  }
  void init_entity(const entities::EntityDef *entity) { execute(entity->ddl); }

  // 事务
  void begin_transaction() {
    auto result = conn_->Query("BEGIN TRANSACTION");
    assert(!result->HasError() && "BEGIN TRANSACTION failed");
  }
  void commit() {
    auto result = conn_->Query("COMMIT");
    assert(!result->HasError() && "COMMIT failed");
  }

  // 游标管理
  std::string get_cursor(const std::string &source, const std::string &entity) {
    return get_sync_field<std::string>(source, entity, "last_id", "");
  }

  int64_t get_total_synced(const std::string &source, const std::string &entity) {
    return get_sync_field<int64_t>(source, entity, "total_synced", 0);
  }

  void save_cursor_total(const std::string &source, const std::string &entity,
                         const std::string &last_id, int64_t total_synced) {
    std::string sql =
        "INSERT OR REPLACE INTO sync_state (source, entity, last_id, last_sync_at, total_synced) "
        "VALUES (" +
        entities::escape_sql(source) + ", " +
        entities::escape_sql(entity) + ", " +
        entities::escape_sql(last_id) + ", CURRENT_TIMESTAMP, " +
        std::to_string(total_synced) + ")";
    auto result = conn_->Query(sql);
    assert(!result->HasError() && "save_cursor failed");
  }

private:
  template <typename T>
  T get_sync_field(const std::string &source, const std::string &entity,
                   const char *field, T default_val) {
    std::string sql = "SELECT " + std::string(field) + " FROM sync_state WHERE source = '" +
                      entities::escape_sql_raw(source) + "' AND entity = '" +
                      entities::escape_sql_raw(entity) + "'";
    auto result = conn_->Query(sql);
    if (result->RowCount() == 0)
      return default_val;
    auto value = result->GetValue(0, 0);
    if (value.IsNull())
      return default_val;
    if constexpr (std::is_same_v<T, std::string>)
      return value.ToString();
    else
      return value.GetValue<T>();
  }

public:
  // 批量操作
  void batch_insert(const std::string &table, const std::string &columns,
                    const std::vector<std::string> &values_list) {
    if (values_list.empty())
      return;
    std::string sql = "INSERT OR REPLACE INTO " + table + " (" + columns + ") VALUES ";
    for (size_t i = 0; i < values_list.size(); ++i) {
      if (i > 0)
        sql += ", ";
      sql += "(" + values_list[i] + ")";
    }
    auto result = conn_->Query(sql);
    assert(!result->HasError() && "batch_insert failed");
  }

  // sync_state 按时间节流，允许丢最近几秒
  void atomic_insert_and_save_cursor(
      const std::string &table, const std::string &columns,
      const std::vector<std::string> &values_list,
      const std::string &source, const std::string &entity, const std::string &cursor,
      bool force_save_cursor = false) {
    auto &m = cursor_meta_[source + "/" + entity];
    if (!m.inited) {
      m.total_synced = get_total_synced(source, entity);
      m.inited = true;
      m.last_save = std::chrono::steady_clock::now();
    }

    m.total_synced += static_cast<int64_t>(values_list.size());
    if (!cursor.empty())
      m.last_cursor = cursor;

    begin_transaction();
    batch_insert(table, columns, values_list);

    auto now = std::chrono::steady_clock::now();
    if (force_save_cursor || (now - m.last_save) >= kCursorPersistInterval) {
      save_cursor_total(source, entity, m.last_cursor, m.total_synced);
      m.last_save = now;
    }
    commit();
  }

  void execute(const std::string &sql) {
    auto result = conn_->Query(sql);
    assert(!result->HasError() && "execute failed");
  }

  // 只读查询
  int64_t get_table_count(const std::string &table) {
    auto result = read_conn_->Query("SELECT COUNT(*) FROM " + table);
    if (result->HasError() || result->RowCount() == 0)
      return 0;
    return result->GetValue(0, 0).GetValue<int64_t>();
  }

  json query_json(const std::string &sql) {
    auto result = read_conn_->Query(sql);
    if (result->HasError()) {
      throw std::runtime_error(result->GetError());
    }

    json rows = json::array();
    auto &types = result->types;
    auto names = result->names;

    for (size_t row = 0; row < result->RowCount(); ++row) {
      json obj = json::object();
      for (size_t col = 0; col < result->ColumnCount(); ++col) {
        auto value = result->GetValue(col, row);
        if (value.IsNull()) {
          obj[names[col]] = nullptr;
        } else {
          // 根据类型转换
          switch (types[col].id()) {
          case duckdb::LogicalTypeId::BOOLEAN:
            obj[names[col]] = value.GetValue<bool>();
            break;
          case duckdb::LogicalTypeId::TINYINT:
          case duckdb::LogicalTypeId::SMALLINT:
          case duckdb::LogicalTypeId::INTEGER:
            obj[names[col]] = value.GetValue<int32_t>();
            break;
          case duckdb::LogicalTypeId::BIGINT:
            obj[names[col]] = value.GetValue<int64_t>();
            break;
          case duckdb::LogicalTypeId::FLOAT:
          case duckdb::LogicalTypeId::DOUBLE:
            obj[names[col]] = value.GetValue<double>();
            break;
          default:
            obj[names[col]] = value.ToString();
            break;
          }
        }
      }
      rows.push_back(std::move(obj));
    }

    return rows;
  }

private:
  std::unique_ptr<duckdb::DuckDB> db_;
  std::unique_ptr<duckdb::Connection> conn_;      // 写连接
  std::unique_ptr<duckdb::Connection> read_conn_; // 读连接

  struct CursorMeta {
    bool inited = false;
    int64_t total_synced = 0;
    std::string last_cursor;
    std::chrono::steady_clock::time_point last_save{};
  };
  std::unordered_map<std::string, CursorMeta> cursor_meta_;
  static constexpr auto kCursorPersistInterval = std::chrono::seconds(5);
};
