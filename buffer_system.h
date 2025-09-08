#ifndef buffer_system
#define buffer_system

#pragma once
#include <queue>
#include <array>
#include <string>
#include <cstdint>  // for uint8_t
#include "ap_uint.h"

// Replace ap_uint<8> with uint8_t for standard C++
using FlitPayload = std::array<uint8_t, 68>; // 64B + 4B meta
struct FlitData {
    // Header fields
    ap_uint<16> tag;
    ap_uint<1> type;
    ap_uint<1> ak;
    ap_uint<1> byte_enable;
    ap_uint<1> size;
    ap_uint<4> reqcrd;
    ap_uint<4> datacrd;
    ap_uint<4> rspcrd;
    ap_uint<3> slot0;
    ap_uint<3> slot1;
    ap_uint<3> slot2;
    ap_uint<3> slot3;
    ap_uint<1> page_or_cacheline;
    ap_uint<2> host_or_device_or_all_data;
    
    // Data fields (extracted based on slot types)
    ap_uint<87> m2s_req[4];
    ap_uint<87> m2s_rwd[4];
    ap_uint<128> data[4];
    ap_uint<30> s2m_ndr[4][2];
    ap_uint<40> s2m_drs[4][3];
    ap_uint<512> full_cacheline;
    
    // CRC
    ap_uint<16> crc;
    
    // Validity flags for extracted data
    bool has_m2s_req[4];
    bool has_m2s_rwd[4];
    bool has_data[4];
    bool has_s2m_ndr[4];
    bool has_s2m_drs[4];
    bool has_full_cacheline;
};

class FlitChannel {
public:
    FlitChannel(const std::string& name,
                int tx_req_count, int rx_req_count,
                int tx_rsp_count, int rx_rsp_count);

    // Transmission interface
    bool transmit_request(FlitChannel& target);
    bool transmit_response(FlitChannel& target);

    void push_tx_request(const FlitPayload& flit);
    void push_tx_response(const FlitPayload& flit);

    // Receive processing
    void process_rx_request(int &n, int &z, bool &crc_verified, FlitData &parsed_data);
    void process_rx_response(int &n, int &z, bool &crc_verified, FlitData &parsed_data);

    // Parse credit from incoming flit (you will implement this)
    void parse_flit_and_apply_credit(const FlitPayload& flit, int &n, int &z, FlitData &parsed_data); // PLACEHOLDER

    // External buffer/credit query
    int get_remote_req_credit() const;
    int get_remote_rsp_credit() const;
    bool has_tx_req() const;
    bool has_tx_rsp() const;
    void reset_credits();

    std::string name;

private:
    // Buffers
    std::queue<FlitPayload> tx_req_buffer;
    std::queue<FlitPayload> rx_req_buffer;
    std::queue<FlitPayload> tx_rsp_buffer;
    std::queue<FlitPayload> rx_rsp_buffer;

    int max_rx_req;
    int max_rx_rsp;

    int current_rx_req_used = 0;
    int current_rx_rsp_used = 0;

    // Remote view of credit
    int remote_rx_req_credit = 0;
    int remote_rx_rsp_credit = 0;

    // Helpers
    void apply_credit_on_pop(std::queue<FlitPayload>& buf, int& used);
};

#endif
