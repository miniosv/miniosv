/*
 * Copyright (C) 2023 Jan Braunwarth
 * Copyright (C) 2024 Waldemar Kozaczuk
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <sys/cdefs.h>

#include <cassert>

#include <osv/trace.hh>
#include <osv/mmio.hh>
#include <osv/align.hh>
#include "nvme-queue.hh"
#include "drivers/pci-device.hh"
#include <osv/mem/mapping.hh>
#include <osv/mem/frames.hh>
#include <osv/mem/phys.hh>
#include "processor.hh"

TRACEPOINT(trace_nvme_cq_wait, "nvme%d qid=%d, cq_head=%d", int, int, int);
TRACEPOINT(trace_nvme_cq_woken, "nvme%d qid=%d, have_elements=%d", int, int, bool);
TRACEPOINT(trace_nvme_cq_not_empty, "nvme%d qid=%d, not_empty=%d", int, int, bool);
TRACEPOINT(trace_nvme_cq_head_advance, "nvme%d qid=%d cq_head=%d", int, int, int);
TRACEPOINT(trace_nvme_cq_new_entry, "nvme%d cid=%d qid=%d sqhd=%d", int,  int, int, int);
TRACEPOINT(trace_nvme_sq_new_entry, "nvme%d cid=%d qid=%d sqhd=%d", int, int, int, int);

TRACEPOINT(trace_nvme_enable_interrupts, "nvme%d qid=%d", int, int);
TRACEPOINT(trace_nvme_disable_interrupts, "nvme%d qid=%d", int, int);

TRACEPOINT(trace_nvme_req_done_error, "nvme%d qid=%d, cid=%d, status type=%#x, status code=%#x, req=%p", int, int, u16, u8, u8, void *);
TRACEPOINT(trace_nvme_req_done_success, "nvme%d qid=%d, cid=%d, req=%p", int, int, u16, void *);

TRACEPOINT(trace_nvme_admin_cmd_submit, "nvme%d qid=%d, cid=%d, opc=%d", int, int, int, u8);
TRACEPOINT(trace_nvme_read_write_cmd_submit, "nvme%d qid=%d cid=%d, bio=%p, slba=%d, nlb=%d, write=%d", int, int, u16, void *, u64, u32, bool);

TRACEPOINT(trace_nvme_sq_tail_advance, "nvme%d qid=%d, sq_tail=%d, sq_head=%d, depth=%d, full=%d", int, int, int, int, int, bool);
TRACEPOINT(trace_nvme_sq_full_wait, "nvme%d qid=%d, sq_tail=%d, sq_head=%d", int, int, int, int);
TRACEPOINT(trace_nvme_sq_full_wake, "nvme%d qid=%d, sq_tail=%d, sq_head=%d", int, int, int, int);

TRACEPOINT(trace_nvme_cid_conflict, "nvme%d qid=%d, cid=%d", int, int, int);

TRACEPOINT(trace_nvme_prp_entry, "nvme%d phy_addr=%d, virt_addr=%d, cnt=%p", int, void *, void *, int);
TRACEPOINT(trace_nvme_prp_alloc, "nvme%d qid=%d, prp=%p", int, int, void *);
TRACEPOINT(trace_nvme_prp_free, "nvme%d qid=%d, prp=%p", int, int, void *);

TRACEPOINT(trace_nvme_op_read, "payload=%lu addr=%d len=%d", unsigned long, void *, int);
TRACEPOINT(trace_nvme_op_resread, "payload=%d cid=%d", int, int);

TRACEPOINT(trace_nvme_op_write, "nvme%d addr=%d len=%d", int, void *, int);


namespace nvme
{
    //
    // queue_pair: machinery shared by the admin and I/O queues
    //
    queue_pair::queue_pair(
        int did,
        u32 id,
        int qsize,
        pci::device& dev,
        u32 *sq_doorbell,
        u32 *cq_doorbell,
        std::map<u32, nvme_ns_t *> &ns)
        : _id(id), _driver_id(did), _qsize(qsize), _dev(&dev), _sq(sq_doorbell), _sq_full(false), _cq(cq_doorbell), _cq_phase_tag(1), _ns(ns)
    {
        _sq_buf_size = qsize * sizeof(nvme_sq_entry_t);
        auto sq_pa = mem::frames::alloc(_sq_buf_size, mem::mapping::page_size);
        assert(sq_pa);
        _sq._addr = (nvme_sq_entry_t *)mem::map_phys(sq_pa, _sq_buf_size);
        memset(_sq._addr, 0, _sq_buf_size);

        _cq_buf_size = qsize * sizeof(nvme_cq_entry_t);
        auto cq_pa = mem::frames::alloc(_cq_buf_size, mem::mapping::page_size);
        assert(cq_pa);
        _cq._addr = (nvme_cq_entry_t *)mem::map_phys(cq_pa, _cq_buf_size);
        memset(_cq._addr, 0, _cq_buf_size);

        assert(!completion_queue_not_empty());
    }

    queue_pair::~queue_pair()
    {
        mem::frames::free(mem::mapping::to_phys(_sq._addr), _sq_buf_size);
        mem::frames::free(mem::mapping::to_phys(_cq._addr), _cq_buf_size);
    }

    inline void queue_pair::advance_sq_tail()
    {
        _sq._tail = (_sq._tail + 1) % _qsize;
        if (((_sq._tail + 1) % _qsize) == _sq._head)
        {
            _sq_full = true;
        }
        trace_nvme_sq_tail_advance(_driver_id, _id, _sq._tail, _sq._head,
                                   (_sq._tail >= _sq._head) ? _sq._tail - _sq._head : _sq._tail + (_qsize - _sq._head),
                                   _sq_full);
    }

    u16 queue_pair::place_sqe(nvme_sq_entry_t *cmd)
    {
        _sq._addr[_sq._tail] = *cmd;

        trace_nvme_sq_new_entry(_driver_id, cmd->rw.common.cid, _id, _sq._tail);

        advance_sq_tail();
        return _sq._tail;
    }

    u16 queue_pair::submit_cmd(nvme_sq_entry_t *cmd)
    {
        auto tail = place_sqe(cmd);
        mmio_setl(_sq._doorbell, _sq._tail);
        return tail;
    }

    void queue_pair::ring_sq_doorbell()
    {
        mmio_setl(_sq._doorbell, _sq._tail);
    }

    nvme_cq_entry_t *queue_pair::get_completion_queue_entry()
    {
        if (!completion_queue_not_empty())
        {
            return nullptr;
        }

        auto *cqe = &_cq._addr[_cq._head];
        assert(cqe->p == _cq_phase_tag);

        trace_nvme_cq_new_entry(_driver_id, cqe->cid, _id, cqe->sqhd);
        return cqe;
    }

    bool queue_pair::completion_queue_not_empty() const
    {
        bool a = reinterpret_cast<volatile nvme_cq_entry_t *>(&_cq._addr[_cq._head])->p == _cq_phase_tag;
        trace_nvme_cq_not_empty(_driver_id, _id, a);
        return a;
    }

    void queue_pair::enable_interrupts()
    {
        _dev->msix_unmask_entry(_id);
        trace_nvme_enable_interrupts(_driver_id, _id);
    }

    void queue_pair::disable_interrupts()
    {
        _dev->msix_mask_entry(_id);
        trace_nvme_disable_interrupts(_driver_id, _id);
    }

    void queue_pair::set_polled(bool polled)
    {
        if (_polled.load(std::memory_order_acquire) == polled) {
            return;
        }

        if (polled) {
            disable_interrupts();
            _polled.store(true, std::memory_order_release);
        } else {
            _polled.store(false, std::memory_order_release);
            enable_interrupts();
        }

        // Wait for any drain that was already under way.
        bool idle = false;
        while (!_draining.compare_exchange_weak(idle, true,
                                                std::memory_order_acquire,
                                                std::memory_order_relaxed)) {
            idle = false;
            processor::spin_hint();
        }
        _draining.store(false, std::memory_order_release);

        if (!polled) {
            process_completions(0);
        }
    }

    //
    // io_queue_pair: read/write/flush path with per-command callbacks and PRPs
    //
    io_queue_pair::io_queue_pair(
        int did,
        u32 id,
        int qsize,
        pci::device& dev,
        u32 *sq_doorbell,
        u32 *cq_doorbell,
        std::map<u32, nvme_ns_t *> &ns)
        : queue_pair(did, id, qsize, dev, sq_doorbell, cq_doorbell, ns)
    {
        init_callbacks(max_pending_levels);
    }

    io_queue_pair::~io_queue_pair()
    {
        for (auto page : _pending_callbacks)
        {
            if (page)
            {
                free(page);
            }
        }
        for (auto locks : _pending_callbacks_locks)
        {
            if (locks)
            {
                free(locks);
            }
        }
    }

    u16 io_queue_pair::submit_flush_cmd(u16 cid, u32 nsid)
    {
        nvme_sq_entry_t cmd;
        memset(&cmd, 0, sizeof(cmd));

        cmd.vs.common.opc = NVME_CMD_FLUSH;
        cmd.vs.common.nsid = nsid;
        cmd.vs.common.cid = cid;

        return submit_cmd(&cmd);
    }

    void io_queue_pair::init_callbacks(u32 max_level)
    {
        for (u32 level = 0; level < max_level; level++)
        {
            _pending_callbacks[level] = (nvme_pending_req *)malloc(sizeof(nvme_pending_req) * _qsize);
            _pending_callbacks_locks[level] = (std::atomic<bool> *)malloc(sizeof(std::atomic<bool>) * _qsize);

            for (u32 idx = 0; idx < _qsize; idx++)
            {
                _pending_callbacks[level][idx] = nvme_pending_req(nullptr, nullptr);
                _pending_callbacks_locks[level][idx] = false;
            }
        }
    }

    void io_queue_pair::map_prps(u32 nsid, nvme_sq_entry_t *cmd, void *payload, nvme_pending_req *pending_req, u64 datasize)
    {
        // u64 nvme_pagesize = _ns[1]->blocksize;
        void *data = (void *)mem::mapping::to_phys(payload);
        pending_req->prp_list = nullptr;

        // The payload is a (possibly scattered, below-phys_mem) buffer-pool page;
        // a wrong translation here would DMA to the wrong physical memory and
        // silently corrupt an unrelated BufferFrame. prp1 may be offset into a
        // page, but the in-page offset MUST survive translation.
        assert(data != nullptr);
        assert(((u64)data & (NVME_PAGESIZE - 1)) == ((u64)payload & (NVME_PAGESIZE - 1)));

        // Depending on the datasize, we map PRPs (Physical Region Page) as follows:
        // 0. We always set the prp1 field to the beginning of the data
        // 1. If data falls within single 4K page then we simply set prp2 to 0
        // 2. If data falls within 2 pages then set prp2 to the second 4K-aligned part of data
        // 3. Otherwise, allocate a physically contigous array long enough to hold addresses
        //    of remaining 4K pages of data
        u64 addr = (u64)data;
        cmd->rw.common.prp1 = addr;
        cmd->rw.common.prp2 = 0;

        // Calculate number of 4K pages and therefore number of entries in the PRP
        // list. The 1st entry rw.common.prp1 can be misaligned but every
        // other one needs to be 4K-aligned
        u64 first_page_start = align_down(addr, NVME_PAGESIZE);
        u64 last_page_end = align_up(addr + datasize, NVME_PAGESIZE);
        int num_of_pages = (last_page_end - first_page_start) / NVME_PAGESIZE;

        trace_nvme_prp_entry(_driver_id, data, payload, num_of_pages);

        if (num_of_pages == 2)
        {
            cmd->rw.common.prp2 = align_down(mem::mapping::to_phys(static_cast<char*>(payload) + NVME_PAGESIZE), NVME_PAGESIZE); // 2nd page start
            // PRP2 must be 4K-aligned, non-zero, a different physical page than
            // prp1, and must back the *next* virtual page (pool is not physically
            // contiguous, so it can't be assumed to be prp1_page + 4K).
            assert(cmd->rw.common.prp2 != 0);
            assert((cmd->rw.common.prp2 & (NVME_PAGESIZE - 1)) == 0);
            assert(cmd->rw.common.prp2 != align_down(addr, NVME_PAGESIZE));
            assert(cmd->rw.common.prp2 ==
                   mem::mapping::to_phys((void *)(align_down((u64)payload, NVME_PAGESIZE) + NVME_PAGESIZE)));
        }
        else if (num_of_pages > 2)
        {
            // Allocate PRP list as the request is larger than 8K.
            // A single list page holds 512 entries and prp1 covers the first
            // page itself, so one list maps a request of up to 512 pages
            // (2 MiB when page-aligned) without chaining. Beyond that the
            // last entry would have to point at a second list, which this
            // driver does not build.
            assert(num_of_pages <= 512);
            u64 *prp_list = nullptr;
            _free_prp_lists.pop(prp_list);
            if (!prp_list)
            { // No free pre-allocated ones, so allocate new one
                prp_list = (u64 *)mem::map_phys(mem::frames::alloc(), 4096);
                trace_nvme_prp_alloc(_driver_id, _id, prp_list);
            }

            assert(prp_list != nullptr);
            cmd->rw.common.prp2 = mem::mapping::to_phys(prp_list);
            // The PRP-list page itself must be 4K-aligned (it points at it).
            assert((cmd->rw.common.prp2 & (NVME_PAGESIZE - 1)) == 0);

            // Save PRP list in bio so it can be de-allocated later
            pending_req->prp_list = prp_list;

            // Fill in the PRP list with the physical address of each subsequent
            // 4K page. The payload is not guaranteed to be physically contiguous
            // (e.g. leanstore's buffer pool is a large scattered anonymous mmap),
            // so every page must be translated individually rather than assuming
            // phys(page i) == phys(page 0) + i*4K. PRP entry i covers virtual
            // page (i+1); page 0 is already in prp1.
            u64 virt_page0 = align_down((u64)payload, NVME_PAGESIZE);
            for (int i = 0; i < num_of_pages - 1; i++)
            {
                void *vaddr = (void *)(virt_page0 + (u64)(i + 1) * NVME_PAGESIZE);
                prp_list[i] = mem::mapping::to_phys(vaddr);
                // Each non-first PRP entry must be a valid, 4K-aligned physical page.
                assert(prp_list[i] != 0);
                assert((prp_list[i] & (NVME_PAGESIZE - 1)) == 0);
            }
        }
    }

    u16 io_queue_pair::submit_read_write_page_cmd(u16 cid, u32 nsid, int opc, u64 slba, u32 nlb, void *payload, nvme_pending_req *req)
    {
        nvme_sq_entry_t cmd;
        memset(&cmd, 0, sizeof(cmd));

        cmd.rw.common.cid = cid;
        cmd.rw.common.opc = opc;
        cmd.rw.common.nsid = nsid;
        // nlb is a 1-based block count; nlb==0 would underflow to 0xFFFFFFFF and
        // the device rejects it as "Invalid Field in Command" (sc=2).
        assert(nlb >= 1);
        cmd.rw.slba = slba;   // starting logical block address
        cmd.rw.nlb = nlb - 1; // number of logical blocks

        u32 datasize = nlb << _ns[nsid]->blockshift;
        assert(datasize > 0);
        map_prps(nsid, &cmd, payload, req, datasize);

        // Place the SQE but do NOT ring the doorbell here; the caller rings once
        // per batch via ring_sq_doorbell() so a whole batch of submissions costs
        // a single MMIO write instead of one per IO.
        return place_sqe(&cmd);
    }

    int io_queue_pair::submit_request(int ns, void *payload, uint64_t addr, uint32_t len, osv_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags, NVME_COMMAND cmd)
    {
        u64 slba = addr >> _ns[ns]->blockshift;
        u32 nlb = len >> _ns[ns]->blockshift;

        // addr/len arrive in bytes and must be block-aligned, and the I/O must
        // stay within the namespace (an out-of-range LBA reads/writes the wrong
        // place). blockcount may be 0 if identify did not populate it; skip then.
        assert((addr & ((1u << _ns[ns]->blockshift) - 1)) == 0);
        assert((len & ((1u << _ns[ns]->blockshift) - 1)) == 0);
        assert(nlb >= 1);
        assert(_ns[ns]->blockcount == 0 || (slba + nlb) <= _ns[ns]->blockcount);

        // SCOPE_LOCK(_lock);
        // u8 counter = 0;
        // TODO: WORKS FOR NOW BUT WE SHOULD ALSO LOOK INTO WHY THE
        // SYNC NOT WORKING
        if (_sq_full || ((_sq._tail + 1) % _qsize) == _sq._head)
        {
            // counter++;
            // if (counter > 10) assert(false);
            // process_completions(10);
            // std::cout << "full" << std::endl;
            return 0;
        }

        assert(!_sq_full);
        assert((((_sq._tail + 1) % _qsize) != _sq._head)); // one left

        //
        // We need to check if there is an outstanding command that uses
        // _sq._tail as command id.
        // This happens if:
        // 1. The SQ is full. Then we just have to wait for an open slot (see above)
        // 2. The Controller already read a SQE but didnt post a CQE yet.
        //    This means we could post the command but need a different cid. To still
        //    use the cid as index to find the corresponding bios we use a matrix
        //    adding columns if we need them
        u16 cid = _sq._tail;

        bool expected = false;
        while (!_pending_callbacks_locks[cid_to_row(cid)][cid_to_col(cid)].compare_exchange_strong(expected, true))
        {
            cid += _qsize;
            auto level = cid_to_row(cid);
            if (level >= max_pending_levels)
                return 0; // this should never be the case
        }

        // Save bio
        assert(cb_fn != nullptr);
        _pending_callbacks[cid_to_row(cid)][cid_to_col(cid)] = nvme_pending_req(cb_fn, cb_arg);

        switch (cmd)
        {
        case NVME_COMMAND::READ:
        {
            trace_nvme_op_read(addr, payload, len);

            submit_read_write_page_cmd(cid, ns, NVME_CMD_READ, slba, nlb, payload, &_pending_callbacks[cid_to_row(cid)][cid_to_col(cid)]);
            break;
        }
        case NVME_COMMAND::WRITE:
        {
        trace_nvme_op_write(addr, payload, len);

            submit_read_write_page_cmd(cid, ns, NVME_CMD_WRITE, slba, nlb, payload, &_pending_callbacks[cid_to_row(cid)][cid_to_col(cid)]);
            break;
        }
        case NVME_COMMAND::FLUSH:
        {
            submit_flush_cmd(cid, ns);
            break;
        }
        default:
        {
            nvme_e("Operation not implemented\n");
            return ENOTBLK;
        }

        }

        ring_sq_doorbell();
	return 1;
    }

    //  returns number of completions processed (may be 0) or negated on error. -ENXIO in the special case that the qpair is failed at the transport layer.
    int io_queue_pair::process_completions(int max)
    {
        int total = 0;
        for (;;) {
            bool idle = false;
            if (!_draining.compare_exchange_strong(idle, true,
                                                   std::memory_order_acquire,
                                                   std::memory_order_relaxed)) {
                return total;
            }
            total += drain_completions(max);
            _draining.store(false, std::memory_order_release);

            if (!completion_queue_not_empty()) {
                return total;
            }
        }
    }

    // The drain itself. Runs with _draining held.
    int io_queue_pair::drain_completions(int max)
    {
        nvme_cq_entry_t *cqep = nullptr;
        int counter = 0;

        max = (max > 0) ? max : _qsize;

        while (counter < max)
        {
            if ((cqep = get_completion_queue_entry()))
            {
                // Read full CQ entry onto stack so we can advance CQ head ASAP
                // and release the CQ slot
                nvme_cq_entry_t cqe = *cqep;
                advance_cq_head();

                // Wake up the requesting thread in case the submission queue was full before
                auto old_sq_head = _sq._head.load();
                _sq._head = cqe.sqhd;

                if (cqe.sc != 0) {
                    // Completion error. Before aborting, dump the failed command
                    // and the page buffer it referenced: a bad PRP / invalid page
                    // (the likely culprit) shows up here as garbage prp1 / magic.
                    // NOTE: the SQ slot may already be reused; this is best-effort
                    // diagnostics on a fatal path.
                    u16 ecid = cqe.cid;
                    nvme_sq_entry_t *failed = &_sq._addr[ecid % _qsize];
                    printf("nvme completion error: sc=%d sct=%d cid=%d opc=%d "
                           "slba=%lu nlb(1based)=%u prp1=0x%lx prp2=0x%lx\n",
                           (int)cqe.sc, (int)cqe.sct, ecid,
                           (int)failed->rw.common.opc, (unsigned long)failed->rw.slba,
                           failed->rw.nlb + 1, (unsigned long)failed->rw.common.prp1,
                           (unsigned long)failed->rw.common.prp2);
                    if (failed->rw.common.prp1) {
                        // prp1 -> the leanstore Page (offset 512 in its frame).
                        // page[0]=GSN, page[2]=magic_debugging_number.
                        auto *page = reinterpret_cast<u64 *>(
                            mem::map_phys(failed->rw.common.prp1, NVME_PAGESIZE));
                        printf("  page(phys->virt)=%p GSN=%lu magic=%lu "
                               "(in-page offset=0x%lx)\n",
                               (void *)page, (unsigned long)page[0],
                               (unsigned long)page[2],
                               (unsigned long)(failed->rw.common.prp1 & (NVME_PAGESIZE - 1)));
                    }
                    assert(false);
                }

                if (old_sq_head != cqe.sqhd && _sq_full)
                {
                    trace_nvme_sq_full_wake(_driver_id, _id, _sq._tail, _sq._head);
                    _sq_full = false;
                }

                // Read cid and release it
                u16 cid = cqe.cid;

                bool expected = true;
                nvme::nvme_pending_req pending_callback = _pending_callbacks[cid_to_row(cid)][cid_to_col(cid)];
                assert(_pending_callbacks_locks[cid_to_row(cid)][cid_to_col(cid)].compare_exchange_strong(expected, false));

                // cb_args is opaque to the driver: only leanstore happens to put a
                // size_t payload id at offset 0. Callers may pass any pointer
                // (including nullptr, e.g. the DMA tests), so guard the deref.
                trace_nvme_op_resread(
                    pending_callback.cb.cb_args ? (int)((size_t*)pending_callback.cb.cb_args)[0] : 0,
                    cid);

                pending_callback.cb.cb(pending_callback.cb.cb_args, nullptr);

                if (pending_callback.prp_list)
                {
                    if (!_free_prp_lists.push((u64 *)pending_callback.prp_list))
                    {
                        //_free_prp_lists is full so free the page
                        mem::frames::free(mem::mapping::to_phys(pending_callback.prp_list));
                    }
                }

                counter++;
            }
            else
            {
                break;
            }
        }

        // Ring the CQ head doorbell once for the whole batch instead of on
        // every completion: the controller only needs the latest head value,
        // so a single MMIO write per poll replaces one write per CQE.
        if (counter > 0)
        {
            mmio_setl(_cq._doorbell, _cq._head);
        }

        return counter;
    }

    //
    // admin_queue_pair: controller setup/configuration commands
    //
    // Admin commands are issued one at a time by submit_and_return_on_completion,
    // which blocks the caller until the matching completion arrives. There is no
    // payload-page DMA to map (map_prps) and no per-command callback table - the
    // completion just has to be handed back to the single waiting submitter.
    //
    admin_queue_pair::admin_queue_pair(
        int did,
        u32 id,
        int qsize,
        pci::device& dev,
        u32 *sq_doorbell,
        u32 *cq_doorbell,
        std::map<u32, nvme_ns_t *> &ns)
        : queue_pair(did, id, qsize, dev, sq_doorbell, cq_doorbell, ns)
    {
    }

    // Called from the MSI-X ISR (same registration path as the I/O queues).
    // Drains the CQ, stashes the last completion for the blocked submitter and
    // wakes it. Admin commands never overlap (submit_and_return_on_completion
    // holds _lock across submit+wait), so a single _req_res slot is enough.
    int admin_queue_pair::process_completions(int max)
    {
        nvme_cq_entry_t *cqe = nullptr;
        int counter = 0;

        max = (max > 0) ? max : _qsize;

        while (counter < max && (cqe = get_completion_queue_entry()))
        {
            u16 cid = cqe->cid;
            if (cqe->sct != 0 || cqe->sc != 0)
            {
                trace_nvme_req_done_error(_driver_id, _id, cid, cqe->sct, cqe->sc, nullptr);
                nvme_e("Admin queue cid=%d, sct=%#x, sc=%#x\n", cid, cqe->sct, cqe->sc);
            }
            else
            {
                trace_nvme_req_done_success(_driver_id, _id, cid, nullptr);
            }

            _sq._head = cqe->sqhd; // update sq_head
            _req_res = *cqe;       // save the cqe so the requesting thread can return it
            advance_cq_head();
            counter++;
        }

        if (counter > 0)
        {
            mmio_setl(_cq._doorbell, _cq._head);

            // Wake the thread blocked in submit_and_return_on_completion.
            new_cq = true;
            _req_waiter.wake_from_kernel_or_with_irq_disabled();
        }

        return counter;
    }

    nvme_cq_entry_t
    admin_queue_pair::submit_and_return_on_completion(nvme_sq_entry_t *cmd, void *data, unsigned int datasize)
    {
        SCOPE_LOCK(_lock);

        _req_waiter.reset(*sched::thread::current());

        // for now admin cid = sq_tail
        u16 cid = _sq._tail;
        cmd->rw.common.cid = cid;

        if (data != nullptr && datasize > 0)
        {
            cmd->rw.common.prp1 = (u64)data;
            cmd->rw.common.prp2 = 0;
        }

        trace_nvme_admin_cmd_submit(_driver_id, _id, cid, cmd->set_features.common.opc);
        submit_cmd(cmd);

        sched::thread::wait_until([this]
                                  { return this->new_cq; });
        _req_waiter.clear();

        new_cq = false;

        return _req_res;
    }
};
