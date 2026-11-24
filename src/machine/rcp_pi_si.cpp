// SPDX-License-Identifier: MIT

// The RCP's interfaces to the cartridge and the PIF: the PI and the SI, with their DMAs.

#include <algorithm>
#include <array>

#include "ultralib/machine/system.h"

namespace ultralib::machine
{
namespace
{

// An SI transfer and the PIF's work on it take about 64 microseconds.
constexpr uint64_t kSiTransferTicks = 6000;

// The legacy players complete a PI transfer that touches no ROM 0x1000 count units after it starts.
constexpr uint64_t kLegacyEmptyPiTicks = 0x2000;

// PI_STATUS bits.
constexpr uint32_t kPiStatusDmaBusy = 1u << 0;
constexpr uint32_t kPiStatusIoBusy = 1u << 1;
constexpr uint32_t kPiStatusError = 1u << 2;
constexpr uint32_t kPiStatusInterrupt = 1u << 3;

// SI_STATUS bits.
constexpr uint32_t kSiStatusDmaBusy = 1u << 0;
constexpr uint32_t kSiStatusInterrupt = 1u << 12;

} // namespace

bool System::RcpDelays() const
{
    return options_.timing != Timing::kLegacy || options_.rcp_delays;
}

// PI.

uint32_t System::PiStatus() const
{
    uint32_t status = 0;
    if (pi_.busy)
    {
        status |= kPiStatusDmaBusy;
    }
    if (pi_.error)
    {
        status |= kPiStatusError;
    }
    if ((mi_.interrupt & kMiInterruptPi) != 0)
    {
        status |= kPiStatusInterrupt;
    }

    return status;
}

uint64_t System::PiTransferTicks(uint32_t cart_address, uint32_t length) const
{
    if (options_.timing == Timing::kLegacy)
    {
        // length ÷ 8 count units.
        return std::max<uint64_t>(2 * (length / 8), 1);
    }

    // Each page of the domain costs its latency, and each 16-bit word its pulse width and release time, in RCP cycles.
    // An RCP cycle is one and a half ticks.
    const bool domain2 = (cart_address >= kCartDomain2Address1 && cart_address < kCartDomain1Address1) ||
                         (cart_address >= kCartDomain2Address2 && cart_address < kCartRom);
    const uint32_t* domain = pi_.domains.data() + (domain2 ? 4 : 0);
    const uint64_t latency = domain[0] + 1;
    const uint64_t pulse = domain[1] + 1;
    const uint64_t page = uint64_t{1} << ((domain[2] & 0xF) + 2);
    const uint64_t release = domain[3] + 1;
    const uint64_t pages = (uint64_t{length} + page - 1) / page;
    const uint64_t words = (uint64_t{length} + 1) / 2;
    const uint64_t cycles = pages * latency + words * (pulse + release);
    return std::max<uint64_t>(cycles * 3 / 2, 1);
}

uint32_t System::ReadPi(uint32_t index)
{
    switch (index)
    {
    case 0:
        return pi_.dram_address;

    case 1:
        return pi_.cart_address;

    case 2:
        return pi_.read_length;

    case 3:
        return pi_.write_length;

    case 4:
        {
            uint32_t status = PiStatus();
            if (pi_.latch_valid && options_.timing == Timing::kHardware && now_ < pi_.latch_until)
            {
                status |= kPiStatusIoBusy; // IO busy: a write to cartridge space is in the latch.
            }
            return status;
        }

    default:
        if (index >= 5 && index < 13)
        {
            return pi_.domains[index - 5];
        }
        return 0;
    }
}

void System::WritePi(uint32_t index, uint32_t value)
{
    switch (index)
    {
    case 0:
        pi_.dram_address = value & kPiDramAddressMask;
        return;

    case 1:
        pi_.cart_address = value & kPiCartAddressMask;
        return;

    case 2:
    case 3:
        break;

    case 4:
        if ((value & 1) != 0)
        {
            pi_.busy = false;
            pi_.error = false;
            Cancel(Event::kPi);
        }
        if ((value & 2) != 0)
        {
            LowerMi(kMiInterruptPi);
        }
        return;

    default:
        if (index >= 5 && index < 13)
        {
            static constexpr std::array<uint32_t, 4> kMasks = {0xFF, 0xFF, 0x0F, 0x03};
            pi_.domains[index - 5] = value & kMasks[(index - 5) & 3];
        }
        return;
    }

    // A transfer: PI_RD_LEN copies from RDRAM to the cartridge, PI_WR_LEN from the cartridge to RDRAM. The length
    // register holds the length minus one.
    const bool to_rdram = index == 3;
    const uint32_t requested = (value & kPiLengthMask) + 1;
    const uint32_t cart = pi_.cart_address;
    const uint32_t dram = pi_.dram_address;
    pi_.busy = true;

    // Only ROM answers. A copy to the cartridge, or from save memory, the 64DD or the PIF, copies nothing.
    const bool touches_rom = to_rdram && cart >= kCartRom && cart < kPifRom && cart - kCartRom < rom_image_size_;
    uint32_t cart_end = 0;
    uint32_t dram_end = 0;
    if (to_rdram)
    {
        CopyPiToRdram(cart, dram, requested, cart_end, dram_end);
    }
    else
    {
        cart_end = cart + ((requested + 1) & ~1u);
        dram_end = (dram + requested + 7) & ~7u;
    }

    pi_.dram_address = dram_end & kPiDramAddressMask;
    pi_.cart_address = cart_end & kPiCartAddressMask;

    const uint32_t length = cart_end - cart;
    uint64_t ticks = PiTransferTicks(cart, length);
    if (options_.timing == Timing::kLegacy && !touches_rom)
    {
        ticks = kLegacyEmptyPiTicks;
    }

    if (!RcpDelays())
    {
        // The interrupt comes at once.
        FinishPiDma();
        return;
    }

    Schedule(Event::kPi, now_ + ticks);
}

// The PI copies in blocks of up to 128 bytes through a buffer, and no block crosses a 2 KiB page of RDRAM. Each 16-bit
// read from the bus fills two bytes. A block of odd length therefore copies one byte more.
//
// The first block is different. Its writes are masked to the byte. An odd length copies exactly, unless the block
// reaches the last two bytes of the buffer. An RDRAM address that isn't a multiple of 8 puts the data at that offset in
// the buffer and shortens the block. The hardware counts the block's length from the start of the buffer. It therefore
// writes `offset` bytes fewer at the end. A first block that fills the buffer still moves the RDRAM address to the
// buffer's end and leaves a hole. When the first block ends at a page within the first 8 bytes, the next block is
// shortened by the offset too, and it leaves a hole of that size after it.
//
// n64brew describes the blocks. n64-systemtest's DMA tests give the exact results, and this follows them all.
void System::CopyPiToRdram(uint32_t cart, uint32_t dram, uint32_t length, uint32_t& cart_end, uint32_t& dram_end)
{
    constexpr uint32_t kBuffer = 128;
    uint32_t offset = dram & 7;
    uint32_t remaining = length;
    bool first = true;
    while (remaining > 0)
    {
        // The size of the block: the rest of the transfer, up to the end of the buffer or of the RDRAM page.
        const uint32_t page_left = kRdramPageSize - (dram & (kRdramPageSize - 1));
        uint32_t size = std::min(remaining, std::min(kBuffer - offset, page_left));

        // How many bytes the block writes, and how far it moves the RDRAM address.
        uint32_t written = 0;
        uint32_t advance = 0;
        if (first)
        {
            if ((size & 1) != 0 && offset + size == kBuffer - 1)
            {
                size++;
            }
            written = size > offset ? size - offset : 0;
            if (offset + size == kBuffer)
            {
                advance = size;
                offset = 0;
            }
            else if (size == page_left)
            {
                advance = size;
                if (offset + size > 8)
                {
                    offset = 0;
                }
            }
            else
            {
                advance = written;
            }
        }
        else
        {
            size = (size + 1) & ~1u;
            written = size;
            if (offset + size >= kBuffer)
            {
                advance = kBuffer;
                offset = 0;
            }
            else
            {
                advance = size;
            }
        }

        // The copy.
        for (uint32_t i = 0; i < written; i++)
        {
            CopyRomByte(cart + i, dram + i);
        }

        // The coverage: the ROM bytes that the block reads and the RDRAM bytes that it writes.
        if (Coverage* coverage = ActiveCoverage())
        {
            if (cart >= kCartRom && cart < kPifRom && cart - kCartRom < rom_image_size_)
            {
                coverage->ReadRom(cart - kCartRom, std::min(size, rom_image_size_ - (cart - kCartRom)));
            }
            if (written > 0)
            {
                coverage->WriteRdram(dram & kRdramAddressMask, written);
            }
        }

        // The next block.
        cart += (size + 1) & ~1u;
        dram += advance;
        remaining = remaining > size ? remaining - size : 0;
        first = false;
    }

    cart_end = cart;
    dram_end = (dram + 7) & ~7u;
}

void System::CopyRomByte(uint32_t cart, uint32_t dram)
{
    const uint32_t target = dram & kRdramAddressMask;
    if (cart < kCartRom || cart >= kPifRom || target >= rdram_.size())
    {
        return;
    }

    const uint32_t source = cart - kCartRom;
    if (source < rom_image_size_)
    {
        rdram_[target] = source < rom_.size() ? rom_[source] : 0;
    }
}

void System::FinishPiDma()
{
    pi_.busy = false;
    RaiseMi(kMiInterruptPi);
}

// SI.

uint32_t System::SiStatus() const
{
    uint32_t status = 0;
    if (si_.busy)
    {
        status |= kSiStatusDmaBusy;
    }
    if ((mi_.interrupt & kMiInterruptSi) != 0)
    {
        status |= kSiStatusInterrupt;
    }

    return status;
}

uint32_t System::ReadSi(uint32_t index)
{
    switch (index)
    {
    case 0:
        return si_.dram_address;

    case 1:
        return si_.pif_address_read;

    case 4:
        return si_.pif_address_write;

    case 6:
        return SiStatus();

    default:
        return 0;
    }
}

void System::WriteSi(uint32_t index, uint32_t value)
{
    switch (index)
    {
    case 0:
        si_.dram_address = value & kDmaRdramAddressMask;
        break;

    case 1:
        si_.pif_address_read = value;
        StartSiDma(true);
        break;

    case 4:
        si_.pif_address_write = value;
        StartSiDma(false);
        break;

    case 6:
        LowerMi(kMiInterruptSi);
        break;

    default:
        break;
    }
}

void System::StartSiDma(bool to_rdram)
{
    // The 64 bytes of PIF RAM move at once. The PIF runs its commands before a read and after a write.
    auto& ram = pif_.Ram();
    const uint32_t dram = si_.dram_address;
    if (to_rdram)
    {
        pif_.OnRead(now_, vi_.fields);
        DmaWriteRdram(dram, ram.data(), kPifRamSize);
    }
    else
    {
        DmaReadRdram(dram, ram.data(), kPifRamSize);
        pif_.OnWrite();
    }

    si_.busy = true;
    if (!RcpDelays())
    {
        // The transfer takes effect at once.
        FinishSiDma();
        return;
    }

    Schedule(Event::kSi, now_ + kSiTransferTicks);
}

void System::FinishSiDma()
{
    si_.busy = false;
    RaiseMi(kMiInterruptSi);
}

} // namespace ultralib::machine
