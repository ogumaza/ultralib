// SPDX-License-Identifier: MIT

// The RSP's state and the registers outside its scalar unit: the reset, the SP registers that the CPU and the RSP's
// COP0 reach, the semaphore and the DMA between RDRAM and DMEM or IMEM.

#include <array>

#include "ultralib/machine/rsp.h"
#include "ultralib/machine/system.h"

namespace ultralib::machine
{
namespace
{

// SP_STATUS's write bits that have no partner: clear broke, and the clear and set bits of the SP interrupt.
constexpr uint32_t kSpWriteClearBroke = 1u << 2;
constexpr uint32_t kSpWriteClearInterrupt = 1u << 3;
constexpr uint32_t kSpWriteSetInterrupt = 1u << 4;

} // namespace

Rsp::Rsp(System& system) : system_(system)
{
}

void Rsp::Reset()
{
    dmem_ = {};
    imem_ = {};
    gpr_ = {};
    pc_ = 0;
    next_pc_ = 4;
    status_ = kSpStatusHalt;
    semaphore_ = 0;
    ibist_ = 0;
    current_ = {};
    current_.length = kDmaLengthMask;
    pending_ = {};
    busy_ = false;
    full_ = false;
    vu_.Reset();
}

Rsp::SpMemory& Rsp::Dmem()
{
    return dmem_;
}

Rsp::SpMemory& Rsp::Imem()
{
    return imem_;
}

const Rsp::SpMemory& Rsp::Dmem() const
{
    return dmem_;
}

const Rsp::SpMemory& Rsp::Imem() const
{
    return imem_;
}

uint32_t Rsp::Gpr(int index) const
{
    return gpr_[index];
}

uint32_t Rsp::Status() const
{
    uint32_t status = status_ & ~(kSpStatusDmaBusy | kSpStatusDmaFull);
    if (busy_)
    {
        status |= kSpStatusDmaBusy;
    }
    if (full_)
    {
        status |= kSpStatusDmaFull;
    }

    return status;
}

void Rsp::SetStatusBits(uint32_t bits)
{
    status_ |= bits;
}

bool Rsp::DmaBusy() const
{
    return busy_;
}

bool Rsp::DmaFull() const
{
    return full_;
}

void Rsp::ExecuteCop0(uint32_t instruction)
{
    const uint32_t rt = (instruction >> 16) & 31;
    const uint32_t rd = (instruction >> 11) & 15;
    switch ((instruction >> 21) & 31)
    {
    case 0x00:
        gpr_[rt] = ReadCop0(rd);
        break;
    case 0x04:
        WriteCop0(rd, gpr_[rt]);
        break;
    default:
        break;
    }
}

uint32_t Rsp::ReadCop0(uint32_t index)
{
    if (index >= 8)
    {
        return system_.ReadDp(index - 8);
    }

    return ReadRegister(index);
}

void Rsp::WriteCop0(uint32_t index, uint32_t value)
{
    if (index >= 8)
    {
        system_.WriteDp(index - 8, value);
        return;
    }

    WriteRegister(index, value);
}

uint32_t Rsp::ReadRegister(uint32_t index)
{
    switch (index & 7)
    {
    case 0:
        return current_.mem_address & kDmaSpAddressMask;

    case 1:
        return current_.dram_address & kDmaRdramAddressMask;

    case 2:
    case 3:
        return current_.LengthRegister();

    case 4:
        return Status();

    case 5:
        return full_ ? 1 : 0;

    case 6:
        return busy_ ? 1 : 0;

    default:
        {
            // Reading the semaphore takes it.
            const uint32_t value = semaphore_;
            semaphore_ = 1;
            return value;
        }
    }
}

void Rsp::WriteRegister(uint32_t index, uint32_t value)
{
    switch (index & 7)
    {
    case 0:
        pending_.mem_address = value & kDmaSpAddressMask;
        break;
    case 1:
        pending_.dram_address = value & kDmaRdramAddressMask;
        break;
    case 2:
        QueueDma(value, false);
        break;
    case 3:
        QueueDma(value, true);
        break;
    case 4:
        WriteStatus(value);
        break;
    case 7:
        semaphore_ = 0;
        break;
    default:
        break;
    }
}

void Rsp::WriteStatus(uint32_t value)
{
    const uint32_t old_status = status_;

    // Halt, single step, interrupt on break and each signal have a clear bit and the set bit above it.
    const auto apply = [this, value](int clear_bit, int set_bit, uint32_t flag)
    {
        const bool clear = ((value >> clear_bit) & 1) != 0;
        const bool set = ((value >> set_bit) & 1) != 0;
        if (clear && !set)
        {
            status_ &= ~flag;
        }
        else if (set && !clear)
        {
            status_ |= flag;
        }
    };

    apply(0, 1, kSpStatusHalt);
    if ((value & kSpWriteClearBroke) != 0)
    {
        status_ &= ~kSpStatusBroke;
    }

    const bool clear_interrupt = (value & kSpWriteClearInterrupt) != 0;
    const bool set_interrupt = (value & kSpWriteSetInterrupt) != 0;
    if (clear_interrupt && !set_interrupt)
    {
        system_.LowerMi(kMiInterruptSp);
    }
    else if (set_interrupt && !clear_interrupt)
    {
        system_.RaiseMi(kMiInterruptSp);
    }

    apply(5, 6, kSpStatusSingleStep);
    apply(7, 8, kSpStatusInterruptOnBreak);
    for (int signal = 0; signal < 8; signal++)
    {
        apply(9 + 2 * signal, 10 + 2 * signal, kSpStatusSignal0 << signal);
    }

    system_.OnSpStatusWritten(old_status);
}

uint32_t Rsp::ReadPc() const
{
    return pc_ & kWordAddressMask;
}

void Rsp::WritePc(uint32_t value)
{
    pc_ = value & kWordAddressMask;
    next_pc_ = (pc_ + 4) & kWordAddressMask;
}

uint32_t Rsp::ReadIbist() const
{
    return ibist_;
}

void Rsp::WriteIbist(uint32_t value)
{
    ibist_ = value & 7;
}

uint32_t Rsp::Dma::LengthRegister() const
{
    return (length & kDmaLengthMask) | (count << kDmaCountShift) | (skip << kDmaSkipShift);
}

void Rsp::Dma::SetLengthRegister(uint32_t value)
{
    length = value & kDmaLengthMask;
    count = (value >> kDmaCountShift) & kDmaCountMask;
    skip = (value >> kDmaSkipShift) & kDmaLengthMask;
}

void Rsp::QueueDma(uint32_t length_register, bool to_rdram)
{
    pending_.SetLengthRegister(length_register);
    pending_.to_rdram = to_rdram;
    full_ = true;

    if (!busy_)
    {
        StartDma();
    }
}

uint64_t Rsp::DmaTicks(const Dma& dma) const
{
    // Three RCP cycles for each 8 bytes of each row. An RCP cycle is one and a half ticks.
    const uint64_t rcp_cycles = uint64_t{dma.count + 1} * ((dma.length + 8) / 8) * 3;
    return rcp_cycles * 3 / 2;
}

void Rsp::StartDma()
{
    current_ = pending_;
    busy_ = true;
    full_ = false;

    if (system_.GetOptions().timing == Timing::kLegacy)
    {
        FinishDma();
        return;
    }

    system_.Schedule(System::Event::kSpDma, system_.Now() + DmaTicks(current_));
}

void Rsp::FinishDma()
{
    if (!busy_)
    {
        return;
    }

    // Each row copies (length + 8) bytes in 8-byte units. SP memory addresses wrap within DMEM or IMEM, and the RDRAM
    // address skips `skip` bytes between rows. PeterLemon's SP DMA tests show no skip after the last row.
    SpMemory& memory = Memory(current_.mem_address);
    uint32_t mem = current_.mem_address & kDmaLengthMask;
    uint32_t dram = current_.dram_address & kDmaRdramAddressMask;
    for (uint32_t row = 0; row <= current_.count; row++)
    {
        for (uint32_t i = 0; i <= current_.length; i += 8)
        {
            std::array<uint8_t, 8> bytes;
            if (current_.to_rdram)
            {
                for (uint32_t b = 0; b < 8; b++)
                {
                    bytes[b] = memory[(mem + b) & kAddressMask];
                }
                system_.DmaWriteRdram(dram, bytes.data(), 8);
            }
            else
            {
                system_.DmaReadRdram(dram, bytes.data(), 8);
                for (uint32_t b = 0; b < 8; b++)
                {
                    memory[(mem + b) & kAddressMask] = bytes[b];
                }
            }

            mem = (mem + 8) & kDmaLengthMask;
            dram = (dram + 8) & kDmaRdramAddressMask;
        }

        if (row < current_.count)
        {
            dram = (dram + current_.skip) & kDmaRdramAddressMask;
        }
    }

    // The registers read back where the transfer ended, with a length of 0xFF8 and a count of 0.
    current_.mem_address = (current_.mem_address & kImemBit) | mem;
    current_.dram_address = dram;
    current_.length = kDmaLengthMask;
    current_.count = 0;
    busy_ = false;

    if (full_)
    {
        StartDma();
    }
}

SpRegisters Rsp::Registers() const
{
    SpRegisters registers;
    registers.mem_address = current_.mem_address & kDmaSpAddressMask;
    registers.dram_address = current_.dram_address & kDmaRdramAddressMask;
    registers.read_length = current_.LengthRegister();
    registers.write_length = registers.read_length;
    registers.status = Status();
    registers.dma_full = full_ ? 1 : 0;
    registers.dma_busy = busy_ ? 1 : 0;
    registers.semaphore = semaphore_;
    registers.pc = pc_ & kWordAddressMask;
    registers.ibist = ibist_;

    return registers;
}

void Rsp::SetRegisters(const SpRegisters& registers)
{
    current_ = {};
    current_.mem_address = registers.mem_address & kDmaSpAddressMask;
    current_.dram_address = registers.dram_address & kDmaRdramAddressMask;
    current_.SetLengthRegister(registers.read_length);
    pending_ = current_;
    status_ = registers.status & ~(kSpStatusDmaBusy | kSpStatusDmaFull | kSpStatusIoFull);
    busy_ = false;
    full_ = false;
    semaphore_ = registers.semaphore & 1;
    WritePc(registers.pc);
    ibist_ = registers.ibist & 7;
}

} // namespace ultralib::machine
