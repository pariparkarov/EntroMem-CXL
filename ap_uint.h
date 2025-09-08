#ifndef AP_UINT_H
#define AP_UINT_H

#include <cstdint>
#include <stdexcept>
#include <iostream>
#include <limits>
#include <type_traits>
#include <cstring>
#include <algorithm>
#include <memory>

// Forward declarations
template <size_t N> class ap_uint;

// Dynamic ap_uint class - moved outside to avoid nested template issues
class DynamicApUint {
private:
    std::unique_ptr<uint64_t[]> data;
    size_t width;
    size_t num_words;
    
    static constexpr size_t WORD_BITS = 64;

    void mask_unused_bits() {
        if (width == 0) return;
        const size_t unused = (num_words * WORD_BITS) - width;
        if (unused > 0) {
            uint64_t mask = ~uint64_t(0) >> unused;
            data[num_words - 1] &= mask;
        }
    }

public:
    DynamicApUint(size_t w = 1) : width(w), num_words((w + WORD_BITS - 1) / WORD_BITS) {
        if (w == 0) throw std::invalid_argument("Width must be positive");
        if (w > 4096) throw std::invalid_argument("Width too large");
        data = std::make_unique<uint64_t[]>(num_words);
        std::memset(data.get(), 0, num_words * sizeof(uint64_t));
    }

    DynamicApUint(const DynamicApUint& other) 
        : width(other.width), num_words(other.num_words) {
        data = std::make_unique<uint64_t[]>(num_words);
        std::memcpy(data.get(), other.data.get(), num_words * sizeof(uint64_t));
    }

    DynamicApUint& operator=(const DynamicApUint& other) {
        if (this != &other) {
            width = other.width;
            num_words = other.num_words;
            data = std::make_unique<uint64_t[]>(num_words);
            std::memcpy(data.get(), other.data.get(), num_words * sizeof(uint64_t));
        }
        return *this;
    }

    DynamicApUint(DynamicApUint&& other) noexcept
        : data(std::move(other.data)), width(other.width), num_words(other.num_words) {
        other.width = 0;
        other.num_words = 0;
    }

    DynamicApUint& operator=(DynamicApUint&& other) noexcept {
        if (this != &other) {
            data = std::move(other.data);
            width = other.width;
            num_words = other.num_words;
            other.width = 0;
            other.num_words = 0;
        }
        return *this;
    }

    size_t get_width() const { return width; }

    bool operator[](size_t idx) const {
        if (idx >= width) throw std::out_of_range("Bit index out of range");
        size_t word = idx / WORD_BITS;
        size_t bit = idx % WORD_BITS;
        return (data[word] >> bit) & 1ULL;
    }

    void set_bit(size_t idx, bool val) {
        if (idx >= width) throw std::out_of_range("Bit index out of range");
        size_t word = idx / WORD_BITS;
        size_t bit = idx % WORD_BITS;
        if (val)
            data[word] |= (1ULL << bit);
        else
            data[word] &= ~(1ULL << bit);
    }

    uint64_t to_uint64() const {
        return data[0];
    }

    explicit operator uint64_t() const {
        return to_uint64();
    }

    explicit operator int() const {
        return static_cast<int>(to_uint64());
    }

    explicit operator bool() const {
        for (size_t i = 0; i < num_words; ++i) {
            if (data[i] != 0) return true;
        }
        return false;
    }

    // Convert to fixed-width ap_uint if widths match
    template<size_t W>
    ap_uint<W> to_ap_uint() const {
        if (width != W) {
            throw std::invalid_argument("Width mismatch in to_ap_uint()");
        }
        ap_uint<W> result;
        size_t min_words = std::min(num_words, ap_uint<W>::get_num_words());
        for (size_t i = 0; i < min_words; ++i) {
            result.set_word(i, data[i]);
        }
        return result;
    }

    // Comparison operators
    template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
    bool operator==(T rhs) const {
        if constexpr (std::is_signed_v<T>) {
            if (rhs < 0) return false;
        }
        uint64_t rhs_val = static_cast<uint64_t>(rhs);
        
        if (data[0] != rhs_val) return false;
        
        for (size_t i = 1; i < num_words; ++i) {
            if (data[i] != 0) return false;
        }
        return true;
    }

    template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
    bool operator!=(T rhs) const { return !(*this == rhs); }

    template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
    bool operator<(T rhs) const {
        if constexpr (std::is_signed_v<T>) {
            if (rhs <= 0) return false;
        }
        uint64_t rhs_val = static_cast<uint64_t>(rhs);
        
        for (size_t i = num_words; i-- > 1;) {
            if (data[i] > 0) return false;
        }
        
        return data[0] < rhs_val;
    }

    template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
    bool operator<=(T rhs) const { return *this < rhs || *this == rhs; }

    template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
    bool operator>(T rhs) const { return !(*this <= rhs); }

    template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
    bool operator>=(T rhs) const { return !(*this < rhs); }

    // Stream output
    friend std::ostream& operator<<(std::ostream& os, const DynamicApUint& val) {
        for (int i = static_cast<int>(val.width) - 1; i >= 0; --i) {
            os << (val[i] ? '1' : '0');
        }
        return os;
    }
};

template <size_t N>
class ap_uint {
    static_assert(N > 0 && N <= 4096, "ap_uint bit width must be between 1 and 4096");

private:
    static constexpr size_t WORD_BITS = 64;
    static constexpr size_t NUM_WORDS = (N + WORD_BITS - 1) / WORD_BITS;
    uint64_t data[NUM_WORDS]{};

    void mask_unused_bits() {
        const size_t unused = (NUM_WORDS * WORD_BITS) - N;
        if (unused > 0) {
            uint64_t mask = ~uint64_t(0) >> unused;
            data[NUM_WORDS - 1] &= mask;
        }
    }

public:
    // Helper functions for friend access
    template<size_t M> friend class ap_uint;
    friend class DynamicApUint;
    
    const uint64_t* get_data() const { return data; }
    uint64_t* get_data() { return data; }
    static constexpr size_t get_num_words() { return NUM_WORDS; }
    void set_word(size_t idx, uint64_t val) { 
        if (idx < NUM_WORDS) data[idx] = val; 
    }

    // Constructors
    ap_uint() {
        std::memset(data, 0, sizeof(data));
    }

    ap_uint(uint64_t val) {
        std::memset(data, 0, sizeof(data));
        data[0] = val;
        mask_unused_bits();
    }

    // Add constructors for other integral types
    template<typename T, typename = std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, uint64_t>>>
    ap_uint(T val) {
        std::memset(data, 0, sizeof(data));
        if constexpr (std::is_signed_v<T>) {
            if (val < 0) {
                throw std::invalid_argument("Cannot construct ap_uint from negative value");
            }
        }
        data[0] = static_cast<uint64_t>(val);
        mask_unused_bits();
    }

    template <size_t M>
    ap_uint(const ap_uint<M>& other) {
        std::memset(data, 0, sizeof(data));
        const size_t min_words = std::min(NUM_WORDS, (M + WORD_BITS - 1) / WORD_BITS);
        for (size_t i = 0; i < min_words; ++i) {
            data[i] = other.get_data()[i];
        }
        mask_unused_bits();
    }

    // Constructor from DynamicApUint
    ap_uint(const DynamicApUint& other) {
        std::memset(data, 0, sizeof(data));
        if (other.get_width() != N) {
            throw std::invalid_argument("Width mismatch in ap_uint constructor from DynamicApUint");
        }
        // Copy bits one by one to ensure correctness
        for (size_t i = 0; i < N; ++i) {
            (*this)[i] = other[i];
        }
    }

    // Assignment
    ap_uint& operator=(uint64_t val) {
        std::memset(data, 0, sizeof(data));
        data[0] = val;
        mask_unused_bits();
        return *this;
    }

    // Add assignment for other integral types
    template<typename T, typename = std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, uint64_t>>>
    ap_uint& operator=(T val) {
        std::memset(data, 0, sizeof(data));
        if constexpr (std::is_signed_v<T>) {
            if (val < 0) {
                throw std::invalid_argument("Cannot assign negative value to ap_uint");
            }
        }
        data[0] = static_cast<uint64_t>(val);
        mask_unused_bits();
        return *this;
    }

    template <size_t M>
    ap_uint& operator=(const ap_uint<M>& other) {
        std::memset(data, 0, sizeof(data));
        const size_t min_words = std::min(NUM_WORDS, (M + WORD_BITS - 1) / WORD_BITS);
        for (size_t i = 0; i < min_words; ++i) {
            data[i] = other.get_data()[i];
        }
        mask_unused_bits();
        return *this;
    }

    // Assignment from DynamicApUint
    ap_uint& operator=(const DynamicApUint& other) {
        std::memset(data, 0, sizeof(data));
        size_t min_width = std::min(N, other.get_width());
        for (size_t i = 0; i < min_width; ++i) {
            (*this)[i] = other[i];
        }
        mask_unused_bits();
        return *this;
    }

    // Conversion operators
    explicit operator uint64_t() const {
        return data[0];
    }

    explicit operator bool() const {
        for (size_t i = 0; i < NUM_WORDS; ++i)
            if (data[i] != 0)
                return true;
        return false;
    }

    // Add conversion operators for other integral types
    template<typename T, typename = std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, uint64_t>>>
    explicit operator T() const {
        static_assert(sizeof(T) <= sizeof(uint64_t), "Type too large for conversion");
        return static_cast<T>(data[0]);
    }

    // Bit access const
    bool operator[](size_t idx) const {
        if (idx >= N) throw std::out_of_range("Bit index out of range");
        size_t word = idx / WORD_BITS;
        size_t bit = idx % WORD_BITS;
        return (data[word] >> bit) & 1ULL;
    }

    // Bit reference proxy for mutable bit access
    class BitRef {
        ap_uint& parent;
        size_t index;
    public:
        BitRef(ap_uint& p, size_t i) : parent(p), index(i) {}

        BitRef(const BitRef& other) : parent(other.parent), index(other.index) {}
        
        BitRef& operator=(const BitRef& other) {
            if (this != &other) {
                return (*this = static_cast<bool>(other));
            }
            return *this;
        }
        
        BitRef& operator=(BitRef&& other) {
            if (this != &other) {
                return (*this = static_cast<bool>(other));
            }
            return *this;
        }

        BitRef& operator=(bool val) {
            size_t word = index / WORD_BITS;
            size_t bit = index % WORD_BITS;
            if (val)
                parent.data[word] |= (1ULL << bit);
            else
                parent.data[word] &= ~(1ULL << bit);
            return *this;
        }

        BitRef& operator=(const ap_uint<1>& val) {
            return (*this = static_cast<bool>(val));
        }

        operator bool() const {
            size_t word = index / WORD_BITS;
            size_t bit = index % WORD_BITS;
            return (parent.data[word] >> bit) & 1ULL;
        }
    };

    BitRef operator[](size_t idx) {
        if (idx >= N) throw std::out_of_range("Bit index out of range");
        return BitRef(*this, idx);
    }

    // Forward declarations for proxy classes
    class RangeProxy;
    class ConstRangeProxy;

    // Range proxy (mutable) - IMPROVED VERSION
    class RangeProxy {
        ap_uint& parent;
        size_t high;
        size_t low;
        size_t width;
    public:
        RangeProxy(ap_uint& p, size_t h, size_t l) : parent(p), high(h), low(l), width(h - l + 1) {
            if (h >= N || l >= N || h < l)
                throw std::out_of_range("Invalid range");
        }

        size_t get_width() const { return width; }

        RangeProxy& operator=(uint64_t val) {
            for (size_t i = 0; i < width; ++i) {
                parent[low + i] = (val >> i) & 1ULL;
            }
            return *this;
        }

        template<typename T, typename = std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, uint64_t>>>
        RangeProxy& operator=(T val) {
            return (*this = static_cast<uint64_t>(val));
        }

        template <size_t M>
        RangeProxy& operator=(const ap_uint<M>& val) {
            if (M <= 64) {
                uint64_t int_val = static_cast<uint64_t>(val);
                for (size_t i = 0; i < width; ++i) {
                    parent[low + i] = (int_val >> i) & 1ULL;
                }
            } else {
                // For larger types, copy bit by bit
                for (size_t i = 0; i < width && i < M; ++i) {
                    parent[low + i] = val[i];
                }
                // Fill remaining bits with 0 if width > M
                for (size_t i = M; i < width; ++i) {
                    parent[low + i] = false;
                }
            }
            return *this;
        }

        // Assignment from DynamicApUint
        RangeProxy& operator=(const DynamicApUint& val) {
            size_t min_width = std::min(width, val.get_width());
            for (size_t i = 0; i < min_width; ++i) {
                parent[low + i] = val[i];
            }
            // Fill remaining bits with 0 if width > val.width
            for (size_t i = val.get_width(); i < width; ++i) {
                parent[low + i] = false;
            }
            return *this;
        }

        // Assignment from another RangeProxy - simplified
        template<size_t M>
        RangeProxy& operator=(const typename ap_uint<M>::RangeProxy& other) {
            auto other_dynamic = other.to_dynamic();
            return (*this = other_dynamic);
        }

        // Assignment from ConstRangeProxy - simplified
        template<size_t M>
        RangeProxy& operator=(const typename ap_uint<M>::ConstRangeProxy& other) {
            auto other_dynamic = other.to_dynamic();
            return (*this = other_dynamic);
        }

        uint64_t get() const {
            uint64_t val = 0;
            for (size_t i = 0; i < width && i < 64; ++i) {
                if (parent[low + i])
                    val |= (1ULL << i);
            }
            return val;
        }

        operator uint64_t() const {
            return get();
        }

        operator int() const {
            return static_cast<int>(get());
        }

        // Convert to DynamicApUint with actual width
        DynamicApUint to_dynamic() const {
            DynamicApUint result(width);
            for (size_t i = 0; i < width; ++i) {
                result.set_bit(i, parent[low + i]);
            }
            return result;
        }

        // Convert to ap_uint<W> where W = range width (compile-time check)
        template <size_t W>
        ap_uint<W> to_ap_uint() const {
            if (width != W) {
                throw std::invalid_argument("Width mismatch in to_ap_uint()");
            }
            
            ap_uint<W> result;
            for (size_t i = 0; i < W && (low + i) < N; ++i) {
                result[i] = parent[low + i];
            }
            return result;
        }

        // Comparison operators
        template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
        bool operator==(T rhs) const {
            if constexpr (std::is_signed_v<T>) {
                if (rhs < 0) return false;
            }
            return get() == static_cast<uint64_t>(rhs);
        }

        template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
        bool operator!=(T rhs) const { return !(*this == rhs); }

        template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
        bool operator<(T rhs) const {
            if constexpr (std::is_signed_v<T>) {
                if (rhs <= 0) return false;
            }
            return get() < static_cast<uint64_t>(rhs);
        }

        template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
        bool operator<=(T rhs) const { return *this < rhs || *this == rhs; }

        template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
        bool operator>(T rhs) const { return !(*this <= rhs); }

        template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
        bool operator>=(T rhs) const { return !(*this < rhs); }
    };

    // Const range proxy - IMPROVED VERSION
    class ConstRangeProxy {
        const ap_uint& parent;
        size_t high;
        size_t low;
        size_t width;
    public:
        ConstRangeProxy(const ap_uint& p, size_t h, size_t l) : parent(p), high(h), low(l), width(h - l + 1) {
            if (h >= N || l >= N || h < l)
                throw std::out_of_range("Invalid range");
        }

        size_t get_width() const { return width; }

        uint64_t get() const {
            uint64_t val = 0;
            for (size_t i = 0; i < width && i < 64; ++i) {
                if (parent[low + i])
                    val |= (1ULL << i);
            }
            return val;
        }

        operator uint64_t() const {
            return get();
        }

        operator int() const {
            return static_cast<int>(get());
        }

        // Convert to DynamicApUint with actual width
        DynamicApUint to_dynamic() const {
            DynamicApUint result(width);
            for (size_t i = 0; i < width; ++i) {
                result.set_bit(i, parent[low + i]);
            }
            return result;
        }

        template <size_t W>
        ap_uint<W> to_ap_uint() const {
            if (width != W) {
                throw std::invalid_argument("Width mismatch in to_ap_uint()");
            }
            
            ap_uint<W> result;
            for (size_t i = 0; i < W && (low + i) < N; ++i) {
                result[i] = parent[low + i];
            }
            return result;
        }

        // Bit access for iteration/assignment purposes
        bool operator[](size_t idx) const {
            if (idx >= width) throw std::out_of_range("Bit index out of range in ConstRangeProxy");
            return parent[low + idx];
        }

        // Comparison operators
        template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
        bool operator==(T rhs) const {
            if constexpr (std::is_signed_v<T>) {
                if (rhs < 0) return false;
            }
            return get() == static_cast<uint64_t>(rhs);
        }

        template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
        bool operator!=(T rhs) const { return !(*this == rhs); }

        template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
        bool operator<(T rhs) const {
            if constexpr (std::is_signed_v<T>) {
                if (rhs <= 0) return false;
            }
            return get() < static_cast<uint64_t>(rhs);
        }

        template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
        bool operator<=(T rhs) const { return *this < rhs || *this == rhs; }

        template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
        bool operator>(T rhs) const { return !(*this <= rhs); }

        template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
        bool operator>=(T rhs) const { return !(*this < rhs); }
    };

    RangeProxy range(size_t high, size_t low) {
        return RangeProxy(*this, high, low);
    }

    ConstRangeProxy range(size_t high, size_t low) const {
        return ConstRangeProxy(*this, high, low);
    }

    // Bitwise ops
    ap_uint operator~() const {
        ap_uint res;
        for (size_t i = 0; i < NUM_WORDS; ++i)
            res.data[i] = ~data[i];
        res.mask_unused_bits();
        return res;
    }

    ap_uint operator&(const ap_uint& rhs) const {
        ap_uint res;
        for (size_t i = 0; i < NUM_WORDS; ++i)
            res.data[i] = data[i] & rhs.data[i];
        return res;
    }

    ap_uint operator|(const ap_uint& rhs) const {
        ap_uint res;
        for (size_t i = 0; i < NUM_WORDS; ++i)
            res.data[i] = data[i] | rhs.data[i];
        return res;
    }

    ap_uint operator^(const ap_uint& rhs) const {
        ap_uint res;
        for (size_t i = 0; i < NUM_WORDS; ++i)
            res.data[i] = data[i] ^ rhs.data[i];
        return res;
    }

    ap_uint& operator&=(const ap_uint& rhs) {
        for (size_t i = 0; i < NUM_WORDS; ++i)
            data[i] &= rhs.data[i];
        return *this;
    }

    ap_uint& operator|=(const ap_uint& rhs) {
        for (size_t i = 0; i < NUM_WORDS; ++i)
            data[i] |= rhs.data[i];
        return *this;
    }

    ap_uint& operator^=(const ap_uint& rhs) {
        for (size_t i = 0; i < NUM_WORDS; ++i)
            data[i] ^= rhs.data[i];
        return *this;
    }

    // Shift ops
    ap_uint operator<<(size_t shift) const {
        if (shift >= N)
            return ap_uint(0);

        ap_uint res;
        size_t word_shift = shift / WORD_BITS;
        size_t bit_shift = shift % WORD_BITS;

        for (size_t i = NUM_WORDS; i-- > word_shift;) {
            uint64_t upper = data[i - word_shift] << bit_shift;
            uint64_t lower = 0;
            if (bit_shift != 0 && i - word_shift > 0) {
                lower = data[i - word_shift - 1] >> (WORD_BITS - bit_shift);
            }
            res.data[i] = upper | lower;
        }
        res.mask_unused_bits();
        return res;
    }

    ap_uint operator>>(size_t shift) const {
        if (shift >= N)
            return ap_uint(0);

        ap_uint res;
        size_t word_shift = shift / WORD_BITS;
        size_t bit_shift = shift % WORD_BITS;

        for (size_t i = 0; i + word_shift < NUM_WORDS; ++i) {
            uint64_t lower = data[i + word_shift] >> bit_shift;
            uint64_t upper = 0;
            if (bit_shift != 0 && (i + word_shift + 1) < NUM_WORDS) {
                upper = data[i + word_shift + 1] << (WORD_BITS - bit_shift);
            }
            res.data[i] = lower | upper;
        }
        res.mask_unused_bits();
        return res;
    }

    ap_uint& operator<<=(size_t shift) {
        *this = *this << shift;
        return *this;
    }

    ap_uint& operator>>=(size_t shift) {
        *this = *this >> shift;
        return *this;
    }

    // Comparison with ap_uint
    bool operator==(const ap_uint& rhs) const {
        for (size_t i = 0; i < NUM_WORDS; ++i)
            if (data[i] != rhs.data[i])
                return false;
        return true;
    }

    bool operator!=(const ap_uint& rhs) const {
        return !(*this == rhs);
    }

    bool operator<(const ap_uint& rhs) const {
        for (size_t i = NUM_WORDS; i-- > 0;) {
            if (data[i] < rhs.data[i])
                return true;
            if (data[i] > rhs.data[i])
                return false;
        }
        return false;
    }

    bool operator<=(const ap_uint& rhs) const {
        return *this < rhs || *this == rhs;
    }

    bool operator>(const ap_uint& rhs) const {
        return !(*this <= rhs);
    }

    bool operator>=(const ap_uint& rhs) const {
        return !(*this < rhs);
    }

    // Cross-type comparison operators with integral types
    template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
    bool operator==(T rhs) const {
        if constexpr (std::is_signed_v<T>) {
            if (rhs < 0) return false;
        }
        uint64_t rhs_val = static_cast<uint64_t>(rhs);
        
        if (data[0] != rhs_val) return false;
        
        for (size_t i = 1; i < NUM_WORDS; ++i) {
            if (data[i] != 0) return false;
        }
        return true;
    }

    template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
    bool operator!=(T rhs) const {
        return !(*this == rhs);
    }

    template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
    bool operator<(T rhs) const {
        if constexpr (std::is_signed_v<T>) {
            if (rhs <= 0) return false;
        }
        uint64_t rhs_val = static_cast<uint64_t>(rhs);
        
        for (size_t i = NUM_WORDS; i-- > 1;) {
            if (data[i] > 0) return false;
        }
        
        return data[0] < rhs_val;
    }

    template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
    bool operator<=(T rhs) const {
        return *this < rhs || *this == rhs;
    }

    template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
    bool operator>(T rhs) const {
        return !(*this <= rhs);
    }

    template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
    bool operator>=(T rhs) const {
        return !(*this < rhs);
    }

    // Arithmetic operators
    ap_uint operator+(const ap_uint& rhs) const {
        ap_uint res;
        uint64_t carry = 0;
        for (size_t i = 0; i < NUM_WORDS; ++i) {
            uint64_t sum = data[i] + rhs.data[i] + carry;
            carry = (sum < data[i] || (carry && sum == data[i])) ? 1 : 0;
            res.data[i] = sum;
        }
        res.mask_unused_bits();
        return res;
    }

    ap_uint& operator+=(const ap_uint& rhs) {
        *this = *this + rhs;
        return *this;
    }

    ap_uint operator-(const ap_uint& rhs) const {
        ap_uint res;
        uint64_t borrow = 0;
        for (size_t i = 0; i < NUM_WORDS; ++i) {
            uint64_t val = data[i];
            uint64_t sub = rhs.data[i] + borrow;
            if (val < sub) {
                res.data[i] = val + (~uint64_t(0)) - sub + 1;
                borrow = 1;
            } else {
                res.data[i] = val - sub;
                borrow = 0;
            }
        }
        res.mask_unused_bits();
        return res;
    }

    ap_uint& operator-=(const ap_uint& rhs) {
        *this = *this - rhs;
        return *this;
    }

    // Multiplication (simple, can be slow for big N)
    ap_uint operator*(const ap_uint& rhs) const {
        ap_uint res;
        for (size_t i = 0; i < N; ++i) {
            if ((*this)[i]) {
                // Add (rhs << i) to result
                ap_uint shifted_rhs = rhs << i;
                res += shifted_rhs;
            }
        }
        res.mask_unused_bits();
        return res;
    }

    ap_uint& operator*=(const ap_uint& rhs) {
        *this = *this * rhs;
        return *this;
    }

    // Division and modulo (basic long division)
    ap_uint operator/(const ap_uint& rhs) const {
        if (rhs == ap_uint(0))
            throw std::domain_error("Division by zero");

        ap_uint dividend = *this;
        ap_uint divisor = rhs;
        ap_uint quotient;
        ap_uint remainder;

        int n_bits = static_cast<int>(N);

        for (int i = n_bits - 1; i >= 0; --i) {
            remainder <<= 1;
            remainder[0] = dividend[i];
            if (remainder >= divisor) {
                remainder -= divisor;
                quotient[i] = true;
            }
        }
        quotient.mask_unused_bits();
        return quotient;
    }

    ap_uint operator%(const ap_uint& rhs) const {
        if (rhs == ap_uint(0))
            throw std::domain_error("Modulo by zero");

        ap_uint dividend = *this;
        ap_uint divisor = rhs;
        ap_uint quotient;
        ap_uint remainder;

        int n_bits = static_cast<int>(N);

        for (int i = n_bits - 1; i >= 0; --i) {
            remainder <<= 1;
            remainder[0] = dividend[i];
            if (remainder >= divisor) {
                remainder -= divisor;
                quotient[i] = true;
            }
        }
        remainder.mask_unused_bits();
        return remainder;
    }

    ap_uint& operator/=(const ap_uint& rhs) {
        *this = *this / rhs;
        return *this;
    }

    ap_uint& operator%=(const ap_uint& rhs) {
        *this = *this % rhs;
        return *this;
    }

    // Increment/decrement operators
    ap_uint& operator++() {
        *this += ap_uint(1);
        return *this;
    }

    ap_uint operator++(int) {
        ap_uint temp = *this;
        ++(*this);
        return temp;
    }

    ap_uint& operator--() {
        *this -= ap_uint(1);
        return *this;
    }

    ap_uint operator--(int) {
        ap_uint temp = *this;
        --(*this);
        return temp;
    }

    // Utility functions
    size_t count_leading_zeros() const {
        for (size_t i = NUM_WORDS; i-- > 0;) {
            if (data[i] != 0) {
                // Count leading zeros in this word
                uint64_t word = data[i];
                size_t clz = 0;
                for (int bit = 63; bit >= 0; --bit) {
                    if (word & (1ULL << bit)) break;
                    clz++;
                }
                return (i * WORD_BITS + clz) - (NUM_WORDS * WORD_BITS - N);
            }
        }
        return N; // All zeros
    }

    size_t count_ones() const {
        size_t count = 0;
        for (size_t i = 0; i < N; ++i) {
            if ((*this)[i]) count++;
        }
        return count;
    }

    // Stream output: binary MSB->LSB
    friend std::ostream& operator<<(std::ostream& os, const ap_uint& val) {
        for (int i = static_cast<int>(N) - 1; i >= 0; --i)
            os << (val[i] ? '1' : '0');
        return os;
    }
};

// Global comparison operators for T op ap_uint (reverse order)
template<size_t N, typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
bool operator==(T lhs, const ap_uint<N>& rhs) {
    return rhs == lhs;
}

template<size_t N, typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
bool operator!=(T lhs, const ap_uint<N>& rhs) {
    return rhs != lhs;
}

template<size_t N, typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
bool operator<(T lhs, const ap_uint<N>& rhs) {
    return rhs > lhs;
}

template<size_t N, typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
bool operator<=(T lhs, const ap_uint<N>& rhs) {
    return rhs >= lhs;
}

template<size_t N, typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
bool operator>(T lhs, const ap_uint<N>& rhs) {
    return rhs < lhs;
}

template<size_t N, typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
bool operator>=(T lhs, const ap_uint<N>& rhs) {
    return rhs <= lhs;
}

// Global comparison operators for T op RangeProxy (reverse order)
template<size_t N, typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
bool operator==(T lhs, const typename ap_uint<N>::RangeProxy& rhs) {
    return rhs == lhs;
}

template<size_t N, typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
bool operator!=(T lhs, const typename ap_uint<N>::RangeProxy& rhs) {
    return rhs != lhs;
}

template<size_t N, typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
bool operator<(T lhs, const typename ap_uint<N>::RangeProxy& rhs) {
    return rhs > lhs;
}

template<size_t N, typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
bool operator<=(T lhs, const typename ap_uint<N>::RangeProxy& rhs) {
    return rhs >= lhs;
}

template<size_t N, typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
bool operator>(T lhs, const typename ap_uint<N>::RangeProxy& rhs) {
    return rhs < lhs;
}

template<size_t N, typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
bool operator>=(T lhs, const typename ap_uint<N>::RangeProxy& rhs) {
    return rhs <= lhs;
}

// Global comparison operators for T op ConstRangeProxy (reverse order)
template<size_t N, typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
bool operator==(T lhs, const typename ap_uint<N>::ConstRangeProxy& rhs) {
    return rhs == lhs;
}

template<size_t N, typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
bool operator!=(T lhs, const typename ap_uint<N>::ConstRangeProxy& rhs) {
    return rhs != lhs;
}

template<size_t N, typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
bool operator<(T lhs, const typename ap_uint<N>::ConstRangeProxy& rhs) {
    return rhs > lhs;
}

template<size_t N, typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
bool operator<=(T lhs, const typename ap_uint<N>::ConstRangeProxy& rhs) {
    return rhs >= lhs;
}

template<size_t N, typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
bool operator>(T lhs, const typename ap_uint<N>::ConstRangeProxy& rhs) {
    return rhs < lhs;
}

template<size_t N, typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
bool operator>=(T lhs, const typename ap_uint<N>::ConstRangeProxy& rhs) {
    return rhs <= lhs;
}

// Global comparison operators for T op DynamicApUint (reverse order)
template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
bool operator==(T lhs, const DynamicApUint& rhs) {
    return rhs == lhs;
}

template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
bool operator!=(T lhs, const DynamicApUint& rhs) {
    return rhs != lhs;
}

template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
bool operator<(T lhs, const DynamicApUint& rhs) {
    return rhs > lhs;
}

template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
bool operator<=(T lhs, const DynamicApUint& rhs) {
    return rhs >= lhs;
}

template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
bool operator>(T lhs, const DynamicApUint& rhs) {
    return rhs < lhs;
}

template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
bool operator>=(T lhs, const DynamicApUint& rhs) {
    return rhs <= lhs;
}

// Arithmetic operators with integral types
template<size_t N, typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
ap_uint<N> operator+(const ap_uint<N>& lhs, T rhs) {
    return lhs + ap_uint<N>(rhs);
}

template<size_t N, typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
ap_uint<N> operator+(T lhs, const ap_uint<N>& rhs) {
    return ap_uint<N>(lhs) + rhs;
}

template<size_t N, typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
ap_uint<N> operator-(const ap_uint<N>& lhs, T rhs) {
    return lhs - ap_uint<N>(rhs);
}

template<size_t N, typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
ap_uint<N> operator-(T lhs, const ap_uint<N>& rhs) {
    return ap_uint<N>(lhs) - rhs;
}

template<size_t N, typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
ap_uint<N> operator*(const ap_uint<N>& lhs, T rhs) {
    return lhs * ap_uint<N>(rhs);
}

template<size_t N, typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
ap_uint<N> operator*(T lhs, const ap_uint<N>& rhs) {
    return ap_uint<N>(lhs) * rhs;
}

template<size_t N, typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
ap_uint<N> operator/(const ap_uint<N>& lhs, T rhs) {
    return lhs / ap_uint<N>(rhs);
}

template<size_t N, typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
ap_uint<N> operator/(T lhs, const ap_uint<N>& rhs) {
    return ap_uint<N>(lhs) / rhs;
}

template<size_t N, typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
ap_uint<N> operator%(const ap_uint<N>& lhs, T rhs) {
    return lhs % ap_uint<N>(rhs);
}

template<size_t N, typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
ap_uint<N> operator%(T lhs, const ap_uint<N>& rhs) {
    return ap_uint<N>(lhs) % rhs;
}

// Utility functions
template<size_t N>
std::string to_string(const ap_uint<N>& val, int base = 10) {
    if (base == 2) {
        std::string result;
        for (int i = static_cast<int>(N) - 1; i >= 0; --i) {
            result += (val[i] ? '1' : '0');
        }
        return result;
    } else if (base == 16) {
        std::string result = "0x";
        bool started = false;
        for (int i = static_cast<int>(N) - 1; i >= 0; i -= 4) {
            uint8_t nibble = 0;
            for (int j = 0; j < 4 && (i - j) >= 0; ++j) {
                if (val[i - j]) {
                    nibble |= (1 << j);
                }
            }
            if (nibble != 0 || started || i < 4) {
                result += "0123456789ABCDEF"[nibble];
                started = true;
            }
        }
        if (!started) result += '0';
        return result;
    } else if (base == 10) {
        if (static_cast<bool>(val) == false) return "0";
        
        std::string result;
        ap_uint<N> temp = val;
        ap_uint<N> ten(10);
        
        while (static_cast<bool>(temp)) {
            ap_uint<N> remainder = temp % ten;
            result = char('0' + static_cast<int>(static_cast<uint64_t>(remainder))) + result;
            temp = temp / ten;
        }
        return result;
    }
    
    throw std::invalid_argument("Unsupported base");
}

#endif // AP_UINT_H