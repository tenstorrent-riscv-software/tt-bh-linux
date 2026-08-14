// SPDX-FileCopyrightText: © 2025 Tenstorrent AI ULC
// SPDX-License-Identifier: Apache-2.0

#include <string> // Added for std::string
#include <iostream> // Added for std::cout, std::cerr
#include <getopt.h> // Added for getopt_long
#include <fstream>
#include <vector>
#include <map>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <array>
#include <sstream>
#include <fcntl.h>
#include <libfdt.h>
#include <chrono>
#include <thread>
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include "ioctl.h"
#include "l2cpu.h"

// Newer tt-kmd reset flags (from tt-kmd tools/tt.c); absent from this repo's ioctl.h
#ifndef TENSTORRENT_RESET_DEVICE_ASIC_RESET
#define TENSTORRENT_RESET_DEVICE_ASIC_RESET 4
#endif
#ifndef TENSTORRENT_RESET_DEVICE_POST_RESET
#define TENSTORRENT_RESET_DEVICE_POST_RESET 6
#endif

std::vector<uint8_t> read_bin_file(std::string file_path){
	std::ifstream file;	
	file.open(file_path, std::ios::binary | std::ios::ate);

	if (!file){
		std::cerr<<"File "<<file_path<<" doesn't exist";
		exit(1);
	}
	auto size = file.tellg();
	file.seekg(0);
	std::vector<uint8_t> buf(size);
	file.read(reinterpret_cast<char*>(buf.data()), size);

	if(size_t rem = buf.size() % 4; rem != 0)
		buf.resize(buf.size() + (4 - rem), 0);

	return buf;
}

void set_pll(int pll_rate, int fd){
	const uint64_t PLL4_BASE = 0x80020500;
	const uint64_t PLL_CNTL_1 = 0x4;
	const uint64_t PLL_CNTL_5 = 0x14;

	std::map<int, std::pair<int, std::array<uint8_t, 4>>> solutions = {
		{200, {128, {15, 15, 15, 15}}},
		{1750, {140, {1, 1, 1, 1}}},
		{15, {120, {99, 99, 99, 99}}},
	};
	
	uint16_t target_fbdiv = solutions.at(pll_rate).first;
	std::array<uint8_t, 4> target_postdiv = solutions.at(pll_rate).second;

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

void reset_x280(std::vector<int> l2cpu_indices, int ttdevice){
	uint64_t reset_unit_base = 0x80030000;
	std::stringstream chardev_string;
	chardev_string<<"/dev/tenstorrent/"<<ttdevice;
	int fd = open(chardev_string.str().c_str(), O_RDWR | O_CLOEXEC);

	TlbWindow2M reset_unit(fd, 8, 0, reset_unit_base);

	set_pll(200, fd);
	uint32_t l2cpu_reset_val = reset_unit.read32(0x14);
	for (auto l2cpu_index: l2cpu_indices)
		l2cpu_reset_val |= 1 << (l2cpu_index + 4);
	reset_unit.write32(0x14, l2cpu_reset_val);
	reset_unit.read32(0x14);
	set_pll(1750, fd);
}

// Port of tt-kmd tools/tt.c 'tt reset' single-device flow: SBR, ASIC reset,
// wait for the device to drop off and return to the PCIe bus, then POST_RESET.
void pcie_reset(int ttdevice){
	static constexpr int RESET_POLL_INTERVAL_MS = 100;
	static constexpr int RESET_MARKER_TIMEOUT_MS = 15000;
	static constexpr int RESET_SETTLE_MS = 500;

	std::string path = "/dev/tenstorrent/" + std::to_string(ttdevice);
	int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
	if (fd < 0){
		std::cerr << path << ": open failed: " << std::strerror(errno) << "\n";
		exit(1);
	}

	// BDF via GET_DEVICE_INFO, needed to poll PCI config space during reset
	tenstorrent_get_device_info info = {};
	info.in.output_size_bytes = sizeof(info.out);
	if (ioctl(fd, TENSTORRENT_IOCTL_GET_DEVICE_INFO, &info) != 0){
		std::cerr << path << ": GET_DEVICE_INFO failed: " << std::strerror(errno) << "\n";
		exit(1);
	}
	char bdf_buf[32];
	snprintf(bdf_buf, sizeof bdf_buf, "%04x:%02x:%02x.%x", info.out.pci_domain,
			(info.out.bus_dev_fn >> 8) & 0xff,
			(info.out.bus_dev_fn >> 3) & 0x1f,
			info.out.bus_dev_fn & 0x7);
	std::string bdf = bdf_buf;

	auto reset_step = [&](uint32_t flags, const char *what){
		tenstorrent_reset_device cmd = {};
		cmd.in.output_size_bytes = sizeof(cmd.out);
		cmd.in.flags = flags;
		if (ioctl(fd, TENSTORRENT_IOCTL_RESET_DEVICE, &cmd) != 0){
			std::cerr << bdf << ": " << what << " ioctl failed: " << std::strerror(errno) << "\n";
			exit(1);
		}
		if (cmd.out.result != 0){
			std::cerr << bdf << ": " << what << " failed: driver result " << cmd.out.result << "\n";
			exit(1);
		}
		std::cout << bdf << ": " << what << " ok\n";
	};

	reset_step(TENSTORRENT_RESET_DEVICE_RESET_PCIE_LINK, "SBR (RESET_PCIE_LINK)");
	reset_step(TENSTORRENT_RESET_DEVICE_ASIC_RESET, "ASIC_RESET");

	// Let the reset land, then wait for the device to vanish and return.
	// Completion is signalled either by reappearance or by the command-register
	// marker (config offset 4, bit 6) clearing.
	std::this_thread::sleep_for(std::chrono::milliseconds(RESET_SETTLE_MS));

	std::string config_path = "/sys/bus/pci/devices/" + bdf + "/config";
	auto start = std::chrono::steady_clock::now();
	bool vanished = false;
	bool done = false;
	while (!done){
		int cfd = open(config_path.c_str(), O_RDONLY | O_CLOEXEC);
		if (cfd < 0){
			if (errno == ENOENT && !vanished){
				std::cout << bdf << ": device dropped off the bus; waiting for it to return\n";
				vanished = true;
			}
		} else {
			uint8_t bytes[2];
			ssize_t n = pread(cfd, bytes, sizeof(bytes), 4);
			close(cfd);
			if (n == (ssize_t)sizeof(bytes)){
				uint16_t command = bytes[0] | (uint16_t)bytes[1] << 8;
				if (vanished){
					std::cout << bdf << ": device is back on the bus\n";
					done = true;
				} else if (command != 0xffff && (command & (1u << 6)) == 0){
					std::cout << bdf << ": reset marker cleared\n";
					done = true;
				}
			}
		}

		if (!done){
			auto elapsed = std::chrono::steady_clock::now() - start;
			if (elapsed > std::chrono::milliseconds(RESET_MARKER_TIMEOUT_MS)){
				std::cerr << bdf << ": device did not complete reset within "
					  << RESET_MARKER_TIMEOUT_MS << " ms\n";
				exit(1);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(RESET_POLL_INTERVAL_MS));
		}
	}

	std::this_thread::sleep_for(std::chrono::milliseconds(RESET_SETTLE_MS));

	// POST_RESET; hotplug may have invalidated fd, so retry once on a fresh one
	for (int tries = 0;; tries++){
		tenstorrent_reset_device cmd = {};
		cmd.in.output_size_bytes = sizeof(cmd.out);
		cmd.in.flags = TENSTORRENT_RESET_DEVICE_POST_RESET;
		if (ioctl(fd, TENSTORRENT_IOCTL_RESET_DEVICE, &cmd) == 0){
			if (cmd.out.result != 0){
				std::cerr << bdf << ": POST_RESET failed: driver result " << cmd.out.result << "\n";
				exit(1);
			}
			std::cout << bdf << ": POST_RESET ok\n";
			break;
		}
		if (errno != ENODEV || tries){
			std::cerr << bdf << ": POST_RESET ioctl failed: " << std::strerror(errno) << "\n";
			exit(1);
		}
		std::cout << bdf << ": POST_RESET returned ENODEV (device re-probed); reopening and retrying\n";
		close(fd);
		fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
		if (fd < 0){
			std::cerr << path << ": reopen failed: " << std::strerror(errno) << "\n";
			exit(1);
		}
	}

	close(fd);
}

int main(int argc, char **argv){

	std::vector<int> l2cpu(1, 0);
	bool l2cpu_nondefault = false;
	int ttdevice = 0;
	bool boot_flag = false;
	bool dt_no_virtio_devices = false;

	std::string rootfs_bin, opensbi_bin, kernel_bin, boot_device = "vda", extra_bootargs;
	std::vector<std::string> dtb_bin;

	std::vector<std::string> rootfs_dst, opensbi_dst, kernel_dst, dtb_dst;

	const option long_opts[] = {
	    {"ttdevice", required_argument, nullptr, 't'},
	    {"l2cpu", required_argument, nullptr, 'l'},
	    {"rootfs_bin", required_argument, nullptr, 1000},
	    {"rootfs_dst", required_argument, nullptr, 1001},
	    {"opensbi_bin", required_argument, nullptr, 1002},
	    {"opensbi_dst", required_argument, nullptr, 1003},
	    {"kernel_bin", required_argument, nullptr, 1004},
	    {"kernel_dst", required_argument, nullptr, 1005},
	    {"dtb_bin", required_argument, nullptr, 1006},
	    {"dtb_dst", required_argument, nullptr, 1007},
	    {"boot_device", required_argument, nullptr, 1008},
	    {"extra_bootargs", required_argument, nullptr, 1009},
	    {"boot", no_argument, nullptr, 1010},
	    {"dt_no_virtio_devices", no_argument, nullptr, 1011},
	    {"help", no_argument, nullptr, 'h'},
	    {nullptr, no_argument, nullptr, 0}
	};

	while (true) {
		const auto opt = getopt_long(argc, argv, "", long_opts, nullptr);

		if (opt == -1)
			break;

		switch (opt){
			case 't':
				ttdevice = std::stoi(optarg);	
				break;
			case 'l':
				if (!l2cpu_nondefault){
					l2cpu.clear();
					l2cpu_nondefault = true;
				}
				l2cpu.push_back(std::stoi(optarg));
				break;
			case 1000:
				rootfs_bin = optarg;
				break;
			case 1001:
				rootfs_dst.push_back(optarg);
				break;
			case 1002:
				opensbi_bin = optarg;
				break;
			case 1003:
				opensbi_dst.push_back(optarg);
				break;
			case 1004:
				kernel_bin = optarg;
				break;
			case 1005:
				kernel_dst.push_back(optarg);
				break;
			case 1006:
				dtb_bin.push_back(optarg);
				break;
			case 1007:
				dtb_dst.push_back(optarg);
				break;
			case 1008:
				boot_device = optarg;
				break;
			case 1009:
				extra_bootargs = optarg;
				break;
			case 1010:
				boot_flag = true;
				break;
			case 1011:
				dt_no_virtio_devices = true;
				break;
			case 'h':
			case '?':
			default:
				std::cout <<
				"--ttdevice <t> \n"
				;
				exit(1);
		}

	}
	if ((l2cpu.size() != rootfs_dst.size()) || (l2cpu.size() != opensbi_dst.size()) || (l2cpu.size() != kernel_dst.size()) || (l2cpu.size() != dtb_bin.size()) || (l2cpu.size() != dtb_dst.size())) {
		std::cerr<<"Length of all vars must be same";
		exit(1);
	}
	for(auto l2cpu_idx: l2cpu) {
		if(l2cpu_idx >= 4 || l2cpu_idx < 0) {
			std::cerr<<"l2cpu IDs must be in [0, 1, 2, 3]";
			exit(1);
		}
	}

	// Reset the card and wait for it to come back (Python: pci_board_reset + sleep 5)
	pcie_reset(ttdevice);
	std::this_thread::sleep_for(std::chrono::seconds(5));
	// TODO: telemetry harvesting check (enabled_l2cpu/enabled_gddr) not ported

	for(size_t idx=0; idx < l2cpu.size(); idx++){
		int l2cpu_idx = l2cpu[idx];

		uint64_t l2cpu_base = 0xfffff7fefff10000ULL;

		uint64_t opensbi_addr = std::stoull(opensbi_dst[idx], NULL, 16);
		std::vector<uint8_t> opensbi_bytes = read_bin_file(opensbi_bin);

		uint64_t rootfs_addr = 0;
		std::vector<uint8_t> rootfs_bytes;
		if(!rootfs_dst.empty() && !rootfs_bin.empty() && std::ifstream(rootfs_bin)){
			rootfs_addr = std::stoull(rootfs_dst[idx], NULL, 16);
			rootfs_bytes = read_bin_file(rootfs_bin);	
		}

		uint64_t kernel_addr = 0;
		std::vector<uint8_t> kernel_bytes;
		if(!kernel_dst.empty() && !kernel_bin.empty()){
			kernel_addr = std::stoull(kernel_dst[idx], NULL, 16);
			kernel_bytes = read_bin_file(kernel_bin);	
		}

		uint64_t dtb_addr = 0;
		std::vector<uint8_t> dtb_bytes;
		if(!dtb_dst.empty() && !dtb_bin.empty()){
			dtb_addr = std::stoull(dtb_dst[idx], NULL, 16);
			dtb_bytes = read_bin_file(dtb_bin[idx]);	
		}


		std::vector<uint8_t> work(dtb_bytes.size() + 2000);
		int err = fdt_open_into(dtb_bytes.data(), work.data(), work.size());
		if(err){
			std::cerr<<"Corrupt FDT bytes";
			exit(1);
		}
		void *fdt = work.data();

		int chosen_offset = fdt_path_offset(fdt, "/chosen");
		if(chosen_offset < 0){
			chosen_offset = fdt_add_subnode(fdt, 0, "chosen");
		}

		std::string bootargs = "rw console=hvc0 earlycon=sbi";
		if(boot_device.substr(0, 3) == "vda"){
			bootargs.append(" root=/dev/" + boot_device);
		} else if(boot_device == "initramfs"){
			if(std::ifstream(rootfs_bin))
				bootargs.append(" initrd=" + rootfs_dst[idx] + "," + std::to_string(rootfs_bytes.size()));
		} else {
			std::cerr<<"Unsupported rootfs type";
			exit(1);
		}
		if(!extra_bootargs.empty())
			bootargs.append(" " + extra_bootargs);
		fdt_setprop(fdt, chosen_offset, "bootargs", bootargs.c_str(), bootargs.size() + 1);

		int memory_node = fdt_path_offset(fdt, "/memory@400030000000");
		if (memory_node < 0){
			std::cerr<<"Memory node not found in DT. Exiting";
			exit(1);
		}

		// Add virtio devices (reserve 6M at top of DRAM, 4 virtio-mmio nodes underneath)
		if(!dt_no_virtio_devices){
			int len;
			const fdt64_t *mem_reg = (const fdt64_t*)fdt_getprop(fdt, memory_node, "reg", &len);
			if(!mem_reg || len != 16){
				std::cerr<<"memory node has no valid reg property";
				exit(1);
			}
			uint64_t mem_start = fdt64_to_cpu(mem_reg[0]);
			uint64_t mem_size  = fdt64_to_cpu(mem_reg[1]);
			uint64_t mem_end = mem_start + mem_size;

			int reserved_memory_offset = fdt_path_offset(fdt, "/reserved-memory");
			if(reserved_memory_offset < 0){
				reserved_memory_offset = fdt_add_subnode(fdt, 0, "reserved-memory");
				fdt_setprop_u32(fdt, reserved_memory_offset, "#address-cells", 2);
				fdt_setprop_u32(fdt, reserved_memory_offset, "#size-cells", 2);
				fdt_setprop(fdt, reserved_memory_offset, "ranges", nullptr, 0);
			}

			int virtio_reserved_offset = fdt_add_subnode(fdt, reserved_memory_offset, "memory@4000afa00000");
			fdt64_t reserved_reg[2] = {cpu_to_fdt64(mem_end - 0x600000), cpu_to_fdt64((uint64_t)0x600000)};
			fdt_setprop(fdt, virtio_reserved_offset, "reg", reserved_reg, sizeof(reserved_reg));
			fdt_setprop(fdt, virtio_reserved_offset, "no-map", nullptr, 0);

			int soc_offset = fdt_path_offset(fdt, "/soc");
			if(soc_offset < 0){
				std::cerr<<"soc node not found in DT. Exiting";
				exit(1);
			}

			int plic_offset = fdt_path_offset(fdt, "/soc/interrupt-controller@c000000");
			if(plic_offset < 0){
				std::cerr<<"plic node not found in DT. Exiting";
				exit(1);
			}

			uint32_t plic_phandle = fdt_get_phandle(fdt, plic_offset);
			if(plic_phandle == 0){
				plic_phandle = fdt_get_max_phandle(fdt) + 1;
				fdt_setprop_u32(fdt, plic_offset, "phandle", plic_phandle);
			}

			for(int i = 3; i >= 0; i--){
				uint64_t virtio_addr = mem_end - 0x200000ULL * (i + 1);
				uint32_t virtio_irq = 33 - i;

				char node_name[32];
				snprintf(node_name, sizeof node_name, "virtio@%llx", (unsigned long long)virtio_addr);
				int virtio_offset = fdt_add_subnode(fdt, soc_offset, node_name);
				fdt_setprop_string(fdt, virtio_offset, "compatible", "virtio,mmio");
				fdt64_t virtio_reg[2] = {cpu_to_fdt64(virtio_addr), cpu_to_fdt64((uint64_t)0x200000)};
				fdt_setprop(fdt, virtio_offset, "reg", virtio_reg, sizeof(virtio_reg));
				fdt_setprop_u32(fdt, virtio_offset, "interrupts", virtio_irq);
				fdt_setprop_u32(fdt, virtio_offset, "interrupt-parent", plic_phandle);
			}
		}

		fdt_pack(fdt);
		dtb_bytes.assign(work.data(), work.data() + fdt_totalsize(fdt));
		// fdt_totalsize is not guaranteed 4-aligned; pad for the word-wise write
		if(size_t rem = dtb_bytes.size() % 4; rem != 0)
			dtb_bytes.resize(dtb_bytes.size() + (4 - rem), 0);

		L2CPU l2cpu(l2cpu_idx, ttdevice);
		uint64_t L3_REG_BASE = 0x2010000ULL;
		l2cpu.write32(L3_REG_BASE + 8, 0xf);
		l2cpu.read32(L3_REG_BASE + 8);

		std::cout<<std::hex<<"Writing OpenSBI to 0x"<<opensbi_addr<<std::dec<<"\n";
		l2cpu.write(opensbi_addr, opensbi_bytes);

		if(!rootfs_dst.empty() && !rootfs_bin.empty() && std::ifstream(rootfs_bin)){
			std::cout<<std::hex<<"Writing rootfs to 0x"<<rootfs_addr<<std::dec<<"\n";
			l2cpu.write(rootfs_addr, rootfs_bytes);
		}

		if(!kernel_dst.empty() && !kernel_bin.empty()){
			std::cout<<std::hex<<"Writing Kernel to 0x"<<kernel_addr<<std::dec<<"\n";
			l2cpu.write(kernel_addr, kernel_bytes);
		}
			
		if(!dtb_dst.empty() && !dtb_bin.empty()){
			std::cout<<std::hex<<"Writing dtb to 0x"<<dtb_addr<<std::dec<<"\n";
			l2cpu.write(dtb_addr, dtb_bytes);
		}

		uint32_t reset_vector_0 = opensbi_addr & 0xffffffff;
		uint32_t reset_vector_1 = opensbi_addr >> 32;

		l2cpu.write32(l2cpu_base + 0x0, reset_vector_0);
		l2cpu.write32(l2cpu_base + 0x4, reset_vector_1);
		l2cpu.write32(l2cpu_base + 0x8, reset_vector_0);
		l2cpu.write32(l2cpu_base + 0xC, reset_vector_1);
		l2cpu.write32(l2cpu_base + 0x10, reset_vector_0);
		l2cpu.write32(l2cpu_base + 0x14, reset_vector_1);
		l2cpu.write32(l2cpu_base + 0x18, reset_vector_0);
		l2cpu.write32(l2cpu_base + 0x1C, reset_vector_1);

	}			

	if(boot_flag)
		reset_x280(l2cpu, ttdevice);
	else
		std::cout<<"Not booting (you didn't pass --boot)"<<std::endl;

	for(size_t idx=0; idx < l2cpu.size(); idx++){
		int l2cpu_idx = l2cpu[idx];
		L2CPU l2cpu(l2cpu_idx, ttdevice);

		uint64_t L2_PREFETCH_BASE = 0x2030000;
		for(int offset=0; offset<4; offset++){
			l2cpu.write32(L2_PREFETCH_BASE + (offset * 0x2000), 0x15811);
			l2cpu.write32(L2_PREFETCH_BASE + (offset * 0x2000) + 4, 0x38c84e);
		}
	
	}
}
