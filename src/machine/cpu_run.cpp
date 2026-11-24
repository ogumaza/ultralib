// SPDX-License-Identifier: MIT

// The VR4300's state and its main loop: the reset, the accessors, Run, the legacy timing's RunLate, Step and the fetch
// of an instruction.

#include "xsflib/bytes.h"

#include "ultralib/machine/bits.h"
#include "ultralib/machine/constants.h"
#include "ultralib/machine/cpu.h"
#include "ultralib/machine/cpu_cop0.h"
#include "ultralib/machine/system.h"

namespace ultralib::machine
{

Cpu::Cpu(System& system) : system_(system), now_(system.Clock())
{
    Reset();
}

void Cpu::Reset()
{
    gpr_ = {};
    hi_ = 0;
    lo_ = 0;
    current_pc_ = kResetVector;
    pc_ = current_pc_;
    next_pc_ = pc_ + 4;
    branch_pending_ = false;
    in_delay_slot_ = false;
    idle_loop_ = false;
    late_events_ = false;
    event_check_ = false;
    status_check_ = false;

    index_ = 0;
    entry_lo0_ = 0;
    entry_lo1_ = 0;
    context_ = kContextBadVpn2Mask;
    page_mask_ = 0;
    wired_ = 0;

    // A console that has just been turned on reads all ones from BadVAddr, EPC and ErrorEPC, Context and XContext that
    // match that BadVAddr, and 3 from the coprocessor field of Cause. A reset doesn't change them. n64-systemtest's
    // startup test and PeterLemon's COP0 and exception tests record these values.
    bad_vaddr_ = ~uint64_t{0};
    count_offset_ = 0 - now_;
    entry_hi_ = 0;
    compare_ = 0;
    status_ = kStatusCu0 | kStatusCu1 | kStatusFr;
    cause_ = kCauseCoprocessorMask;
    epc_ = ~uint64_t{0};
    config_ = kConfigReset;
    ll_addr_ = 0;
    watch_lo_ = 0;
    watch_hi_ = 0;
    x_context_ = kXContextBadVpn2Mask;
    parity_error_ = 0;
    tag_lo_ = 0;
    error_epc_ = ~uint64_t{0};
    latch_ = 0;
    cop2_latch_ = 0;
    random_epoch_ = 0;
    ll_bit_ = false;
    tlb_ = {};
    tlb_pages_ = system_.GetOptions().timing == Timing::kLegacy;
    refused_board_pages_ = 0;
    read_pages_.Clear();
    write_pages_.Clear();
    ip2_stale_ = false;
    count_updated_ = 0;

    fpr_ = {};
    fcr31_ = 0;
    legacy_rounding_ = 0;
    instructions_ = 0;
    interrupt_pending_ = false;
    fetch_page_ = ~uint64_t{0};
    translation_epoch_++;
    fast_pages_.clear();

    // The TLB's contents at power-on are undefined, but they don't map the low addresses: PeterLemon's TLB tests see a
    // refill exception for address 0x10. Like libultra when it clears the TLB, the core gives each entry a page of
    // kseg0. The CPU never translates kseg0.
    for (int i = 0; i < 32; i++)
    {
        TlbEntry entry;
        entry.entry_hi = SignExtend32(kKseg0) + static_cast<uint64_t>(i) * 0x2000;
        SetTlb(i, entry);
    }
}

uint64_t Cpu::Gpr(int index) const
{
    return gpr_[index & 31];
}

void Cpu::SetGpr(int index, uint64_t value)
{
    if ((index & 31) != 0)
    {
        gpr_[index & 31] = value;
    }
}

uint64_t Cpu::Pc() const
{
    return pc_;
}

void Cpu::SetPc(uint64_t pc)
{
    pc_ = pc;
    next_pc_ = pc + 4;
    branch_pending_ = false;
}

uint64_t Cpu::InstructionCount() const
{
    return instructions_;
}

bool Cpu::InDelaySlot() const
{
    return branch_pending_;
}

void Cpu::Run(uint64_t until)
{
    late_events_ = !system_.GetOptions().exact_events && system_.GetOptions().timing == Timing::kLegacy;
    if (late_events_)
    {
        RunLate(until);
        return;
    }

    const uint64_t ticks = system_.TicksPerInstruction();
    const bool& slice_break = system_.SliceBreakFlag();
    const bool breakpoints = system_.HasBreakpoints();
    while (now_ < until && !slice_break)
    {
        if (breakpoints && system_.ShouldBreak(pc_))
        {
            break;
        }

        Step();
        now_ += ticks;
        if (idle_loop_)
        {
            // Nothing can change until the next event. Skip to it. Random counts the skipped instructions.
            idle_loop_ = false;
            if (!interrupt_pending_ && now_ < until)
            {
                const uint64_t skipped = (until - now_ + ticks - 1) / ticks;
                instructions_ += skipped;
                now_ += skipped * ticks;
            }
        }
    }
}

void Cpu::RunLate(uint64_t until)
{
    // The legacy timing. Count includes each instruction while it runs. A device that the instruction reaches sees that
    // Count. An interrupt is taken only at a check point.
    const uint64_t ticks = system_.TicksPerInstruction();
    const bool& slice_break = system_.SliceBreakFlag();
    const bool breakpoints = system_.HasBreakpoints();
    const bool fast = !breakpoints && system_.ActiveCoverage() == nullptr;
    while (now_ < until && !slice_break)
    {
        if (fast && RunFast(until))
        {
            continue;
        }
        if (breakpoints && system_.ShouldBreak(pc_))
        {
            break;
        }

        const uint64_t before = now_;
        const bool delay_slot = branch_pending_;
        event_check_ = false;
        status_check_ = false;
        now_ += ticks;
        Step();
        if (idle_loop_)
        {
            // Count jumps to the next event. The branch then adds its 2 count units.
            idle_loop_ = false;
            const uint64_t next = system_.NextEvent();
            if (next != ~uint64_t{0} && next > before)
            {
                const uint64_t target = next + ticks;
                instructions_ += (target - now_) / ticks;
                now_ = target;
            }
        }

        // Each check point handles one due event at most, and brings Count up to date.
        if (status_check_)
        {
            system_.LateCheck(now_ - ticks);
            count_updated_ = now_;
        }
        if (delay_slot || event_check_)
        {
            system_.LateCheck(now_);
            count_updated_ = now_;
        }
    }
}

void Cpu::RequestEventCheck()
{
    event_check_ = true;
}

bool Cpu::InterruptPending() const
{
    return interrupt_pending_;
}

void Cpu::EnterInterrupt()
{
    // Taking an interrupt adds one instruction's time to Count. With this charge, the output follows the legacy
    // players' output for longer than without it.
    now_ += system_.TicksPerInstruction();
    TakeInterrupt();
}

void Cpu::Step()
{
    // Without exact events, interrupts are taken only at check points.
    if (interrupt_pending_ && !late_events_)
    {
        TakeInterrupt();
        return;
    }

    current_pc_ = pc_;
    in_delay_slot_ = branch_pending_;
    branch_pending_ = false;
    instructions_++;

    uint32_t instruction = 0;
    if (!Fetch(instruction))
    {
        return;
    }

    pc_ = next_pc_;
    next_pc_ += 4;
    Execute(instruction);
    gpr_[0] = 0;
}

bool Cpu::Fetch(uint32_t& instruction)
{
    const uint64_t pc = current_pc_;
    if ((pc & 3) != 0)
    {
        RaiseAddressError(pc, Access::kFetch);
        return false;
    }

    // The cached page, or a translation. An instruction outside plain RDRAM comes from the bus. It starts no cached
    // page.
    uint32_t paddr = 0;
    if ((pc & ~uint64_t{kPageOffsetMask}) == fetch_page_ && fetch_epoch_ == translation_epoch_)
    {
        paddr = fetch_page_paddr_ | static_cast<uint32_t>(pc & kPageOffsetMask);
    }
    else
    {
        if (!Translate(pc, Access::kFetch, paddr))
        {
            return false;
        }
        if (paddr >= system_.DirectRdramSize())
        {
            instruction = system_.ReadBus32(paddr);
            return true;
        }

        fetch_page_ = pc & ~uint64_t{kPageOffsetMask};
        fetch_page_paddr_ = paddr & ~kPageOffsetMask;
        fetch_epoch_ = translation_epoch_;
    }

    instruction = xsflib::LoadBe32(system_.RdramData() + paddr);
    if (Coverage* coverage = system_.ActiveCoverage())
    {
        coverage->ReadRdram(paddr, 4);
    }

    return true;
}

} // namespace ultralib::machine
