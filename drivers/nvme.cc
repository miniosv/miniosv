/*
 * Copyright (C) 2023 Jan Braunwarth
 * Copyright (C) 2024 Waldemar Kozaczuk
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <sys/cdefs.h>

#include "drivers/nvme.hh"
#include "drivers/nvme-queue.hh"
#include "drivers/pci-device.hh"

#include <cassert>
#include <errno.h>
#include <map>
#include <string.h>
#include <string>
#include <sys/mman.h>

#include <osv/aligned_new.hh>
#include <osv/debug.h>
#include <osv/drivers_config.h>
#include <osv/interrupt.hh>
#include <osv/sched.hh>
#include <osv/trace.hh>
#include <osv/mem/mapping.hh>


namespace nvme {

// ===========================================================================
// Static members
// ===========================================================================

// Registry of all probed controllers, indexed by _id (see get_nvme_device).
std::vector<nvme_driver *> nvme_driver::nvme_drives{};
int nvme_driver::_instance = 0;

// ===========================================================================
// Admin command builders
// ===========================================================================

static void setup_features_cmd(nvme_sq_entry_t *cmd, u8 feature_id, u32 val) {
  memset(cmd, 0, sizeof(nvme_sq_entry_t));
  cmd->set_features.common.opc = NVME_ACMD_SET_FEATURES;
  cmd->set_features.fid = feature_id;
  cmd->set_features.val = val;
}

static void setup_identify_cmd(nvme_sq_entry_t *cmd, u32 namespace_id, u32 cns) {
  memset(cmd, 0, sizeof(nvme_sq_entry_t));
  cmd->identify.common.opc = NVME_ACMD_IDENTIFY;
  cmd->identify.common.nsid = namespace_id;
  cmd->identify.cns = cns;
}

template <typename Q>
static void setup_create_io_queue_cmd(Q *create_queue_cmd, int qid, int qsize,
                                      u8 command_opcode, u64 queue_addr) {
  assert(create_queue_cmd);
  memset(create_queue_cmd, 0, sizeof(*create_queue_cmd));

  create_queue_cmd->common.opc = command_opcode;
  create_queue_cmd->common.prp1 = queue_addr;
  create_queue_cmd->qid = qid;
  create_queue_cmd->qsize = qsize - 1;
  create_queue_cmd->pc = 1;
}

template <typename Q>
static void setup_delete_io_queue_cmd(Q *delete_queue_cmd, int qid,
                                      u8 command_opcode, u64 queue_addr) {
  assert(delete_queue_cmd);
  memset(delete_queue_cmd, 0, sizeof(*delete_queue_cmd));

  delete_queue_cmd->common.opc = command_opcode;
  delete_queue_cmd->common.prp1 = queue_addr;
  delete_queue_cmd->qid = qid;
}

// ===========================================================================
// Lifecycle / probing
// ===========================================================================

nvme_driver::nvme_driver(pci::device &pci_dev) : _dev(pci_dev), _msi(&pci_dev) {
  auto parse_ok = parse_pci_config();
  assert(parse_ok);

  enable_msix();

  _id = _instance++;

  _doorbell_stride = 1 << (2 + _control_reg->cap.dstrd);
  _qsize = _control_reg->cap.mqes + 1;

  // A controller must be configured while disabled, and it cannot be
  // enabled before AQA/ASQ/ACQ are valid - the spec makes that a fatal
  // status, and QEMU's NVMe never raises CSTS.RDY in that case. (The
  // old enable-first sequence only worked on x86 because SeaBIOS's
  // NVMe boot driver had already set up admin queues and left the
  // controller enabled; on aarch64 there is no firmware.)
  // If firmware did leave it enabled, disable it first.
  assert(enable_disable_controller(false) == 0);

  init_controller_config();
  create_admin_queue();

  assert(enable_disable_controller(true) == 0);

  assert(identify_controller() == 0);
  assert(identify_namespace(NVME_NAMESPACE_DEFAULT_NS) == 0);

  // Ask for I/O queues before creating any
  u16 want = static_cast<u16>(sched::cpus.size());
  u16 granted = 0;
  if (set_number_of_queues(want, &granted) == 0 && granted) {
    _max_io_queues = granted;
  }
  printf("nvme: %u I/O queues allocated (asked for %u)\n", _max_io_queues, want);
  // Read MDTS from the controller
  if (_identify_controller->mdts) {
    const size_t min_page = size_t(1) << (12 + _control_reg->cap.mpsmin);
    _max_transfer = min_page << _identify_controller->mdts;
  }
  printf("nvme: max transfer %zu KiB (mdts=%u)\n",
         _max_transfer >> 10, _identify_controller->mdts);

  // Enable write cache if available
  if (_identify_controller->vwc & 0x1 && NVME_VWC_ENABLED) {
    enable_write_cache();
  }

  nvme_drives.push_back(this);
}

hw_driver *nvme_driver::probe(hw_device *dev) {
  if (auto pci_dev = dynamic_cast<pci::device *>(dev)) {
    if ((pci_dev->get_base_class_code() == pci::function::PCI_CLASS_STORAGE) &&
        (pci_dev->get_sub_class_code() ==
         pci::function::PCI_SUB_CLASS_STORAGE_NVMC) &&
        (pci_dev->get_programming_interface() == 2)) // detect NVMe device
      return aligned_new<nvme_driver>(*pci_dev);
  }
  return nullptr;
}

nvme_driver *nvme_driver::get_nvme_device(int id) {
  if (id < 0 || (size_t)id >= nvme_drives.size()) {
    // Return nullptr if the id is out of bounds
    // Call side checks handles it appropriately.
    return nullptr;
  }
  return nvme_drives[id];
}

// ===========================================================================
// Controller bring-up / configuration
// ===========================================================================

bool nvme_driver::parse_pci_config() {
  _bar0 = _dev.get_bar(1);
  if (_bar0 == nullptr) {
    return false;
  }
  _bar0->map();
  if (!_bar0->is_mapped()) {
    return false;
  }
  _control_reg = (nvme_controller_reg_t *)_bar0->get_mmio();
  return true;
}

void nvme_driver::init_controller_config() {
  nvme_controller_config_t cc;
  cc.val = mmio_getl(&_control_reg->cc.val);
  cc.iocqes = NVME_CTRL_CONFIG_IO_CQ_ENTRY_SIZE_16_BYTES;
  cc.iosqes = NVME_CTRL_CONFIG_IO_SQ_ENTRY_SIZE_64_BYTES;
  cc.mps = NVME_CTRL_CONFIG_PAGE_SIZE_4K;

  mmio_setl(&_control_reg->cc, cc.val);
}

int nvme_driver::enable_disable_controller(bool enable) {
  nvme_controller_config_t cc;
  cc.val = mmio_getl(&_control_reg->cc);

  u32 expected_en = enable ? CTRL_EN_DISABLE : CTRL_EN_ENABLE;
  u32 new_en = enable ? CTRL_EN_ENABLE : CTRL_EN_DISABLE;

  if (cc.en == new_en)
    return 0;

  assert(cc.en == expected_en); // check current status
  cc.en = new_en;

  mmio_setl(&_control_reg->cc, cc.val);
  return wait_for_controller_ready_change(new_en);
}

int nvme_driver::get_worst_cast_time() {
  // field is in 500ms units (-> FFh = 127.5s)
  if (_control_reg->cap.to > 0) {
    return _control_reg->cap.to;
  }
  if (_control_reg->cc.crime == 0) {
    return _control_reg->crto.crwmt;
  } else {
    return _control_reg->crto.crimt;
  }
}

int nvme_driver::wait_for_controller_ready_change(int ready) {
  int timeout = get_worst_cast_time(); // timeout in 0.05ms steps
  nvme_controller_status_t csts;
  for (int i = 0; i < timeout; i++) {
    csts.val = mmio_getl(&_control_reg->csts);
    if (csts.rdy == ready)
      return 0;
    usleep(500 * 1000); // steps are in 500ms units
  }
  nvme_e("timeout=%d waiting for ready %d", timeout, ready);
  return ETIME;
}

int nvme_driver::wait_for_controller_shutdown_done() {
  int timeout = get_worst_cast_time(); // timeout in 0.05ms steps
  nvme_controller_status_t csts;
  for (int i = 0; i < timeout; i++) {
    csts.val = mmio_getl(&_control_reg->csts);
    if (csts.shst == 2 && csts.st == 0) {
      printf("controller shutdown properly\n");
      return 0;
    }
    usleep(500 * 1000); // steps are in 500ms units
  }
  nvme_e("timeout=%d waiting for shutdown with current status%d type%d", timeout,
         csts.shst, csts.st);
  return ETIME;
}

bool nvme_driver::shutdown_controller() {
  nvme_controller_config_t cc;
  cc.val = mmio_getl(&_control_reg->cc);

  // 1. If the controller is enabled (i.e., CC.EN is set to '1')
  assert(cc.en == 1);

  cc.shn = 1; // normal shutdown
  mmio_setl(&_control_reg->cc, cc.val);
  return wait_for_controller_shutdown_done();
}

bool nvme_driver::reset_and_destroy_controller() {
  // 1. disable the controller
  shutdown_controller();

  // 2. check for other stuff idky
  usleep(1000000);

  return true;
}

// ===========================================================================
// Admin queue + admin commands
// ===========================================================================

void nvme_driver::create_admin_queue() {
  u32 *sq_doorbell = _control_reg->sq0tdbl;
  u32 *cq_doorbell = (u32 *)((u64)sq_doorbell + _doorbell_stride);

  int qsize = NVME_ADMIN_QUEUE_SIZE;
  _admin_queue =
      std::unique_ptr<admin_queue_pair, aligned_new_deleter<admin_queue_pair>>(
          aligned_new<admin_queue_pair>(_id, 0, qsize, _dev, sq_doorbell,
                                        cq_doorbell, _ns_data));

  // The admin queue always uses MSI-X vector 0.
  msix_register_completion_interrupt(0, _admin_queue.get(), sched::current_cpu);

  nvme_adminq_attr_t aqa;
  aqa.val = 0;
  aqa.asqs = aqa.acqs = qsize - 1;

  mmio_setl(&_control_reg->aqa, aqa.val);
  mmio_setq(&_control_reg->asq, _admin_queue->sq_phys_addr());
  mmio_setq(&_control_reg->acq, _admin_queue->cq_phys_addr());
}

int nvme_driver::identify_controller() {
  assert(_admin_queue);
  nvme_sq_entry_t cmd;
  auto data = new nvme_identify_ctlr_t;

  // On AWS Nitro a freshly attached EBS volume can report CSTS.RDY and
  // complete admin commands successfully while its backing attachment
  // is still settling - the identify payload comes back all zeros.
  // cqes is architecturally required to be nonzero on a functional
  // controller, so treat zero as "not ready yet" and retry.
  for (int attempt = 0;; attempt++) {
    setup_identify_cmd(&cmd, 0, CMD_IDENTIFY_CONTROLLER);
    auto res = _admin_queue->submit_and_return_on_completion(
        &cmd, (void *)mem::mapping::to_phys(data), mem::mapping::page_size);

    if (res.sc != 0 || res.sct != 0) {
      nvme_e("Identify controller failed nvme%d, sct=%d, sc=%d", _id, res.sct,
             res.sc);
      return EIO;
    }
    if (data->cqes != 0) {
      break;
    }
    if (attempt >= 60) { // 30s, same order as EBS attach worst cases
      nvme_e("Identify controller returned no data nvme%d", _id);
      return EIO;
    }
    if (attempt == 0) {
      nvme_i("nvme%d: identify data empty, waiting for backing storage\n", _id);
    }
    usleep(500 * 1000);
  }

  printf("cqes min: %lu cqes max: %lu\n", (u64)(data->cqes >> 4),
         (u64)(data->cqes & 0xf));

  _identify_controller.reset(data);
  return 0;
}

int nvme_driver::identify_namespace(u32 nsid) {
  assert(_admin_queue);
  nvme_sq_entry_t cmd;
  auto data = std::unique_ptr<nvme_identify_ns_t>(new nvme_identify_ns_t);

  // Same EBS-attach race as in identify_controller: success status with
  // a zeroed payload means the backing volume is not ready yet. A real
  // namespace always has a nonzero capacity here.
  for (int attempt = 0;; attempt++) {
    setup_identify_cmd(&cmd, nsid, CMD_IDENTIFY_NAMESPACE);
    auto res = _admin_queue->submit_and_return_on_completion(
        &cmd, (void *)mem::mapping::to_phys(data.get()), mem::mapping::page_size);
    if (res.sc != 0 || res.sct != 0) {
      nvme_e("Identify namespace failed nvme%d nsid=%d, sct=%d, sc=%d", _id,
             nsid, res.sct, res.sc);
      return EIO;
    }
    if (data->ncap != 0) {
      break;
    }
    if (attempt >= 60) {
      nvme_e("Identify namespace returned no data nvme%d nsid=%d", _id, nsid);
      return EIO;
    }
    if (attempt == 0) {
      nvme_e("nvme%d: namespace %d empty, waiting for backing storage\n", _id,
             nsid);
    }
    usleep(500 * 1000);
  }

  _ns_data.insert(std::make_pair(nsid, new nvme_ns_t));
  _ns_data[nsid]->blockcount = data->ncap;
  _ns_data[nsid]->blockshift = data->lbaf[data->flbas & 0xF].lbads;
  _ns_data[nsid]->blocksize = 1 << _ns_data[nsid]->blockshift;
  _ns_data[nsid]->bpshift = NVME_PAGESHIFT - _ns_data[nsid]->blockshift;
  _ns_data[nsid]->id = nsid;

  printf("Identified namespace with nsid=%d, blockcount=%lu, blocksize=%d\n",
         nsid, (unsigned long)_ns_data[nsid]->blockcount,
         _ns_data[nsid]->blocksize);
  return 0;
}

int nvme_driver::set_number_of_queues(u16 num, u16 *ret) {
  nvme_sq_entry_t cmd;
  setup_features_cmd(&cmd, NVME_FEATURE_NUM_QUEUES, (num << 16) | num);
  auto res = _admin_queue->submit_and_return_on_completion(&cmd);

  u16 cq_num = res.cs >> 16;
  u16 sq_num = res.cs & 0xffff;

  nvme_i("Queues supported: CQ num=%d, SQ num=%d, MSI/X entries=%d",
         res.cs >> 16, res.cs & 0xffff, _dev.msix_get_num_entries());

  if (res.sct != 0 || res.sc != 0)
    return EIO;

  u16 usable = (cq_num < sq_num ? cq_num : sq_num) + 1;
  *ret = num < usable ? num : usable;
  return 0;
}

int nvme_driver::set_interrupt_coalescing(u8 threshold, u8 time) {
  nvme_sq_entry_t cmd;
  setup_features_cmd(&cmd, NVME_FEATURE_INT_COALESCING,
                     threshold | (time << 8));
  auto res = _admin_queue->submit_and_return_on_completion(&cmd);

  if (res.sct != 0 || res.sc != 0) {
    nvme_e("Failed to enable interrupt coalescing: sc=%#x sct=%#x", res.sc,
           res.sct);
    return EIO;
  }
  nvme_i("Enabled interrupt coalescing");
  return 0;
}

void nvme_driver::enable_write_cache() {
  nvme_sq_entry_t cmd;
  setup_features_cmd(&cmd, NVME_FEATURE_WRITE_CACHE, 1);
  auto res = _admin_queue->submit_and_return_on_completion(&cmd);
  if (res.sct != 0 || res.sc != 0) {
    nvme_e("Failed to enable write cache: sc=%#x sct=%#x", res.sc, res.sct);
  } else {
    nvme_i("Enabled write cache");
  }
}

// ===========================================================================
// I/O queue management
// ===========================================================================

void *nvme_driver::create_io_queue(int qsize, sched::cpu *target_interrupt_cpu,
                                   bool interrupts) {
  assert(qsize > 1 && qsize < _qsize);
  SCOPE_LOCK(_queue_admin_lock);

  size_t qid = ++_queue_id_counter;
  assert(qid < (1 << 16) && qid > 0);

  if (qid > _max_io_queues) {
    nvme_e("asked for I/O queue %zu of %u allocated", qid, _max_io_queues);
    return nullptr;
  }

  u32 *sq_doorbell =
      (u32 *)((u64)_control_reg->sq0tdbl + 2 * _doorbell_stride * qid);
  u32 *cq_doorbell = (u32 *)((u64)sq_doorbell + _doorbell_stride);

  // Allocate the queue pair (owns its SQ and CQ ring buffers).
  auto queue =
      std::unique_ptr<io_queue_pair, aligned_new_deleter<io_queue_pair>>(
          aligned_new<io_queue_pair>(_id, qid, qsize, _dev, sq_doorbell,
                                     cq_doorbell, _ns_data));

  int iv = qid;

  // Completion queue command (created before the SQ per the NVMe spec).
  nvme_acmd_create_cq_t cmd_cq;
  setup_create_io_queue_cmd<nvme_acmd_create_cq_t>(
      &cmd_cq, qid, qsize, NVME_ACMD_CREATE_CQ, queue->cq_phys_addr());
  cmd_cq.iv = iv;
  cmd_cq.ien = 1;

  // Submission queue command.
  nvme_acmd_create_sq_t cmd_sq;
  setup_create_io_queue_cmd<nvme_acmd_create_sq_t>(
      &cmd_sq, qid, qsize, NVME_ACMD_CREATE_SQ, queue->sq_phys_addr());
  cmd_sq.qprio = NVME_IO_QUEUE_PRIORITY_URGENT;
  cmd_sq.cqid = qid;

  // Hand ownership to _io_queues, but keep a raw pointer: `queue` is moved-from
  // below and must not be dereferenced afterwards (this used to register the
  // interrupt with a moved-from, i.e. null, pointer).
  io_queue_pair *qp = queue.get();
  _io_queues.push_back(std::move(queue));
  assert(target_interrupt_cpu != nullptr);
  if (!msix_register_completion_interrupt(iv, qp, target_interrupt_cpu)) {
    nvme_e("no interrupt vector for qid %d; not creating the queue\n", qid);
    _io_queues.pop_back();
    return nullptr;
  }

  // The status matters: a rejected create leaves no queue on the controller,
  // and a caller given one anyway submits into nothing and waits for ever.
  auto cq_res = _admin_queue->submit_and_return_on_completion(
      (nvme_sq_entry_t *)&cmd_cq);
  if (cq_res.sct != 0 || cq_res.sc != 0) {
    nvme_e("create CQ %d rejected: sct=%#x sc=%#x", qid, cq_res.sct, cq_res.sc);
    _io_queues.pop_back();
    return nullptr;
  }
  auto sq_res = _admin_queue->submit_and_return_on_completion(
      (nvme_sq_entry_t *)&cmd_sq);
  if (sq_res.sct != 0 || sq_res.sc != 0) {
    nvme_e("create SQ %d rejected: sct=%#x sc=%#x", qid, sq_res.sct, sq_res.sc);
    _io_queues.pop_back();
    return nullptr;
  }

  if (!interrupts) {
    qp->set_polled(true);
  }

  printf("nvme: Created I/O queue pair for qid:%d with size:%d pointer=%p (%s)\n",
         qid, qsize, qp, interrupts ? "interrupt" : "polled");
  return qp;
}

void nvme_driver::remove_io_user_queue(io_queue_pair *queue) {
  SCOPE_LOCK(_queue_admin_lock);

  u32 qid = queue->_id;

  // Completion queue command (removed after the SQ per the NVMe spec).
  nvme_acmd_delete_ioq_t cmd_cq;
  setup_delete_io_queue_cmd<nvme_acmd_delete_ioq_t>(
      &cmd_cq, qid, NVME_ACMD_DELETE_CQ, _io_queues[qid - 1]->cq_phys_addr());

  // Submission queue command.
  nvme_acmd_delete_ioq_t cmd_sq;
  setup_delete_io_queue_cmd<nvme_acmd_delete_ioq_t>(
      &cmd_sq, qid, NVME_ACMD_DELETE_SQ, _io_queues[qid - 1]->sq_phys_addr());

  // Deleting is best effort: the queue is going away either way, but a refusal
  // is worth knowing about.
  auto del_cq = _admin_queue->submit_and_return_on_completion(
      (nvme_sq_entry_t *)&cmd_cq);
  if (del_cq.sct != 0 || del_cq.sc != 0) {
    nvme_e("delete CQ %d refused: sct=%#x sc=%#x", qid, del_cq.sct, del_cq.sc);
  }
  auto del_sq = _admin_queue->submit_and_return_on_completion(
      (nvme_sq_entry_t *)&cmd_sq);
  if (del_sq.sct != 0 || del_sq.sc != 0) {
    nvme_e("delete SQ %d refused: sct=%#x sc=%#x", qid, del_sq.sct, del_sq.sc);
  }

  debugf("nvme: Removed I/O user queue pair for qid:%d with size:%d\n", qid,
         _qsize);
}

// ===========================================================================
// Interrupts (MSI-X)
// ===========================================================================

void nvme_driver::enable_msix() {
  _dev.set_bus_master(true);
  _dev.msix_enable();
  assert(_dev.is_msix());

  // one slot per MSI-X table entry of the device (entry 0 is the admin queue)
  unsigned int vectors_num =
      std::min((size_t)_dev.msix_get_num_entries(), sched::cpus.size() + 1);

  printf("we have %i vectors_num\n", vectors_num); 

  // and not push them; currently i am not sure if this can be done mt so
  // i'll go with that solution to just make it big enough
  _msix_vectors = std::vector<std::unique_ptr<msix_vector>>(vectors_num);
}

bool nvme_driver::msix_register_completion_interrupt(unsigned iv, queue_pair *qp,
                                                     sched::cpu *affinity_cpu) {
  _dev.msix_mask_all();
  _dev.msix_mask_entry(iv);

  // The ISR just drains the queue's completions.
  auto vec = std::unique_ptr<msix_vector>(new msix_vector(&_dev));
  _msi.assign_isr(vec.get(), [qp]() { qp->process_completions(512); });

  printf("running on isr %i\n", vec.get()->get_vector()); 

  if (!_msi.setup_entry(iv, vec.get())) {
    return false;
  }

  vec->set_affinity(affinity_cpu);

  if (iv < _msix_vectors.size()) {
    _msix_vectors[iv] = std::move(vec);
  } else {
    nvme_e("binding_entry %d registration failed\n", iv);
    return false;
  }
  _msix_vectors[iv]->msix_unmask_entries();

  _dev.msix_unmask_all();
  _dev.msix_unmask_entry(iv);
  return true;
}

// ===========================================================================
// Misc
// ===========================================================================

void nvme_driver::dump_config(void) {
  u8 B, D, F;
  _dev.get_bdf(B, D, F);

  _dev.dump_config();
  nvme_i("%s [%x:%x.%x] vid:id= %x:%x\n", get_name().c_str(), (u16)B, (u16)D,
         (u16)F, _dev.get_vendor_id(), _dev.get_device_id());
}

} // namespace nvme
