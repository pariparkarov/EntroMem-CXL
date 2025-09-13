#include <ap_int.h>
#include <hls_stream.h>
#include <hls_math.h>
#include <cmath>

// Constants matching your metrics system
const int MAX_PAGES = 1024;
const int MAX_TOP_N = 32;
const int COOLDOWN_BITS = 8;
const int METRIC_BITS = 32;
const int ENTROPY_WINDOW = 64;
const int MIN_MIGRATION_INTERVAL = 4;
const int MAX_MIGRATION_INTERVAL = 256;

// CMS Constants
const int CMS_D = 5;           // Depth of Count-Min Sketch
const int CMS_W = 272;         // Width per row
const int COUNTER_BITS = 10;
const ap_uint<COUNTER_BITS> MAX_COUNTER = (1 << COUNTER_BITS) - 1;
const int EMA_ALPHA_FIXED = 205; // 0.8 * 256 for fixed-point arithmetic

// Data types
typedef ap_uint<64> page_id_t;
typedef ap_ufixed<32,16> metric_t;
typedef ap_uint<COOLDOWN_BITS> cooldown_t;
typedef ap_uint<2> mesi_state_t;
typedef ap_uint<8> entropy_sample_t;
typedef ap_uint<COUNTER_BITS> cms_counter_t;
typedef ap_uint<64> timestamp_t;

// MESI States
enum MESIState : mesi_state_t {
    INVALID = 0,
    SHARED = 1,
    EXCLUSIVE = 2,
    MODIFIED = 3
};

// Page entry structure with metrics
struct PageEntry {
    page_id_t page_num;
    metric_t metrics[5];  // [hotness, access_score, write_score, reuse_score, ema_score]
    mesi_state_t mesi_state;
    cooldown_t cooldown_timer;
    timestamp_t last_access_time;
    ap_int<16> reuse_distance;  // -1 for first access
    ap_uint<1> valid;
    
    PageEntry() {
        #pragma HLS INLINE
        page_num = 0;
        for (int i = 0; i < 5; i++) {
            #pragma HLS UNROLL
            metrics[i] = 0;
        }
        mesi_state = INVALID;
        cooldown_timer = 0;
        last_access_time = 0;
        reuse_distance = -1;
        valid = 0;
    }
};

// CMS Tables
struct CMSTable {
    cms_counter_t access_table[CMS_D][CMS_W];
    cms_counter_t write_table[CMS_D][CMS_W];
    
    CMSTable() {
        #pragma HLS ARRAY_PARTITION variable=access_table complete dim=1
        #pragma HLS ARRAY_PARTITION variable=write_table complete dim=1
        
        INIT_OUTER: for (int i = 0; i < CMS_D; i++) {
            #pragma HLS UNROLL
            INIT_INNER: for (int j = 0; j < CMS_W; j++) {
                #pragma HLS PIPELINE II=1
                access_table[i][j] = 0;
                write_table[i][j] = 0;
            }
        }
    }
};

// Reuse distance tracking
struct ReuseTracker {
    page_id_t lru_stack[MAX_PAGES];
    ap_uint<11> stack_size;
    
    ReuseTracker() {
        #pragma HLS ARRAY_PARTITION variable=lru_stack cyclic factor=4
        stack_size = 0;
        INIT_STACK: for (int i = 0; i < MAX_PAGES; i++) {
            #pragma HLS PIPELINE II=1
            lru_stack[i] = 0;
        }
    }
};

// Top page pair structure
struct TopPagePair {
    page_id_t page_num;
    metric_t metric_value;
    
    TopPagePair() : page_num(0), metric_value(0) {}
    TopPagePair(page_id_t pn, metric_t mv) : page_num(pn), metric_value(mv) {}
};

// Hash function implementation
ap_uint<32> hash_page(page_id_t page, ap_uint<8> seed) {
#pragma HLS INLINE
    
    // Simple hash function optimized for HLS
    ap_uint<64> hash_input = page ^ (ap_uint<64>(0x9e3779b97f4a7c15ULL) * seed);
    ap_uint<32> hash1 = hash_input.range(31, 0);
    ap_uint<32> hash2 = hash_input.range(63, 32);
    return hash1 ^ hash2;
}

// Update CMS access table
void cms_update_access(CMSTable& cms, page_id_t page_id, cms_counter_t& min_count) {
#pragma HLS INLINE
    
    min_count = MAX_COUNTER;
    
    CMS_UPDATE_LOOP: for (int i = 0; i < CMS_D; i++) {
        #pragma HLS UNROLL
        
        ap_uint<32> hash_val = hash_page(page_id, i);
        ap_uint<16> index = hash_val % CMS_W;
        
        if (cms.access_table[i][index] < MAX_COUNTER) {
            cms.access_table[i][index]++;
        }
        
        if (cms.access_table[i][index] < min_count) {
            min_count = cms.access_table[i][index];
        }
    }
}

// Update CMS write table
void cms_update_write(CMSTable& cms, page_id_t page_id, cms_counter_t& min_count) {
#pragma HLS INLINE
    
    min_count = MAX_COUNTER;
    
    CMS_WRITE_LOOP: for (int i = 0; i < CMS_D; i++) {
        #pragma HLS UNROLL
        
        ap_uint<32> hash_val = hash_page(page_id, i);
        ap_uint<16> index = hash_val % CMS_W;
        
        if (cms.write_table[i][index] < MAX_COUNTER) {
            cms.write_table[i][index]++;
        }
        
        if (cms.write_table[i][index] < min_count) {
            min_count = cms.write_table[i][index];
        }
    }
}

// Estimate from CMS
cms_counter_t cms_estimate_access(CMSTable& cms, page_id_t page_id) {
#pragma HLS INLINE
    
    cms_counter_t min_count = MAX_COUNTER;
    
    CMS_EST_LOOP: for (int i = 0; i < CMS_D; i++) {
        #pragma HLS UNROLL
        
        ap_uint<32> hash_val = hash_page(page_id, i);
        ap_uint<16> index = hash_val % CMS_W;
        
        if (cms.access_table[i][index] < min_count) {
            min_count = cms.access_table[i][index];
        }
    }
    
    return min_count;
}

// Update reuse distance
void update_reuse_distance(ReuseTracker& tracker, page_id_t page_id, ap_int<16>& distance) {
#pragma HLS INLINE
    
    distance = -1;  // Default for new pages
    
    // Find page in LRU stack
    FIND_PAGE: for (ap_uint<11> i = 0; i < tracker.stack_size; i++) {
        #pragma HLS PIPELINE II=1
        
        if (tracker.lru_stack[i] == page_id) {
            distance = i;
            
            // Move page to front
            SHIFT_STACK: for (ap_uint<11> j = i; j > 0; j--) {
                #pragma HLS PIPELINE II=1
                tracker.lru_stack[j] = tracker.lru_stack[j-1];
            }
            
            tracker.lru_stack[0] = page_id;
            return;
        }
    }
    
    // New page - add to front
    if (tracker.stack_size < MAX_PAGES) {
        // Shift existing entries
        SHIFT_NEW: for (ap_uint<11> i = tracker.stack_size; i > 0; i--) {
            #pragma HLS PIPELINE II=1
            tracker.lru_stack[i] = tracker.lru_stack[i-1];
        }
        
        tracker.lru_stack[0] = page_id;
        tracker.stack_size++;
    } else {
        // Stack full - shift and insert at front
        SHIFT_FULL: for (ap_uint<11> i = MAX_PAGES-1; i > 0; i--) {
            #pragma HLS PIPELINE II=1
            tracker.lru_stack[i] = tracker.lru_stack[i-1];
        }
        
        tracker.lru_stack[0] = page_id;
    }
}

// Decay CMS tables
void decay_cms_tables(CMSTable& cms) {
#pragma HLS INLINE
    
    DECAY_ACCESS_OUTER: for (int i = 0; i < CMS_D; i++) {
        #pragma HLS UNROLL
        DECAY_ACCESS_INNER: for (int j = 0; j < CMS_W; j++) {
            #pragma HLS PIPELINE II=1
            // Fixed-point decay: multiply by 0.8
            cms.access_table[i][j] = (cms.access_table[i][j] * EMA_ALPHA_FIXED) >> 8;
        }
    }
    
    DECAY_WRITE_OUTER: for (int i = 0; i < CMS_D; i++) {
        #pragma HLS UNROLL
        DECAY_WRITE_INNER: for (int j = 0; j < CMS_W; j++) {
            #pragma HLS PIPELINE II=1
            cms.write_table[i][j] = (cms.write_table[i][j] * EMA_ALPHA_FIXED) >> 8;
        }
    }
}

// Calculate Shannon entropy
metric_t calculate_shannon_entropy(entropy_sample_t samples[ENTROPY_WINDOW]) {
#pragma HLS INLINE
    
    ap_uint<8> freq[256];
    #pragma HLS ARRAY_PARTITION variable=freq complete
    
    // Initialize frequency array
    INIT_FREQ: for (int i = 0; i < 256; i++) {
        #pragma HLS UNROLL
        freq[i] = 0;
    }
    
    // Count frequencies
    COUNT_FREQ: for (int i = 0; i < ENTROPY_WINDOW; i++) {
        #pragma HLS PIPELINE II=1
        freq[samples[i]]++;
    }
    
    // Calculate entropy using lookup table for log2
    metric_t entropy = 0;
    CALC_ENTROPY: for (int i = 0; i < 256; i++) {
        #pragma HLS PIPELINE II=1
        if (freq[i] > 0) {
            metric_t prob = (metric_t)freq[i] / ENTROPY_WINDOW;
            entropy -= prob * hls::log2(prob);
        }
    }
    
    return entropy;
}

// Update hotness metrics for a page
void update_page_hotness(PageEntry& page_entry, 
                        CMSTable& cms,
                        ap_int<16> reuse_distance,
                        timestamp_t current_time) {
#pragma HLS INLINE
    
    page_id_t page_id = page_entry.page_num;
    
    // Get access and write counts from CMS
    cms_counter_t access_count = cms_estimate_access(cms, page_id);
    cms_counter_t write_count = 0;  // Would need similar estimate for write
    
    // Calculate component scores
    metric_t access_score = (metric_t)access_count / MAX_COUNTER;
    metric_t write_score = (metric_t)write_count / MAX_COUNTER;
    
    // Reuse distance score using exponential decay
    metric_t reuse_score;
    if (reuse_distance < 0) {
        reuse_score = 0.5;  // First access
    } else {
        // Approximate exp(-0.05 * reuse_distance) using shift and lookup
        if (reuse_distance > 100) {
            reuse_score = 0.01;
        } else {
            reuse_score = hls::exp(-0.05f * reuse_distance);
        }
    }
    
    // EMA score update
    metric_t current_ema = page_entry.metrics[4];  // Previous EMA score
    metric_t new_ema = (EMA_ALPHA_FIXED * current_ema + (256 - EMA_ALPHA_FIXED)) / 256;
    
    // Calculate weighted hotness
    metric_t hotness = 0.4f * access_score + 0.2f * write_score + 
                      0.2f * reuse_score + 0.2f * new_ema;
    
    // Store all metrics
    page_entry.metrics[0] = hotness;
    page_entry.metrics[1] = access_score;
    page_entry.metrics[2] = write_score;
    page_entry.metrics[3] = reuse_score;
    page_entry.metrics[4] = new_ema;
    page_entry.last_access_time = current_time;
    page_entry.reuse_distance = reuse_distance;
}

// Process page access and update metrics
void process_page_access(PageEntry page_table[MAX_PAGES],
                        ap_uint<11> table_size,
                        CMSTable& cms,
                        ReuseTracker& reuse_tracker,
                        page_id_t page_id,
                        ap_uint<1> is_write,
                        timestamp_t current_time) {
#pragma HLS INTERFACE m_axi port=page_table bundle=mem_port
#pragma HLS INTERFACE s_axilite port=table_size
#pragma HLS INTERFACE s_axilite port=page_id
#pragma HLS INTERFACE s_axilite port=is_write
#pragma HLS INTERFACE s_axilite port=current_time
#pragma HLS INTERFACE s_axilite port=return

    // Update CMS tables
    cms_counter_t access_min_count, write_min_count;
    cms_update_access(cms, page_id, access_min_count);
    
    if (is_write) {
        cms_update_write(cms, page_id, write_min_count);
    }
    
    // Update reuse distance
    ap_int<16> reuse_distance;
    update_reuse_distance(reuse_tracker, page_id, reuse_distance);
    
    // Find and update page in table
    FIND_UPDATE_PAGE: for (ap_uint<11> i = 0; i < table_size; i++) {
        #pragma HLS PIPELINE II=1
        
        if (page_table[i].valid && page_table[i].page_num == page_id) {
            update_page_hotness(page_table[i], cms, reuse_distance, current_time);
            break;
        }
    }
}

// Get top hot pages
void get_top_hot_pages(PageEntry page_table[MAX_PAGES],
                      ap_uint<11> table_size,
                      ap_uint<6> topN,
                      ap_uint<3> metric_index,
                      TopPagePair out_top_pages[MAX_TOP_N],
                      ap_uint<6>& out_count) {
#pragma HLS INTERFACE m_axi port=page_table bundle=mem_port
#pragma HLS INTERFACE s_axilite port=table_size
#pragma HLS INTERFACE s_axilite port=topN
#pragma HLS INTERFACE s_axilite port=metric_index
#pragma HLS INTERFACE m_axi port=out_top_pages bundle=mem_port
#pragma HLS INTERFACE s_axilite port=out_count
#pragma HLS INTERFACE s_axilite port=return

    TopPagePair temp_pages[MAX_PAGES];
    #pragma HLS ARRAY_PARTITION variable=temp_pages cyclic factor=4
    
    ap_uint<11> valid_count = 0;
    
    // Collect valid pages with non-zero cooldown check
    COLLECT_PAGES: for (ap_uint<11> i = 0; i < table_size; i++) {
        #pragma HLS PIPELINE II=1
        
        PageEntry entry = page_table[i];
        
        if (entry.valid && 
            entry.mesi_state != INVALID && 
            entry.cooldown_timer == 0) {
            
            temp_pages[valid_count].page_num = entry.page_num;
            temp_pages[valid_count].metric_value = entry.metrics[metric_index];
            valid_count++;
        }
    }
    
    // Partial sort to find top N
    SORT_OUTER: for (ap_uint<6> i = 0; i < topN && i < valid_count; i++) {
        #pragma HLS LOOP_TRIPCOUNT min=4 max=32
        
        ap_uint<11> max_idx = i;
        metric_t max_val = temp_pages[i].metric_value;
        
        SORT_INNER: for (ap_uint<11> j = i + 1; j < valid_count; j++) {
            #pragma HLS PIPELINE II=1
            #pragma HLS LOOP_TRIPCOUNT min=0 max=1024
            
            if (temp_pages[j].metric_value > max_val) {
                max_idx = j;
                max_val = temp_pages[j].metric_value;
            }
        }
        
        if (max_idx != i) {
            TopPagePair temp = temp_pages[i];
            temp_pages[i] = temp_pages[max_idx];
            temp_pages[max_idx] = temp;
        }
        
        out_top_pages[i] = temp_pages[i];
    }
    
    out_count = (topN < valid_count) ? topN : valid_count;
}

// Get top cold pages
void get_top_cold_pages(PageEntry page_table[MAX_PAGES],
                       ap_uint<11> table_size,
                       ap_uint<6> topN,
                       ap_uint<3> metric_index,
                       TopPagePair out_cold_pages[MAX_TOP_N],
                       ap_uint<6>& out_count) {
#pragma HLS INTERFACE m_axi port=page_table bundle=mem_port
#pragma HLS INTERFACE s_axilite port=table_size
#pragma HLS INTERFACE s_axilite port=topN
#pragma HLS INTERFACE s_axilite port=metric_index
#pragma HLS INTERFACE m_axi port=out_cold_pages bundle=mem_port
#pragma HLS INTERFACE s_axilite port=out_count
#pragma HLS INTERFACE s_axilite port=return

    TopPagePair temp_pages[MAX_PAGES];
    #pragma HLS ARRAY_PARTITION variable=temp_pages cyclic factor=4
    
    ap_uint<11> valid_count = 0;
    
    COLLECT_COLD_PAGES: for (ap_uint<11> i = 0; i < table_size; i++) {
        #pragma HLS PIPELINE II=1
        
        PageEntry entry = page_table[i];
        
        if (entry.valid && 
            entry.mesi_state != INVALID && 
            entry.cooldown_timer == 0) {
            
            temp_pages[valid_count].page_num = entry.page_num;
            temp_pages[valid_count].metric_value = entry.metrics[metric_index];
            valid_count++;
        }
    }
    
    // Sort for coldest pages
    SORT_COLD_OUTER: for (ap_uint<6> i = 0; i < topN && i < valid_count; i++) {
        #pragma HLS LOOP_TRIPCOUNT min=4 max=32
        
        ap_uint<11> min_idx = i;
        metric_t min_val = temp_pages[i].metric_value;
        
        SORT_COLD_INNER: for (ap_uint<11> j = i + 1; j < valid_count; j++) {
            #pragma HLS PIPELINE II=1
            #pragma HLS LOOP_TRIPCOUNT min=0 max=1024
            
            if (temp_pages[j].metric_value < min_val) {
                min_idx = j;
                min_val = temp_pages[j].metric_value;
            }
        }
        
        if (min_idx != i) {
            TopPagePair temp = temp_pages[i];
            temp_pages[i] = temp_pages[min_idx];
            temp_pages[min_idx] = temp;
        }
        
        out_cold_pages[i] = temp_pages[i];
    }
    
    out_count = (topN < valid_count) ? topN : valid_count;
}

// Decide promotions and evictions
void decide_promotions_evictions(TopPagePair hot_pages[MAX_TOP_N],
                                ap_uint<6> hot_count,
                                TopPagePair cold_pages[MAX_TOP_N],
                                ap_uint<6> cold_count,
                                page_id_t out_promote_pages[MAX_TOP_N],
                                page_id_t out_evict_pages[MAX_TOP_N],
                                ap_uint<6>& out_decision_count) {
#pragma HLS INTERFACE m_axi port=hot_pages bundle=mem_port
#pragma HLS INTERFACE s_axilite port=hot_count
#pragma HLS INTERFACE m_axi port=cold_pages bundle=mem_port
#pragma HLS INTERFACE s_axilite port=cold_count
#pragma HLS INTERFACE m_axi port=out_promote_pages bundle=mem_port
#pragma HLS INTERFACE m_axi port=out_evict_pages bundle=mem_port
#pragma HLS INTERFACE s_axilite port=out_decision_count
#pragma HLS INTERFACE s_axilite port=return

    ap_uint<6> decision_count = 0;
    ap_uint<6> hot_idx = 0;
    ap_uint<6> cold_idx = cold_count;
    
    DECISION_LOOP: while (hot_idx < hot_count && cold_idx > 0 && 
                         decision_count < MAX_TOP_N) {
        #pragma HLS PIPELINE II=1
        #pragma HLS LOOP_TRIPCOUNT min=0 max=32
        
        cold_idx--;
        
        metric_t hotness = hot_pages[hot_idx].metric_value;
        metric_t coldness = cold_pages[cold_idx].metric_value;
        
        if (hotness > coldness) {
            out_promote_pages[decision_count] = hot_pages[hot_idx].page_num;
            out_evict_pages[decision_count] = cold_pages[cold_idx].page_num;
            decision_count++;
            hot_idx++;
        } else {
            break;
        }
    }
    
    out_decision_count = decision_count;
}

// Update cooldown timers
void update_cooldown_timers(PageEntry page_table[MAX_PAGES],
                           ap_uint<11> table_size,
                           page_id_t promoted_pages[MAX_TOP_N],
                           page_id_t evicted_pages[MAX_TOP_N],
                           ap_uint<6> decision_count,
                           cooldown_t cooldown_period) {
#pragma HLS INTERFACE m_axi port=page_table bundle=mem_port
#pragma HLS INTERFACE s_axilite port=table_size
#pragma HLS INTERFACE m_axi port=promoted_pages bundle=mem_port
#pragma HLS INTERFACE m_axi port=evicted_pages bundle=mem_port
#pragma HLS INTERFACE s_axilite port=decision_count
#pragma HLS INTERFACE s_axilite port=cooldown_period
#pragma HLS INTERFACE s_axilite port=return

    // Decrement all cooldown timers
    UPDATE_ALL_TIMERS: for (ap_uint<11> i = 0; i < table_size; i++) {
        #pragma HLS PIPELINE II=1
        if (page_table[i].valid && page_table[i].cooldown_timer > 0) {
            page_table[i].cooldown_timer--;
        }
    }
    
    // Set cooldown for promoted and evicted pages
    SET_COOLDOWN: for (ap_uint<6> i = 0; i < decision_count; i++) {
        #pragma HLS PIPELINE II=1
        
        page_id_t promote_page = promoted_pages[i];
        page_id_t evict_page = evicted_pages[i];
        
        // Set cooldown for promoted page
        FIND_PROMOTE: for (ap_uint<11> j = 0; j < table_size; j++) {
            #pragma HLS PIPELINE II=1
            if (page_table[j].valid && page_table[j].page_num == promote_page) {
                page_table[j].cooldown_timer = cooldown_period;
                break;
            }
        }
        
        // Set cooldown for evicted page
        FIND_EVICT: for (ap_uint<11> j = 0; j < table_size; j++) {
            #pragma HLS PIPELINE II=1
            if (page_table[j].valid && page_table[j].page_num == evict_page) {
                page_table[j].cooldown_timer = cooldown_period;
                break;
            }
        }
    }
}

// Adaptive migration parameters
void calculate_migration_params(metric_t entropy, 
                               ap_uint<8>& migration_interval,
                               ap_uint<6>& top_n) {
#pragma HLS INLINE
    
    metric_t normalized_entropy = entropy / 8.0f;
    
    migration_interval = MIN_MIGRATION_INTERVAL + 
                        (ap_uint<8>)((1.0f - normalized_entropy) * 
                        (MAX_MIGRATION_INTERVAL - MIN_MIGRATION_INTERVAL));
    
    top_n = 4 + (ap_uint<6>)((1.0f - normalized_entropy) * (MAX_TOP_N - 4));
}

// Advance time tick and perform decay
void advance_time_tick(CMSTable& cms, 
                      PageEntry page_table[MAX_PAGES], 
                      ap_uint<11> table_size,
                      timestamp_t& global_time) {
#pragma HLS INTERFACE s_axilite port=global_time
#pragma HLS INTERFACE s_axilite port=table_size
#pragma HLS INTERFACE m_axi port=page_table bundle=mem_port
#pragma HLS INTERFACE s_axilite port=return

    global_time++;
    
    // Decay CMS tables
    decay_cms_tables(cms);
    
    // Decay EMA scores for all pages
    DECAY_EMA: for (ap_uint<11> i = 0; i < table_size; i++) {
        #pragma HLS PIPELINE II=1
        if (page_table[i].valid) {
            page_table[i].metrics[4] = (page_table[i].metrics[4] * EMA_ALPHA_FIXED) >> 8;
        }
    }
}

// Main hot page detector and migration controller
void hot_page_detector_controller(
    PageEntry page_table[MAX_PAGES],
    ap_uint<11> table_size,
    CMSTable& cms_tables,
    ReuseTracker& reuse_tracker,
    entropy_sample_t entropy_samples[ENTROPY_WINDOW],
    page_id_t out_promote_pages[MAX_TOP_N],
    page_id_t out_evict_pages[MAX_TOP_N],
    ap_uint<6>& out_decision_count,
    cooldown_t cooldown_period,
    timestamp_t& global_time,
    ap_uint<3> metric_index) {
    
#pragma HLS INTERFACE m_axi port=page_table bundle=mem_port
#pragma HLS INTERFACE s_axilite port=table_size
#pragma HLS INTERFACE m_axi port=entropy_samples bundle=mem_port
#pragma HLS INTERFACE m_axi port=out_promote_pages bundle=mem_port
#pragma HLS INTERFACE m_axi port=out_evict_pages bundle=mem_port
#pragma HLS INTERFACE s_axilite port=out_decision_count
#pragma HLS INTERFACE s_axilite port=cooldown_period
#pragma HLS INTERFACE s_axilite port=global_time
#pragma HLS INTERFACE s_axilite port=metric_index
#pragma HLS INTERFACE s_axilite port=return

    // Calculate entropy and adaptive parameters
    metric_t current_entropy = calculate_shannon_entropy(entropy_samples);
    ap_uint<8> migration_interval;
    ap_uint<6> adaptive_topN;
    calculate_migration_params(current_entropy, migration_interval, adaptive_topN);
    
    // Get hot and cold pages
    TopPagePair hot_pages[MAX_TOP_N];
    TopPagePair cold_pages[MAX_TOP_N];
    ap_uint<6> hot_count, cold_count;
    
    #pragma HLS ARRAY_PARTITION variable=hot_pages complete
    #pragma HLS ARRAY_PARTITION variable=cold_pages complete
    
    get_top_hot_pages(page_table, table_size, adaptive_topN, metric_index, 
                      hot_pages, hot_count);
    get_top_cold_pages(page_table, table_size, adaptive_topN, metric_index, 
                       cold_pages, cold_count);
    
    // Decide promotions and evictions
    decide_promotions_evictions(hot_pages, hot_count, cold_pages, cold_count,
                               out_promote_pages, out_evict_pages, out_decision_count);
    
    // Update cooldown timers
    update_cooldown_timers(page_table, table_size, out_promote_pages, out_evict_pages,
                          out_decision_count, cooldown_period);
    
    // Advance global time and decay
    advance_time_tick(cms_tables, page_table, table_size, global_time);
}
