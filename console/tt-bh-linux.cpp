// SPDX-FileCopyrightText: © 2025 Tenstorrent AI ULC
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <mutex> // Added for std::mutex
#include <string> // Added for std::string
#include <iostream> // Added for std::cout, std::cerr
#include <getopt.h> // Added for getopt_long
#include <thread> // Added for std::thread

#include "console.hpp"
#include "disk.hpp"
#include "network.hpp"

std::atomic<bool> exit_thread_flag{false};
std::mutex interrupt_register_lock; // Global mutex for MMIO access

void console_main(int ttdevice, int l2cpu){
    printf("Press Ctrl-A x to exit.\n\n");
    while (!exit_thread_flag) {
        try {
            int r = uart_loop(ttdevice, l2cpu, exit_thread_flag);
            if (r == -EAGAIN) {
                printf("Error (UART vanished) -- was the chip reset?  Retrying...\n");
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            } else {
                exit_thread_flag = true;
                return;
            }
        } catch (const std::exception& e) {
            printf("Error (%s) -- was the chip reset?  Retrying...\n", e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void disk_main(int ttdevice, int l2cpu, std::mutex& interrupt_register_lock, int interrupt_number, uint64_t mmio_region_offset, const std::string& disk_image_path){
    while (!exit_thread_flag){
        VirtioBlk device(ttdevice, l2cpu, exit_thread_flag, interrupt_register_lock, interrupt_number, mmio_region_offset, disk_image_path);
        device.device_setup();
        device.device_loop();
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void network_main(int ttdevice, int l2cpu, std::mutex& interrupt_register_lock, int interrupt_number, uint64_t mmio_region_offset){
    while (!exit_thread_flag){
        VirtioNet device(ttdevice, l2cpu, exit_thread_flag, interrupt_register_lock, interrupt_number, mmio_region_offset);
        device.device_setup();
        device.device_loop();
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

int main(int argc, char **argv){
    int l2cpu=0;
    std::string disk_image_path = "";
    std::string cloud_init_path = "";
    bool enable_network = false;
    int ttdevice = 0;

    const char* const short_opts = "t:l:d:c:nh";
    const option long_opts[] = {
            {"ttdevice", required_argument, nullptr, 't'},
            {"l2cpu", required_argument, nullptr, 'l'},
            {"disk", required_argument, nullptr, 'd'},
            {"cloud-init", required_argument, nullptr, 'c'},
            {"network", no_argument, nullptr, 'n'},
            {"help", no_argument, nullptr, 'h'},
            {nullptr, no_argument, nullptr, 0}
    };

    while (true)
    {
        const auto opt = getopt_long(argc, argv, short_opts, long_opts, nullptr);

        if (-1 == opt)
            break;

        switch (opt)
        {
        case 't':
            ttdevice = std::stoi(optarg);
            break;
        case 'l':
            l2cpu = std::stoi(optarg);
            break;
        case 'd': // Handle disk image option
            disk_image_path = optarg;
            break;
        case 'c': // Handle cloud init option
            cloud_init_path = optarg;
            break;
        case 'n': // Enable virtio-net networking
            enable_network = true;
            break;
        case 'h': // -h or --help
        case '?': // Unrecognized option
        default:
            std::cout <<
            "--ttdevice <t>:        Tenstorrent device index (default: 0)\n"
            "--l2cpu <l>:           L2CPU to attach to\n"
            "--disk <path>:         Path to the disk image (enables virtio-blk)\n"
            "--cloud-init <path>:   Path to the cloud-init image (enables virtio-blk for it)\n"
            "--network:             Enable virtio-net networking via slirp\n"
            "--help:                Show help\n";
            exit(1);
        }
    }

    if (l2cpu < 0 || l2cpu > 3){
        std::cerr<<"l2cpu must be one of 0,1,2,3"<<"\n";
        exit(1);
    }


  std::vector<std::thread> threads;
  threads.emplace_back(console_main, ttdevice,  l2cpu);
  if (!disk_image_path.empty()) {
    threads.emplace_back(disk_main, ttdevice, l2cpu, std::ref(interrupt_register_lock), 33, 2ULL*1024*1024, disk_image_path);
  }
  if (enable_network) {
    threads.emplace_back(network_main, ttdevice, l2cpu, std::ref(interrupt_register_lock), 32, 4ULL*1024*1024);
  }
  if (!cloud_init_path.empty()) {
    threads.emplace_back(disk_main, ttdevice, l2cpu, std::ref(interrupt_register_lock), 31, 6ULL*1024*1024, cloud_init_path);
  }
  for (auto& thread: threads){
    thread.join();
  }
}
