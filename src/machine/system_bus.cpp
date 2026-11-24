// SPDX-License-Identifier: MIT

// The system's buses: the DMAs' access to RDRAM, and the CPU's reads and writes of physical addresses. These reach
// RDRAM, the RCP's registers, the cartridge and the PIF.

#include <algorithm>
#include <string_view>

#include "xsflib/bytes.h"

#include "ultralib/machine/bits.h"
#include "ultralib/machine/system.h"

namespace ultralib::machine
{
namespace
{

// How long a write to cartridge space stays in the PI's latch, with the hardware's timing.
constexpr uint64_t kCartWriteLatchTicks = 200;

} // namespace

void System::DmaReadRdram(uint32_t address, uint8_t* out, uint32_t length)
{
    for (uint32_t i = 0; i < length; i++)
    {
        const uint32_t a = address + i;
        out[i] = a < rdram_.size() ? rdram_[a] : 0;
    }

    if (Coverage* coverage = ActiveCoverage())
    {
        coverage->ReadRdram(address, length);
    }
}

void System::DmaWriteRdram(uint32_t address, const uint8_t* data, uint32_t length)
{
    for (uint32_t i = 0; i < length; i++)
    {
        const uint32_t a = address + i;
        if (a < rdram_.size())
        {
            rdram_[a] = data[i];
        }
    }

    if (Coverage* coverage = ActiveCoverage())
    {
        coverage->WriteRdram(address, length);
    }
}

uint32_t System::ReadBus32(uint32_t paddr)
{
    paddr &= ~3u;
    if (paddr < kRdramRegisters)
    {
        if (paddr + 4 <= direct_rdram_size_)
        {
            if (Coverage* coverage = ActiveCoverage())
            {
                coverage->ReadRdram(paddr, 4);
            }
            return xsflib::LoadBe32(rdram_.data() + paddr);
        }
        return ReadMappedRdram32(paddr);
    }
    if (paddr < kCartDomain2Address1)
    {
        return ReadRcp32(paddr);
    }
    if (paddr >= kBoardAddresses)
    {
        board_accesses_++;
        if (options_.board != Board::kConsole)
        {
            return ReadBoard32(paddr);
        }
    }

    return ReadCart32(paddr);
}

uint32_t System::ReadBusPartial32(uint32_t paddr)
{
    if (paddr >= kCartDomain2Address1 && paddr < kPifRom)
    {
        return ReadCart32(paddr & ~1u);
    }

    return ReadBus32(paddr);
}

uint64_t System::ReadBus64(uint32_t paddr)
{
    const uint64_t high = ReadBus32(paddr & ~7u);
    const uint64_t low = ReadBus32((paddr & ~7u) + 4);
    return (high << 32) | low;
}

void System::WriteBus(uint32_t paddr, uint64_t value, int size)
{
    if (paddr < kRdramRegisters)
    {
        if ((mi_.mode & kMiModeRepeat) != 0)
        {
            WriteRdramRepeated(paddr, value, size);
            return;
        }
        if (paddr + size <= direct_rdram_size_)
        {
            StoreBe(rdram_.data() + paddr, value, size);
            if (Coverage* coverage = ActiveCoverage())
            {
                coverage->WriteRdram(paddr, static_cast<uint32_t>(size));
            }
            return;
        }
        WriteMappedRdram(paddr, value, size);
        return;
    }

    if (paddr >= kBoardAddresses)
    {
        board_accesses_++;
        if (options_.board != Board::kConsole)
        {
            WriteBoard(paddr, value, size);
            return;
        }
    }

    // The debug port is a buffer that takes stores of any size.
    if (!is_viewer_.empty() && paddr >= kIsViewer && paddr + size <= kIsViewer + kIsViewerSize && size < 4)
    {
        StoreBe(is_viewer_.data() + (paddr - kIsViewer), value, size);
        return;
    }

    // The RCP and the PI take 32-bit writes. A smaller store shifts the whole register to its lanes and writes the low
    // 32 bits of the result: the lanes below it get zeros, the lanes above it the register's next bytes. A doubleword
    // store writes its upper word.
    const uint32_t word = BusWord(paddr, value, size);
    paddr &= ~3u;
    if (paddr < kCartDomain2Address1)
    {
        WriteRcp32(paddr, word);
    }
    else
    {
        WriteCart32(paddr, word);
    }
}

uint32_t System::BusWord(uint32_t paddr, uint64_t value, int size)
{
    switch (size)
    {
    case 1:
        return static_cast<uint32_t>(value << (8 * (3 - (paddr & 3))));
    case 2:
        return static_cast<uint32_t>(value << (8 * (2 - (paddr & 2))));
    case 4:
        return static_cast<uint32_t>(value);
    default:
        return static_cast<uint32_t>(value >> 32);
    }
}

uint32_t System::ReadRcp32(uint32_t paddr)
{
    cpu_->NoteCountUpdated();

    if (paddr < kSpDmem)
    {
        return ReadRdramRegister(paddr);
    }
    if (paddr < kSpRegisters)
    {
        const auto& memory = rsp_->Memory(paddr);
        return xsflib::LoadBe32(memory.data() + (paddr & Rsp::kWordAddressMask));
    }
    if (paddr < kSpPcRegister)
    {
        return rsp_->ReadRegister((paddr >> 2) & 7);
    }
    if (paddr < kDpCommandRegisters)
    {
        return (paddr & 4) != 0 ? rsp_->ReadIbist() : rsp_->ReadPc();
    }
    if (paddr < kDpSpanRegisters)
    {
        return ReadDp((paddr >> 2) & 7);
    }
    if (paddr < kMiRegisters)
    {
        return 0;
    }
    if (paddr < kViRegisters)
    {
        return ReadMi((paddr >> 2) & 3);
    }
    if (paddr < kAiRegisters)
    {
        return ReadVi((paddr >> 2) & 15);
    }
    if (paddr < kPiRegisters)
    {
        return ReadAi((paddr >> 2) & 7);
    }
    if (paddr < kRiRegisters)
    {
        return ReadPi((paddr >> 2) & 15);
    }
    if (paddr < kSiRegisters)
    {
        return ReadRi((paddr >> 2) & 7);
    }
    if (paddr < kSiRegistersEnd)
    {
        return ReadSi((paddr >> 2) & 7);
    }

    return 0;
}

void System::WriteRcp32(uint32_t paddr, uint32_t value)
{
    cpu_->NoteCountUpdated();

    if (paddr < kSpDmem)
    {
        WriteRdramRegister(paddr, value);
        return;
    }
    if (paddr < kSpRegisters)
    {
        auto& memory = rsp_->Memory(paddr);
        xsflib::StoreBe32(memory.data() + (paddr & Rsp::kWordAddressMask), value);
        return;
    }
    if (paddr < kSpPcRegister)
    {
        rsp_->WriteRegister((paddr >> 2) & 7, value);
        return;
    }
    if (paddr < kDpCommandRegisters)
    {
        if ((paddr & 4) != 0)
        {
            rsp_->WriteIbist(value);
        }
        else
        {
            rsp_->WritePc(value);
        }
        return;
    }
    if (paddr < kDpSpanRegisters)
    {
        WriteDp((paddr >> 2) & 7, value);
        return;
    }
    if (paddr < kMiRegisters)
    {
        return;
    }
    if (paddr < kViRegisters)
    {
        WriteMi((paddr >> 2) & 3, value);
        return;
    }
    if (paddr < kAiRegisters)
    {
        WriteVi((paddr >> 2) & 15, value);
        return;
    }
    if (paddr < kPiRegisters)
    {
        WriteAi((paddr >> 2) & 7, value);
        return;
    }
    if (paddr < kRiRegisters)
    {
        WritePi((paddr >> 2) & 15, value);
        return;
    }
    if (paddr < kSiRegisters)
    {
        WriteRi((paddr >> 2) & 7, value);
        return;
    }
    if (paddr < kSiRegistersEnd)
    {
        WriteSi((paddr >> 2) & 7, value);
    }
}

uint32_t System::ReadRom32(uint32_t offset)
{
    if (offset >= rom_image_size_)
    {
        const uint32_t low = (kCartRom + offset) & 0xFFFF;
        return (low << 16) | low;
    }

    if (Coverage* coverage = ActiveCoverage())
    {
        coverage->ReadRom(offset, 4);
    }

    if (offset + 4 <= rom_.size())
    {
        return xsflib::LoadBe32(rom_.data() + offset);
    }

    uint32_t word = 0;
    for (uint32_t i = 0; i < 4; i++)
    {
        word <<= 8;
        if (offset + i < rom_.size())
        {
            word |= rom_[offset + i];
        }
    }

    return word;
}

uint32_t System::ReadCart32(uint32_t paddr)
{
    // Like a DMA, a direct access through the PI leaves PI_CART_ADDR just past the word it reached. IPL3's last read of
    // the header leaves 0x1000000C there.
    if (paddr < kPifRom)
    {
        pi_.cart_address = (paddr + 4) & kPiCartAddressMask;
    }

    // A write to cartridge space is latched, and the next read returns it.
    if (pi_.latch_valid)
    {
        pi_.latch_valid = false;
        if (options_.timing == Timing::kLegacy || now_ < pi_.latch_until)
        {
            return pi_.latch;
        }
    }

    if (paddr >= kPifRam && paddr < kPifRam + kPifRamSize)
    {
        return xsflib::LoadBe32(pif_.Ram().data() + (paddr - kPifRam));
    }
    if (paddr >= kPifRom && paddr < kPifRam)
    {
        // The PIF locks its ROM before the game runs.
        return 0;
    }
    if (!is_viewer_.empty() && paddr >= kIsViewer && paddr + 4 <= kIsViewer + kIsViewerSize)
    {
        return xsflib::LoadBe32(is_viewer_.data() + (paddr - kIsViewer));
    }
    if (paddr >= kCartRom && paddr < kPifRom)
    {
        return ReadRom32(paddr - kCartRom);
    }

    // Nothing answers: the bus holds the low 16 bits of the address.
    const uint32_t low = paddr & 0xFFFF;
    return (low << 16) | low;
}

void System::WriteCart32(uint32_t paddr, uint32_t value)
{
    if (paddr >= kPifRam && paddr < kPifRam + kPifRamSize)
    {
        xsflib::StoreBe32(pif_.Ram().data() + (paddr - kPifRam), value);
        pif_.OnWrite();
        return;
    }
    if (!is_viewer_.empty() && paddr >= kIsViewer && paddr + 4 <= kIsViewer + kIsViewerSize)
    {
        WriteIsViewer(paddr - kIsViewer, value);
        return;
    }
    if (paddr >= kPifRom && paddr < kPifRam)
    {
        return;
    }

    if (paddr < kPifRom)
    {
        pi_.cart_address = (paddr + 4) & kPiCartAddressMask;
    }

    // With the legacy timing, the latch keeps the last write until a read. On the hardware, the first write fills it
    // and marks the PI busy, later writes are lost, and the value goes after about 200 CPU cycles: n64-systemtest's
    // cart-writing tests see it after 10 turns of a three-instruction loop, and not after 110.
    if (options_.timing == Timing::kLegacy)
    {
        pi_.latch = value;
        pi_.latch_valid = true;
        return;
    }
    if (pi_.latch_valid && now_ < pi_.latch_until)
    {
        return;
    }

    pi_.latch = value;
    pi_.latch_valid = true;
    pi_.latch_until = now_ + kCartWriteLatchTicks;
}

void System::WriteIsViewer(uint32_t offset, uint32_t value)
{
    xsflib::StoreBe32(is_viewer_.data() + offset, value);
    if (offset != kIsViewerLength - kIsViewer)
    {
        return;
    }

    const uint32_t start = kIsViewerBuffer - kIsViewer;
    const uint32_t length = std::min<uint32_t>(value, kIsViewerSize - start);
    if (debug_output_ && length > 0)
    {
        debug_output_(std::string_view(reinterpret_cast<const char*>(is_viewer_.data() + start), length));
    }
}

} // namespace ultralib::machine
