#include <ap_int.h>
#include <hls_stream.h>
#include <hls_math.h>
#include <cmath>

// Optimized constants for ultra-low latency
const int MAX_PAGES = 500;        // Reduced from 1024
const int MAX_TOP_N = 250;          // Reduced from 32
const int COOLDOWN_BITS = 3;      // Reduced from 8
const int METRIC_BITS = 16;       // Reduced from 32
const int ENTROPY_WINDOW = 8;     // Reduced from 64

// EMA CMS Constants - much smaller
const int CMS_D = 4;              // Reduced from 5
const int CMS_W = 256;             // Reduced from 272
const int COUNTER_BITS = 8;
const ap_uint<COUNTER_BITS> MAX_COUNTER = 255;
const int EMA_ALPHA_FIXED = 153;  // 0.6 * 256 for EMA

// Reuse distance bins (4 bins total)
const int REUSE_BINS = 4;
const ap_uint<8> REUSE_THRESHOLDS[REUSE_BINS] = {15, 63, 255, 1023};

// Optimized data types
typedef ap_uint<32> page_id_t;
typedef ap_ufixed<16,8> metric_t;
typedef ap_uint<COOLDOWN_BITS> cooldown_t;
typedef ap_uint<2> mesi_state_t;
typedef ap_uint<3> entropy_sample_t; // Reduced bits
typedef ap_uint<COUNTER_BITS> cms_counter_t;
typedef ap_uint<16> timestamp_t;     // Much smaller timestamps

// MESI States
const mesi_state_t INVALID = 0;
const mesi_state_t SHARED = 1;
const mesi_state_t EXCLUSIVE = 2;
const mesi_state_t MODIFIED = 3;

// Simplified page entry - only essential metrics
struct PageEntry {
    page_id_t page_num;
    metric_t hotness;              // Single combined hotness score
    mesi_state_t mesi_state;
    cooldown_t cooldown_timer;
    timestamp_t last_access_time;
    ap_uint<1> valid;
    
    PageEntry() {
        #pragma HLS INLINE
        page_num = 0;
        hotness = 0;
        mesi_state = INVALID;
        cooldown_timer = 0;
        last_access_time = 0;
        valid = 0;
    }
};

// EMA CMS for recency-weighted activity tracking
struct EMA_CMS {
    cms_counter_t activity_table[CMS_D][CMS_W];
    
    EMA_CMS() {
        #pragma HLS ARRAY_PARTITION variable=activity_table complete dim=0
        
        INIT_EMA: for (int i = 0; i < CMS_D; i++) {
            #pragma HLS UNROLL
            for (int j = 0; j < CMS_W; j++) {
                #pragma HLS UNROLL
                activity_table[i][j] = 0;
            }
        }
    }
};

// Reuse distance CMS - tracks reuse patterns
struct ReuseCMS {
    cms_counter_t reuse_table[CMS_D][CMS_W];
    
    ReuseCMS() {
        #pragma HLS ARRAY_PARTITION variable=reuse_table complete dim=0

        INIT_REUSE: for (int i = 0; i < CMS_D; i++) {
            #pragma HLS UNROLL
            for (int j = 0; j < CMS_W; j++) {
                #pragma HLS UNROLL
                reuse_table[i][j] = 0;
            }
        }
    }
};

// Top page pair - simplified
struct TopPagePair {
    page_id_t page_num;
    metric_t hotness;
    
    TopPagePair() : page_num(0), hotness(0) {}
};

// Ultra-fast hash function
ap_uint<16> fast_hash(page_id_t page, ap_uint<4> seed) {
#pragma HLS INLINE
    ap_uint<32> x = page ^ (seed << 28);
    x ^= x >> 16;
    x *= 0x85ebca6b;
    x ^= x >> 13;
    return x.range(15, 0);
}

// Get reuse bin from time difference
ap_uint<2> get_reuse_bin(ap_uint<16> time_diff) {
#pragma HLS INLINE
    if (time_diff <= 15) return 0;        // High reuse
    if (time_diff <= 63) return 1;        // Medium reuse
    if (time_diff <= 255) return 2;       // Low reuse
    return 3;                              // Very low reuse
}

// Update EMA CMS on page access
void ema_cms_update(EMA_CMS& ema_cms, page_id_t page_id, cms_counter_t& activity_score) {
#pragma HLS INLINE
    
    activity_score = 0;
    
    EMA_UPDATE: for (int i = 0; i < CMS_D; i++) {
        #pragma HLS UNROLL
        
        ap_uint<16> hash_val = fast_hash(page_id, i);
        ap_uint<8> index = hash_val % CMS_W;
        
        // EMA update: new_value = old * α + (1-α) * MAX_COUNTER
        cms_counter_t old_val = ema_cms.activity_table[i][index];
        cms_counter_t new_val = (old_val * EMA_ALPHA_FIXED +
                                (256 - EMA_ALPHA_FIXED) * MAX_COUNTER) >> 8;
        ema_cms.activity_table[i][index] = new_val;
        
        // Track minimum for final score
        if (i == 0 || new_val < activity_score) {
            activity_score = new_val;
        }
    }
}

// Update reuse CMS
void reuse_cms_update(ReuseCMS& reuse_cms, page_id_t page_id, ap_uint<2> reuse_bin,
                     cms_counter_t& reuse_score) {
#pragma HLS INLINE
    
    reuse_score = 0;
    
    REUSE_UPDATE: for (int i = 0; i < CMS_D; i++) {
        #pragma HLS UNROLL
        
        // Hash (page_id, reuse_bin) combination
        ap_uint<16> hash_val = fast_hash(page_id ^ (reuse_bin << 28), i);
        ap_uint<8> index = hash_val % CMS_W;
        
        if (reuse_cms.reuse_table[i][index] < MAX_COUNTER) {
            reuse_cms.reuse_table[i][index]++;
        }
        
        // Get current reuse evidence for this bin
        cms_counter_t bin_count = reuse_cms.reuse_table[i][index];
        if (i == 0 || bin_count < reuse_score) {
            reuse_score = bin_count;
        }
    }
}

// Global decay for EMA CMS
void ema_cms_decay(EMA_CMS& ema_cms) {
#pragma HLS INLINE
    
    DECAY_OUTER: for (int i = 0; i < CMS_D; i++) {
        #pragma HLS UNROLL
        DECAY_INNER: for (int j = 0; j < CMS_W; j++) {
            #pragma HLS UNROLL
            ema_cms.activity_table[i][j] = (ema_cms.activity_table[i][j] * EMA_ALPHA_FIXED) >> 8;
        }
    }
}

// Fast entropy calculation with reduced window
metric_t calculate_fast_entropy(entropy_sample_t samples[ENTROPY_WINDOW]) {
#pragma HLS INLINE
    
    ap_uint<4> freq[8];  // Only 8 possible values for 3-bit samples
    #pragma HLS ARRAY_PARTITION variable=freq complete
    
    // Initialize
    INIT_FREQ_FAST: for (int i = 0; i < 8; i++) {
        #pragma HLS UNROLL
        freq[i] = 0;
    }
    
    // Count frequencies
    COUNT_FREQ_FAST: for (int i = 0; i < ENTROPY_WINDOW; i++) {
        #pragma HLS UNROLL
        freq[samples[i]]++;
    }
    
    // Simple entropy approximation
    metric_t entropy = 0;
    CALC_ENTROPY_FAST: for (int i = 0; i < 8; i++) {
        #pragma HLS UNROLL
        if (freq[i] > 0) {
            entropy += (metric_t)freq[i];
        }
    }
    
    return entropy / (metric_t)ENTROPY_WINDOW;
}

// Ultra-fast hotness calculation
void calculate_hotness(PageEntry& page, cms_counter_t activity_score,
                      cms_counter_t reuse_score, ap_uint<2> reuse_bin) {
#pragma HLS INLINE
    
    // Convert activity to normalized score
    metric_t activity_norm = (metric_t)activity_score / (metric_t)MAX_COUNTER;
    
    // Convert reuse bin to reuse score (smaller bin = higher reuse)
    metric_t reuse_norm;
    switch(reuse_bin) {
        case 0: reuse_norm = (metric_t)1.0; break;   // High reuse
        case 1: reuse_norm = (metric_t)0.7; break;   // Medium reuse
        case 2: reuse_norm = (metric_t)0.4; break;   // Low reuse
        case 3: reuse_norm = (metric_t)0.1; break;   // Very low reuse
        default: reuse_norm = (metric_t)0.1; break;
    }
    
    // Simple weighted combination
    page.hotness = (metric_t)0.7 * activity_norm + (metric_t)0.3 * reuse_norm;
}

// Main page access processing - highly optimized
void process_page_access_fast(PageEntry page_table[MAX_PAGES],
                             ap_uint<8> table_size,
                             EMA_CMS& ema_cms,
                             ReuseCMS& reuse_cms,
                             page_id_t page_id,
                             timestamp_t current_time) {
#pragma HLS INTERFACE m_axi port=page_table bundle=mem_port
#pragma HLS INTERFACE s_axilite port=table_size
#pragma HLS INTERFACE s_axilite port=page_id
#pragma HLS INTERFACE s_axilite port=current_time
#pragma HLS INTERFACE s_axilite port=return

#pragma HLS PIPELINE II=1

    // Update EMA CMS
    cms_counter_t activity_score;
    ema_cms_update(ema_cms, page_id, activity_score);
    
    // Find page and update
    FIND_UPDATE_FAST: for (ap_uint<8> i = 0; i < table_size; i++) {
        #pragma HLS PIPELINE II=1
        
        if (page_table[i].valid && page_table[i].page_num == page_id) {
            // Calculate reuse distance
            ap_uint<16> time_diff = current_time - page_table[i].last_access_time;
            ap_uint<2> reuse_bin = get_reuse_bin(time_diff);

            // Update reuse CMS
            cms_counter_t reuse_score;
            reuse_cms_update(reuse_cms, page_id, reuse_bin, reuse_score);

            // Update page
            calculate_hotness(page_table[i], activity_score, reuse_score, reuse_bin);
            page_table[i].last_access_time = current_time;
            break;
        }
    }
}

// Ultra-fast top N selection using partial sort
void get_top_pages_fast(PageEntry page_table[MAX_PAGES],
                       ap_uint<8> table_size,
                       ap_uint<3> topN,
                       TopPagePair out_pages[MAX_TOP_N],
                       ap_uint<3>& out_count) {
#pragma HLS INTERFACE m_axi port=page_table bundle=mem_port
#pragma HLS INTERFACE s_axilite port=table_size
#pragma HLS INTERFACE s_axilite port=topN
#pragma HLS INTERFACE m_axi port=out_pages bundle=mem_port
#pragma HLS INTERFACE s_axilite port=out_count
#pragma HLS INTERFACE s_axilite port=return

#pragma HLS PIPELINE II=1

    TopPagePair candidates[MAX_PAGES];
    #pragma HLS ARRAY_PARTITION variable=candidates cyclic factor=8
    
    ap_uint<8> valid_count = 0;
    
    // Collect valid pages
    COLLECT_FAST: for (ap_uint<8> i = 0; i < table_size; i++) {
        #pragma HLS PIPELINE II=1
        
        if (page_table[i].valid &&
            page_table[i].mesi_state != INVALID &&
            page_table[i].cooldown_timer == 0) {
            
            candidates[valid_count].page_num = page_table[i].page_num;
            candidates[valid_count].hotness = page_table[i].hotness;
            valid_count++;
        }
    }
    
    // Partial sort - only sort what we need
    PARTIAL_SORT: for (ap_uint<3> i = 0; i < topN && i < valid_count; i++) {
        #pragma HLS PIPELINE II=2
        
        ap_uint<8> max_idx = i;
        metric_t max_val = candidates[i].hotness;
        
        FIND_MAX: for (ap_uint<8> j = i + 1; j < valid_count; j++) {
            #pragma HLS PIPELINE II=1
            if (candidates[j].hotness > max_val) {
                max_idx = j;
                max_val = candidates[j].hotness;
            }
        }
        
        // Swap
        if (max_idx != i) {
            TopPagePair temp = candidates[i];
            candidates[i] = candidates[max_idx];
            candidates[max_idx] = temp;
        }
        
        out_pages[i] = candidates[i];
    }
    
    out_count = (topN < valid_count) ? topN : (ap_uint<3>)valid_count;
}

// Fast migration decision
void decide_migration_fast(TopPagePair hot_pages[MAX_TOP_N],
                          ap_uint<3> hot_count,
                          TopPagePair cold_pages[MAX_TOP_N],
                          ap_uint<3> cold_count,
                          page_id_t out_promote[MAX_TOP_N],
                          page_id_t out_evict[MAX_TOP_N],
                          ap_uint<3>& decision_count) {
#pragma HLS INTERFACE m_axi port=hot_pages bundle=mem_port
#pragma HLS INTERFACE s_axilite port=hot_count
#pragma HLS INTERFACE m_axi port=cold_pages bundle=mem_port
#pragma HLS INTERFACE s_axilite port=cold_count
#pragma HLS INTERFACE m_axi port=out_promote bundle=mem_port
#pragma HLS INTERFACE m_axi port=out_evict bundle=mem_port
#pragma HLS INTERFACE s_axilite port=decision_count
#pragma HLS INTERFACE s_axilite port=return

#pragma HLS PIPELINE II=1

    decision_count = 0;

    // Simple threshold-based decision
    const metric_t HOT_THRESHOLD = (metric_t)0.7;
    const metric_t COLD_THRESHOLD = (metric_t)0.3;
    
    DECISION_FAST: for (ap_uint<3> i = 0; i < hot_count && i < cold_count &&
                       decision_count < MAX_TOP_N; i++) {
        #pragma HLS PIPELINE II=1
        
        if (hot_pages[i].hotness > HOT_THRESHOLD &&
            cold_pages[cold_count-1-i].hotness < COLD_THRESHOLD) {

            out_promote[decision_count] = hot_pages[i].page_num;
            out_evict[decision_count] = cold_pages[cold_count-1-i].page_num;
            decision_count++;
        }
    }
}

// Fast cooldown update
void update_cooldown_fast(PageEntry page_table[MAX_PAGES],
                         ap_uint<8> table_size,
                         page_id_t promote_pages[MAX_TOP_N],
                         page_id_t evict_pages[MAX_TOP_N],
                         ap_uint<3> decision_count) {
#pragma HLS INTERFACE m_axi port=page_table bundle=mem_port
#pragma HLS INTERFACE s_axilite port=table_size
#pragma HLS INTERFACE m_axi port=promote_pages bundle=mem_port
#pragma HLS INTERFACE m_axi port=evict_pages bundle=mem_port
#pragma HLS INTERFACE s_axilite port=decision_count
#pragma HLS INTERFACE s_axilite port=return

#pragma HLS PIPELINE II=1

    const cooldown_t COOLDOWN_PERIOD = 4;

    // Decrement all cooldowns
    UPDATE_COOLDOWNS: for (ap_uint<8> i = 0; i < table_size; i++) {
        #pragma HLS PIPELINE II=1
        if (page_table[i].valid && page_table[i].cooldown_timer > 0) {
            page_table[i].cooldown_timer--;
        }
    }
    
    // Set cooldown for migrated pages
    SET_COOLDOWN_FAST: for (ap_uint<3> i = 0; i < decision_count; i++) {
        #pragma HLS PIPELINE II=1
        
        // Set cooldown for both promoted and evicted pages
        FIND_PROMOTE_FAST: for (ap_uint<8> j = 0; j < table_size; j++) {
            #pragma HLS PIPELINE II=1
            if (page_table[j].valid &&
                (page_table[j].page_num == promote_pages[i] ||
                 page_table[j].page_num == evict_pages[i])) {
                page_table[j].cooldown_timer = COOLDOWN_PERIOD;
            }
        }
    }
}

// Get cold pages (inverted hotness logic)
void get_cold_pages_fast(PageEntry page_table[MAX_PAGES],
                        ap_uint<8> table_size,
                        ap_uint<3> topN,
                        TopPagePair out_pages[MAX_TOP_N],
                        ap_uint<3>& out_count) {
#pragma HLS INTERFACE m_axi port=page_table bundle=mem_port
#pragma HLS INTERFACE s_axilite port=table_size
#pragma HLS INTERFACE s_axilite port=topN
#pragma HLS INTERFACE m_axi port=out_pages bundle=mem_port
#pragma HLS INTERFACE s_axilite port=out_count
#pragma HLS INTERFACE s_axilite port=return

    TopPagePair candidates[MAX_PAGES];
    #pragma HLS ARRAY_PARTITION variable=candidates cyclic factor=8

    ap_uint<8> valid_count = 0;

    // Collect valid pages
    COLLECT_COLD: for (ap_uint<8> i = 0; i < table_size; i++) {
        #pragma HLS PIPELINE II=1

        if (page_table[i].valid &&
            page_table[i].mesi_state != INVALID &&
            page_table[i].cooldown_timer == 0) {

            candidates[valid_count].page_num = page_table[i].page_num;
            candidates[valid_count].hotness = page_table[i].hotness;
            valid_count++;
        }
    }

    // Partial sort for COLDEST pages (minimum hotness)
    PARTIAL_SORT_COLD: for (ap_uint<3> i = 0; i < topN && i < valid_count; i++) {
        #pragma HLS PIPELINE II=2

        ap_uint<8> min_idx = i;
        metric_t min_val = candidates[i].hotness;

        FIND_MIN: for (ap_uint<8> j = i + 1; j < valid_count; j++) {
            #pragma HLS PIPELINE II=1
            if (candidates[j].hotness < min_val) {
                min_idx = j;
                min_val = candidates[j].hotness;
            }
        }

        // Swap
        if (min_idx != i) {
            TopPagePair temp = candidates[i];
            candidates[i] = candidates[min_idx];
            candidates[min_idx] = temp;
        }

        out_pages[i] = candidates[i];
    }

    out_count = (topN < valid_count) ? topN : (ap_uint<3>)valid_count;
}

// Main ultra-low latency controller
void hot_page_detector_controller(
    PageEntry page_table[MAX_PAGES],
    ap_uint<8> table_size,
    EMA_CMS& ema_cms,
    ReuseCMS& reuse_cms,
    entropy_sample_t entropy_samples[ENTROPY_WINDOW],
    page_id_t out_promote[MAX_TOP_N],
    page_id_t out_evict[MAX_TOP_N],
    ap_uint<3>& decision_count,
    timestamp_t& global_time) {
    
#pragma HLS INTERFACE m_axi port=page_table bundle=mem_port
#pragma HLS INTERFACE s_axilite port=table_size
#pragma HLS INTERFACE m_axi port=entropy_samples bundle=mem_port
#pragma HLS INTERFACE m_axi port=out_promote bundle=mem_port
#pragma HLS INTERFACE m_axi port=out_evict bundle=mem_port
#pragma HLS INTERFACE s_axilite port=decision_count
#pragma HLS INTERFACE s_axilite port=global_time
#pragma HLS INTERFACE s_axilite port=return

    global_time++;
    
    // Get hot and cold pages
    TopPagePair hot_pages[MAX_TOP_N];
    TopPagePair cold_pages[MAX_TOP_N];
    ap_uint<3> hot_count, cold_count;
    
    #pragma HLS ARRAY_PARTITION variable=hot_pages complete
    #pragma HLS ARRAY_PARTITION variable=cold_pages complete
    
    // Get top hot pages
    get_top_pages_fast(page_table, table_size, MAX_TOP_N, hot_pages, hot_count);

    // Get top cold pages
    get_cold_pages_fast(page_table, table_size, MAX_TOP_N, cold_pages, cold_count);

    // Make migration decisions
    decide_migration_fast(hot_pages, hot_count, cold_pages, cold_count,
                         out_promote, out_evict, decision_count);

    // Update cooldowns
    update_cooldown_fast(page_table, table_size, out_promote, out_evict, decision_count);

    // Periodic decay (every 16 cycles to reduce overhead)
    if ((global_time & 0xF) == 0) {
        ema_cms_decay(ema_cms);
    }
}
