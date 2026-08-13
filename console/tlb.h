// SPDX-FileCopyrightText: © 2025 Tenstorrent AI ULC
// SPDX-License-Identifier: Apache-2.0

#ifndef TLB_H
#define TLB_H
#include <unistd.h>
#include <cstdint>
#include <iostream>
#include <memory>
#include <cassert>

#include "ioctl.h"

static constexpr size_t TWO_MEG = 1 << 21;
static constexpr size_t FOUR_GIG = 1ULL << 32;

struct xy_t
{
    uint16_t x;
    uint16_t y;
};


class TlbHandle
{
    int fd;
    int tlb_id;
    uint8_t *tlb_base;
    size_t tlb_size;

public:
    TlbHandle(int fd, size_t size, const tenstorrent_noc_tlb_config &config, void* base=nullptr, bool use_wc=false);

    uint8_t* data();
    size_t size() const;

    ~TlbHandle() noexcept;

};

template <size_t WINDOW_SIZE>
class TlbWindow
{
    static constexpr size_t WINDOW_MASK = WINDOW_SIZE - 1;
    static_assert((WINDOW_SIZE & WINDOW_MASK) == 0, "WINDOW_SIZE must be a power of 2");

    const uint64_t offset;  // Within the window, to reach target address.
    std::unique_ptr<TlbHandle> window;

public:
    TlbWindow(int fd, uint16_t x, uint16_t y, uint64_t addr, void* base=nullptr, bool use_wc=false);

    void write32(uint64_t addr, uint32_t value);

    uint32_t read32(uint64_t addr);

    uint8_t* get_window();
};

template <size_t WINDOW_SIZE>
TlbWindow<WINDOW_SIZE>::TlbWindow(int fd, uint16_t x, uint16_t y, uint64_t addr, void* base, bool use_wc)
: offset(addr & WINDOW_MASK)
{
    tenstorrent_noc_tlb_config config{
        .addr = addr & ~WINDOW_MASK,
        .x_end = x,
        .y_end = y,
    };

    window = std::make_unique<TlbHandle>(fd, WINDOW_SIZE, config, base, use_wc);
}


template <size_t WINDOW_SIZE>
void TlbWindow<WINDOW_SIZE>::write32(uint64_t addr, uint32_t value){
    assert(offset + addr + 4 <= WINDOW_SIZE);
    assert(((offset + addr) % 4) == 0);
    void *ptr = window->data() + offset + addr;
    *reinterpret_cast<volatile uint32_t *>(ptr) = value;
}

template <size_t WINDOW_SIZE>
uint32_t TlbWindow<WINDOW_SIZE>::read32(uint64_t addr){
    assert(offset + addr + 4 <= WINDOW_SIZE);
    assert(((offset + addr) % 4) == 0);
    void *ptr = window->data() + offset + addr;
    return *reinterpret_cast<volatile uint32_t *>(ptr);
}

template <size_t WINDOW_SIZE>
uint8_t* TlbWindow<WINDOW_SIZE>::get_window(){
    return window.get()->data() + offset;
}

using TlbWindow2M = TlbWindow<TWO_MEG>;
using TlbWindow4G = TlbWindow<FOUR_GIG>;
#endif
