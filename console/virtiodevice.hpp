#pragma once
#include <atomic>
#include <cstdint>
#include <cassert>
#include <cstdio>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>
#include <mutex> // Added for std::mutex
#include "l2cpu.h"

/*
Virtual Base Class that implements most of the device-agnostic functionality needed
to emulate a the device side of a virtio-mmio device added to the L2CPU's device tree
*/
class VirtioDevice {
protected:
    int ttdevice;
    int l2cpu_idx;
    L2CPU l2cpu;
    // Starting address of L2CPU's DRAM
    uint64_t starting_address;

    // Ptr to starting address of L2CPU's DRAM
    // This ptr is used to interact with the virtqueues
    uint8_t* memory;
    
    std::shared_ptr<TlbWindow2M> window, interrupt_address_window; // MMIO window as a class member

    // Ptr to virtio-mmio device's reg region
    // None of the virtqueues/actual data transfer happens here
    // Only used for config/negotiation
    uint8_t* mmio_base;
    uint64_t mmio_region_offset;

    // Interrupt Number specified in device tree for virtio-mmio device
    int interrupt_number;
    // Ptr to L2CPU's special interrupt register
    uint32_t* interrupt_register;
    // We need to read and write back the interrupt register while setting/clearing interrupts
    // And since we have multiple threads in parallel doing this, we need a lock for this
    std::mutex& interrupt_register_lock;
    
    std::atomic<bool>& exit_thread_flag;

    // Properties of Virtqueues used by device
    // Number of virtqueues used by device
    // Most use 1, some like network may use many
    // can be overridden by derived class
    uint32_t num_queues = 1; 
     // Size of header in descriptor table, 
     // essentially sort of a "number of bytes to skip" number
     // can be overridden by derived class
    uint64_t queue_header_size = 0;
    // Max size of virtqueue, probably should make this as large as possible, 16384 maybe?
    uint16_t queue_size = 16384;

    // Pointers to registers within virtio-mmio device's reg region
    uint32_t *magic_value; // VIRTIO_MMIO_MAGIC_VALUE
    uint32_t *version; // VIRTIO_MMIO_VERSION
    uint32_t *device_id; // VIRTIO_MMIO_DEVICE_ID
    uint32_t *device_features; // VIRTIO_MMIO_DEVICE_FEATURES
    uint32_t *device_features_sel; // VIRTIO_MMIO_DEVICE_FEATURES_SEL
    uint32_t *driver_features; // VIRTIO_MMIO_DRIVER_FEATURES
    uint32_t *driver_features_sel; // VIRTIO_MMIO_DRIVER_FEATURES_SEL
    uint32_t *queue_num_max; // VIRTIO_MMIO_QUEUE_NUM_MAX
    uint32_t *queue_ready; // VIRTIO_MMIO_QUEUE_READY
    uint32_t *queue_notify; // VIRTIO_MMIO_QUEUE_NOTIFY
    uint32_t *interrupt_status; // VIRTIO_MMIO_INTERRUPT_STATUS
    uint32_t *interrupt_ack; // VIRTIO_MMIO_INTERRUPT_ACK
    uint32_t *status; // VIRTIO_MMIO_STATUS
    uint32_t *queue_desc_low; // VIRTIO_MMIO_QUEUE_DESC_LOW
    uint32_t *queue_desc_high; // VIRTIO_MMIO_QUEUE_DESC_HIGH
    uint32_t *queue_avail_low; // VIRTIO_MMIO_QUEUE_AVAIL_LOW
    uint32_t *queue_avail_high; // VIRTIO_MMIO_QUEUE_AVAIL_HIGH
    uint32_t *queue_used_low; // VIRTIO_MMIO_QUEUE_USED_LOW
    uint32_t *queue_used_high; // VIRTIO_MMIO_QUEUE_USED_HIGH
    uint32_t *queue_select; // VIRTIO_MMIO_QUEUE_SEL
    uint32_t *sw_impl; // VIRTIO_MMIO_SW_IMPL
    uint32_t *sel_generation; // VIRTIO_MMIO_SEL_GENERATION

    uint32_t device_features_list[2] = {0, 0}; // Default, set in subclass constructor or setup
    uint32_t driver_features_list[2] = {0, 0}; // Default, set in subclass constructor or setup

    // Virtqueue addresses that the driver provides the device
    std::vector<uint64_t> descriptor_table_address;
    std::vector<uint64_t> available_ring_address;
    std::vector<uint64_t> used_ring_address;
    // Pointers to the virtqueues in L2CPU memory
    std::vector<struct vring_desc*> desc;
    std::vector<struct vring_avail*> avail;
    std::vector<struct vring_used*> used;


public:
    VirtioDevice(int ttdevice_, int l2cpu_idx_, std::atomic<bool>& exit_flag, std::mutex& lock, int interrupt_number_, uint64_t mmio_region_offset_)
        : ttdevice(ttdevice_),
          l2cpu_idx(l2cpu_idx_),
          l2cpu(l2cpu_idx_, ttdevice_),
          mmio_region_offset(mmio_region_offset_),
          interrupt_number(interrupt_number_),
          interrupt_register_lock(lock),
          exit_thread_flag(exit_flag) {

        starting_address = l2cpu.get_starting_address();

        uint64_t address = starting_address + l2cpu.get_memory_size() - mmio_region_offset;
        window = l2cpu.get_persistent_2M_tlb_window(address);
        mmio_base = reinterpret_cast<uint8_t*>(window->get_window());

        memory = l2cpu.get_memory_ptr();
        if (memory == nullptr) {
            fprintf(stderr, "Error: L2CPU 4G TLB windows unavailable (get_memory_ptr returned null).\n"
                    "Virtio cannot run without DRAM access; check TLB allocation (dmesg) and TLB availability after reset.\n");
            exit(1);
        }

        // TODO: Check if (interrupt_number-5) is in valid 
        // range, and adjust which register to use accordingly
        uint64_t interrupt_address = 0x2FF10000 + 0x404;
        interrupt_address_window = l2cpu.get_persistent_2M_tlb_window(interrupt_address);
        interrupt_register = reinterpret_cast<uint32_t*>(interrupt_address_window->get_window());

        // 0->0x100 for generic virtio-mmio config, 0x100 onwards for device specific config
        // Should probably check if 0x100 is enough for device specific config
        memset(mmio_base, 0, 0x200);
        magic_value = reinterpret_cast<uint32_t *>(mmio_base + VIRTIO_MMIO_MAGIC_VALUE);
        version = reinterpret_cast<uint32_t *>(mmio_base + VIRTIO_MMIO_VERSION);
        device_id = reinterpret_cast<uint32_t *>(mmio_base + VIRTIO_MMIO_DEVICE_ID);
        device_features = reinterpret_cast<uint32_t *>(mmio_base + VIRTIO_MMIO_DEVICE_FEATURES);
        device_features_sel = reinterpret_cast<uint32_t *>(mmio_base + VIRTIO_MMIO_DEVICE_FEATURES_SEL);
        driver_features = reinterpret_cast<uint32_t *>(mmio_base + VIRTIO_MMIO_DRIVER_FEATURES);
        driver_features_sel = reinterpret_cast<uint32_t *>(mmio_base + VIRTIO_MMIO_DRIVER_FEATURES_SEL);
        queue_num_max = reinterpret_cast<uint32_t *>(mmio_base + VIRTIO_MMIO_QUEUE_NUM_MAX);
        queue_ready = reinterpret_cast<uint32_t *>(mmio_base + VIRTIO_MMIO_QUEUE_READY);
        queue_notify = reinterpret_cast<uint32_t *>(mmio_base + VIRTIO_MMIO_QUEUE_NOTIFY);
        interrupt_status = reinterpret_cast<uint32_t *>(mmio_base + VIRTIO_MMIO_INTERRUPT_STATUS);
        interrupt_ack = reinterpret_cast<uint32_t *>(mmio_base + VIRTIO_MMIO_INTERRUPT_ACK);
        status = reinterpret_cast<uint32_t *>(mmio_base + VIRTIO_MMIO_STATUS);
        queue_desc_low = reinterpret_cast<uint32_t *>(mmio_base + VIRTIO_MMIO_QUEUE_DESC_LOW);
        queue_desc_high = reinterpret_cast<uint32_t *>(mmio_base + VIRTIO_MMIO_QUEUE_DESC_HIGH);
        queue_avail_low = reinterpret_cast<uint32_t *>(mmio_base + VIRTIO_MMIO_QUEUE_AVAIL_LOW);
        queue_avail_high = reinterpret_cast<uint32_t *>(mmio_base + VIRTIO_MMIO_QUEUE_AVAIL_HIGH);
        queue_used_low = reinterpret_cast<uint32_t *>(mmio_base + VIRTIO_MMIO_QUEUE_USED_LOW);
        queue_used_high = reinterpret_cast<uint32_t *>(mmio_base + VIRTIO_MMIO_QUEUE_USED_HIGH);
        queue_select = reinterpret_cast<uint32_t *>(mmio_base + VIRTIO_MMIO_QUEUE_SEL);
        sw_impl = reinterpret_cast<uint32_t*>(mmio_base + 0x018);
        sel_generation = reinterpret_cast<uint32_t*>(mmio_base + 0x01c);

        *magic_value = ('v' | 'i' << 8 | 'r' << 16 | 't' << 24);
        *version = 2;
        *queue_num_max = queue_size;
        *sw_impl = 1;
    }

    /*
    Each queue in a device needs to implement these functions on how to actually send/recv data from the virtqueue
    Sometimes we just get one descriptor that we need to fill in with data, the size of this descriptor is normally
    header size + amount of data to fill. The logic for these kind of queues can be implemented using process_queue_start alone

    Sometimes we need to fill in data across multiple desciptors, the first descriptor is of header size, and the next n descriptors
    have the actual data filled in. The header gets processed by process_queue_start, the next n descriptors get processed by
    process_queue_data

    Sometimes the last descriptor is just of size 1 byte, normally used to store the status value of the read or write
    That can be done using process_queue_complete
    */
    virtual void process_queue_start(int queue_idx, uint8_t* addr, uint64_t len) = 0;
    virtual void process_queue_data(int queue_idx, uint8_t* addr, uint64_t len) = 0;
    virtual void process_queue_complete(int queue_idx, uint8_t* addr, uint64_t len) = 0;
    
    // Each queue in a device can implement a custom "do I have data/should I process the queue method"
    // For most devices/queues this isn't needed because we want to feed data into/read data from
    // the queue as long as the tail of the queue is lagging behind the head
    // But in some cases like  network devices, we want to wait till slirp has a packet ready for us
    // so this is useful in cases like that
    virtual bool queue_has_data(int queue_idx) = 0;

    inline void ack_interrupt(){
        /*
        What we're supposed to do is this:
        If driver has acknlowedged VIRTIO_MMIO_INT_VRING interrupt by setting bit 0 in interrupt_ack register
        We unset the interrupt by writing 0 to the correct bit in interrupt_register

        What we actually do is: nothing.
        Plic driver in X280 Acks interrupt
        We also skip unsetting the bit in interrupt_status as this isn't needed
        (even though the spec says so, the kernel implementation doesn't rely on it being unset)
        and also happens too fast on the host end.
        Because it happens too fast, the virtio_mmio device's interrupt handler
        (https://elixir.bootlin.com/linux/v6.14.6/source/drivers/virtio/virtio_mmio.c#L317)
        thinks the interrupt was for not of type VIRTIO_MMIO_INT_VRING and doesn't check the virtqueue
        for processed descriptors
        */
        // uint32_t interrupt_status_val = *interrupt_status;
        uint32_t interrupt_ack_val = *interrupt_ack;
        if ((interrupt_ack_val & 1)==1) {
            // *interrupt_status = ~VIRTIO_MMIO_INT_VRING & interrupt_status_val;
            // *interrupt_ack = ~1 & interrupt_ack_val;
            // std::lock_guard<std::mutex> guard(interrupt_register_lock);
            // *interrupt_register = *interrupt_register & ~(1 << (interrupt_number - 5));
        }
    }

    inline void set_interrupt(){
        /*
        Set required bit in interrupt_register to 1 if we need to trigger an interrupt
        */
        uint32_t interrupt_status_val = *interrupt_status;
        if (true){
            *interrupt_status = VIRTIO_MMIO_INT_VRING | interrupt_status_val;
            std::lock_guard<std::mutex> guard(interrupt_register_lock);
            /*
            FIXME: setting multiple interrupts on the plic seems to be buggy
            so we just set our interrupt instead
            */
            // *interrupt_register = *interrupt_register | (1 << (interrupt_number - 5));
            *interrupt_register = (1 << (interrupt_number - 5));
            __sync_synchronize();
            *interrupt_register = 0;
        }
    }

    void device_setup(){
        /*
        TODO: Draw a state transition diagram here maybe?
        */
        uint32_t prev_sel_generation = 0, curr_sel_generation=0;
        while (!exit_thread_flag) {
            if (*status & VIRTIO_CONFIG_S_DRIVER) {
                break;
            }
        }
        // uint32_t curr_device_features_sel=0;
        while (!exit_thread_flag) {
            curr_sel_generation = *sel_generation;
            if (curr_sel_generation != prev_sel_generation){
                *device_features = device_features_list[*device_features_sel];
                *driver_features = driver_features_list[*driver_features_sel];
                *sel_generation = curr_sel_generation + 1;
                prev_sel_generation = curr_sel_generation + 1;
            }
            // TODO: read driver_features and do negotiation?

            if (*status & VIRTIO_CONFIG_S_FEATURES_OK) {
                break;
            }
        }

        // Resize vectors for queue pointers
        desc.resize(num_queues, nullptr);
        avail.resize(num_queues, nullptr);
        used.resize(num_queues, nullptr);
        descriptor_table_address.resize(num_queues, 0);
        available_ring_address.resize(num_queues, 0);
        used_ring_address.resize(num_queues, 0);

        /*
        Stage where we get virtqueue addresses from driver
        This implementation is still buggy timing wise sometimes
        We fail to get past this stage. Improve this
        */
        while (!exit_thread_flag) {
            curr_sel_generation = *sel_generation;
            *queue_ready = 0;
            if (curr_sel_generation != prev_sel_generation){
                uint32_t queue_select_val = *queue_select;
                // uint32_t queue_ready_val = *queue_ready;

                descriptor_table_address[queue_select_val] = ((uint64_t)(*queue_desc_high) << 32) | (*queue_desc_low);
                available_ring_address[queue_select_val] = ((uint64_t)(*queue_avail_high) << 32) | (*queue_avail_low);
                used_ring_address[queue_select_val] = ((uint64_t)(*queue_used_high) << 32) | (*queue_used_low);


                *sel_generation = curr_sel_generation + 1;
                prev_sel_generation = curr_sel_generation + 1;

                if (queue_select_val == (num_queues - 1))
                    break;
            }
            usleep(1);
        }
        for (uint32_t i = 0; i < num_queues; i++) {
            desc[i] = (struct vring_desc*) (memory + (descriptor_table_address[i] - starting_address));
            avail[i] = (struct vring_avail*) (memory + (available_ring_address[i] - starting_address));
            used[i] = (struct vring_used*) (memory + (used_ring_address[i] - starting_address));
        }
        while (!exit_thread_flag){
            if (*status & VIRTIO_CONFIG_S_DRIVER_OK) {
                break;
            }
        }
    }

    void device_loop(){
        std::vector<uint16_t> processed(num_queues, 0);

        while (!exit_thread_flag) {
            if (*magic_value != ('v' | 'i' << 8 | 'r' << 16 | 't' << 24)) {
                return;
            }

            // uint32_t queue_notify_val = *queue_notify;
            // If any interrupts have been acked by device, unset interrupt on plic
            ack_interrupt();

            // Process each virtqueue
            for(uint32_t queue_idx=0; queue_idx<num_queues; queue_idx++){
                struct vring_desc *desc_q = desc[queue_idx];
                struct vring_avail *avail_q = avail[queue_idx];
                struct vring_used *used_q = used[queue_idx];
                
                // if (queue_notify_val == i) {
                    __sync_synchronize();
                    bool should_i_set_interrupt=false;
                    uint16_t avail_idx = avail_q->idx;
                    /*
                    processed[i] represents the tail of the queue (our point of view)
                    avail_idx represents the head of the queue (driver's point of view)
                    */
                    if (processed[queue_idx] != avail_idx && queue_has_data(queue_idx)) {
                        should_i_set_interrupt=true;
                        /*
                        avail_q stores a list of descriptors for us to process
                        We pick a desc_idx to process from the avail queue
                        */
                        uint16_t desc_idx = avail_q->ring[processed[queue_idx] % queue_size];
                        uint16_t desc_idx_first = desc_idx;
                        
                        /*
                        desc_idx points to an index of desc_q
                        We either read or write data to that index in the descriptor queue
                        */
                        uint64_t num_bytes_written = 0;
                        uint64_t l = desc_q[desc_idx % queue_size].len;
                        uint64_t a = desc_q[desc_idx % queue_size].addr;
                        uint8_t *addr = memory + (a - starting_address);;
                        
                        /*
                        Sometimes we just process one entry in the desc_q
                        Sometimes the entries have a next flag set 
                        (desc_q[desc_idx].flags & VRING_DESC_F_NEXT)
                        which means that we need to process multiple entries
                        till we encounter an entry without that flag
                        */
                        while (true) {
                            l = desc_q[desc_idx % queue_size].len;
                            a = desc_q[desc_idx % queue_size].addr;
                            addr = memory + (a - starting_address);
                            
                            if ((desc_q[desc_idx % queue_size].flags & VRING_DESC_F_NEXT)) {
                                if (num_bytes_written < queue_header_size) {
                                    process_queue_start(queue_idx, addr, l);
                                } else {
                                    process_queue_data(queue_idx, addr, l);
                                }
                                num_bytes_written += l;
                                desc_idx = desc_q[desc_idx % queue_size].next;
                            } else {
                                process_queue_complete(queue_idx, addr, l);
                                num_bytes_written += l;
                                break;
                            }
                        }
                        
                        /*
                        We then update the used queue to inform the driver
                        that we've processed desc_idx_first in the descriptor queue
                        */
                        uint16_t used_idx = used_q->idx;
                        used_q->ring[used_idx % queue_size].id = desc_idx_first;
                        used_q->ring[used_idx % queue_size].len = num_bytes_written; // Is this the right value? Should it be device dependent
                        __sync_synchronize();
                        used_q->idx = used_idx + 1;

                        processed[queue_idx] += 1;
                    }
                    if (should_i_set_interrupt){
                        // Set interrupt on plic if we processed at least 1 descriptor
                        set_interrupt();
                    }
                // }
            }
            usleep(1);
        }
    }
    virtual ~VirtioDevice() = default;

    
};
