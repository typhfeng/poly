#pragma once

// ============================================================================
// Entity 定义
// 每个 entity 包含：GraphQL 字段、DDL、转换函数
// ============================================================================

#include <cassert>
#include <cctype>
#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

namespace entities {

// ============================================================================
// 工具函数
// ============================================================================

// SQL 字符串转义(不带引号)
inline std::string escape_sql_raw(const std::string &s) {
  std::string r;
  r.reserve(s.size());
  for (char c : s) {
    if (c == '\'')
      r += "''";
    else
      r += c;
  }
  return r;
}

// SQL 字符串转义(带引号)
inline std::string escape_sql(const std::string &s) {
  return "'" + escape_sql_raw(s) + "'";
}

// JSON 提取：字符串
inline std::string json_str(const json &j, const char *key) {
  if (!j.contains(key) || j[key].is_null())
    return "NULL";
  if (j[key].is_string())
    return escape_sql(j[key].get<std::string>());
  return escape_sql(j[key].dump());
}

// JSON 提取：整数
inline std::string json_int(const json &j, const char *key) {
  if (!j.contains(key) || j[key].is_null())
    return "NULL";
  if (j[key].is_number())
    return std::to_string(j[key].get<int64_t>());
  if (j[key].is_string())
    return j[key].get<std::string>();
  return "NULL";
}

// JSON 提取：布尔
inline std::string json_bool(const json &j, const char *key) {
  if (!j.contains(key) || j[key].is_null())
    return "NULL";
  if (j[key].is_boolean())
    return j[key].get<bool>() ? "TRUE" : "FALSE";
  return "NULL";
}

// JSON 提取：浮点
inline std::string json_decimal(const json &j, const char *key) {
  if (!j.contains(key) || j[key].is_null())
    return "NULL";
  if (j[key].is_number())
    return std::to_string(j[key].get<double>());
  if (j[key].is_string())
    return j[key].get<std::string>();
  return "NULL";
}

// JSON 提取：关系字段（兼容 {id:"xxx"} 和直接字符串两种格式）
inline std::string json_ref(const json &j, const char *key) {
  if (!j.contains(key) || j[key].is_null())
    return "NULL";
  if (j[key].is_object() && j[key].contains("id")) {
    return escape_sql(j[key]["id"].get<std::string>());
  }
  if (j[key].is_string()) {
    return escape_sql(j[key].get<std::string>());
  }
  return "NULL";
}

// JSON 提取：数组(序列化为 JSON 字符串)
inline std::string json_array(const json &j, const char *key) {
  if (!j.contains(key) || j[key].is_null())
    return "NULL";
  return escape_sql(j[key].dump());
}

// ============================================================================
// 基础设施：同步状态表
// ============================================================================

inline const char *SYNC_STATE_DDL = R"(
CREATE TABLE IF NOT EXISTS sync_state (
    source VARCHAR NOT NULL,
    entity VARCHAR NOT NULL,
    last_id VARCHAR,
    last_sync_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (source, entity)
))";

// Entity Stats Meta 表（存储历史统计信息）
inline const char *ENTITY_STATS_META_DDL = R"(
CREATE TABLE IF NOT EXISTS entity_stats_meta (
    source VARCHAR NOT NULL,
    entity VARCHAR NOT NULL,
    total_requests BIGINT DEFAULT 0,
    success_requests BIGINT DEFAULT 0,
    fail_network BIGINT DEFAULT 0,
    fail_json BIGINT DEFAULT 0,
    fail_graphql BIGINT DEFAULT 0,
    fail_format BIGINT DEFAULT 0,
    total_rows_synced BIGINT DEFAULT 0,
    total_api_time_ms BIGINT DEFAULT 0,
    success_rate DOUBLE DEFAULT 100.0,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (source, entity)
))";

// indexer 维度失败计数（只有失败能归因到具体 indexer）
inline const char *INDEXER_FAIL_META_DDL = R"(
CREATE TABLE IF NOT EXISTS indexer_fail_meta (
    source VARCHAR NOT NULL,
    entity VARCHAR NOT NULL,
    indexer VARCHAR NOT NULL,
    fail_requests BIGINT DEFAULT 0,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (source, entity, indexer)
))";

// ============================================================================
// Entity 定义结构
// ============================================================================

struct EntityDef {
  const char *name;                       // Entity 名称(GraphQL 单数)
  const char *plural;                     // GraphQL 复数形式
  const char *table;                      // 数据库表名
  const char *fields;                     // GraphQL 查询字段
  const char *ddl;                        // CREATE TABLE 语句
  const char *columns;                    // INSERT 列名
  std::string (*to_values)(const json &); // JSON 转 SQL values
};

// ============================================================================
// 估算：单条记录的“结构体大小”(字节)
// 用于 WebUI 展示：row_size_bytes * count => db_size_mb
// 说明：DuckDB 实际存储为变长/压缩，此处只做稳定的估算展示。
// ============================================================================
inline int64_t estimate_row_size_bytes(const EntityDef *e) {
  assert(e != nullptr);
  std::string ddl = e->ddl ? std::string(e->ddl) : std::string();
  assert(!ddl.empty());

  auto lp = ddl.find('(');
  auto rp = ddl.find(')', lp == std::string::npos ? 0 : lp + 1);
  assert(lp != std::string::npos && rp != std::string::npos && rp > lp);

  std::string cols = ddl.substr(lp + 1, rp - lp - 1);

  auto trim = [](std::string s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
      ++b;
    size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n' || s[e - 1] == ','))
      --e;
    return s.substr(b, e - b);
  };

  auto upper = [](std::string s) {
    for (auto &c : s)
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
  };

  auto type_token = [&](const std::string &line, std::string *out_col) -> std::string {
    // col_name TYPE ...
    size_t sp1 = line.find_first_of(" \t");
    if (sp1 == std::string::npos)
      return "";
    std::string col = trim(line.substr(0, sp1));
    size_t sp2 = line.find_first_not_of(" \t", sp1);
    if (sp2 == std::string::npos)
      return "";
    size_t sp3 = line.find_first_of(" \t", sp2);
    std::string type = (sp3 == std::string::npos) ? line.substr(sp2) : line.substr(sp2, sp3 - sp2);
    type = upper(trim(type));
    // 去掉 VARCHAR(…) / DECIMAL(…)
    auto p = type.find('(');
    if (p != std::string::npos)
      type = type.substr(0, p);
    if (out_col)
      *out_col = upper(col);
    return type;
  };

  auto varchar_guess = [&](const std::string &col_upper) -> int64_t {
    // 纯展示估算：做成小而稳定的规则，避免 entity 级 if/else
    if (col_upper == "ID" || col_upper.ends_with("_ID"))
      return 66; // 常见：hex/hash/id
    if (col_upper.find("HASH") != std::string::npos)
      return 66;
    if (col_upper.find("ADDR") != std::string::npos || col_upper.find("ADDRESS") != std::string::npos)
      return 42;
    return 32;
  };

  auto fixed_size = [&](const std::string &type_upper, const std::string &col_upper) -> int64_t {
    // 常见 duckdb 类型的展示尺寸（不是物理存储）
    struct Pair {
      const char *t;
      int64_t sz;
    };
    static constexpr Pair kFixed[] = {
        {"INT", 4},
        {"INTEGER", 4},
        {"BIGINT", 8},
        {"DOUBLE", 8},
        {"FLOAT", 8},
        {"BOOLEAN", 1},
        {"BOOL", 1},
        {"TIMESTAMP", 8},
    };
    for (const auto &p : kFixed) {
      if (type_upper == p.t)
        return p.sz;
    }
    if (type_upper == "VARCHAR" || type_upper == "TEXT" || type_upper == "STRING")
      return varchar_guess(col_upper);
    // 兜底：给一个中等值，避免 0
    return 16;
  };

  int64_t total = 8; // 轻微固定开销
  size_t pos = 0;
  while (pos < cols.size()) {
    size_t nl = cols.find('\n', pos);
    std::string line = (nl == std::string::npos) ? cols.substr(pos) : cols.substr(pos, nl - pos);
    pos = (nl == std::string::npos) ? cols.size() : nl + 1;

    line = trim(line);
    if (line.empty())
      continue;

    std::string u = upper(line);
    // 跳过约束/非列定义
    if (u.starts_with("PRIMARY KEY") || u.starts_with("UNIQUE") || u.starts_with("CONSTRAINT"))
      continue;

    std::string col_upper;
    std::string type = type_token(line, &col_upper);
    if (type.empty())
      continue;
    total += fixed_size(type, col_upper);
  }

  if (total < 16)
    total = 16;
  return total;
}

// ============================================================================
// Main Subgraph Entities
// ============================================================================

// ----------------------------------------------------------------------------
// Condition - 条件
// ----------------------------------------------------------------------------
inline std::string condition_to_values(const json &j) {
  return json_str(j, "id") + "," +
         json_str(j, "oracle") + "," +
         json_str(j, "questionId") + "," +
         json_int(j, "outcomeSlotCount") + "," +
         json_int(j, "resolutionTimestamp") + "," +
         json_array(j, "payoutNumerators") + "," +
         json_int(j, "payoutDenominator") + "," +
         json_str(j, "resolutionHash");
}

inline const EntityDef Condition = {
    .name = "Condition",
    .plural = "conditions",
    .table = "condition",
    .fields = "id oracle questionId outcomeSlotCount resolutionTimestamp payoutNumerators payoutDenominator resolutionHash",
    .ddl = R"(CREATE TABLE IF NOT EXISTS condition (
        id VARCHAR PRIMARY KEY,
        oracle VARCHAR,
        question_id VARCHAR,
        outcome_slot_count INT,
        resolution_timestamp BIGINT,
        payout_numerators VARCHAR,
        payout_denominator VARCHAR,
        resolution_hash VARCHAR
    ))",
    .columns = "id, oracle, question_id, outcome_slot_count, resolution_timestamp, payout_numerators, payout_denominator, resolution_hash",
    .to_values = condition_to_values};

// ----------------------------------------------------------------------------
// Merge - 合并
// ----------------------------------------------------------------------------
inline std::string merge_to_values(const json &j) {
  return json_str(j, "id") + "," +
         json_int(j, "timestamp") + "," +
         json_ref(j, "stakeholder") + "," +
         json_ref(j, "collateralToken") + "," +
         json_str(j, "parentCollectionId") + "," +
         json_ref(j, "condition") + "," +
         json_array(j, "partition") + "," +
         json_int(j, "amount");
}

inline const EntityDef Merge = {
    .name = "Merge",
    .plural = "merges",
    .table = "merge",
    .fields = "id timestamp stakeholder collateralToken parentCollectionId condition partition amount",
    .ddl = R"(CREATE TABLE IF NOT EXISTS merge (
        id VARCHAR PRIMARY KEY,
        timestamp BIGINT,
        stakeholder VARCHAR,
        collateral_token VARCHAR,
        parent_collection_id VARCHAR,
        condition_id VARCHAR,
        partition VARCHAR,
        amount VARCHAR
    );
    CREATE INDEX IF NOT EXISTS idx_merge_ts ON merge(timestamp);
    CREATE INDEX IF NOT EXISTS idx_merge_user ON merge(stakeholder))",
    .columns = "id, timestamp, stakeholder, collateral_token, parent_collection_id, condition_id, partition, amount",
    .to_values = merge_to_values};

// ----------------------------------------------------------------------------
// Redemption - 赎回
// ----------------------------------------------------------------------------
inline std::string redemption_to_values(const json &j) {
  return json_str(j, "id") + "," +
         json_int(j, "timestamp") + "," +
         json_ref(j, "redeemer") + "," +
         json_ref(j, "collateralToken") + "," +
         json_str(j, "parentCollectionId") + "," +
         json_ref(j, "condition") + "," +
         json_array(j, "indexSets") + "," +
         json_int(j, "payout");
}

inline const EntityDef Redemption = {
    .name = "Redemption",
    .plural = "redemptions",
    .table = "redemption",
    .fields = "id timestamp redeemer collateralToken parentCollectionId condition indexSets payout",
    .ddl = R"(CREATE TABLE IF NOT EXISTS redemption (
        id VARCHAR PRIMARY KEY,
        timestamp BIGINT,
        redeemer VARCHAR,
        collateral_token VARCHAR,
        parent_collection_id VARCHAR,
        condition_id VARCHAR,
        index_sets VARCHAR,
        payout VARCHAR
    );
    CREATE INDEX IF NOT EXISTS idx_redemption_ts ON redemption(timestamp);
    CREATE INDEX IF NOT EXISTS idx_redemption_user ON redemption(redeemer))",
    .columns = "id, timestamp, redeemer, collateral_token, parent_collection_id, condition_id, index_sets, payout",
    .to_values = redemption_to_values};

// ----------------------------------------------------------------------------
// Account - 账户
// ----------------------------------------------------------------------------
inline std::string account_to_values(const json &j) {
  return json_str(j, "id") + "," +
         json_int(j, "creationTimestamp") + "," +
         json_int(j, "lastSeenTimestamp") + "," +
         json_int(j, "collateralVolume") + "," +
         json_int(j, "numTrades") + "," +
         json_decimal(j, "scaledCollateralVolume") + "," +
         json_int(j, "lastTradedTimestamp") + "," +
         json_int(j, "profit") + "," +
         json_decimal(j, "scaledProfit");
}

inline const EntityDef Account = {
    .name = "Account",
    .plural = "accounts",
    .table = "account",
    .fields = "id creationTimestamp lastSeenTimestamp collateralVolume numTrades scaledCollateralVolume lastTradedTimestamp profit scaledProfit",
    .ddl = R"(CREATE TABLE IF NOT EXISTS account (
        id VARCHAR PRIMARY KEY,
        creation_timestamp BIGINT,
        last_seen_timestamp BIGINT,
        collateral_volume VARCHAR,
        num_trades VARCHAR,
        scaled_collateral_volume DOUBLE,
        last_traded_timestamp BIGINT,
        profit VARCHAR,
        scaled_profit DOUBLE
    );
    CREATE INDEX IF NOT EXISTS idx_account_profit ON account(profit))",
    .columns = "id, creation_timestamp, last_seen_timestamp, collateral_volume, num_trades, scaled_collateral_volume, last_traded_timestamp, profit, scaled_profit",
    .to_values = account_to_values};

// ----------------------------------------------------------------------------
// EnrichedOrderFilled - 订单成交
// ----------------------------------------------------------------------------
inline std::string enriched_order_filled_to_values(const json &j) {
  return json_str(j, "id") + "," +
         json_str(j, "transactionHash") + "," +
         json_int(j, "timestamp") + "," +
         json_ref(j, "maker") + "," +
         json_ref(j, "taker") + "," +
         json_str(j, "orderHash") + "," +
         json_ref(j, "market") + "," +
         json_str(j, "side") + "," +
         json_int(j, "size") + "," +
         json_decimal(j, "price");
}

inline const EntityDef EnrichedOrderFilled = {
    .name = "EnrichedOrderFilled",
    .plural = "enrichedOrderFilleds",
    .table = "enriched_order_filled",
    .fields = "id transactionHash timestamp maker taker orderHash market side size price",
    .ddl = R"(CREATE TABLE IF NOT EXISTS enriched_order_filled (
        id VARCHAR PRIMARY KEY,
        transaction_hash VARCHAR,
        timestamp BIGINT,
        maker VARCHAR,
        taker VARCHAR,
        order_hash VARCHAR,
        market_id VARCHAR,
        side VARCHAR,
        size VARCHAR,
        price DOUBLE
    );
    CREATE INDEX IF NOT EXISTS idx_eof_ts ON enriched_order_filled(timestamp);
    CREATE INDEX IF NOT EXISTS idx_eof_maker ON enriched_order_filled(maker);
    CREATE INDEX IF NOT EXISTS idx_eof_market ON enriched_order_filled(market_id))",
    .columns = "id, transaction_hash, timestamp, maker, taker, order_hash, market_id, side, size, price",
    .to_values = enriched_order_filled_to_values};

// ----------------------------------------------------------------------------
// MarketData - 市场数据
// ----------------------------------------------------------------------------
inline std::string market_data_to_values(const json &j) {
  return json_str(j, "id") + "," +
         json_ref(j, "condition") + "," +
         json_int(j, "outcomeIndex") + "," +
         json_ref(j, "fpmm") + "," +
         json_decimal(j, "priceOrderbook");
}

inline const EntityDef MarketData = {
    .name = "MarketData",
    .plural = "marketDatas",
    .table = "market_data",
    .fields = "id condition outcomeIndex fpmm priceOrderbook",
    .ddl = R"(CREATE TABLE IF NOT EXISTS market_data (
        id VARCHAR PRIMARY KEY,
        condition_id VARCHAR,
        outcome_index INT,
        fpmm_id VARCHAR,
        price_orderbook DOUBLE
    );
    CREATE INDEX IF NOT EXISTS idx_md_cond ON market_data(condition_id))",
    .columns = "id, condition_id, outcome_index, fpmm_id, price_orderbook",
    .to_values = market_data_to_values};

// ----------------------------------------------------------------------------
// Global - 全局统计
// ----------------------------------------------------------------------------
inline std::string global_to_values(const json &j) {
  return json_str(j, "id") + "," +
         json_int(j, "numConditions") + "," +
         json_int(j, "numOpenConditions") + "," +
         json_int(j, "numClosedConditions") + "," +
         json_int(j, "numTraders") + "," +
         json_int(j, "tradesQuantity") + "," +
         json_int(j, "buysQuantity") + "," +
         json_int(j, "sellsQuantity") + "," +
         json_int(j, "collateralVolume") + "," +
         json_decimal(j, "scaledCollateralVolume") + "," +
         json_int(j, "collateralFees") + "," +
         json_decimal(j, "scaledCollateralFees") + "," +
         json_int(j, "collateralBuyVolume") + "," +
         json_decimal(j, "scaledCollateralBuyVolume") + "," +
         json_int(j, "collateralSellVolume") + "," +
         json_decimal(j, "scaledCollateralSellVolume");
}

inline const EntityDef Global = {
    .name = "Global",
    .plural = "globals",
    .table = "global",
    .fields = "id numConditions numOpenConditions numClosedConditions numTraders tradesQuantity buysQuantity sellsQuantity collateralVolume scaledCollateralVolume collateralFees scaledCollateralFees collateralBuyVolume scaledCollateralBuyVolume collateralSellVolume scaledCollateralSellVolume",
    .ddl = R"(CREATE TABLE IF NOT EXISTS global (
        id VARCHAR PRIMARY KEY,
        num_conditions INT,
        num_open_conditions INT,
        num_closed_conditions INT,
        num_traders VARCHAR,
        trades_quantity VARCHAR,
        buys_quantity VARCHAR,
        sells_quantity VARCHAR,
        collateral_volume VARCHAR,
        scaled_collateral_volume DOUBLE,
        collateral_fees VARCHAR,
        scaled_collateral_fees DOUBLE,
        collateral_buy_volume VARCHAR,
        scaled_collateral_buy_volume DOUBLE,
        collateral_sell_volume VARCHAR,
        scaled_collateral_sell_volume DOUBLE
    ))",
    .columns = "id, num_conditions, num_open_conditions, num_closed_conditions, num_traders, trades_quantity, buys_quantity, sells_quantity, collateral_volume, scaled_collateral_volume, collateral_fees, scaled_collateral_fees, collateral_buy_volume, scaled_collateral_buy_volume, collateral_sell_volume, scaled_collateral_sell_volume",
    .to_values = global_to_values};

// ============================================================================
// PnL Subgraph Entities
// ============================================================================

// ----------------------------------------------------------------------------
// UserPosition - 用户持仓
// ----------------------------------------------------------------------------
inline std::string user_position_to_values(const json &j) {
  return json_str(j, "id") + "," +
         json_str(j, "user") + "," +
         json_int(j, "tokenId") + "," +
         json_int(j, "amount") + "," +
         json_int(j, "avgPrice") + "," +
         json_int(j, "realizedPnl") + "," +
         json_int(j, "totalBought");
}

inline const EntityDef UserPosition = {
    .name = "UserPosition",
    .plural = "userPositions",
    .table = "user_position",
    .fields = "id user tokenId amount avgPrice realizedPnl totalBought",
    .ddl = R"(CREATE TABLE IF NOT EXISTS user_position (
        id VARCHAR PRIMARY KEY,
        user_addr VARCHAR,
        token_id VARCHAR,
        amount VARCHAR,
        avg_price VARCHAR,
        realized_pnl VARCHAR,
        total_bought VARCHAR
    );
    CREATE INDEX IF NOT EXISTS idx_up_user ON user_position(user_addr);
    CREATE INDEX IF NOT EXISTS idx_up_token ON user_position(token_id))",
    .columns = "id, user_addr, token_id, amount, avg_price, realized_pnl, total_bought",
    .to_values = user_position_to_values};

// ----------------------------------------------------------------------------
// PnlCondition - PnL 条件(简化版)
// ----------------------------------------------------------------------------
inline std::string pnl_condition_to_values(const json &j) {
  return json_str(j, "id") + "," +
         json_array(j, "positionIds") + "," +
         json_array(j, "payoutNumerators") + "," +
         json_int(j, "payoutDenominator");
}

inline const EntityDef PnlCondition = {
    .name = "Condition",
    .plural = "conditions",
    .table = "pnl_condition",
    .fields = "id positionIds payoutNumerators payoutDenominator",
    .ddl = R"(CREATE TABLE IF NOT EXISTS pnl_condition (
        id VARCHAR PRIMARY KEY,
        position_ids VARCHAR,
        payout_numerators VARCHAR,
        payout_denominator VARCHAR
    ))",
    .columns = "id, position_ids, payout_numerators, payout_denominator",
    .to_values = pnl_condition_to_values};

// ----------------------------------------------------------------------------
// NegRiskEvent - 负风险事件
// ----------------------------------------------------------------------------
inline std::string neg_risk_event_to_values(const json &j) {
  return json_str(j, "id") + "," +
         json_int(j, "questionCount");
}

inline const EntityDef NegRiskEvent = {
    .name = "NegRiskEvent",
    .plural = "negRiskEvents",
    .table = "neg_risk_event",
    .fields = "id questionCount",
    .ddl = R"(CREATE TABLE IF NOT EXISTS neg_risk_event (
        id VARCHAR PRIMARY KEY,
        question_count INT
    ))",
    .columns = "id, question_count",
    .to_values = neg_risk_event_to_values};

// ----------------------------------------------------------------------------
// PnlFPMM - PnL FPMM(简化版)
// ----------------------------------------------------------------------------
inline std::string pnl_fpmm_to_values(const json &j) {
  return json_str(j, "id") + "," +
         json_str(j, "conditionId");
}

inline const EntityDef PnlFPMM = {
    .name = "FPMM",
    .plural = "fpmms",
    .table = "pnl_fpmm",
    .fields = "id conditionId",
    .ddl = R"(CREATE TABLE IF NOT EXISTS pnl_fpmm (
        id VARCHAR PRIMARY KEY,
        condition_id VARCHAR
    );
    CREATE INDEX IF NOT EXISTS idx_pnl_fpmm_cond ON pnl_fpmm(condition_id))",
    .columns = "id, condition_id",
    .to_values = pnl_fpmm_to_values};

// ============================================================================
// Entity 注册表
// ============================================================================

inline const EntityDef *MAIN_ENTITIES[] = {
    &Condition, &Merge, &Redemption,
    &Account, &EnrichedOrderFilled,
    &MarketData, &Global};
inline constexpr size_t MAIN_ENTITY_COUNT = sizeof(MAIN_ENTITIES) / sizeof(MAIN_ENTITIES[0]);

inline const EntityDef *PNL_ENTITIES[] = {
    &UserPosition, &PnlCondition, &NegRiskEvent, &PnlFPMM};
inline constexpr size_t PNL_ENTITY_COUNT = sizeof(PNL_ENTITIES) / sizeof(PNL_ENTITIES[0]);

// 查找 entity
inline const EntityDef *find_entity(const char *name, const EntityDef *const *list, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (std::string(list[i]->name) == name) {
      return list[i];
    }
  }
  return nullptr;
}

} // namespace entities
