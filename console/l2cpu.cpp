// SPDX-FileCopyrightText: © 2025 Tenstorrent AI ULC
// SPDX-License-Identifier: Apache-2.0

#include <fcntl.h>
#include <cassert>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <sys/mman.h>
#include <time.h>
#include "l2cpu.h"

/*
The blackhole chip consists of different tiles (tensix, gddr, pcie, ethernet, l2cpu) that are addressable using their location on the NOC.

A L2CPU tile represents the Sifive X280 Quad core riscv64 CPU along with it's attached peripherals (interrupt controllers, etc..)

There are 4 such L2CPU tiles in blackhole This entry represents the location of each of these L2CPUs on the NOC.
*/
const std::map<int, xy_t> l2cpu_tile_mapping {
    {0, xy_t{x: 8, y: 3}},
    {1, xy_t{x: 8, y: 9}},
    {2, xy_t{x: 8, y: 5}},
    {3, xy_t{x: 8, y: 7}},
};

/*
This represents the starting address of the DRAM for each L2CPU
*/
const std::map<int, uint64_t> l2cpu_starting_address_mapping {
    {0, 0x4000'3000'0000ULL},
    {1, 0x4000'3000'0000ULL},
    {2, 0x4000'3000'0000ULL},
    {3, 0x4000'b000'0000ULL},
};


/*
This represents the size of the memory available to each L2CPU
*/
const std::map<int, uint64_t> l2cpu_memory_size_mapping {
    {0, 0x1'0000'0000ULL},
    {1, 0x1'0000'0000ULL},
    {2, 0x8000'0000ULL},
    {3, 0x8000'0000ULL},
};

L2CPU::L2CPU(int idx, int card_idx)
    : idx(idx), card_idx(card_idx)
{
    assert(idx >=0 && idx < 4);
    std::stringstream chardev_string;
    chardev_string<<"/dev/tenstorrent/"<<card_idx;
    fd = open(chardev_string.str().c_str(), O_RDWR | O_CLOEXEC);
    set_frequency();
    starting_address = l2cpu_starting_address_mapping.at(idx);
    coordinates = l2cpu_tile_mapping.at(idx);
    memory_size = l2cpu_memory_size_mapping.at(idx);

    // Try to map the two 4G memory windows; if either fails, warn and continue
    // without them instead of crashing.
    try {
        memory = reinterpret_cast<uint8_t*>(mmap(nullptr, (2ULL<<32), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (memory == MAP_FAILED) {
            memory = nullptr;
            throw std::runtime_error("mmap of 8G L2CPU memory region failed");
        }

        first = std::make_unique<TlbWindow4G>(fd, coordinates.x, coordinates.y, 0x4000'0000'0000ULL, memory, true);
        second = std::make_unique<TlbWindow4G>(fd, coordinates.x, coordinates.y, 0x4001'0000'0000ULL, memory+(1ULL<<32), true);
    } catch (const std::exception &e) {
        // Virtio code errors out later via get_memory_ptr() == nullptr
        // Unwind anything that did succeed
        second.reset();
        first.reset();
        if (memory != nullptr) {
            munmap(memory, 2ULL<<32);
            memory = nullptr;
        }
    }
}

uint64_t L2CPU::get_starting_address(){
  return starting_address;
}

uint64_t L2CPU::get_memory_size(){
  return memory_size;
}

xy_t L2CPU::get_coordinates(){
    return coordinates;
}

/*
These functions create a "temporary" TlbWindow to do accomplish the task they want to do
This TlbWindow should get cleaned up immediately
*/

void L2CPU::write32(uint64_t addr, uint32_t value) {
    TlbWindow2M temporary_window(fd, coordinates.x, coordinates.y, addr);
    temporary_window.write32(0, value);
}

uint32_t L2CPU::read32(uint64_t addr) {
    TlbWindow2M temporary_window(fd, coordinates.x, coordinates.y, addr);
    return temporary_window.read32(0);
}

/*
While read32/write32 are enough for many one off operations, we need some way to "persistently" map a memory location to a struct
This function creates a TlbWindow on the heap that gets cleaned up when the L2CPU goes out of scope
*/
std::unique_ptr<TlbWindow2M> L2CPU::get_persistent_2M_tlb_window(uint64_t addr){
    return std::unique_ptr<TlbWindow2M>(new TlbWindow2M(fd, coordinates.x, coordinates.y, addr));
}

// Returns the starting address of the L2CPU's memory
uint8_t* L2CPU::get_memory_ptr(){
    if (memory == nullptr) {
        return nullptr;  // 4G windows were never mapped
    }
    return memory+(starting_address-0x4000'0000'0000ULL);
}

void L2CPU::set_frequency(){
  const uint64_t PLL4_BASE = 0x80020500;
  const uint64_t PLL_CNTL_1 = 0x4;
  const uint64_t PLL_CNTL_5 = 0x14;

  // Default to 1750 MHz
  uint16_t target_fbdiv = 140;
  uint8_t target_postdiv[4] = {1, 1, 1, 1};

  union PLLCNTL5 {
    uint32_t raw;
    struct __attribute__((packed)) {
      uint8_t postdiv[4];
    } reg;
  };

  union PLLCNTL1 {
    uint32_t raw;
    struct __attribute__((packed)) {
      uint8_t refdiv;
      uint8_t postdiv;
      uint16_t fbdiv;
    } reg;
  };

  // Create TLB windows for accessing PLL registers
  TlbWindow2M window_cntl5(fd, 8, 0, PLL4_BASE + PLL_CNTL_5);
  TlbWindow2M window_cntl1(fd, 8, 0, PLL4_BASE + PLL_CNTL_1);

  // Read initial values
  PLLCNTL5 current_postdivs;
  current_postdivs.raw = window_cntl5.read32(0);

  PLLCNTL1 current_fbdiv;
  current_fbdiv.raw = window_cntl1.read32(0);

  struct timespec sleep_time = {0, 1}; // 1 nanosecond

  // Step 1: Increase postdivs that need to go up
  for (int i = 0; i < 4; i++) {
    if (target_postdiv[i] > current_postdivs.reg.postdiv[i]) {
      int8_t one_step = 1;
      while (current_postdivs.reg.postdiv[i] != target_postdiv[i]) {
        current_postdivs.reg.postdiv[i] += one_step;
        window_cntl5.write32(0, current_postdivs.raw);
        nanosleep(&sleep_time, nullptr);
      }
    }
  }

  // Step 2: Adjust fbdiv
  if (current_fbdiv.reg.fbdiv != target_fbdiv) {
    int16_t one_step = (target_fbdiv > current_fbdiv.reg.fbdiv) ? 1 : -1;
    while (current_fbdiv.reg.fbdiv != target_fbdiv) {
      current_fbdiv.reg.fbdiv += one_step;
      window_cntl1.write32(0, current_fbdiv.raw);
      nanosleep(&sleep_time, nullptr);
    }
  }

  // Step 3: Decrease postdivs that need to go down
  for (int i = 0; i < 4; i++) {
    if (target_postdiv[i] < current_postdivs.reg.postdiv[i]) {
      int8_t one_step = -1;
      while (current_postdivs.reg.postdiv[i] != target_postdiv[i]) {
        current_postdivs.reg.postdiv[i] += one_step;
        window_cntl5.write32(0, current_postdivs.raw);
        nanosleep(&sleep_time, nullptr);
      }
    }
  }
}

L2CPU::~L2CPU() noexcept
{
    if (memory != nullptr) {
        munmap(memory, 2ULL<<32);
    }
    close(fd);
}

