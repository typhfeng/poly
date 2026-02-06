#pragma once

// ============================================================================
// 简单 HTTP 服务器，提供查询 API
// ============================================================================

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <nlohmann/json.hpp>

#include "db.hpp"
#include "entity_stats.hpp"
#include "rebuilder.hpp"

namespace fs = std::filesystem;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using json = nlohmann::json;

// ============================================================================
// HTTP Session
// ============================================================================
class HttpSession : public std::enable_shared_from_this<HttpSession> {
public:
  HttpSession(tcp::socket socket, Database &db, rebuilder::Rebuilder &rebuilder)
      : socket_(std::move(socket)), db_(db), rebuilder_(rebuilder) {}

  void run() {
    do_read();
  }

private:
  void do_read() {
    req_ = {};
    http::async_read(socket_, buffer_, req_,
                     [self = shared_from_this()](beast::error_code ec, std::size_t) {
                       if (ec)
                         return;
                       self->handle_request();
                     });
  }

  void handle_request() {
    res_ = {};
    res_.version(req_.version());
    res_.keep_alive(req_.keep_alive());

    // CORS 头
    res_.set(http::field::access_control_allow_origin, "*");
    res_.set(http::field::access_control_allow_methods, "GET, POST, OPTIONS");
    res_.set(http::field::access_control_allow_headers, "Content-Type");

    // OPTIONS 预检请求
    if (req_.method() == http::verb::options) {
      res_.result(http::status::ok);
      return do_write();
    }

    std::string target(req_.target());

    try {
      // 路由
      if (target.starts_with("/api/sql")) {
        handle_sql();
      } else if (target.starts_with("/api/indexer-fails")) {
        handle_indexer_fails();
      } else if (target.starts_with("/api/entity-latest")) {
        handle_entity_latest();
      } else if (target.starts_with("/api/entity-stats")) {
        handle_entity_stats();
      } else if (target.starts_with("/api/stats")) {
        handle_stats();
      } else if (target.starts_with("/api/sync")) {
        handle_sync_state();
      } else if (target.starts_with("/api/rebuild-status")) {
        handle_rebuild_status();
      } else if (target.starts_with("/api/rebuild-all")) {
        handle_rebuild_all();
      } else if (target.starts_with("/api/rebuild")) {
        handle_rebuild_user();
      } else if (target.starts_with("/api/export-raw")) {
        handle_export_raw();
      } else {
        res_.result(http::status::not_found);
        res_.set(http::field::content_type, "application/json");
        res_.body() = R"({"error":"Not found"})";
      }
    } catch (const std::exception &e) {
      res_.result(http::status::internal_server_error);
      res_.set(http::field::content_type, "application/json");
      res_.body() = json{{"error", e.what()}}.dump();
    } catch (...) {
      res_.result(http::status::internal_server_error);
      res_.set(http::field::content_type, "application/json");
      res_.body() = R"({"error":"Unknown error"})";
    }

    res_.prepare_payload();
    do_write();
  }

  void handle_sql() {
    res_.set(http::field::content_type, "application/json");

    std::string query = get_param("q");
    assert(!query.empty() && "Missing query parameter 'q'");

    // SQL 注入防护
    std::string upper = query;
    for (auto &c : upper)
      c = std::toupper(c);
    assert(upper.starts_with("SELECT") && "Only SELECT queries allowed");
    assert(query.find(';') == std::string::npos && "Semicolon not allowed");
    assert(query.find("--") == std::string::npos && "SQL comment not allowed");
    assert(query.find("/*") == std::string::npos && "SQL comment not allowed");
    assert(upper.find("INSERT") == std::string::npos && "INSERT not allowed");
    assert(upper.find("UPDATE") == std::string::npos && "UPDATE not allowed");
    assert(upper.find("DELETE") == std::string::npos && "DELETE not allowed");
    assert(upper.find("DROP") == std::string::npos && "DROP not allowed");
    assert(upper.find("CREATE") == std::string::npos && "CREATE not allowed");
    assert(upper.find("ALTER") == std::string::npos && "ALTER not allowed");
    assert(upper.find("TRUNCATE") == std::string::npos && "TRUNCATE not allowed");

    json result = db_.query_json(query);
    res_.result(http::status::ok);
    res_.body() = result.dump();
  }

  std::string get_param(const char *name) {
    std::string target(req_.target());
    std::string key = std::string(name) + "=";
    auto pos = target.find(key);
    if (pos == std::string::npos)
      return "";
    std::string value = url_decode(target.substr(pos + key.size()));
    auto amp = value.find('&');
    return (amp != std::string::npos) ? value.substr(0, amp) : value;
  }

  void handle_stats() {
    res_.set(http::field::content_type, "application/json");

    json stats = json::object();
    // 从 entities 定义动态获取表名
    auto collect_stats = [&](const entities::EntityDef *const *list, size_t count) {
      for (size_t i = 0; i < count; ++i) {
        const auto *e = list[i];
        assert(e != nullptr);
        // 极限优化：不做 COUNT(*) 扫表，直接复用 puller 维护的内存计数(跨 source 汇总)
        stats[e->table] = EntityStatsManager::instance().get_total_count_for_entity(e->name);
      }
    };
    collect_stats(entities::MAIN_ENTITIES, entities::MAIN_ENTITY_COUNT);
    collect_stats(entities::PNL_ENTITIES, entities::PNL_ENTITY_COUNT);

    res_.result(http::status::ok);
    res_.body() = stats.dump();
  }

  void handle_sync_state() {
    res_.set(http::field::content_type, "application/json");
    json result = db_.query_json(
        "SELECT source, entity, last_id, last_sync_at "
        "FROM sync_state ORDER BY last_sync_at DESC");
    res_.result(http::status::ok);
    res_.body() = result.dump();
  }

  void handle_entity_stats() {
    res_.set(http::field::content_type, "application/json");
    res_.result(http::status::ok);
    res_.body() = EntityStatsManager::instance().get_all_dump();
  }

  void handle_entity_latest() {
    res_.set(http::field::content_type, "application/json");

    std::string entity = get_param("entity");
    assert(!entity.empty() && "Missing query parameter 'entity'");

    const entities::EntityDef *e =
        entities::find_entity(entity.c_str(), entities::MAIN_ENTITIES, entities::MAIN_ENTITY_COUNT);
    if (!e) {
      e = entities::find_entity(entity.c_str(), entities::PNL_ENTITIES, entities::PNL_ENTITY_COUNT);
    }
    assert(e && "Unknown entity");

    json schema = db_.query_json(std::string("PRAGMA table_info('") + e->table + "')");
    json rows = db_.query_json(std::string("SELECT * FROM ") + e->table + " ORDER BY id DESC LIMIT 1");
    json row = rows.empty() ? json(nullptr) : rows[0];

    json result = {
        {"entity", e->name},
        {"table", e->table},
        {"columns", schema},
        {"row", row},
    };

    res_.result(http::status::ok);
    res_.body() = result.dump();
  }

  void handle_indexer_fails() {
    res_.set(http::field::content_type, "application/json");
    std::string source = get_param("source");
    std::string entity = get_param("entity");
    assert(!source.empty() && "Missing query parameter 'source'");
    assert(!entity.empty() && "Missing query parameter 'entity'");

    std::string sql =
        "SELECT indexer, fail_requests "
        "FROM indexer_fail_meta "
        "WHERE source = " +
        entities::escape_sql(source) +
        " AND entity = " + entities::escape_sql(entity) +
        " ORDER BY fail_requests DESC";
    json rows = db_.query_json(sql);
    res_.result(http::status::ok);
    res_.body() = rows.dump();
  }

  void handle_rebuild_user() {
    res_.set(http::field::content_type, "application/json");
    std::string user = get_param("user");
    assert(!user.empty() && "Missing query parameter 'user'");

    auto result = rebuilder_.rebuild_user(user);
    res_.result(http::status::ok);
    res_.body() = rebuilder::Rebuilder::result_to_json(result).dump();
  }

  void handle_rebuild_all() {
    res_.set(http::field::content_type, "application/json");

    // 检查是否已经在运行
    if (rebuilder_.get_progress().running) {
      res_.result(http::status::ok);
      res_.body() = R"({"status":"already_running"})";
      return;
    }

    // 异步执行全量重建
    std::thread([&rebuilder = rebuilder_]() {
      rebuilder.rebuild_all();
    }).detach();

    res_.result(http::status::ok);
    res_.body() = R"({"status":"started"})";
  }

  void handle_rebuild_status() {
    res_.set(http::field::content_type, "application/json");
    auto progress = rebuilder_.get_progress();

    json result = {
        {"running", progress.running},
        {"total_users", progress.total_users},
        {"processed_users", progress.processed_users},
        {"total_events", progress.total_events},
        {"processed_events", progress.processed_events},
        {"error", progress.error}};

    res_.result(http::status::ok);
    res_.body() = result.dump();
  }

  void handle_export_raw() {
    res_.set(http::field::content_type, "application/json");

    std::string limit_str = get_param("limit");
    int limit = limit_str.empty() ? 100 : std::stoi(limit_str);
    if (limit > 1000)
      limit = 1000;

    std::string order_str = get_param("order");
    std::string order_dir = (order_str == "asc") ? "ASC" : "DESC";

    std::string export_dir = fs::current_path().string() + "/data/export";
    fs::create_directories(export_dir);

    json j_results = json::object();
    int ok_count = 0;

    auto parse_columns = [](const char *columns) {
      std::vector<std::string> names;
      std::string cols = columns;
      size_t pos = 0;
      while (pos < cols.size()) {
        size_t comma = cols.find(',', pos);
        std::string col = (comma == std::string::npos)
                              ? cols.substr(pos)
                              : cols.substr(pos, comma - pos);
        size_t b = col.find_first_not_of(" ");
        size_t e = col.find_last_not_of(" ");
        if (b != std::string::npos)
          names.push_back(col.substr(b, e - b + 1));
        pos = (comma == std::string::npos) ? cols.size() : comma + 1;
      }
      return names;
    };

    auto export_entities = [&](const entities::EntityDef *const *list, size_t count) {
      for (size_t i = 0; i < count; ++i) {
        const auto *e = list[i];
        std::string table = e->table;
        std::string sql = "SELECT " + std::string(e->columns) + " FROM " + table +
                          " ORDER BY id " + order_dir + " LIMIT " + std::to_string(limit);

        json rows = db_.query_json(sql);
        auto col_names = parse_columns(e->columns);

        std::string path = export_dir + "/" + table + ".csv";
        std::ofstream ofs(path);
        assert(ofs.is_open());

        // header
        for (size_t k = 0; k < col_names.size(); ++k) {
          if (k > 0)
            ofs << ",";
          ofs << col_names[k];
        }
        ofs << "\n";

        // rows
        for (const auto &row : rows) {
          for (size_t k = 0; k < col_names.size(); ++k) {
            if (k > 0)
              ofs << ",";
            if (!row.contains(col_names[k]) || row[col_names[k]].is_null())
              continue;
            const auto &v = row[col_names[k]];
            if (v.is_string())
              ofs << escape_csv(v.get<std::string>());
            else
              ofs << v.dump();
          }
          ofs << "\n";
        }

        int row_count = static_cast<int>(rows.size());
        j_results[table] = {{"ok", row_count}};
        if (row_count > 0)
          ++ok_count;
      }
    };

    export_entities(entities::MAIN_ENTITIES, entities::MAIN_ENTITY_COUNT);
    export_entities(entities::PNL_ENTITIES, entities::PNL_ENTITY_COUNT);

    res_.result(http::status::ok);
    res_.body() = json{
        {"path", export_dir},
        {"exported_tables", ok_count},
        {"results", j_results}}
                      .dump();
  }

  static std::string escape_csv(const std::string &s) {
    if (s.find(',') == std::string::npos &&
        s.find('"') == std::string::npos &&
        s.find('\n') == std::string::npos)
      return s;
    std::string r = "\"";
    for (char c : s) {
      if (c == '"')
        r += "\"\"";
      else
        r += c;
    }
    r += "\"";
    return r;
  }

  void do_write() {
    http::async_write(socket_, res_,
                      [self = shared_from_this()](beast::error_code ec, std::size_t) {
                        beast::error_code shutdown_ec;
                        [[maybe_unused]] auto ret = self->socket_.shutdown(tcp::socket::shutdown_send, shutdown_ec);
                        // shutdown 失败不是致命错误，显式忽略
                      });
  }

  static std::string url_decode(const std::string &str) {
    std::string result;
    for (size_t i = 0; i < str.size(); ++i) {
      if (str[i] == '%' && i + 2 < str.size()) {
        int hex = std::stoi(str.substr(i + 1, 2), nullptr, 16);
        result += static_cast<char>(hex);
        i += 2;
      } else if (str[i] == '+') {
        result += ' ';
      } else {
        result += str[i];
      }
    }
    return result;
  }

  tcp::socket socket_;
  Database &db_;
  rebuilder::Rebuilder &rebuilder_;
  beast::flat_buffer buffer_;
  http::request<http::string_body> req_;
  http::response<http::string_body> res_;
};

// ============================================================================
// HTTP Server
// ============================================================================
class HttpServer {
public:
  HttpServer(asio::io_context &ioc, Database &db, unsigned short port)
      : ioc_(ioc), acceptor_(ioc, tcp::endpoint(tcp::v4(), port)), db_(db),
        rebuilder_(db.get_duckdb()) {
    std::cout << "[HTTP] 监听端口 " << port << std::endl;
    do_accept();
  }

private:
  void do_accept() {
    acceptor_.async_accept(
        [this](beast::error_code ec, tcp::socket socket) {
          if (!ec) {
            std::make_shared<HttpSession>(std::move(socket), db_, rebuilder_)
                ->run();
          }
          do_accept();
        });
  }

  asio::io_context &ioc_;
  tcp::acceptor acceptor_;
  Database &db_;
  rebuilder::Rebuilder rebuilder_;
};
