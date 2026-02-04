#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <nlohmann/json.hpp>
#include <cassert>

using json = nlohmann::json;

// ============================================================================
// 配置结构
// ============================================================================

struct SourceConfig {
    std::string name;
    std::string subgraph_id;
    bool enabled;
    std::vector<std::string> entities;
};

struct Config {
    std::string api_key;
    std::string db_path;
    int sync_interval_seconds;
    std::vector<SourceConfig> sources;
    
    static Config load(const std::string& path) {
        std::ifstream f(path);
        assert(f.is_open() && "无法打开配置文件");
        
        json j;
        f >> j;
        
        Config config;
        config.api_key = j["api_key"].get<std::string>();
        config.db_path = j["db_path"].get<std::string>();
        config.sync_interval_seconds = j.value("sync_interval_seconds", 60);
        
        if (j.contains("sources")) {
            for (auto& [name, source] : j["sources"].items()) {
                SourceConfig sc;
                sc.name = name;
                sc.subgraph_id = source["subgraph_id"].get<std::string>();
                sc.enabled = source.value("enabled", true);
                for (const auto& e : source["entities"]) {
                    sc.entities.push_back(e.get<std::string>());
                }
                if (sc.enabled) {
                    config.sources.push_back(sc);
                }
            }
        }
        
        return config;
    }
};
