#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace vavra {
// Osirus's single-producer/single-consumer ring, with explicit publication on ARM.
template<class T, uint32_t N> struct Ring {
    static_assert(std::atomic<uint32_t>::is_always_lock_free);
    T data[N]{};
    std::atomic<uint32_t> read{0}, write{0};
    uint32_t available() const {
        return (write.load(std::memory_order_acquire)+N-read.load(std::memory_order_acquire))%N;
    }
    bool push(const T& item) {
        auto w=write.load(std::memory_order_relaxed), next=(w+1)%N;
        if(next==read.load(std::memory_order_acquire)) return false;
        data[w]=item; write.store(next,std::memory_order_release); return true;
    }
    bool pop(T& item) {
        auto r=read.load(std::memory_order_relaxed);
        if(r==write.load(std::memory_order_acquire)) return false;
        item=data[r]; read.store((r+1)%N,std::memory_order_release); return true;
    }
};
inline bool normalizeRom(std::vector<uint8_t>& bytes) {
    if(bytes.size()!=524288) return false;
    if(!memcmp(bytes.data(),"2.23",4)) return true;
    if(memcmp(bytes.data(),".232",4)) return false;
    for(size_t i=0;i<bytes.size();i+=2) std::swap(bytes[i],bytes[i+1]);
    return true;
}
}
