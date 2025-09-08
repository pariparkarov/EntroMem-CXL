#ifndef TABLE_H
#define TABLE_H

#include <iostream>
#include <unordered_map>
#include <vector>
#include <string>
#include <stdexcept>
#include <iomanip>
#include <memory>
#include <utility> // for std::pair
#include <cstdint>
#include <algorithm>
#include <mutex>
#include <array>
#include <atomic>


// Table configuration structure
struct TableConfig {
    uint64_t page_size;
    uint64_t base_address;
    uint64_t total_memory;
    uint64_t total_pages;
    size_t initial_metrics;
    std::string name;
    
    TableConfig(const std::string& table_name, uint64_t page_sz = 4096, 
                uint64_t base_addr = 0x0040000000ULL, 
                uint64_t total_mem = 1024ULL * 1024 * 1024 * 1024,
                size_t initial_met = 4) 
        : page_size(page_sz), base_address(base_addr), total_memory(total_mem),
          total_pages(total_mem / page_sz), initial_metrics(initial_met), name(table_name) {}
};

// MESI states enumeration
enum class MESIState : uint8_t {
    MODIFIED = 0,
    EXCLUSIVE = 1,
    SHARED = 2,
    INVALID = 3
};

// PERFORMANCE OPTIMIZATION: Fixed-size arrays instead of dynamic vectors
struct PageEntry {
    MESIState mesi_state;
    // Use fixed arrays for better cache performance and memory efficiency
    std::array<float, 5> metrics;
    std::array<uint64_t, 8> address_metrics; // Pre-allocate reasonable size
    uint8_t active_float_metrics;    // Track actually used metrics
    uint8_t active_address_metrics;  // Track actually used address metrics
    
    PageEntry() : mesi_state(MESIState::INVALID), active_float_metrics(0), active_address_metrics(0) {
        metrics.fill(0.0f);
        address_metrics.fill(0ULL);
    }
    
    PageEntry(size_t initial_metrics) : mesi_state(MESIState::INVALID) {
        metrics.fill(0.0f);
        address_metrics.fill(0ULL);
        
        if (initial_metrics <= 5) {
            active_float_metrics = static_cast<uint8_t>(initial_metrics);
            active_address_metrics = 0;
        } else {
            active_float_metrics = 5;
            active_address_metrics = static_cast<uint8_t>(std::min(initial_metrics - 5, size_t(8)));
        }
    }
    
    size_t total_metric_count() const {
        return active_float_metrics + active_address_metrics;
    }
};

// Performance statistics structure
struct PerformanceStats {
    size_t table_size;
    size_t index_size;
    bool index_built;
    size_t index_hits;
    size_t index_misses;
    
    double get_hit_ratio() const {
        size_t total = index_hits + index_misses;
        return total > 0 ? static_cast<double>(index_hits) / total : 0.0;
    }
};

// MEMORY POOL for better allocation performance
class PageEntryPool {
private:
    std::vector<PageEntry> pool;
    std::vector<size_t> free_indices;
    size_t next_index;

public:
    PageEntryPool() : next_index(0) {
        pool.reserve(100000); // Pre-allocate reasonable size
        free_indices.reserve(10000);
    }
    
    PageEntry* allocate(size_t initial_metrics) {
        if (!free_indices.empty()) {
            size_t idx = free_indices.back();
            free_indices.pop_back();
            pool[idx] = PageEntry(initial_metrics);
            return &pool[idx];
        }
        
        if (next_index >= pool.capacity()) {
            pool.reserve(pool.capacity() * 2);
        }
        
        pool.emplace_back(initial_metrics);
        return &pool[next_index++];
    }
    
    void deallocate(PageEntry* entry) {
        if (entry >= &pool[0] && entry < &pool[0] + pool.size()) {
            size_t idx = entry - &pool[0];
            free_indices.push_back(idx);
        }
    }
    
    void clear() {
        pool.clear();
        free_indices.clear();
        next_index = 0;
    }
    
    size_t memory_usage() const {
        return pool.size() * sizeof(PageEntry) + free_indices.size() * sizeof(size_t);
    }
};

// Page Table class
class PageTable {
private:
    // PERFORMANCE OPTIMIZATION: Use sparse storage - only allocate entries when needed
    std::unordered_map<uint64_t, std::unique_ptr<PageEntry>> table;
    TableConfig config;
    size_t current_metric_count;
    
    // Memory pool for better allocation performance
    mutable PageEntryPool entry_pool;
    
    // LOCK-FREE INDEX using atomic operations where possible
    mutable std::unordered_map<uint64_t, uint64_t> metric6_to_page_index;
    mutable std::atomic<bool> index_built{false};
    mutable std::mutex index_mutex; // Only for building index
    
    // Performance tracking - using atomic for thread safety without locks
    mutable std::atomic<size_t> index_hits{0};
    mutable std::atomic<size_t> index_misses{0};
    
    // PERFORMANCE: Build reverse index on demand - O(n) operation done once
    void build_metric6_index() const {
        bool expected = false;
        if (!index_built.compare_exchange_strong(expected, false)) return;
        
        std::lock_guard<std::mutex> lock(index_mutex);
        if (index_built.load()) return; // Double-check
        
        metric6_to_page_index.clear();
        metric6_to_page_index.reserve(table.size());
        
        for (const auto& [page_number, entry_ptr] : table) {
            if (entry_ptr && entry_ptr->active_address_metrics > 1) {
                metric6_to_page_index[entry_ptr->address_metrics[1]] = page_number;
            }
        }
        
        index_built.store(true);
    }
    
    // Mark index as dirty when table is modified
    void mark_index_dirty() const {
        index_built.store(false);
    }

    // INTERNAL HELPER: Find free page with boolean return
    bool find_free_page_internal(uint64_t& page_number) const {
        static uint64_t last_free = 0;
        
        // Start searching from last known free page
        for (uint64_t i = last_free; i < config.total_pages; ++i) {
            if (table.find(i) == table.end()) {
                last_free = i + 1;
                page_number = i;
                return true;  // Success
            }
        }
        
        // Wrap around search
        for (uint64_t i = 0; i < last_free; ++i) {
            if (table.find(i) == table.end()) {
                last_free = i + 1;
                page_number = i;
                return true;  // Success
            }
        }
        
        return false;  // No free pages
    }

public:
    PageTable(const TableConfig& cfg) 
        : config(cfg), current_metric_count(std::max(cfg.initial_metrics, size_t(6))) {}

    bool init() {
        table.clear();
        entry_pool.clear();
        mark_index_dirty();
        current_metric_count = std::max(config.initial_metrics, size_t(7));
        
        std::cout << config.name << " initialized:\n"
                  << "- Base Address: 0x" << std::hex << config.base_address << std::dec << "\n"
                  << "- Total Memory: " << config.total_memory / (1024ULL * 1024 * 1024) << " GB\n"
                  << "- Page Size: " << config.page_size << " bytes\n"
                  << "- Total Pages: " << config.total_pages << "\n"
                  << "- Initial Metrics: " << current_metric_count << " (0-4: float, 5+: uint64_t address)\n"
                  << "- Memory Mode: SPARSE (pages allocated on-demand)\n\n";
        
        // DON'T pre-allocate all pages - this was causing memory issues!
        // Pages will be created on-demand in get_or_create_page_entry()
                  
        return true;   
    }

    // ORIGINAL METHODS (with exception handling) - NO CHANGES TO FUNCTION SIGNATURES
uint64_t address_to_page_number(uint64_t virtual_address) const {
    // Mask the address to the actual memory size if it comes from a wide flit
uint64_t max_address = config.base_address + (config.total_pages * config.page_size) - 1;

if (virtual_address < config.base_address || virtual_address > max_address) {
    std::cerr << "[address_to_page_number] Out of range! "
              << "addr=0x" << std::hex << virtual_address
              << " base=0x" << config.base_address
              << " max=0x" << max_address
              << " (dec addr=" << std::dec << virtual_address
              << ", base=" << config.base_address
              << ", max=" << max_address << ")\n";
    throw std::out_of_range("Virtual address is out of memory bounds");
}

    uint64_t offset = virtual_address - config.base_address;
    uint64_t page_num = offset / config.page_size;
    return page_num;
}

uint64_t page_number_to_address(uint64_t page_number) const {
    if (page_number >= config.total_pages) {
        throw std::out_of_range("Page number exceeds total pages");
    }

    uint64_t addr = config.base_address + page_number * config.page_size;

    // Extra safety: ensure no overflow
    if (addr < config.base_address) {
        throw std::overflow_error("Address calculation overflow");
    }

    return addr;
}


 PageEntry& get_or_create_page_entry(uint64_t page_number) {
    if (page_number >= config.total_pages) throw std::invalid_argument("Page number out of bounds");
    
    auto it = table.find(page_number);
    if (it == table.end()) {
        // Create new entry using memory pool
        table[page_number] = std::make_unique<PageEntry>(current_metric_count);
        
        // ADD OPTION 1 HERE - Initialize metric 5 (address_metrics[0]) with the page's base address
        if (current_metric_count >= 6 && table[page_number]->active_address_metrics >= 1) {
            table[page_number]->address_metrics[0] = page_number_to_address(page_number);
        }
        
        // Initialize metric 7 (address_metrics[1]) with default value if it exists
        if (current_metric_count >= 7 && table[page_number]->active_address_metrics >= 2) {
            table[page_number]->address_metrics[1] = page_number;
        }
        
        mark_index_dirty();
    }
    
    return *table[page_number];
}
    // OPTIMIZED FAST METHODS (no exceptions, maximum performance) - NO SIGNATURE CHANGES
    
    bool is_valid_address(uint64_t address) const noexcept {
        return address >= config.base_address && 
               address < (config.base_address + config.total_memory);
    }
    
    // ULTRA-FAST: Using bit shifts for power-of-2 page sizes
    uint64_t address_to_page_number_fast(uint64_t address) const noexcept {
        // Optimized for 4KB pages (4096 = 2^12)
        if (config.page_size == 4096) {
            return (address - config.base_address) >> 12;
        }
        // Fallback for non-power-of-2 page sizes
        return (address - config.base_address) / config.page_size;
    }
    
    // ULTRA-FAST: O(1) reverse lookup using lock-free index
    bool find_page_by_metric6_fast(uint64_t metric6, uint64_t& page_num) const {
        if (!index_built.load(std::memory_order_acquire)) {
            build_metric6_index();
        }
        
        auto it = metric6_to_page_index.find(metric6);
        if (it != metric6_to_page_index.end()) {
            page_num = it->second;
            index_hits.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        
        index_misses.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    
    bool page_number_to_address_fast(uint64_t page_number, uint64_t& address) const noexcept {
        if (page_number >= config.total_pages) return false;
        
        if (config.page_size == 4096) {
            address = config.base_address + (page_number << 12);
        } else {
            address = config.base_address + (page_number * config.page_size);
        }
        return true;
    }

    // ORIGINAL METHODS - NO SIGNATURE CHANGES
    bool set_mesi_state(uint64_t page_number, MESIState state) {
        try {
            PageEntry& entry = get_or_create_page_entry(page_number);
            entry.mesi_state = state;
            return true;
        } catch (const std::exception& e) {
            std::cerr << "Error setting MESI state: " << e.what() << std::endl;
            return false;
        }
    }

    MESIState get_mesi_state(uint64_t page_number) {
        try {
            PageEntry& entry = get_or_create_page_entry(page_number);
            return entry.mesi_state;
        } catch (const std::exception& e) {
            std::cerr << "Error getting MESI state: " << e.what() << std::endl;
            return MESIState::INVALID;
        }
    }

    bool set_metric(uint64_t page_number, size_t metric_index, float value) {
        try {
            PageEntry& entry = get_or_create_page_entry(page_number);
            if (metric_index >= 5 || metric_index >= entry.active_float_metrics) return false;
            entry.metrics[metric_index] = value;
            return true;
        } catch (...) { return false; }
    }

    float get_metric(uint64_t page_number, size_t metric_index) {
        try {
            PageEntry& entry = get_or_create_page_entry(page_number);
            if (metric_index >= 5 || metric_index >= entry.active_float_metrics) return 0.0f;
            return entry.metrics[metric_index];
        } catch (...) { return 0.0f; }
    }

    bool set_address_metric(uint64_t page_number, size_t metric_index, uint64_t address_value) {
        try {
            PageEntry& entry = get_or_create_page_entry(page_number);
            if (metric_index < 5) return false;
            size_t addr_index = metric_index - 5;
            if (addr_index >= 8 || addr_index >= entry.active_address_metrics) return false;
            
            // If modifying metric 6 (address_metrics[1]), invalidate index
            if (metric_index == 6) {
                mark_index_dirty();
            }
            
            entry.address_metrics[addr_index] = address_value;
            return true;
        } catch (...) { return false; }
    }

    uint64_t get_address_metric(uint64_t page_number, size_t metric_index) {
        try {
            PageEntry& entry = get_or_create_page_entry(page_number);
            if (metric_index < 5) return 0ULL;
            size_t addr_index = metric_index - 5;
            if (addr_index >= 8 || addr_index >= entry.active_address_metrics) return 0ULL;
            return entry.address_metrics[addr_index];
        } catch (...) { return 0ULL; }
    }

    bool add_metric(float default_value = 0.0f) {
        current_metric_count++;
        
        for (auto& [page_num, entry_ptr] : table) {
            if (!entry_ptr) continue;
            
            if (current_metric_count <= 5) {
                if (entry_ptr->active_float_metrics < 5) {
                    entry_ptr->metrics[entry_ptr->active_float_metrics] = default_value;
                    entry_ptr->active_float_metrics++;
                }
            } else {
                if (entry_ptr->active_address_metrics < 8) {
                    entry_ptr->address_metrics[entry_ptr->active_address_metrics] = 0ULL;
                    entry_ptr->active_address_metrics++;
                }
            }
        }
        mark_index_dirty();
        return true;
    }

    void free_page(uint64_t page_number) {
        auto it = table.find(page_number);
        if (it != table.end()) {
            table.erase(it);
            mark_index_dirty();
        }
    }

    // ENHANCED find_free_page_number with automatic cleanup and graceful failure
    uint64_t find_free_page_number() const {
        uint64_t page_number;
        
        // First attempt: normal search
        if (find_free_page_internal(page_number)) {
            return page_number;
        }
        
        // Second attempt: try cleanup and search again
        std::cout << config.name << " full, attempting cleanup..." << std::endl;
        const_cast<PageTable*>(this)->trim_unused_pages();
        
        if (find_free_page_internal(page_number)) {
            std::cout << "Found free page " << page_number << " after cleanup" << std::endl;
            return page_number;
        }
        
        // Third attempt: emergency cleanup
        std::cout << config.name << " still full, attempting emergency cleanup..." << std::endl;
        const_cast<PageTable*>(this)->emergency_cleanup();
        
        if (find_free_page_internal(page_number)) {
            std::cout << "Found free page " << page_number << " after emergency cleanup" << std::endl;
            return page_number;
        }
        
        // All attempts failed - log error and return sentinel value
        std::cerr << "CRITICAL ERROR: " << config.name << " completely exhausted! "
                  << "Pages allocated: " << table.size() << " / " << config.total_pages << std::endl;
        
        return UINT64_MAX;  // Indicates failure
    }

    // NEW: Check if free pages are available
    bool has_free_pages() const {
        return table.size() < config.total_pages;
    }

    // NEW: Safe version that returns success/failure
    bool try_find_free_page_number(uint64_t& page_number) const {
        // First attempt: normal search
        if (find_free_page_internal(page_number)) {
            return true;
        }
        
        // Second attempt: try cleanup and search again
        std::cout << config.name << " full, attempting cleanup..." << std::endl;
        const_cast<PageTable*>(this)->trim_unused_pages();
        
        if (find_free_page_internal(page_number)) {
            std::cout << "Found free page " << page_number << " after cleanup" << std::endl;
            return true;
        }
        
        // Third attempt: emergency cleanup
        std::cout << config.name << " still full, attempting emergency cleanup..." << std::endl;
        const_cast<PageTable*>(this)->emergency_cleanup();
        
        if (find_free_page_internal(page_number)) {
            std::cout << "Found free page " << page_number << " after emergency cleanup" << std::endl;
            return true;
        }
        
        // All attempts failed
        std::cerr << "CRITICAL ERROR: " << config.name << " completely exhausted! "
                  << "Pages allocated: " << table.size() << " / " << config.total_pages << std::endl;
        
        return false;
    }

    // PERFORMANCE METHODS - NO SIGNATURE CHANGES
    void preload_index() const {
        build_metric6_index();
    }
    
    void clear_index() const {
        std::lock_guard<std::mutex> lock(index_mutex);
        metric6_to_page_index.clear();
        index_built.store(false);
        index_hits.store(0);
        index_misses.store(0);
    }
    
    PerformanceStats get_performance_stats() const {
        return {
            table.size(),
            metric6_to_page_index.size(),
            index_built.load(),
            index_hits.load(),
            index_misses.load()
        };
    }
    
    void batch_set_address_metrics(const std::vector<std::pair<uint64_t, uint64_t>>& updates) {
        // OPTIMIZATION: Batch process to reduce index invalidations
        bool needs_index_update = false;
        
        for (const auto& [page_num, value] : updates) {
            if (set_address_metric(page_num, 6, value)) {
                needs_index_update = true;
            }
        }
        
        if (needs_index_update) {
            mark_index_dirty();
        }
    }

    // GETTERS - NO CHANGES
    size_t get_total_pages() const { return config.total_pages; }
    size_t get_metric_count() const { return current_metric_count; }
    std::string get_name() const { return config.name; }
    
    // WARNING: These methods now return different types due to optimization
    // Original code using these may need adjustment
    const std::unordered_map<uint64_t, std::unique_ptr<PageEntry>>& get_table() const { return table; }
    std::unordered_map<uint64_t, std::unique_ptr<PageEntry>>& get_table_mutable() { return table; }

    void print_statistics() const {
        auto stats = get_performance_stats();
        
        std::cout << config.name << " Statistics:\n";
        std::cout << "- Total allocated pages: " << table.size() << " / " << config.total_pages << "\n";
        std::cout << "- Memory efficiency: " << std::fixed << std::setprecision(2)
                  << (double(table.size()) / config.total_pages * 100.0) << "% pages allocated\n";
        std::cout << "- Current metric count: " << current_metric_count << "\n";
        std::cout << "- Index built: " << (stats.index_built ? "Yes" : "No") << "\n";
        std::cout << "- Index size: " << stats.index_size << " entries\n";
        std::cout << "- Index hit ratio: " << std::fixed << std::setprecision(2) 
                  << (stats.get_hit_ratio() * 100.0) << "%\n";
        std::cout << "- Index hits: " << stats.index_hits << "\n";
        std::cout << "- Index misses: " << stats.index_misses << "\n";
        std::cout << "- Memory usage: " << (get_memory_usage() / 1024 / 1024) << " MB\n\n";
    }

    void cleanup() { 
        table.clear(); 
        clear_index();
        entry_pool.clear();
    }
    
    // ENHANCED memory usage calculation
    size_t get_memory_usage() const {
        size_t base_size = sizeof(*this);
        size_t table_size = table.size() * (sizeof(uint64_t) + sizeof(std::unique_ptr<PageEntry>) + sizeof(PageEntry));
        size_t index_size = metric6_to_page_index.size() * (sizeof(uint64_t) * 2);
        size_t pool_size = entry_pool.memory_usage();
        
        return base_size + table_size + index_size + pool_size;
    }
    
    // NEW: Memory management methods to prevent OOM
    void trim_unused_pages() {
        std::vector<uint64_t> to_remove;
        
        for (const auto& [page_num, entry_ptr] : table) {
            if (entry_ptr && entry_ptr->mesi_state == MESIState::INVALID) {
                // Check if page has any non-zero metrics
                bool has_data = false;
                for (size_t i = 0; i < entry_ptr->active_float_metrics; ++i) {
                    if (entry_ptr->metrics[i] != 0.0f) {
                        has_data = true;
                        break;
                    }
                }
                
                if (!has_data) {
                    for (size_t i = 0; i < entry_ptr->active_address_metrics; ++i) {
                        if (entry_ptr->address_metrics[i] != 0ULL) {
                            has_data = true;
                            break;
                        }
                    }
                }
                
                if (!has_data) {
                    to_remove.push_back(page_num);
                }
            }
        }
        
        for (uint64_t page_num : to_remove) {
            free_page(page_num);
        }
        
        std::cout << "Trimmed " << to_remove.size() << " unused pages from " << config.name << std::endl;
    }
    
    bool get_page_metric6_fast(uint64_t page_number, uint64_t& metric6_value) const noexcept {
        // Check if page number is valid
        if (page_number >= config.total_pages) return false;
        
        // Look for existing page entry
        auto it = table.find(page_number);
        if (it == table.end() || !it->second) return false;
        
        const PageEntry& entry = *it->second;
        
        // Check if metric6 (address_metrics[1]) exists
        if (entry.active_address_metrics < 2) return false;
        
        metric6_value = entry.address_metrics[1];
        return true;
    }
    
    // NEW: Emergency memory cleanup
    void emergency_cleanup() {
        std::cout << "Starting emergency cleanup for " << config.name << "..." << std::endl;
        
        // Clear all indexes first
        clear_index();
        
        // Trim unused pages
        trim_unused_pages();
        
        // Clear memory pool
        entry_pool.clear();
        
        // Force garbage collection of map by swapping with temporary
        std::unordered_map<uint64_t, std::unique_ptr<PageEntry>> temp;
        temp.swap(table);
        table.swap(temp);
        
        std::cout << "Emergency cleanup completed for " << config.name 
                  << ". Current pages: " << table.size() << std::endl;
    }

    // NEW: Get memory pressure level (0-100%)
    double get_memory_pressure() const {
        return (double(table.size()) / config.total_pages) * 100.0;
    }

    // NEW: Check if table is near capacity
    bool is_near_capacity(double threshold_percent = 90.0) const {
        return get_memory_pressure() >= threshold_percent;
    }
};

#endif // TABLE_H