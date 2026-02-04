#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <cmath>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ============================================================================
// 单个 Entity 的实时统计
// ============================================================================
struct EntityStat {
    std::string source;
    std::string entity;
    
    // 记录数（从 DB 初始化，之后累加）
    int64_t count = 0;

    // 估算：单条记录的“结构体大小”(字节)，用于展示 DB 规模
    int64_t row_size_bytes = 0;
    
    // 速度统计（滑动窗口）
    double speed = 0;  // records/s
    int64_t recent_records = 0;
    std::chrono::steady_clock::time_point speed_window_start;
    
    // QoS 统计
    double avg_latency_ms = 0;
    double success_rate = 100.0;
    int64_t total_requests = 0;
    int64_t success_requests = 0;
    int64_t latency_sum_ms = 0;
    
    // 状态
    bool is_syncing = false;
    bool sync_done = false;
    std::chrono::steady_clock::time_point last_update;
};

// ============================================================================
// 全局 Entity Stats 管理器
// ============================================================================
class EntityStatsManager {
public:
    static EntityStatsManager& instance() {
        static EntityStatsManager inst;
        return inst;
    }
    
    // 初始化 entity（设置初始 count）
    void init(const std::string& source, const std::string& entity, int64_t count, int64_t row_size_bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto key = make_key(source, entity);
        auto& stat = stats_[key];
        stat.source = source;
        stat.entity = entity;
        stat.count = count;
        stat.row_size_bytes = row_size_bytes;
        stat.speed_window_start = std::chrono::steady_clock::now();
        stat.last_update = std::chrono::steady_clock::now();
    }
    
    // 开始同步
    void start_sync(const std::string& source, const std::string& entity) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto key = make_key(source, entity);
        auto& stat = stats_[key];
        stat.is_syncing = true;
        stat.sync_done = false;
        stat.speed_window_start = std::chrono::steady_clock::now();
        stat.recent_records = 0;
    }
    
    // 完成同步
    void end_sync(const std::string& source, const std::string& entity) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto key = make_key(source, entity);
        if (stats_.count(key)) {
            stats_[key].is_syncing = false;
            stats_[key].sync_done = true;
            update_speed(stats_[key]);
        }
    }
    
    // 记录成功的请求
    void record_success(const std::string& source, const std::string& entity, 
                        int64_t records, int64_t latency_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto key = make_key(source, entity);
        auto& stat = stats_[key];
        
        stat.count += records;
        stat.recent_records += records;
        stat.total_requests++;
        stat.success_requests++;
        stat.latency_sum_ms += latency_ms;
        stat.last_update = std::chrono::steady_clock::now();
        
        // 更新平均延时
        if (stat.total_requests > 0) {
            stat.avg_latency_ms = static_cast<double>(stat.latency_sum_ms) / stat.total_requests;
        }
        // 更新成功率
        stat.success_rate = static_cast<double>(stat.success_requests) / stat.total_requests * 100.0;
        
        // 更新速度
        update_speed(stat);
    }
    
    // 记录失败的请求
    void record_failure(const std::string& source, const std::string& entity, int64_t latency_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto key = make_key(source, entity);
        auto& stat = stats_[key];
        
        stat.total_requests++;
        stat.latency_sum_ms += latency_ms;
        stat.last_update = std::chrono::steady_clock::now();
        
        // 更新平均延时
        if (stat.total_requests > 0) {
            stat.avg_latency_ms = static_cast<double>(stat.latency_sum_ms) / stat.total_requests;
        }
        // 更新成功率
        stat.success_rate = static_cast<double>(stat.success_requests) / stat.total_requests * 100.0;
    }
    
    // 获取所有统计（JSON 格式）
    json get_all() {
        std::lock_guard<std::mutex> lock(mutex_);
        json result = json::object();
        
        for (auto& [key, stat] : stats_) {
            // 更新速度（如果正在同步）
            if (stat.is_syncing) {
                update_speed(stat);
            }
            
            result[key] = {
                {"source", stat.source},
                {"entity", stat.entity},
                {"count", stat.count},
                {"row_size_bytes", stat.row_size_bytes},
                {"db_size_mb", (stat.row_size_bytes > 0) ? (static_cast<double>(stat.row_size_bytes) * static_cast<double>(stat.count) / (1024.0 * 1024.0)) : 0.0},
                {"speed", std::round(stat.speed * 10) / 10},  // 保留1位小数
                {"avg_latency_ms", std::round(stat.avg_latency_ms)},
                {"success_rate", std::round(stat.success_rate * 10) / 10},
                {"is_syncing", stat.is_syncing},
                {"sync_done", stat.sync_done},
                {"total_requests", stat.total_requests},
            };
        }
        
        return result;
    }

private:
    EntityStatsManager() = default;
    
    std::string make_key(const std::string& source, const std::string& entity) {
        return source + "/" + entity;
    }
    
    void update_speed(EntityStat& stat) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<double>(now - stat.speed_window_start).count();
        
        if (elapsed >= 1.0) {  // 至少 1 秒才计算
            stat.speed = stat.recent_records / elapsed;
            // 重置窗口
            stat.speed_window_start = now;
            stat.recent_records = 0;
        }
    }
    
    std::mutex mutex_;
    std::unordered_map<std::string, EntityStat> stats_;
};
