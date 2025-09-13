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

// Forward declarations
class GlobalPageManager;
class PageTable;

// Table configuration structure
struct TableConfig {
    uint64_t page_size;
    uint64_t base_address;
    uint64_t total_memory;
    uint64_t total_pages;
    size_t initial_metrics;
    std::string name;
    
    // NEW: Global page management
    uint64_t global_page_offset;      // Starting global page number for initial allocation
    double reserved_space_percent;    // % of space reserved for migrations (default 10%)
    uint64_t max_usable_pages;       // total_pages - reserved pages
    
    TableConfig(const std::string& table_name, 
                uint64_t page_sz = 4096, 
                uint64_t base_addr = 0x0040000000ULL, 
                uint64_t total_mem = 1024ULL * 1024 * 1024 * 1024,
                size_t initial_met = 4,
                uint64_t global_offset = 0,
                double reserved_percent = 10.0) 
        : page_size(page_sz), base_address(base_addr), total_memory(total_mem),
          total_pages((total_mem + base_addr) / page_sz), initial_metrics(initial_met), name(table_name),
          global_page_offset(global_offset), reserved_space_percent(reserved_percent) {
        
        // Calculate usable pages (reserve space for migrations)
        uint64_t reserved_pages = static_cast<uint64_t>(total_pages * reserved_percent / 100.0);
        max_usable_pages = total_pages - std::max(reserved_pages, uint64_t(10)); // At least 10 pages reserved
    }
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
    
    // NEW: Copy constructor for migration
    PageEntry(const PageEntry& other) 
        : mesi_state(other.mesi_state), 
          metrics(other.metrics),
          address_metrics(other.address_metrics),
          active_float_metrics(other.active_float_metrics),
          active_address_metrics(other.active_address_metrics) {}
    
    // NEW: Assignment operator for migration
    PageEntry& operator=(const PageEntry& other) {
        if (this != &other) {
            mesi_state = other.mesi_state;
            metrics = other.metrics;
            address_metrics = other.address_metrics;
            active_float_metrics = other.active_float_metrics;
            active_address_metrics = other.active_address_metrics;
        }
        return *this;
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
    std::unordered_map<uint64_t, std::unique_ptr<PageEntry>> table; // Uses LOCAL page numbers as keys

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
    
    // NEW: Global page manager reference (will be set after construction)
    GlobalPageManager* global_manager;
    
    // PERFORMANCE: Build reverse index on demand - O(n) operation done once
    void build_metric6_index() const {
        bool expected = false;
        if (!index_built.compare_exchange_strong(expected, false)) return;
        
        std::lock_guard<std::mutex> lock(index_mutex);
        if (index_built.load()) return; // Double-check
        
        metric6_to_page_index.clear();
        metric6_to_page_index.reserve(table.size());
        
        for (const auto& [local_page_number, entry_ptr] : table) {
            if (entry_ptr && entry_ptr->active_address_metrics > 1) {
                // Map metric6 value to the global page stored in address_metrics[1]
                uint64_t global_page = entry_ptr->address_metrics[1];
                metric6_to_page_index[entry_ptr->address_metrics[1]] = global_page;
            }
        }
        
        index_built.store(true);
    }
    
    // Mark index as dirty when table is modified
    void mark_index_dirty() const {
        index_built.store(false);
    }

    // INTERNAL HELPER: Find free local page slot (uses LOCAL page numbers)
    bool find_free_local_page_internal(uint64_t& local_page_number) const {
        static uint64_t last_free = 0;
        
        // Check if we're approaching reserved space limit
        if (table.size() >= config.max_usable_pages) {
            return false; // No space available for normal allocation
        }
        
        // Start searching from last known free page
        for (uint64_t i = last_free; i < config.total_pages; ++i) {
            if (table.find(i) == table.end()) {
                last_free = i + 1;
                local_page_number = i;
                return true;  // Success
            }
        }
        
        // Wrap around search
        for (uint64_t i = 0; i < last_free; ++i) {
            if (table.find(i) == table.end()) {
                last_free = i + 1;
                local_page_number = i;
                return true;  // Success
            }
        }
        
        return false;  // No free pages
    }

public:
	    std::unordered_map<uint64_t, uint64_t> global_to_local_map; // Maps global page -> local slot
    PageTable(const TableConfig& cfg) 
        : config(cfg), current_metric_count(std::max(cfg.initial_metrics, size_t(6))), global_manager(nullptr) {}
        
        
	const std::unordered_map<uint64_t, uint64_t>& get_global_to_local_map() const { 
    return global_to_local_map; 
	}
    // NEW: Set global manager (called by GlobalPageManager)
    void set_global_manager(GlobalPageManager* manager) {
        global_manager = manager;
    }
    
    

    // MODIFIED: Global ↔ Local page conversion methods for virtual pages
    uint64_t global_to_local_page(uint64_t global_page) const {
        auto it = global_to_local_map.find(global_page);
        if (it == global_to_local_map.end()) {
            throw std::out_of_range("Global page not found in this table");
        }
        return it->second;
    }
    
    uint64_t local_to_global_page(uint64_t local_page) const {
        // Find global page that maps to this local page
        for (const auto& [global_page, local_slot] : global_to_local_map) {
            if (local_slot == local_page) {
                return global_page;
            }
        }
        throw std::out_of_range("Local page has no global mapping");
    }
    
    // MODIFIED: Any table can hold any global page now
    bool owns_global_page(uint64_t global_page) const noexcept {
        return global_to_local_map.find(global_page) != global_to_local_map.end();
    }

    bool init() {
        table.clear();
        global_to_local_map.clear();
        entry_pool.clear();
        mark_index_dirty();
        current_metric_count = std::max(config.initial_metrics, size_t(7));
        
        std::cout << config.name << " initialized:\n"
                  << "- Base Address: 0x" << std::hex << config.base_address << std::dec << "\n"
                  << "- Total Memory: " << config.total_memory / (1024ULL * 1024 * 1024) << " GB\n"
                  << "- Page Size: " << config.page_size << " bytes\n"
                  << "- Total Pages: " << config.total_pages << "\n"
                  << "- Initial Global Page Range: " << config.global_page_offset 
                  << " - " << (config.global_page_offset + config.total_pages - 1) << " (for initial allocation)\n"
                  << "- Usable Pages: " << config.max_usable_pages << " (Reserved: " 
                  << (config.total_pages - config.max_usable_pages) << ")\n"
                  << "- Initial Metrics: " << current_metric_count << " (0-4: float, 5+: uint64_t address)\n"
                  << "- Memory Mode: SPARSE (pages allocated on-demand)\n"
                  << "- Virtual Pages: Any global page can reside in any table\n\n";
        
        return true;   
    }

    // MODIFIED: Address methods now use physical address calculation based on current location
    uint64_t address_to_page_number(uint64_t virtual_address) const {
        uint64_t max_address = config.base_address + (config.total_pages * config.page_size) - 1;
        
        if (virtual_address < config.base_address || virtual_address > max_address) {
            throw std::out_of_range("Virtual address is out of this table's physical bounds");
        }
        
        uint64_t offset = virtual_address - config.base_address;
        uint64_t local_page_num = offset / config.page_size;
        
        if (local_page_num >= config.total_pages) {
            throw std::out_of_range("Calculated local page exceeds table bounds");
        }
        
        // Find which global page is stored at this local slot
        try {
            return local_to_global_page(local_page_num);
        } catch (...) {
            throw std::out_of_range("No global page mapped to this address");
        }
    }

    uint64_t page_number_to_address(uint64_t global_page_number) const {
        // Find local slot for this global page
        uint64_t local_page = global_to_local_page(global_page_number);
        
        if (local_page >= config.total_pages) {
            throw std::out_of_range("Local page number exceeds total pages");
        }
        
        uint64_t addr = config.base_address + local_page * config.page_size;
        
        // Check for overflow
        if (addr < config.base_address) {
            throw std::overflow_error("Address calculation overflow");
        }
        
        return addr;
    }

    // MODIFIED: Now takes GLOBAL page numbers, maps to local slots
    PageEntry& get_or_create_page_entry(uint64_t global_page_number) {
        auto global_it = global_to_local_map.find(global_page_number);
        uint64_t local_page;
        
        if (global_it == global_to_local_map.end()) {
            // Need to allocate a new local slot
            if (!find_free_local_page_internal(local_page)) {
                throw std::runtime_error("Table full - cannot allocate new page");
            }
            
            // Create the mapping
            global_to_local_map[global_page_number] = local_page;
        } else {
            local_page = global_it->second;
        }
        
        auto it = table.find(local_page);
        if (it == table.end()) {
            // Create new entry
            table[local_page] = std::make_unique<PageEntry>(current_metric_count);
            
            // Initialize address metrics
            if (current_metric_count >= 6 && table[local_page]->active_address_metrics >= 1) {
                table[local_page]->address_metrics[0] = config.base_address + (local_page * config.page_size);
            }
            
            if (current_metric_count >= 7 && table[local_page]->active_address_metrics >= 2) {
                table[local_page]->address_metrics[1] = global_page_number;
            }
            
            mark_index_dirty();
        }
        
        return *table[local_page];
    }

    // Migration method declarations
    bool migrate_page_to(PageTable& destination_table, uint64_t global_page_number);
    bool exchange_pages_with(PageTable& other_table, uint64_t my_global_page, uint64_t their_global_page);
    
    // Extract page data for migration (doesn't remove from table)
    std::unique_ptr<PageEntry> extract_page_data(uint64_t global_page_number) {
        auto global_it = global_to_local_map.find(global_page_number);
        if (global_it == global_to_local_map.end()) {
            return nullptr;
        }
        
        uint64_t local_page = global_it->second;
        auto it = table.find(local_page);
        if (it == table.end()) {
            return nullptr;
        }
        
        // Create a copy of the page entry
        auto copy = std::make_unique<PageEntry>(*it->second);
        return copy;
    }
    
    // Insert migrated page data
    bool insert_migrated_page(uint64_t global_page_number, std::unique_ptr<PageEntry> page_data) {
        // Check if we can accept this page
        if (table.size() >= config.total_pages) {
            return false; // Absolutely no space
        }
        
        // Find a free local slot
        uint64_t local_page;
        if (!find_free_local_page_internal(local_page)) {
            // Try to use reserved space for migration
            if (table.size() >= config.total_pages) {
                return false;
            }
            // Find any free slot including reserved space
            for (uint64_t i = 0; i < config.total_pages; ++i) {
                if (table.find(i) == table.end()) {
                    local_page = i;
                    break;
                }
            }
        }
        
        // Update address metrics to reflect new physical location
        if (page_data->active_address_metrics >= 1) {
            page_data->address_metrics[0] = config.base_address + (local_page * config.page_size);
        }
        if (page_data->active_address_metrics >= 2) {
            page_data->address_metrics[1] = global_page_number; // Keep global identity
        }
        
        // Create the mapping and insert the page
        global_to_local_map[global_page_number] = local_page;
        table[local_page] = std::move(page_data);
        mark_index_dirty();
        return true;
    }
    
    // Remove page from table (for migration)
    void remove_page(uint64_t global_page_number) {
        auto global_it = global_to_local_map.find(global_page_number);
        if (global_it != global_to_local_map.end()) {
            uint64_t local_page = global_it->second;
            
            auto table_it = table.find(local_page);
            if (table_it != table.end()) {
                table.erase(table_it);
            }
            
            global_to_local_map.erase(global_it);
            mark_index_dirty();
        }
    }

    // Address validation for this table's physical range
    bool is_valid_address(uint64_t address) const noexcept {
        return address >= config.base_address && 
               address < (config.base_address + config.total_memory);
    }
    
    uint64_t address_to_page_number_fast(uint64_t address) const noexcept {
        if (!is_valid_address(address)) return UINT64_MAX;
        
        uint64_t local_page;
        if (config.page_size == 4096) {
            local_page = (address - config.base_address) >> 12;
        } else {
            local_page = (address - config.base_address) / config.page_size;
        }
        
        try {
            return local_to_global_page(local_page);
        } catch (...) {
            return UINT64_MAX;
        }
    }
    
    bool find_page_by_metric6_fast(uint64_t metric6, uint64_t& global_page_num) const {
        if (!index_built.load(std::memory_order_acquire)) {
            build_metric6_index();
        }
        
        auto it = metric6_to_page_index.find(metric6);
        if (it != metric6_to_page_index.end()) {
            global_page_num = it->second;
            index_hits.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        
        index_misses.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    
    bool page_number_to_address_fast(uint64_t global_page_number, uint64_t& address) const noexcept {
        try {
            auto global_it = global_to_local_map.find(global_page_number);
            if (global_it == global_to_local_map.end()) return false;
            
            uint64_t local_page = global_it->second;
            if (local_page >= config.total_pages) return false;
            
            if (config.page_size == 4096) {
                address = config.base_address + (local_page << 12);
            } else {
                address = config.base_address + (local_page * config.page_size);
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    // All existing methods - unchanged function names, modified to work with virtual global pages
    bool set_mesi_state(uint64_t global_page_number, MESIState state) {
        try {
            PageEntry& entry = get_or_create_page_entry(global_page_number);
            entry.mesi_state = state;
            return true;
        } catch (const std::exception& e) {
            std::cerr << "Error setting MESI state: " << e.what() << std::endl;
            return false;
        }
    }

    MESIState get_mesi_state(uint64_t global_page_number) {
        try {
            PageEntry& entry = get_or_create_page_entry(global_page_number);
            return entry.mesi_state;
        } catch (const std::exception& e) {
            std::cerr << "Error getting MESI state: " << e.what() << std::endl;
            return MESIState::INVALID;
        }
    }

    bool set_metric(uint64_t global_page_number, size_t metric_index, float value) {
        try {
            PageEntry& entry = get_or_create_page_entry(global_page_number);
            if (metric_index >= 5 || metric_index >= entry.active_float_metrics) return false;
            entry.metrics[metric_index] = value;
            return true;
        } catch (...) { return false; }
    }

    float get_metric(uint64_t global_page_number, size_t metric_index) {
        try {
            PageEntry& entry = get_or_create_page_entry(global_page_number);
            if (metric_index >= 5 || metric_index >= entry.active_float_metrics) return 0.0f;
            return entry.metrics[metric_index];
        } catch (...) { return 0.0f; }
    }

    bool set_address_metric(uint64_t global_page_number, size_t metric_index, uint64_t address_value) {
        try {
            PageEntry& entry = get_or_create_page_entry(global_page_number);
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

    uint64_t get_address_metric(uint64_t global_page_number, size_t metric_index) {
        try {
            PageEntry& entry = get_or_create_page_entry(global_page_number);
            if (metric_index < 5) return 0ULL;
            size_t addr_index = metric_index - 5;
            if (addr_index >= 8 || addr_index >= entry.active_address_metrics) return 0ULL;
            return entry.address_metrics[addr_index];
        } catch (...) { return 0ULL; }
    }

    bool add_metric(float default_value = 0.0f) {
        current_metric_count++;
        
        for (auto& [local_page_num, entry_ptr] : table) {
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

    void free_page(uint64_t global_page_number) {
        remove_page(global_page_number);
    }

    // Find free GLOBAL page number (allocate new global ID from this table's initial range)
    uint64_t find_free_page_number() const {
        uint64_t local_page_number;
        
        if (find_free_local_page_internal(local_page_number)) {
            // For new allocations, use the table's initial global page range
            uint64_t new_global_page = config.global_page_offset + local_page_number;
            return new_global_page;
        }
        
        // Cleanup attempts...
        std::cout << config.name << " full, attempting cleanup..." << std::endl;
        const_cast<PageTable*>(this)->trim_unused_pages();
        
        if (find_free_local_page_internal(local_page_number)) {
            uint64_t new_global_page = config.global_page_offset + local_page_number;
            std::cout << "Found free page " << new_global_page << " after cleanup" << std::endl;
            return new_global_page;
        }
        
        std::cout << config.name << " still full, attempting emergency cleanup..." << std::endl;
        const_cast<PageTable*>(this)->emergency_cleanup();
        
        if (find_free_local_page_internal(local_page_number)) {
            uint64_t new_global_page = config.global_page_offset + local_page_number;
            std::cout << "Found free page " << new_global_page << " after emergency cleanup" << std::endl;
            return new_global_page;
        }
        
        std::cerr << "CRITICAL ERROR: " << config.name << " completely exhausted! "
                  << "Pages allocated: " << table.size() << " / " << config.max_usable_pages << " usable" << std::endl;
        
        return UINT64_MAX;  // Indicates failure
    }

    bool has_free_pages() const {
        return table.size() < config.max_usable_pages;
    }

    bool try_find_free_page_number(uint64_t& global_page_number) const {
        uint64_t local_page_number;
        
        if (find_free_local_page_internal(local_page_number)) {
            global_page_number = config.global_page_offset + local_page_number;
            return true;
        }
        
        // Try cleanup...
        const_cast<PageTable*>(this)->trim_unused_pages();
        
        if (find_free_local_page_internal(local_page_number)) {
            global_page_number = config.global_page_offset + local_page_number;
            return true;
        }
        
        const_cast<PageTable*>(this)->emergency_cleanup();
        
        if (find_free_local_page_internal(local_page_number)) {
            global_page_number = config.global_page_offset + local_page_number;
            return true;
        }
        
        return false;
    }

    // Rest of existing methods remain the same...
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
        bool needs_index_update = false;
        
        for (const auto& [global_page_num, value] : updates) {
            if (set_address_metric(global_page_num, 6, value)) {
                needs_index_update = true;
            }
        }
        
        if (needs_index_update) {
            mark_index_dirty();
        }
    }

    // GETTERS
    size_t get_total_pages() const { return config.total_pages; }
    size_t get_metric_count() const { return current_metric_count; }
    std::string get_name() const { return config.name; }
    uint64_t get_global_page_offset() const { return config.global_page_offset; }
    uint64_t get_max_usable_pages() const { return config.max_usable_pages; }
    
    const std::unordered_map<uint64_t, std::unique_ptr<PageEntry>>& get_table() const { return table; }
    std::unordered_map<uint64_t, std::unique_ptr<PageEntry>>& get_table_mutable() { return table; }

    void print_statistics() const {
        auto stats = get_performance_stats();
        
        std::cout << config.name << " Statistics:\n";
        std::cout << "- Initial Global Page Range: " << config.global_page_offset 
                  << " - " << (config.global_page_offset + config.total_pages - 1) << " (for new allocations)\n";
        std::cout << "- Currently Holding: " << global_to_local_map.size() << " global pages\n";
        std::cout << "- Total allocated local slots: " << table.size() << " / " << config.max_usable_pages << " usable\n";
        std::cout << "- Memory efficiency: " << std::fixed << std::setprecision(2)
                  << (double(table.size()) / config.max_usable_pages * 100.0) << "% of usable space\n";
        std::cout << "- Reserved pages: " << (config.total_pages - config.max_usable_pages) << "\n";
        std::cout << "- Current metric count: " << current_metric_count << "\n";
        std::cout << "- Index built: " << (stats.index_built ? "Yes" : "No") << "\n";
        std::cout << "- Index size: " << stats.index_size << " entries\n";
        std::cout << "- Index hit ratio: " << std::fixed << std::setprecision(2) 
                  << (stats.get_hit_ratio() * 100.0) << "%\n";
        std::cout << "- Memory usage: " << (get_memory_usage() / 1024 / 1024) << " MB\n\n";
    }

    void cleanup() { 
        table.clear(); 
        global_to_local_map.clear();
        clear_index();
        entry_pool.clear();
    }
    
    size_t get_memory_usage() const {
        size_t base_size = sizeof(*this);
        size_t table_size = table.size() * (sizeof(uint64_t) + sizeof(std::unique_ptr<PageEntry>) + sizeof(PageEntry));
        size_t mapping_size = global_to_local_map.size() * (sizeof(uint64_t) * 2);
        size_t index_size = metric6_to_page_index.size() * (sizeof(uint64_t) * 2);
        size_t pool_size = entry_pool.memory_usage();
        
        return base_size + table_size + mapping_size + index_size + pool_size;
    }
    
    void trim_unused_pages() {
        std::vector<uint64_t> to_remove;
        
        for (const auto& [local_page_num, entry_ptr] : table) {
            if (entry_ptr && entry_ptr->mesi_state == MESIState::INVALID) {
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
                    to_remove.push_back(local_page_num);
                }
            }
        }
        
        for (uint64_t local_page_num : to_remove) {
            // Find and remove the global mapping too
            uint64_t global_page_to_remove = UINT64_MAX;
            for (const auto& [global_page, local_slot] : global_to_local_map) {
                if (local_slot == local_page_num) {
                    global_page_to_remove = global_page;
                    break;
                }
            }
            
            table.erase(local_page_num);
            if (global_page_to_remove != UINT64_MAX) {
                global_to_local_map.erase(global_page_to_remove);
            }
        }
        
        mark_index_dirty();
        std::cout << "Trimmed " << to_remove.size() << " unused pages from " << config.name << std::endl;
    }
    
    bool get_page_metric6_fast(uint64_t global_page_number, uint64_t& metric6_value) const noexcept {
        try {
            auto global_it = global_to_local_map.find(global_page_number);
            if (global_it == global_to_local_map.end()) return false;
            
            uint64_t local_page = global_it->second;
            if (local_page >= config.total_pages) return false;
            
            auto it = table.find(local_page);
            if (it == table.end() || !it->second) return false;
            
            const PageEntry& entry = *it->second;
            
            if (entry.active_address_metrics < 2) return false;
            
            metric6_value = entry.address_metrics[1];
            return true;
        } catch (...) {
            return false;
        }
    }
    
    void emergency_cleanup() {
        std::cout << "Starting emergency cleanup for " << config.name << "..." << std::endl;
        
        clear_index();
        trim_unused_pages();
        entry_pool.clear();
        
        std::unordered_map<uint64_t, std::unique_ptr<PageEntry>> temp;
        temp.swap(table);
        table.swap(temp);
        
        std::cout << "Emergency cleanup completed for " << config.name 
                  << ". Current pages: " << table.size() << std::endl;
    }

    double get_memory_pressure() const {
        return (double(table.size()) / config.max_usable_pages) * 100.0;
    }

    bool is_near_capacity(double threshold_percent = 90.0) const {
        return get_memory_pressure() >= threshold_percent;
    }
    
    // Check if migration is possible (in reserved space)
    bool can_accept_migration() const {
        return table.size() < config.total_pages; // Can use reserved space for migrations
    }
};

// Global Page Manager class
class GlobalPageManager {
private:
    std::unordered_map<uint64_t, PageTable*> global_to_table_map;
    std::vector<PageTable*> tables;
    mutable std::mutex registry_mutex;

public:
    GlobalPageManager() = default;
    
    // Register a table (no longer pre-populate ownership map)
    void register_table(PageTable* table, uint64_t start_global_page, uint64_t end_global_page) {
        std::lock_guard<std::mutex> lock(registry_mutex);
        
        tables.push_back(table);
        table->set_global_manager(this);
        
        std::cout << "Registered table " << table->get_name() 
                  << " with initial global page allocation range " << start_global_page << "-" << end_global_page << std::endl;
    }
    
    // Find which table currently owns a global page
    PageTable* find_table_for_global_page(uint64_t global_page) {
        std::lock_guard<std::mutex> lock(registry_mutex);
        
        // Search all tables to find who currently holds this global page
        for (PageTable* table : tables) {
            if (table->owns_global_page(global_page)) {
                global_to_table_map[global_page] = table; // Cache for faster future lookups
                return table;
            }
        }
        
        // Check cache for faster lookups
        auto it = global_to_table_map.find(global_page);
        if (it != global_to_table_map.end()) {
            // Verify the cached entry is still valid
            if (it->second->owns_global_page(global_page)) {
                return it->second;
            } else {
                // Cache is stale, remove it
                global_to_table_map.erase(it);
            }
        }
        
        return nullptr;
    }
    
    // Update registry when page ownership changes (called by migration methods)
    void update_page_ownership(uint64_t global_page, PageTable* new_owner) {
        std::lock_guard<std::mutex> lock(registry_mutex);
        global_to_table_map[global_page] = new_owner;
    }
    
    // Wrapper methods that automatically route to correct table
    bool set_mesi_state(uint64_t global_page, MESIState state) {
        PageTable* owner = find_table_for_global_page(global_page);
        if (owner) {
            return owner->set_mesi_state(global_page, state);
        }
        std::cerr << "Error: No table owns global page " << global_page << std::endl;
        return false;
    }
    
    MESIState get_mesi_state(uint64_t global_page) {
        PageTable* owner = find_table_for_global_page(global_page);
        if (owner) {
            return owner->get_mesi_state(global_page);
        }
        std::cerr << "Error: No table owns global page " << global_page << std::endl;
        return MESIState::INVALID;
    }
    
    bool set_metric(uint64_t global_page, size_t metric_index, float value) {
        PageTable* owner = find_table_for_global_page(global_page);
        if (owner) {
            return owner->set_metric(global_page, metric_index, value);
        }
        return false;
    }
    
    float get_metric(uint64_t global_page, size_t metric_index) {
        PageTable* owner = find_table_for_global_page(global_page);
        if (owner) {
            return owner->get_metric(global_page, metric_index);
        }
        return 0.0f;
    }
    
    bool set_address_metric(uint64_t global_page, size_t metric_index, uint64_t address_value) {
        PageTable* owner = find_table_for_global_page(global_page);
        if (owner) {
            return owner->set_address_metric(global_page, metric_index, address_value);
        }
        return false;
    }
    
    uint64_t get_address_metric(uint64_t global_page, size_t metric_index) {
        PageTable* owner = find_table_for_global_page(global_page);
        if (owner) {
            return owner->get_address_metric(global_page, metric_index);
        }
        return 0ULL;
    }
    
    // Address conversion methods - now searches all tables
    uint64_t address_to_page_number(uint64_t virtual_address) {
        // Find which table's address range this belongs to
        for (PageTable* table : tables) {
            if (table->is_valid_address(virtual_address)) {
                return table->address_to_page_number(virtual_address);
            }
        }
        throw std::out_of_range("Address not in any registered table");
    }
    
    uint64_t page_number_to_address(uint64_t global_page) {
        PageTable* owner = find_table_for_global_page(global_page);
        if (owner) {
            return owner->page_number_to_address(global_page);
        }
        throw std::out_of_range("Global page not found");
    }
    
    // Migration methods
    bool migrate_page(uint64_t global_page, PageTable* destination_table) {
        PageTable* source_table = find_table_for_global_page(global_page);
        if (!source_table) {
            std::cerr << "Error: No source table found for global page " << global_page << std::endl;
            return false;
        }
        
        if (source_table == destination_table) {
            std::cerr << "Error: Source and destination are the same table" << std::endl;
            return false;
        }
        
        return source_table->migrate_page_to(*destination_table, global_page);
    }
    
    bool exchange_pages(uint64_t global_page1, uint64_t global_page2) {
        PageTable* table1 = find_table_for_global_page(global_page1);
        PageTable* table2 = find_table_for_global_page(global_page2);
        
        if (!table1 || !table2) {
            std::cerr << "Error: Cannot find owner tables for page exchange" << std::endl;
            return false;
        }
        
        if (table1 == table2) {
            std::cerr << "Error: Both pages are in the same table" << std::endl;
            return false;
        }
        
        return table1->exchange_pages_with(*table2, global_page1, global_page2);
    }
    
    // Utility methods
    void print_ownership_map() const {
        std::lock_guard<std::mutex> lock(registry_mutex);
        
        std::cout << "\n=== Global Page Ownership Map (Live Scan) ===\n";
        std::unordered_map<PageTable*, std::vector<uint64_t>> table_pages;
        
        // Scan all tables to find their current global pages
        for (PageTable* table : tables) {
            const auto& global_map = table->global_to_local_map;
            for (const auto& [global_page, local_slot] : global_map) {
                table_pages[table].push_back(global_page);
            }
        }
        
        for (const auto& [table, pages] : table_pages) {
            std::cout << table->get_name() << " holds " << pages.size() << " global pages: ";
            for (size_t i = 0; i < std::min(pages.size(), size_t(10)); ++i) {
                std::cout << pages[i] << " ";
            }
            if (pages.size() > 10) {
                std::cout << "... (" << (pages.size() - 10) << " more)";
            }
            std::cout << "\n";
        }
        std::cout << "==========================================\n\n";
    }
    
    void print_all_statistics() const {
        std::cout << "\n=== ALL TABLE STATISTICS ===\n";
        for (PageTable* table : tables) {
            table->print_statistics();
        }
        std::cout << "============================\n\n";
    }
    
    // Find pages that can be migrated 
    std::vector<uint64_t> find_migratable_pages(PageTable* source_table) const {
        std::vector<uint64_t> migratable_pages;
        
        const auto& global_map = source_table->global_to_local_map;
        for (const auto& [global_page, local_slot] : global_map) {
            migratable_pages.push_back(global_page);
        }
        
        return migratable_pages;
    }
};

// IMPLEMENTATION of migration methods (now that GlobalPageManager is defined)
bool PageTable::migrate_page_to(PageTable& destination_table, uint64_t global_page_number) {
    // Check if we own this page
    if (!owns_global_page(global_page_number)) {
        std::cerr << "Error: " << config.name << " doesn't own global page " << global_page_number << std::endl;
        return false;
    }
    
    // Check if destination can accept the page
    if (!destination_table.can_accept_migration()) {
        std::cerr << "Error: Destination table " << destination_table.get_name() << " cannot accept migration" << std::endl;
        return false;
    }
    
    // Check if destination already has this page
    if (destination_table.owns_global_page(global_page_number)) {
        std::cerr << "Error: Destination table " << destination_table.get_name() << " already owns global page " << global_page_number << std::endl;
        return false;
    }
    
    // Extract page data
    auto page_data = extract_page_data(global_page_number);
    if (!page_data) {
        std::cerr << "Error: Cannot extract page data for global page " << global_page_number << std::endl;
        return false;
    }
    
    // Insert into destination table
    if (!destination_table.insert_migrated_page(global_page_number, std::move(page_data))) {
        std::cerr << "Error: Cannot insert page into destination table" << std::endl;
        return false;
    }
    
    // Remove from source table
    remove_page(global_page_number);
    
    // Notify global manager about ownership change
    if (global_manager) {
        global_manager->update_page_ownership(global_page_number, &destination_table);
    }
    
    std::cout << "Successfully migrated global page " << global_page_number 
              << " from " << config.name << " to " << destination_table.get_name() << std::endl;
    
    return true;
}

bool PageTable::exchange_pages_with(PageTable& other_table, uint64_t my_global_page, uint64_t their_global_page) {
    std::cout << "DEBUG: Attempting virtual page exchange between tables" << std::endl;
    std::cout << "  Exchanging global page " << my_global_page << " <-> " << their_global_page << std::endl;
    
    // Check ownership - now uses virtual ownership
    if (!owns_global_page(my_global_page)) {
        std::cerr << "ERROR: " << config.name << " doesn't currently hold global page " << my_global_page << std::endl;
        return false;
    }
    
    if (!other_table.owns_global_page(their_global_page)) {
        std::cerr << "ERROR: " << other_table.get_name() << " doesn't currently hold global page " << their_global_page << std::endl;
        return false;
    }
    
    // Extract both pages
    auto my_page_data = extract_page_data(my_global_page);
    auto their_page_data = other_table.extract_page_data(their_global_page);
    
    if (!my_page_data || !their_page_data) {
        std::cerr << "Error: Cannot extract page data for exchange" << std::endl;
        return false;
    }
    
    // Check if both tables can accept the exchange
    if (!can_accept_migration() || !other_table.can_accept_migration()) {
        std::cerr << "Error: One or both tables cannot accept migration" << std::endl;
        return false;
    }
    
    // Remove both pages from their current tables
    remove_page(my_global_page);
    other_table.remove_page(their_global_page);
    
    // Insert pages into their new homes (global page numbers stay the same)
    bool success1 = insert_migrated_page(their_global_page, std::move(their_page_data));
    bool success2 = other_table.insert_migrated_page(my_global_page, std::move(my_page_data));
    
    if (!success1 || !success2) {
        std::cerr << "Error: Failed to complete page exchange" << std::endl;
        return false;
    }
    
    // Notify global manager about ownership changes
    if (global_manager) {
        global_manager->update_page_ownership(my_global_page, &other_table);
        global_manager->update_page_ownership(their_global_page, this);
    }
    
    std::cout << "Successfully exchanged global pages:" << std::endl;
    std::cout << "  - Global page " << my_global_page << " moved from " << config.name << " to " << other_table.get_name() << std::endl;
    std::cout << "  - Global page " << their_global_page << " moved from " << other_table.get_name() << " to " << config.name << std::endl;
    
    return true;
}

#endif // TABLE_H
