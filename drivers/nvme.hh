/*
 * Copyright (C) 2023 Jan Braunwarth
 * Copyright (C) 2024 Waldemar Kozaczuk
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef NVME_DRIVER_H
#define NVME_DRIVER_H

#include "drivers/driver.hh"
#include "drivers/nvme-queue.hh"
#include "drivers/pci-device.hh"
#include <map>
#include <memory>
#include <osv/aligned_new.hh>
#include <osv/interrupt.hh>
#include <osv/msi.hh>
#include <osv/nvme-structs.h>
#include <vector>

// Volatile Write Cache
#define NVME_VWC_ENABLED 1

#define NVME_ADMIN_QUEUE_SIZE 8
#define NVME_IO_QUEUE_SIZE 32
#define NVME_NAMESPACE_DEFAULT_NS 1

#define NVME_CTRL_CONFIG_IO_CQ_ENTRY_SIZE_16_BYTES 4
#define NVME_CTRL_CONFIG_IO_SQ_ENTRY_SIZE_64_BYTES 6
#define NVME_CTRL_CONFIG_PAGE_SIZE_4K 0

namespace nvme {

enum NVME_IO_QUEUE_PRIORITY {
  NVME_IO_QUEUE_PRIORITY_URGENT = 0,
  NVME_IO_QUEUE_PRIORITY_HIGH = 1,
  NVME_IO_QUEUE_PRIORITY_MEDIUM = 2,
  NVME_IO_QUEUE_PRIORITY_LOW = 3,
};

enum CMD_IDENTIFY_CNS {
  CMD_IDENTIFY_NAMESPACE = 0,
  CMD_IDENTIFY_CONTROLLER = 1,
};

enum NVME_CONTROLLER_EN {
  CTRL_EN_DISABLE = 0,
  CTRL_EN_ENABLE = 1,
};

// Driver for a single NVMe controller (one PCI function). It owns the admin
// queue used to bring the controller up and configure it, and a set of I/O
// queues created on demand. Completions are delivered through MSI-X: every
// queue's interrupt vector simply calls queue_pair::process_completions().
class nvme_driver : public hw_driver {
public:
  explicit nvme_driver(pci::device &dev);
  virtual ~nvme_driver() {}

  // hw_driver interface
  virtual std::string get_name() const { return "nvme"; }
  virtual void dump_config();
  static hw_driver *probe(hw_device *dev);
  static std::vector<nvme_driver *> nvme_drives;
  
  // Max IO op size and max queue depth (read from the controller)
  size_t max_transfer_bytes() const { return _max_transfer; }
  u32 max_queue_depth() const { return _qsize; }

  // --- I/O queue management (public API used by the app/io backend) ---

  // Create an I/O queue pair of the given depth and return an opaque handle
  // (really an io_queue_pair*). target_interrupt_cpu pins the queue's MSI-X
  // completion interrupt; nullptr uses the current CPU.
  void *create_io_queue(int qsize, sched::cpu *target_interrupt_cpu = nullptr,
                        bool interrupts = true);
  void remove_io_user_queue(io_queue_pair *queue);

  bool reset_and_destroy_controller();
  bool shutdown_controller();

  // --- multi-controller registry ---
  static nvme_driver *get_nvme_device(int id);
  const int get_id() { return _id; }

  // Namespace geometry, keyed by nsid (normally a single entry keyed by 1).
  // Shared (by reference) with every queue_pair so they can size transfers.
  std::map<u32, nvme_ns_t *> _ns_data;

private:
  // --- controller bring-up / configuration ---
  bool parse_pci_config();
  void init_controller_config();
  int enable_disable_controller(bool enable);
  int wait_for_controller_ready_change(int ready);
  int wait_for_controller_shutdown_done();
  int get_worst_cast_time();

  // --- admin queue + admin commands ---
  void create_admin_queue();
  int identify_controller();
  int identify_namespace(u32 ns);
  int set_number_of_queues(u16 num, u16 *ret);
  int set_interrupt_coalescing(u8 threshold, u8 time);
  void enable_write_cache();

  // --- interrupts (MSI-X) ---
  void enable_msix();
  // Wire MSI-X vector `iv` so its ISR drains `qp` via process_completions().
  // Used for both the admin queue (iv 0) and every I/O queue.
  bool msix_register_completion_interrupt(unsigned iv, queue_pair *qp,
                                          sched::cpu *affinity_cpu);

  // --- registry / instance bookkeeping ---
  static int _instance;
  int _id;

  // --- queues ---
  std::unique_ptr<admin_queue_pair, aligned_new_deleter<admin_queue_pair>>
      _admin_queue;
  std::vector<
      std::unique_ptr<io_queue_pair, aligned_new_deleter<io_queue_pair>>>
      _io_queues;
  size_t _queue_id_counter = 0;

  // create_io_queue and remove_io_user_queue are called from application
  // threads -- the fio engine creates one queue per job, all at once -- and
  // between them they hand out queue ids, grow _io_queues, claim an MSI-X
  // vector and run an admin command. None of that is safe to interleave: two
  // threads reading _queue_id_counter together take the same id, and with it
  // the same interrupt vector and doorbell.
  mutex _queue_admin_lock;

  // --- controller registers / PCI ---
  pci::device &_dev;
  pci::bar *_bar0 = nullptr;
  nvme_controller_reg_t *_control_reg = nullptr;
  u32 _doorbell_stride;
  u32 _qsize;

  std::unique_ptr<nvme_identify_ctlr_t> _identify_controller;
  size_t _max_transfer = 0;

  // What the controller allocated in response to Set Features / Number of
  // Queues. Creating more than this is refused by the controller, so it is the
  // ceiling create_io_queue enforces. One until the request has been made.
  u16 _max_io_queues = 1;

  // --- interrupt plumbing ---
  interrupt_manager _msi;
  std::vector<std::unique_ptr<msix_vector>> _msix_vectors;
};

} // namespace nvme
#endif
