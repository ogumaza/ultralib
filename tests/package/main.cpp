// SPDX-License-Identifier: MIT

// Uses ultralib from another project: counts the words in a set of the emulator core, parses a VADPCM frame and,
// where xsflib builds its ROM sets, finds an Aleck64 set.

#include <array>
#include <cstdint>

#include "xsflib/testing/check.h"

#include "ultralib/machine/word_set.h"
#include "ultralib/n64/vadpcm.h"

#if ULTRALIB_PACKAGE_ROM_SET
#include "ultralib/machine/rom_set.h"
#endif

int main()
{
    ultralib::machine::WordSet words(64);
    words.AddWord(1);
    words.AddWord(5);
    XSFLIB_CHECK(words.Count() == 2);
    XSFLIB_CHECK(words.Has(5) && !words.Has(4));

    // A VADPCM frame: the scale in the high nibble of the first byte, the predictor in the low one, then 16 signed
    // 4-bit residuals, high nibble first.
    const std::array<uint8_t, ultralib::n64::kVadpcmFrameSize> kFrame = {0x35, 0x1f, 0x80, 0, 0, 0, 0, 0, 0};
    const ultralib::n64::VadpcmFrame frame = ultralib::n64::VadpcmFrame::Parse(kFrame);
    XSFLIB_CHECK(frame.scale == 3);
    XSFLIB_CHECK(frame.predictor == 5);
    XSFLIB_CHECK(frame.residuals[0] == 1 && frame.residuals[1] == -1 && frame.residuals[2] == -8);

#if ULTRALIB_PACKAGE_ROM_SET
    const ultralib::machine::Aleck64Set* set =
        ultralib::machine::FindAleck64Set("5432056f34c378b5862009e5b79a256c10c5a20e");
    XSFLIB_CHECK(set != nullptr && set->name == "Eleven Beat");
#endif

    return xsflib::testing::Summary("package");
}
