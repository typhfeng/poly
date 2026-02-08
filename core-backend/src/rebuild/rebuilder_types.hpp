#pragma once

#include <cstdint>
#include <vector>

namespace rebuild {

static constexpr int MAX_OUTCOMES = 8;

// ============================================================================
// Event types
// ============================================================================
enum EventType : uint8_t {
  Buy = 0,
  Sell = 1,
  Split = 2,
  Merge = 3,
  Redemption = 4,
};

// ============================================================================
// Phase 1: Condition metadata (per condition)
// ============================================================================
struct ConditionInfo {
  uint8_t outcome_count = 0;
  std::vector<int64_t> payout_numerators; // 结算后非空
  int64_t payout_denominator = 0;
};

// ============================================================================
// Phase 2: Compact event — 32 bytes, DDR aligned
// ============================================================================
struct RawEvent {
  int64_t timestamp; // 8
  uint32_t cond_idx; // 4  哪个 condition
  uint8_t type;      // 1  EventType
  uint8_t token_idx; // 1  哪个 token (Buy/Sell), 0xFF = all
  uint16_t _pad;     // 2
  int64_t amount;    // 8  raw token units (1e6 = 1 token = $1 face)
  int64_t price;     // 8  price * 1e6
};
static_assert(sizeof(RawEvent) == 32);

// ============================================================================
// Phase 3: Snapshot — 112 bytes, DDR contiguous in vector
// ============================================================================
struct Snapshot {
  int64_t timestamp;               // 8
  int64_t delta;                   // 8   event amount (raw token units)
  int64_t price;                   // 8   event price (price * 1e6)
  int64_t positions[MAX_OUTCOMES]; // 64  post-event positions (raw token units)
  int64_t cost_basis;              // 8   sum of cost (raw USDC units)
  int64_t realized_pnl;            // 8   cumulative realized PnL (raw USDC units)
  uint8_t event_type;              // 1
  uint8_t token_idx;               // 1
  uint8_t outcome_count;           // 1
  uint8_t _pad[5];                 // 5
};
static_assert(sizeof(Snapshot) == 112);

// ============================================================================
// Per user-condition: snapshot chain
// ============================================================================
struct UserConditionHistory {
  uint32_t cond_idx;
  std::vector<Snapshot> snapshots; // chronological, contiguous DDR
};

// ============================================================================
// Per user: all conditions
// ============================================================================
struct UserState {
  std::vector<UserConditionHistory> conditions;
};

// ============================================================================
// Replay temp state (per user-condition, discarded after replay)
// ============================================================================
struct ReplayState {
  int64_t positions[MAX_OUTCOMES] = {};
  int64_t cost[MAX_OUTCOMES] = {}; // total cost per token, (amount * price_1e6) units
  int64_t realized_pnl = 0;        // raw USDC units
};

// ============================================================================
// Progress
// ============================================================================
struct RebuildProgress {
  int phase = 0; // 0=idle 1=p1 2=p2_eof 3=p2_split 4=p2_merge 5=p2_redemption 6=p3 7=done
  int64_t total_conditions = 0;
  int64_t total_tokens = 0;
  int64_t total_events = 0;
  int64_t total_users = 0;
  int64_t processed_users = 0;
  bool running = false;
  double phase1_ms = 0;
  double phase2_ms = 0;
  double phase3_ms = 0;
  int64_t eof_rows = 0;
  int64_t eof_events = 0;
  int64_t split_rows = 0;
  int64_t split_events = 0;
  int64_t merge_rows = 0;
  int64_t merge_events = 0;
  int64_t redemption_rows = 0;
  int64_t redemption_events = 0;
};

} // namespace rebuild
