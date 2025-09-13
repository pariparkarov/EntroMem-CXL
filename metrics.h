#ifndef METRICS_H
#define METRICS_H

#include <iostream>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <list>
#include <limits>
#include <functional>
#include <cmath>
#include "table.h"

// Constants
constexpr int D = 5;           // Depth of Count-Min Sketch
constexpr int W = 272;         // Width per row
constexpr int COUNTER_BITS = 10;
constexpr uint16_t MAX_COUNTER = (1 << COUNTER_BITS) - 1;
constexpr double EMA_ALPHA = 0.6;  // Decay rate for EMA

// Hash function with seeding
uint64_t hash_page(uint64_t page, int seed) {
    return std::hash<uint64_t>{}(page ^ (0x9e3779b97f4a7c15ULL * seed));
}

// === Access Count CMS ===
std::vector<std::vector<uint16_t>> cms_access_table(D, std::vector<uint16_t>(W, 0));
std::unordered_map<uint64_t, uint16_t> page_access_counts;

void cms_access(uint64_t page_id) {
    uint16_t min_val = std::numeric_limits<uint16_t>::max();

    for (int i = 0; i < D; ++i) {
        int index = hash_page(page_id, i) % W;
        if (cms_access_table[i][index] < MAX_COUNTER)
            ++cms_access_table[i][index];
        if (cms_access_table[i][index] < min_val)
            min_val = cms_access_table[i][index];
    }

    page_access_counts[page_id] = min_val;
}

uint16_t cms_estimate(uint64_t page_id) {
    uint16_t min_val = std::numeric_limits<uint16_t>::max();
    for (int i = 0; i < D; ++i) {
        int index = hash_page(page_id, i) % W;
        if (cms_access_table[i][index] < min_val)
            min_val = cms_access_table[i][index];
    }
    return min_val;
}

// === Write Count CMS ===
std::vector<std::vector<uint16_t>> cms_write_table(D, std::vector<uint16_t>(W, 0));
std::unordered_map<uint64_t, uint16_t> page_write_counts;

void update_write_cms(uint64_t page_id) {
    uint16_t min_val = std::numeric_limits<uint16_t>::max();

    for (int i = 0; i < D; ++i) {
        int index = hash_page(page_id, i) % W;
        if (cms_write_table[i][index] < MAX_COUNTER)
            ++cms_write_table[i][index];
        if (cms_write_table[i][index] < min_val)
            min_val = cms_write_table[i][index];
    }

    page_write_counts[page_id] = min_val;
}

// === Decay logic for CMS counts ===
void decay_cms(std::vector<std::vector<uint16_t>>& table) {
    for (auto& row : table)
        for (auto& cell : row)
            cell = std::floor(cell * EMA_ALPHA);
}

// === Last Access Time ===
uint64_t global_time = 0;
std::unordered_map<uint64_t, uint64_t> last_access_table;

void update_last_access_time(uint64_t page_id) {
    last_access_table[page_id] = global_time;
}

// === Reuse Distance Tracking ===
std::list<uint64_t> reuse_stack;  // LRU list: front = most recent
std::unordered_map<uint64_t, std::list<uint64_t>::iterator> reuse_map;
std::unordered_map<uint64_t, int> reuse_distance_map;  // Stores last reuse distance per page

void update_reuse_distance(uint64_t page_id) {
    if (reuse_map.find(page_id) != reuse_map.end()) {
        auto it = reuse_map[page_id];
        int distance = std::distance(reuse_stack.begin(), it);
        reuse_stack.erase(it);
        reuse_stack.push_front(page_id);
        reuse_map[page_id] = reuse_stack.begin();
        reuse_distance_map[page_id] = distance;
    } else {
        reuse_stack.push_front(page_id);
        reuse_map[page_id] = reuse_stack.begin();
        reuse_distance_map[page_id] = -1;
    }
}

// === EMA Hotness Tracker ===
std::unordered_map<uint64_t, double> ema_hotness_scores;

void ema_access(uint64_t page_id) {
    ema_hotness_scores[page_id] = EMA_ALPHA * ema_hotness_scores[page_id] + (1.0 - EMA_ALPHA);
}

void decay_ema_scores() {
    for (auto& [page, score] : ema_hotness_scores)
        score *= EMA_ALPHA;
}

// === Global Tick Advance ===
void advance_time_tick() {
    ++global_time;
    decay_ema_scores();
    decay_cms(cms_access_table);
    decay_cms(cms_write_table);
}


// === Print ===
void print_ema_scores() {
    std::cout << "\nEMA Hotness Scores:\n";
    for (const auto& [page, score] : ema_hotness_scores)
        std::cout << "Page 0x" << std::hex << page << std::dec << " ? Score: " << score << "\n";
}

// === Hotness Calculation ===
uint64_t DR = 0;
uint64_t pa = 4096;


void update_hotness_metrics(GlobalPageManager& manager) {
    for (const auto& [page_id, score] : ema_hotness_scores) {
        // CRITICAL FIX: Convert page_id to global page number if needed
        uint64_t global_page_num;
        
        try {
            // If page_id is a physical page ID, convert to address first
            uint64_t addr = page_id * pa + DR; // Assume DRAM_START as base
            
            // Then convert address to global page number
            global_page_num = manager.address_to_page_number(addr);
        } catch (...) {
            // If conversion fails, assume page_id is already a global page number
            global_page_num = page_id;
        }
        
        // Calculate all components
        double access_score = static_cast<double>(cms_estimate(page_id)) / MAX_COUNTER;
        double write_score = static_cast<double>(page_write_counts[page_id]) / MAX_COUNTER;
        int reuse_distance = reuse_distance_map.count(page_id) ? reuse_distance_map[page_id] : -1;
        double reuse_score = (reuse_distance < 0) ? 0.5 : std::exp(-0.05 * reuse_distance);
        double ema_score = ema_hotness_scores[page_id];
        
        // Weighted combination
        double hotness = 0.4 * access_score + 0.2 * write_score + 0.2 * reuse_score + 0.2 * ema_score;
        
        // Clamp hotness to valid range
        hotness = std::max(0.0, std::min(1.0, hotness));
        
        try {
            // Use global_page_num instead of page_id
            manager.set_metric(global_page_num, 0, static_cast<float>(hotness));        // Hotness
            manager.set_metric(global_page_num, 1, static_cast<float>(access_score));   // Access score
            manager.set_metric(global_page_num, 2, static_cast<float>(write_score));    // Write score
            manager.set_metric(global_page_num, 3, static_cast<float>(reuse_score));    // Reuse score
            manager.set_metric(global_page_num, 4, static_cast<float>(ema_score));      // EMA score
            
            // Debug output for verification
            if (global_page_num != page_id) {

            }
        } catch (const std::exception& e) {
            std::cerr << "Failed to write metrics for global page " << global_page_num 
                     << " (original page_id " << page_id << "): " << e.what() << "\n";
        }
    }
}
#endif
