#pragma once

// ============================================================================
// PnL Rebuild Engine — 三阶段全量重建
//
// Phase 1: load_metadata()    — 扫描 condition 表, 构建 token→condition 映射
// Phase 2: collect_events()   — 4次全表扫描, 事件写入 per-user 桶
// Phase 3: replay_all()       — 并行回放, 生成 Snapshot 链, 释放 RawEvent
// ============================================================================

#include "rebuilder_types.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <duckdb.hpp>
#include <future>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

namespace rebuild {

class Engine {
public:
  explicit Engine(duckdb::DuckDB &db) : db_(db) {}

  // ==========================================================================
  // 入口: 全量重建
  // ==========================================================================
  void rebuild_all() {
    bool expected = false;
    assert(running_.compare_exchange_strong(expected, true) && "rebuild already running");

    eof_rows_ = eof_events_ = split_rows_ = split_events_ = 0;
    merge_rows_ = merge_events_ = redemption_rows_ = redemption_events_ = 0;
    phase1_ms_ = phase2_ms_ = phase3_ms_ = 0;

    phase_ = 1;
    auto t0 = clock::now();
    load_metadata();
    phase1_ms_ = ms_since(t0);

    auto t1 = clock::now();
    collect_events();
    phase2_ms_ = ms_since(t1);

    phase_ = 6;
    auto t2 = clock::now();
    replay_all();
    phase3_ms_ = ms_since(t2);

    phase_ = 7;
    running_ = false;

    std::cout << "[rebuild] done: "
              << users_.size() << " users, "
              << total_events_ << " events | "
              << "p1=" << (int)phase1_ms_ << "ms "
              << "p2=" << (int)phase2_ms_ << "ms "
              << "p3=" << (int)phase3_ms_ << "ms "
              << "total=" << (int)(phase1_ms_ + phase2_ms_ + phase3_ms_) << "ms" << std::endl;
  }

  // ==========================================================================
  // Accessors
  // ==========================================================================
  const std::vector<std::string> &users() const { return users_; }
  const std::vector<UserState> &user_states() const { return user_states_; }
  const std::vector<ConditionInfo> &conditions() const { return conditions_; }
  const std::vector<std::string> &condition_ids() const { return cond_ids_; }

  const UserState *find_user(const std::string &user_id) const {
    auto it = user_map_.find(user_id);
    if (it == user_map_.end())
      return nullptr;
    return &user_states_[it->second];
  }

  RebuildProgress get_progress() const {
    RebuildProgress p;
    p.phase = phase_.load(std::memory_order_relaxed);
    p.total_conditions = (int64_t)conditions_.size();
    p.total_tokens = (int64_t)token_map_.size();
    p.total_events = total_events_;
    p.total_users = (int64_t)users_.size();
    p.processed_users = processed_users_.load(std::memory_order_relaxed);
    p.running = running_.load(std::memory_order_relaxed);
    p.phase1_ms = phase1_ms_;
    p.phase2_ms = phase2_ms_;
    p.phase3_ms = phase3_ms_;
    p.eof_rows = eof_rows_;
    p.eof_events = eof_events_;
    p.split_rows = split_rows_;
    p.split_events = split_events_;
    p.merge_rows = merge_rows_;
    p.merge_events = merge_events_;
    p.redemption_rows = redemption_rows_;
    p.redemption_events = redemption_events_;
    return p;
  }

private:
  using clock = std::chrono::high_resolution_clock;
  static double ms_since(clock::time_point t) {
    return std::chrono::duration<double, std::milli>(clock::now() - t).count();
  }

  // ==========================================================================
  // Phase 1: Metadata — condition 表 → cond_map_ + token_map_
  // ==========================================================================
  void load_metadata() {
    conditions_.clear();
    cond_ids_.clear();
    cond_map_.clear();
    token_map_.clear();

    auto conn = std::make_unique<duckdb::Connection>(db_);
    auto result = conn->Query(
        "SELECT id, outcomeSlotCount, positionIds, payoutNumerators, payoutDenominator "
        "FROM condition");
    assert(!result->HasError());

    for (size_t row = 0; row < result->RowCount(); ++row) {
      std::string cond_id = result->GetValue(0, row).ToString();
      int32_t outcome_count = result->GetValue(1, row).GetValue<int32_t>();
      assert(outcome_count > 0 && outcome_count <= MAX_OUTCOMES);

      uint32_t idx = (uint32_t)conditions_.size();
      ConditionInfo info;
      info.outcome_count = (uint8_t)outcome_count;

      // positionIds: JSON array → token_map_ entries
      auto pos_val = result->GetValue(2, row);
      if (!pos_val.IsNull()) {
        std::string s = pos_val.ToString();
        if (!s.empty()) {
          auto arr = json::parse(s);
          for (uint8_t i = 0; i < (uint8_t)arr.size(); ++i) {
            token_map_[arr[i].get<std::string>()] = {idx, i};
          }
        }
      }

      // payoutNumerators: JSON array of ints/strings
      auto pn_val = result->GetValue(3, row);
      if (!pn_val.IsNull()) {
        std::string s = pn_val.ToString();
        if (!s.empty() && s != "NULL") {
          auto arr = json::parse(s);
          for (const auto &n : arr) {
            info.payout_numerators.push_back(
                n.is_string() ? std::stoll(n.get<std::string>()) : n.get<int64_t>());
          }
        }
      }

      // payoutDenominator
      auto pd_val = result->GetValue(4, row);
      if (!pd_val.IsNull()) {
        std::string s = pd_val.ToString();
        if (!s.empty())
          info.payout_denominator = std::stoll(s);
      }

      conditions_.push_back(std::move(info));
      cond_ids_.push_back(cond_id);
      cond_map_[cond_id] = idx;
    }

    std::cout << "[rebuild] p1: " << conditions_.size() << " conditions, "
              << token_map_.size() << " tokens" << std::endl;
  }

  // ==========================================================================
  // Phase 2: Event collection — 4 table scans → per-user RawEvent vectors
  // ==========================================================================
  uint32_t intern_user(const std::string &id) {
    auto [it, ok] = user_map_.emplace(id, (uint32_t)users_.size());
    if (ok) {
      users_.push_back(id);
      user_events_.emplace_back();
    }
    return it->second;
  }

  void collect_events() {
    users_.clear();
    user_map_.clear();
    user_events_.clear();
    total_events_ = 0;

    auto conn = std::make_unique<duckdb::Connection>(db_);
    phase_ = 2;
    eof_events_ = scan_eof(*conn);
    total_events_ += eof_events_;
    phase_ = 3;
    split_events_ = scan_split(*conn);
    total_events_ += split_events_;
    phase_ = 4;
    merge_events_ = scan_merge(*conn);
    total_events_ += merge_events_;
    phase_ = 5;
    redemption_events_ = scan_redemption(*conn);
    total_events_ += redemption_events_;

    std::cout << "[rebuild] p2: " << total_events_ << " events → "
              << users_.size() << " users" << std::endl;
  }

  // --- enriched_order_filled: 每行 → maker event + taker event (opposite side)
  int64_t scan_eof(duckdb::Connection &conn) {
    auto r = conn.Query(
        "SELECT timestamp, maker, taker, market, side, size, price "
        "FROM enriched_order_filled ORDER BY timestamp");
    assert(!r->HasError());
    eof_rows_ = (int64_t)r->RowCount();

    int64_t n = 0;
    for (size_t i = 0; i < r->RowCount(); ++i) {
      int64_t ts = r->GetValue(0, i).GetValue<int64_t>();
      std::string maker = r->GetValue(1, i).ToString();
      std::string taker = r->GetValue(2, i).ToString();
      std::string market = r->GetValue(3, i).ToString();
      std::string side = r->GetValue(4, i).ToString();
      int64_t size = std::stoll(r->GetValue(5, i).ToString());
      int64_t price = (int64_t)(r->GetValue(6, i).GetValue<double>() * 1000000);

      auto tok = token_map_.find(market);
      if (tok == token_map_.end())
        continue; // unknown token

      uint32_t ci = tok->second.first;
      uint8_t ti = tok->second.second;
      bool is_buy = (side == "Buy");

      // maker
      {
        auto ui = intern_user(maker);
        RawEvent e{ts, ci, (uint8_t)(is_buy ? Buy : Sell), ti, 0, size, price};
        user_events_[ui].push_back(e);
      }
      // taker (opposite side)
      {
        auto ui = intern_user(taker);
        RawEvent e{ts, ci, (uint8_t)(is_buy ? Sell : Buy), ti, 0, size, price};
        user_events_[ui].push_back(e);
      }
      n += 2;
    }
    std::cout << "[rebuild]   eof: " << r->RowCount() << " rows → " << n << " events" << std::endl;
    return n;
  }

  // --- split: stakeholder gets all tokens
  int64_t scan_split(duckdb::Connection &conn) {
    auto r = conn.Query(
        "SELECT timestamp, stakeholder, condition, amount "
        "FROM split ORDER BY timestamp");
    assert(!r->HasError());
    split_rows_ = (int64_t)r->RowCount();

    int64_t n = 0;
    for (size_t i = 0; i < r->RowCount(); ++i) {
      int64_t ts = r->GetValue(0, i).GetValue<int64_t>();
      std::string user = r->GetValue(1, i).ToString();
      std::string cond = r->GetValue(2, i).ToString();
      int64_t amt = std::stoll(r->GetValue(3, i).ToString());

      auto ci = cond_map_.find(cond);
      if (ci == cond_map_.end())
        continue;

      auto ui = intern_user(user);
      RawEvent e{ts, ci->second, (uint8_t)Split, 0xFF, 0, amt, 0};
      user_events_[ui].push_back(e);
      ++n;
    }
    std::cout << "[rebuild]   split: " << r->RowCount() << " rows → " << n << " events" << std::endl;
    return n;
  }

  // --- merge: stakeholder destroys all tokens → USDC
  int64_t scan_merge(duckdb::Connection &conn) {
    auto r = conn.Query(
        "SELECT timestamp, stakeholder, condition, amount "
        "FROM merge ORDER BY timestamp");
    assert(!r->HasError());
    merge_rows_ = (int64_t)r->RowCount();

    int64_t n = 0;
    for (size_t i = 0; i < r->RowCount(); ++i) {
      int64_t ts = r->GetValue(0, i).GetValue<int64_t>();
      std::string user = r->GetValue(1, i).ToString();
      std::string cond = r->GetValue(2, i).ToString();
      int64_t amt = std::stoll(r->GetValue(3, i).ToString());

      auto ci = cond_map_.find(cond);
      if (ci == cond_map_.end())
        continue;

      auto ui = intern_user(user);
      RawEvent e{ts, ci->second, (uint8_t)Merge, 0xFF, 0, amt, 0};
      user_events_[ui].push_back(e);
      ++n;
    }
    std::cout << "[rebuild] merge: " << r->RowCount() << " rows → " << n << " events" << std::endl;
    return n;
  }

  // --- redemption: redeemer clears positions → USDC payout
  int64_t scan_redemption(duckdb::Connection &conn) {
    auto r = conn.Query(
        "SELECT timestamp, redeemer, condition, payout "
        "FROM redemption ORDER BY timestamp");
    assert(!r->HasError());
    redemption_rows_ = (int64_t)r->RowCount();

    int64_t n = 0;
    for (size_t i = 0; i < r->RowCount(); ++i) {
      int64_t ts = r->GetValue(0, i).GetValue<int64_t>();
      std::string user = r->GetValue(1, i).ToString();
      std::string cond = r->GetValue(2, i).ToString();
      int64_t payout = std::stoll(r->GetValue(3, i).ToString());

      auto ci = cond_map_.find(cond);
      if (ci == cond_map_.end())
        continue;

      auto ui = intern_user(user);
      RawEvent e{ts, ci->second, (uint8_t)Redemption, 0xFF, 0, payout, 0};
      user_events_[ui].push_back(e);
      ++n;
    }
    std::cout << "[rebuild]   redemption: " << r->RowCount() << " rows → " << n << " events" << std::endl;
    return n;
  }

  // ==========================================================================
  // Phase 3: Parallel replay — sort per-user, build Snapshots, free RawEvents
  // ==========================================================================
  void replay_all() {
    size_t nu = users_.size();
    user_states_.resize(nu);
    processed_users_ = 0;

    int nw = std::min(16u, std::max(1u, std::thread::hardware_concurrency()));
    size_t chunk = (nu + nw - 1) / nw;

    std::vector<std::future<void>> futs;
    for (int w = 0; w < nw; ++w) {
      size_t s = w * chunk, e = std::min(s + chunk, nu);
      if (s >= nu)
        break;
      futs.push_back(std::async(std::launch::async, [this, s, e]() {
        for (size_t u = s; u < e; ++u) {
          replay_user(u);
          processed_users_.fetch_add(1, std::memory_order_relaxed);
        }
      }));
    }
    for (auto &f : futs)
      f.get();

    // Free all raw events
    user_events_.clear();
    user_events_.shrink_to_fit();

    std::cout << "[rebuild] p3: " << nu << " users, " << nw << " workers" << std::endl;
  }

  void replay_user(size_t uid) {
    auto &events = user_events_[uid];

    // Sort by timestamp
    std::sort(events.begin(), events.end(),
              [](const RawEvent &a, const RawEvent &b) {
                return a.timestamp < b.timestamp;
              });

    // Per-condition replay state and snapshots
    std::unordered_map<uint32_t, ReplayState> states;
    std::unordered_map<uint32_t, std::vector<Snapshot>> snaps;

    for (const auto &evt : events) {
      auto &st = states[evt.cond_idx];
      const auto &cond = conditions_[evt.cond_idx];

      apply_event(evt, st, cond);

      // Record snapshot
      Snapshot snap;
      snap.timestamp = evt.timestamp;
      snap.delta = evt.amount;
      snap.price = evt.price;
      std::memcpy(snap.positions, st.positions, sizeof(st.positions));

      // cost_basis = sum(cost[i]) / 1e6 → raw USDC
      int64_t total_cost = 0;
      for (int k = 0; k < cond.outcome_count; ++k)
        total_cost += st.cost[k];
      snap.cost_basis = total_cost / 1000000;

      snap.realized_pnl = st.realized_pnl;
      snap.event_type = evt.type;
      snap.token_idx = evt.token_idx;
      snap.outcome_count = cond.outcome_count;
      std::memset(snap._pad, 0, sizeof(snap._pad));

      snaps[evt.cond_idx].push_back(snap);
    }

    // Build UserState
    auto &us = user_states_[uid];
    us.conditions.clear();
    us.conditions.reserve(snaps.size());
    for (auto &[ci, sv] : snaps) {
      us.conditions.push_back(UserConditionHistory{ci, std::move(sv)});
    }

    // Free raw events for this user
    events.clear();
    events.shrink_to_fit();
  }

  // ==========================================================================
  // Event application logic
  // ==========================================================================
  static void apply_event(const RawEvent &evt, ReplayState &st, const ConditionInfo &cond) {
    switch ((EventType)evt.type) {
    case Buy:
      apply_buy(evt, st);
      break;
    case Sell:
      apply_sell(evt, st);
      break;
    case Split:
      apply_split(evt, st, cond);
      break;
    case Merge:
      apply_merge(evt, st, cond);
      break;
    case Redemption:
      apply_redemption(evt, st, cond);
      break;
    }
  }

  // Buy token[i]: position += amount, cost += amount * price
  static void apply_buy(const RawEvent &evt, ReplayState &st) {
    int i = evt.token_idx;
    assert(i < MAX_OUTCOMES);
    st.cost[i] += evt.amount * evt.price;
    st.positions[i] += evt.amount;
  }

  // Sell token[i]: realize PnL, reduce position proportionally
  static void apply_sell(const RawEvent &evt, ReplayState &st) {
    int i = evt.token_idx;
    assert(i < MAX_OUTCOMES);

    int64_t pos = st.positions[i];
    if (pos > 0 && evt.amount > 0) {
      int64_t sell = std::min(evt.amount, pos);
      int64_t cost_removed = st.cost[i] * sell / pos;
      st.realized_pnl += (sell * evt.price - cost_removed) / 1000000;
      st.cost[i] -= cost_removed;
      st.positions[i] -= sell;
    } else {
      // 无仓位，允许负仓位追踪
      st.positions[i] -= evt.amount;
    }
  }

  // Split: pay amount USDC → get amount of each outcome token
  // Implied price per token = 1e6 / outcome_count
  static void apply_split(const RawEvent &evt, ReplayState &st, const ConditionInfo &cond) {
    int64_t implied_price = 1000000 / cond.outcome_count;
    for (int i = 0; i < cond.outcome_count; ++i) {
      st.cost[i] += evt.amount * implied_price;
      st.positions[i] += evt.amount;
    }
  }

  // Merge: destroy amount of each token → receive amount USDC
  // Implied sell price per token = 1e6 / outcome_count
  static void apply_merge(const RawEvent &evt, ReplayState &st, const ConditionInfo &cond) {
    int64_t implied_price = 1000000 / cond.outcome_count;
    for (int i = 0; i < cond.outcome_count; ++i) {
      int64_t pos = st.positions[i];
      if (pos > 0) {
        int64_t sell = std::min(evt.amount, pos);
        int64_t cost_removed = st.cost[i] * sell / pos;
        st.realized_pnl += (sell * implied_price - cost_removed) / 1000000;
        st.cost[i] -= cost_removed;
        st.positions[i] -= sell;
      } else {
        st.positions[i] -= evt.amount;
      }
    }
  }

  // Redemption: clear all positions at payout price
  static void apply_redemption(const RawEvent &evt, ReplayState &st, const ConditionInfo &cond) {
    if (cond.payout_denominator == 0)
      return; // 未结算

    for (int i = 0; i < cond.outcome_count && i < (int)cond.payout_numerators.size(); ++i) {
      int64_t pos = st.positions[i];
      if (pos <= 0)
        continue;
      int64_t payout_price = cond.payout_numerators[i] * 1000000 / cond.payout_denominator;
      int64_t cost_removed = st.cost[i];
      st.realized_pnl += (pos * payout_price - cost_removed) / 1000000;
      st.cost[i] = 0;
      st.positions[i] = 0;
    }
  }

  // ==========================================================================
  // Data members
  // ==========================================================================
  duckdb::DuckDB &db_;

  // Phase 1
  std::vector<ConditionInfo> conditions_;                                   // cond_idx → info
  std::vector<std::string> cond_ids_;                                       // cond_idx → id
  std::unordered_map<std::string, uint32_t> cond_map_;                      // id → cond_idx
  std::unordered_map<std::string, std::pair<uint32_t, uint8_t>> token_map_; // token_id → (cond_idx, tok_idx)

  // Phase 2 (freed after Phase 3)
  std::vector<std::string> users_;                     // user_idx → id
  std::unordered_map<std::string, uint32_t> user_map_; // id → user_idx
  std::vector<std::vector<RawEvent>> user_events_;     // user_idx → events

  // Phase 3
  std::vector<UserState> user_states_; // user_idx → state

  // Stats
  int64_t total_events_ = 0;
  std::atomic<int64_t> processed_users_{0};
  std::atomic<bool> running_{false};
  std::atomic<int> phase_{0};
  double phase1_ms_ = 0, phase2_ms_ = 0, phase3_ms_ = 0;
  int64_t eof_rows_ = 0, eof_events_ = 0;
  int64_t split_rows_ = 0, split_events_ = 0;
  int64_t merge_rows_ = 0, merge_events_ = 0;
  int64_t redemption_rows_ = 0, redemption_events_ = 0;
};

} // namespace rebuild
