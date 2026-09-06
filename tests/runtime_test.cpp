#include "dsp/runtime.h"
#include <cassert>
#include <cstdio>
#include <vector>
#include <thread>

int main() {
    vavra::Ring<int, 8> ring;
    int value = -1;
    assert(!ring.pop(value));
    for (int round = 0; round < 100; ++round) {
        for (int i = 0; i < 7; ++i) assert(ring.push(round * 7 + i));
        assert(!ring.push(-1));
        for (int i = 0; i < 7; ++i) {
            assert(ring.pop(value));
            assert(value == round * 7 + i);
        }
        assert(!ring.pop(value));
    }
    std::vector<uint8_t> rom(524288);
    rom[0]='2'; rom[1]='.'; rom[2]='2'; rom[3]='3';
    auto original = rom;
    assert(vavra::normalizeRom(rom));
    assert(rom == original);
    for (size_t i=0; i<rom.size(); i+=2) std::swap(rom[i],rom[i+1]);
    assert(vavra::normalizeRom(rom));
    assert(rom == original);
    rom[0]=0;
    assert(!vavra::normalizeRom(rom));
    rom.resize(4);
    assert(!vavra::normalizeRom(rom));
    vavra::Ring<uint32_t,1024> concurrent;
    std::thread producer([&] {
        for(uint32_t i=0;i<500000;++i) while(!concurrent.push(i)) std::this_thread::yield();
    });
    for(uint32_t i=0;i<500000;++i) {
        uint32_t next;
        while(!concurrent.pop(next)) std::this_thread::yield();
        assert(next==i);
    }
    producer.join();
    puts("PASS: ring wrap/full/empty, concurrent publication, and ROM normalization");
}
