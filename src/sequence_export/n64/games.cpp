// SPDX-License-Identifier: MIT

#include "ultralib/sequence_export/n64/games.h"

#include <array>
#include <cstdint>
#include <string>

#include "xsflib/text.h"

namespace ultralib::sequence_export::n64
{
namespace
{

// The table of the game's code that gives each song a volume, as a run of the game's code holds it at 0x80024358. The
// image keeps it compressed.
constexpr std::array<uint16_t, 64> kGoldenEyeVolumes = {
    0x6665, 0x7332, 0x7332, 0x7998, 0x7332, 0x5998, 0x6665, 0x6665, 0x6665, 0x6665, 0x7332, 0x6665, 0x7332,
    0x6665, 0x6665, 0x7332, 0x7332, 0x6665, 0x3FFF, 0x6665, 0x6665, 0x3FFF, 0x6665, 0x6665, 0x6665, 0x6665,
    0x6665, 0x7998, 0x6665, 0x5998, 0x6665, 0x6665, 0x6665, 0x6665, 0x6665, 0x6665, 0x6665, 0x5998, 0x6665,
    0x3332, 0x6665, 0x7332, 0x7332, 0x5998, 0x7332, 0x6665, 0x6665, 0x6665, 0x6665, 0x6665, 0x6665, 0x6665,
    0x6CCB, 0x6665, 0x6665, 0x6665, 0x6665, 0x6665, 0x6665, 0x6665, 0x7332, 0x6665, 0x7332, 0x7998,
};

// The sequences that hold only the games' sound effects.
constexpr std::array<std::size_t, 1> kFirstSequenceIsEffects = {0};

// The level scripts ask for the title screen's and the water levels' sequences with bit 7 set in two places: 0x269FF8
// and 0x46C2CC in the image. The game's call passes that bit to the sequences' variation commands. The table of each
// level's music dynamics fades sets of channels to nothing, by where Mario is. Near the merry-go-round, the call at
// 0x802C5C10 plays sequence 0x13 through the game's function at 0x80321E48, on the second player at a volume of 78.
// The function at 0x80320F68 sets the table's volume for the first player's sequence. It returns at 0x80320F94 for
// sequence 0x1A before it reads the table. The credits therefore keep the 85 that their script sets.
constexpr std::array kSuperMario64Tracks = {
    EadTrack{.sequence = 0x02, .name = "title screen"},
    EadTrack{.sequence = 0x02, .name = "title screen, with the variation", .variation = -128},
    EadTrack{.sequence = 0x05, .name = "water levels, all channels"},
    EadTrack{.sequence = 0x05, .name = "water levels, without channels 6, 9, 10 and 11", .silent_channels = 0x0E40},
    EadTrack{
        .sequence = 0x05, .name = "water levels, without channels 0, 1, 6, 9, 10 and 11", .silent_channels = 0x0E43},
    EadTrack{.sequence = 0x05, .name = "water levels, with the variation", .variation = -128},
    EadTrack{.sequence = 0x05,
             .name = "water levels, with the variation, without channels 6, 9, 10 and 11",
             .variation = -128,
             .silent_channels = 0x0E40},
    EadTrack{.sequence = 0x05,
             .name = "water levels, with the variation, without channels 0, 1, 6, 9, 10 and 11",
             .variation = -128,
             .silent_channels = 0x0E43},
    EadTrack{.sequence = 0x0A, .name = "Big Boo's Haunt, indoors"},
    EadTrack{.sequence = 0x0A, .name = "Big Boo's Haunt, outdoors", .silent_channels = 0x0070},
    EadTrack{.sequence = 0x0C, .name = "underground levels, all channels"},
    EadTrack{.sequence = 0x0C, .name = "underground levels, without channel 8", .silent_channels = 0x0100},
    EadTrack{.sequence = 0x0C, .name = "underground levels, without channel 3", .silent_channels = 0x0008},
    EadTrack{.sequence = 0x13, .song_volume = 78},
    EadTrack{.sequence = 0x1A, .song_volume = 85},
};

// Super Mario 64 (USA). The game's code plays a sequence at the volume that its table of 34 bytes at 0xEE188 gives.
// Each script sets the same volume again, except the script of sequence 0x1A (see the tracks). The game's call gives a
// sequence a variation of 0 unless it asks for another.
constexpr EadProfile kSuperMario64 = {.engine = ultralib::n64::EadEngine::kEarly,
                                      .output_rate = 32006.0,
                                      .wave_rate = 32006.0,
                                      .updates_per_frame = 4,
                                      .song_volumes = 0xEE188,
                                      .song_volumes_count = 34,
                                      .effect_sequences = kFirstSequenceIsEffects,
                                      .variation = 0,
                                      .tracks = kSuperMario64Tracks,
                                      .names = ultralib::n64::kSuperMario64SongNames};

// Mario Kart 64 (USA). The game asks for 26800 samples a second, and the video interface's divider gives 26807.
constexpr EadProfile kMarioKart64 = {.engine = ultralib::n64::EadEngine::kMiddle,
                                     .output_rate = 26807.0,
                                     .wave_rate = 32000.0,
                                     .updates_per_frame = 3,
                                     .levels = EadLevels::kVolumesSquared,
                                     .effect_sequences = kFirstSequenceIsEffects};

// Wave Race 64 (USA, Rev 1). The function at 0x800C1504 that starts its music sets the player's scale to 0.55.
constexpr EadProfile kWaveRace64 = {.engine = ultralib::n64::EadEngine::kMiddle,
                                    .output_rate = 32006.0,
                                    .wave_rate = 32000.0,
                                    .updates_per_frame = 4,
                                    .levels = EadLevels::kEnvelopeSquared,
                                    .player_volume = 0.55,
                                    .effect_sequences = kFirstSequenceIsEffects};

// Sound effects, the nature sounds that the game's code runs through ports, and a list of effects for cutscenes.
constexpr std::array<std::size_t, 3> kOcarinaOfTimeEffects = {0x00, 0x01, 0x6D};

// The game's code tells Hyrule Field's script through a port what the player is doing. It sets channel volumes in Lon
// Lon Ranch's sequence by the player's distance from Malon: her voice on two channels and a melody on another.
//
// The file select starts sequence 0x57 with 1 for port 7. Sequence 0x57 points to sequence 0x28. With the port at 1,
// the sequence leaves out the harp's introduction. The Great Fairy's Fountain plays it with -1.
//
// Each room of Ganon's Tower calls the function at 0x800C5CD8 with a level, from 7 at the entrance to 0 in
// Ganondorf's lair. The game never plays sequence 0x2E without it. The function sets every channel's pan weight: 0x7F
// in the lair and 0 elsewhere. It then calls the function at 0x800C5D84 with the level's volume from the table at
// 0x80101F48. The entrance waits 2 game frames for that call. The second function sets the player's volume scale and
// writes a low-pass cutoff to IO port 4 of channel 15: 0x10 below a volume of 0x40, and otherwise a byte of
// ((volume - 0x40) / 4 + 1) * 16, which comes to 0 at 127. Each channel whose IO port 5 holds a value then gets a
// reverb from it. The tracks call the function on the 10th game frame, once the channels play. A game frame lasts 3
// audio frames.
constexpr int kGameFrame = 3;

constexpr EadGameCall GanonsTowerCall(int level)
{
    constexpr std::array<int, 8> kVolumes = {127, 80, 75, 73, 70, 68, 65, 60};
    const int volume = kVolumes[static_cast<std::size_t>(level)];
    const int cutoff = volume < 0x40 ? 0x10 : ((((volume - 0x40) >> 2) + 1) << 4) & 0xFF;

    return {.frame = 10 * kGameFrame,
            .volume_delay = level == 7 ? 2 * kGameFrame : 0,
            .pan_weight = level == 0 ? 0x7F : 0,
            .volume = volume,
            .io_channel = 15,
            .io_port = 4,
            .io_value = cutoff,
            .reverb_port = 5};
}

constexpr std::array kOcarinaOfTimeTracks = {
    EadTrack{.sequence = 0x02, .name = "Hyrule Field, moving", .ports = EadPorts(2, 0)},
    EadTrack{.sequence = 0x02, .name = "Hyrule Field, enemies near", .ports = EadPorts(2, 1)},
    EadTrack{.sequence = 0x02, .name = "Hyrule Field, standing still", .ports = EadPorts(2, 2)},
    EadTrack{.sequence = 0x2F, .name = "Lon Lon Ranch, Malon away"},
    EadTrack{.sequence = 0x2F, .name = "Lon Lon Ranch, far from Malon", .silent_channels = 0x0003},
    EadTrack{.sequence = 0x2F, .name = "Lon Lon Ranch, close to Malon", .silent_channels = 0x2000},
    EadTrack{.sequence = 0x2E, .name = "Ganon's Tower, entrance", .call = GanonsTowerCall(7)},
    EadTrack{.sequence = 0x2E, .name = "Ganon's Tower, level 6", .call = GanonsTowerCall(6)},
    EadTrack{.sequence = 0x2E, .name = "Ganon's Tower, level 5", .call = GanonsTowerCall(5)},
    EadTrack{.sequence = 0x2E, .name = "Ganon's Tower, level 4", .call = GanonsTowerCall(4)},
    EadTrack{.sequence = 0x2E, .name = "Ganon's Tower, level 3", .call = GanonsTowerCall(3)},
    EadTrack{.sequence = 0x2E, .name = "Ganon's Tower, level 2", .call = GanonsTowerCall(2)},
    EadTrack{.sequence = 0x2E, .name = "Ganon's Tower, level 1", .call = GanonsTowerCall(1)},
    EadTrack{.sequence = 0x2E, .name = "Ganon's Tower, Ganondorf's lair", .call = GanonsTowerCall(0)},
    EadTrack{.sequence = 0x28},
    EadTrack{.sequence = 0x28, .name = "file select, without the harp's introduction", .ports = EadPorts(7, 1)},
};

// Sound effects, the nature sounds, and a sequence that only waits for the game's code to name another.
constexpr std::array<std::size_t, 3> kMajorasMaskEffects = {0x00, 0x01, 0x7A};

// The game's code writes the day, from 0, to a port. Some sequences leave out channels by it. A port also starts some
// scripts at dawn, with a piece of their own. Romani Ranch's sequence works as Lon Lon Ranch's does.
//
// The function at 0x801A3000 starts a song's fanfare and writes a value for Link's form to port 7. Fourteen fanfares
// play another arrangement for Goron, Zora and Deku Link (0x5C, 0x5D and 0x5E). Link's value, 0x35, plays as the port's
// -1 does. The file select starts sequence 0x18 with 1 there. The sequence then leaves out the harp's introduction.
constexpr std::array kMajorasMaskTracks = {
    EadTrack{.sequence = 0x02, .name = "Termina Field"},
    EadTrack{.sequence = 0x02, .name = "Termina Field, at dawn", .ports = EadPorts(0, 1)},
    EadTrack{.sequence = 0x1D, .name = "Clock Town, dawn of the first day", .ports = EadPorts(0, 1, 4, 0)},
    EadTrack{.sequence = 0x1D, .name = "Clock Town, dawn of the second day", .ports = EadPorts(0, 1, 4, 1)},
    EadTrack{.sequence = 0x1D, .name = "Clock Town, dawn of the final day", .ports = EadPorts(0, 1, 4, 2)},
    EadTrack{.sequence = 0x2F, .name = "Romani Ranch, Romani away"},
    EadTrack{.sequence = 0x2F, .name = "Romani Ranch, far from Romani", .silent_channels = 0x0003},
    EadTrack{.sequence = 0x2F, .name = "Romani Ranch, close to Romani", .silent_channels = 0x2000},
    EadTrack{.sequence = 0x4D, .name = "New Wave Bossa Nova, first day", .ports = EadPorts(4, 0)},
    EadTrack{.sequence = 0x4D, .name = "New Wave Bossa Nova, later days", .ports = EadPorts(4, 1)},
    EadTrack{.sequence = 0x54, .name = "Ballad of the Wind Fish, first day", .ports = EadPorts(4, 0)},
    EadTrack{.sequence = 0x54, .name = "Ballad of the Wind Fish, second day", .ports = EadPorts(4, 1)},
    EadTrack{.sequence = 0x54, .name = "Ballad of the Wind Fish, final day", .ports = EadPorts(4, 2)},
    EadTrack{.sequence = 0x18},
    EadTrack{.sequence = 0x18, .name = "file select, without the harp's introduction", .ports = EadPorts(7, 1)},
    EadTrack{.sequence = 0x32},
    EadTrack{.sequence = 0x32, .name = "Epona's Song, as Goron Link", .ports = EadPorts(7, 0x5C)},
    EadTrack{.sequence = 0x32, .name = "Epona's Song, as Zora Link", .ports = EadPorts(7, 0x5D)},
    EadTrack{.sequence = 0x32, .name = "Epona's Song, as Deku Link", .ports = EadPorts(7, 0x5E)},
    EadTrack{.sequence = 0x33},
    EadTrack{.sequence = 0x33, .name = "Sun's Song, as Goron Link", .ports = EadPorts(7, 0x5C)},
    EadTrack{.sequence = 0x33, .name = "Sun's Song, as Zora Link", .ports = EadPorts(7, 0x5D)},
    EadTrack{.sequence = 0x33, .name = "Sun's Song, as Deku Link", .ports = EadPorts(7, 0x5E)},
    EadTrack{.sequence = 0x34},
    EadTrack{.sequence = 0x34, .name = "Song of Time, as Goron Link", .ports = EadPorts(7, 0x5C)},
    EadTrack{.sequence = 0x34, .name = "Song of Time, as Zora Link", .ports = EadPorts(7, 0x5D)},
    EadTrack{.sequence = 0x34, .name = "Song of Time, as Deku Link", .ports = EadPorts(7, 0x5E)},
    EadTrack{.sequence = 0x35},
    EadTrack{.sequence = 0x35, .name = "Song of Storms, ocarina, as Goron Link", .ports = EadPorts(7, 0x5C)},
    EadTrack{.sequence = 0x35, .name = "Song of Storms, ocarina, as Zora Link", .ports = EadPorts(7, 0x5D)},
    EadTrack{.sequence = 0x35, .name = "Song of Storms, ocarina, as Deku Link", .ports = EadPorts(7, 0x5E)},
    EadTrack{.sequence = 0x47},
    EadTrack{.sequence = 0x47, .name = "Song of Soaring, ocarina, as Goron Link", .ports = EadPorts(7, 0x5C)},
    EadTrack{.sequence = 0x47, .name = "Song of Soaring, ocarina, as Zora Link", .ports = EadPorts(7, 0x5D)},
    EadTrack{.sequence = 0x47, .name = "Song of Soaring, ocarina, as Deku Link", .ports = EadPorts(7, 0x5E)},
    EadTrack{.sequence = 0x48},
    EadTrack{.sequence = 0x48, .name = "Song of Healing, ocarina, as Goron Link", .ports = EadPorts(7, 0x5C)},
    EadTrack{.sequence = 0x48, .name = "Song of Healing, ocarina, as Zora Link", .ports = EadPorts(7, 0x5D)},
    EadTrack{.sequence = 0x48, .name = "Song of Healing, ocarina, as Deku Link", .ports = EadPorts(7, 0x5E)},
    EadTrack{.sequence = 0x49},
    EadTrack{.sequence = 0x49, .name = "Inverted Song of Time, as Goron Link", .ports = EadPorts(7, 0x5C)},
    EadTrack{.sequence = 0x49, .name = "Inverted Song of Time, as Zora Link", .ports = EadPorts(7, 0x5D)},
    EadTrack{.sequence = 0x49, .name = "Inverted Song of Time, as Deku Link", .ports = EadPorts(7, 0x5E)},
    EadTrack{.sequence = 0x4A},
    EadTrack{.sequence = 0x4A, .name = "Song of Double Time, as Goron Link", .ports = EadPorts(7, 0x5C)},
    EadTrack{.sequence = 0x4A, .name = "Song of Double Time, as Zora Link", .ports = EadPorts(7, 0x5D)},
    EadTrack{.sequence = 0x4A, .name = "Song of Double Time, as Deku Link", .ports = EadPorts(7, 0x5E)},
    EadTrack{.sequence = 0x51},
    EadTrack{.sequence = 0x51, .name = "Goron Lullaby, introduction, as Goron Link", .ports = EadPorts(7, 0x5C)},
    EadTrack{.sequence = 0x51, .name = "Goron Lullaby, introduction, as Zora Link", .ports = EadPorts(7, 0x5D)},
    EadTrack{.sequence = 0x51, .name = "Goron Lullaby, introduction, as Deku Link", .ports = EadPorts(7, 0x5E)},
    EadTrack{.sequence = 0x5B},
    EadTrack{.sequence = 0x5B, .name = "Sonata of Awakening, ocarina, as Goron Link", .ports = EadPorts(7, 0x5C)},
    EadTrack{.sequence = 0x5B, .name = "Sonata of Awakening, ocarina, as Zora Link", .ports = EadPorts(7, 0x5D)},
    EadTrack{.sequence = 0x5B, .name = "Sonata of Awakening, ocarina, as Deku Link", .ports = EadPorts(7, 0x5E)},
    EadTrack{.sequence = 0x5C},
    EadTrack{.sequence = 0x5C, .name = "Goron Lullaby, ocarina, as Goron Link", .ports = EadPorts(7, 0x5C)},
    EadTrack{.sequence = 0x5C, .name = "Goron Lullaby, ocarina, as Zora Link", .ports = EadPorts(7, 0x5D)},
    EadTrack{.sequence = 0x5C, .name = "Goron Lullaby, ocarina, as Deku Link", .ports = EadPorts(7, 0x5E)},
    EadTrack{.sequence = 0x5D},
    EadTrack{.sequence = 0x5D, .name = "New Wave Bossa Nova, ocarina, as Goron Link", .ports = EadPorts(7, 0x5C)},
    EadTrack{.sequence = 0x5D, .name = "New Wave Bossa Nova, ocarina, as Zora Link", .ports = EadPorts(7, 0x5D)},
    EadTrack{.sequence = 0x5D, .name = "New Wave Bossa Nova, ocarina, as Deku Link", .ports = EadPorts(7, 0x5E)},
    EadTrack{.sequence = 0x5E},
    EadTrack{.sequence = 0x5E, .name = "Elegy of Emptiness, ocarina, as Goron Link", .ports = EadPorts(7, 0x5C)},
    EadTrack{.sequence = 0x5E, .name = "Elegy of Emptiness, ocarina, as Zora Link", .ports = EadPorts(7, 0x5D)},
    EadTrack{.sequence = 0x5E, .name = "Elegy of Emptiness, ocarina, as Deku Link", .ports = EadPorts(7, 0x5E)},
    EadTrack{.sequence = 0x5F},
    EadTrack{.sequence = 0x5F, .name = "Oath to Order, ocarina, as Goron Link", .ports = EadPorts(7, 0x5C)},
    EadTrack{.sequence = 0x5F, .name = "Oath to Order, ocarina, as Zora Link", .ports = EadPorts(7, 0x5D)},
    EadTrack{.sequence = 0x5F, .name = "Oath to Order, ocarina, as Deku Link", .ports = EadPorts(7, 0x5E)},
};

constexpr EadProfile kOcarinaOfTime = {.engine = ultralib::n64::EadEngine::kLater,
                                       .output_rate = 32006.0,
                                       .wave_rate = 32006.0,
                                       .updates_per_frame = 3,
                                       .levels = EadLevels::kVolumesSquared,
                                       .effect_sequences = kOcarinaOfTimeEffects,
                                       .tracks = kOcarinaOfTimeTracks,
                                       .names = ultralib::n64::kOcarinaOfTimeSongNames};

constexpr EadProfile kMajorasMask = {.engine = ultralib::n64::EadEngine::kLaterMajora,
                                     .output_rate = 32006.0,
                                     .wave_rate = 32006.0,
                                     .updates_per_frame = 3,
                                     .levels = EadLevels::kVolumesSquared,
                                     .effect_sequences = kMajorasMaskEffects,
                                     .tracks = kMajorasMaskTracks,
                                     .names = ultralib::n64::kMajorasMaskSongNames};

constexpr std::array<std::size_t, 40> kStadium2Effects = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D,
    0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B,
    0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
};

// The game's music call at 0x80021ED8 changes the tempo of four sequences and masks channels of two others. It switches
// between the two versions of a masked sequence during play.
constexpr std::array kStadium2Tracks = {
    EadTrack{.sequence = 0x2E, .silent_channels = 0x3000},
    EadTrack{.sequence = 0x2E, .name = "second version", .silent_channels = 0x07FF},
    EadTrack{.sequence = 0x49},
    EadTrack{.sequence = 0x49, .name = "faster", .tempo_change = 17},
    EadTrack{.sequence = 0x5D},
    EadTrack{.sequence = 0x5D, .name = "faster", .tempo_change = 40},
    EadTrack{.sequence = 0x65, .silent_channels = 0xFD00},
    EadTrack{.sequence = 0x65, .name = "second version", .silent_channels = 0x021F},
    EadTrack{.sequence = 0x6D, .tempo_change = 4},
    EadTrack{.sequence = 0x73},
    EadTrack{.sequence = 0x73, .name = "faster", .tempo_change = 20},
};

// The game's table at 0x878C0 gives each sequence's banks. Its code gives each sequence from 0x28 on a level out of
// 127, from the table at 0x80090138. A value above 127 there names an entry of the table at 0x800917F8. The player's
// scale is 0.81 of that level.
constexpr EadProfile kStadium2 = {.engine = ultralib::n64::EadEngine::kLaterMajora,
                                  .output_rate = 32006.0,
                                  .wave_rate = 32006.0,
                                  .updates_per_frame = 3,
                                  .levels = EadLevels::kVolumesSquared,
                                  .envelope_skip = true,
                                  .channel_layers = 6,
                                  .inverse_vibrato = true,
                                  .player_volume = 0.81,
                                  .player_levels = 0x90D38,
                                  .player_levels_indirect = 0x923F8,
                                  .first_player_level = 0x28,
                                  .player_level_count = 0x61,
                                  .effect_sequences = kStadium2Effects,
                                  .tracks = kStadium2Tracks};

constexpr std::array kPuzzleLeagueBanks = {
    LibmusBankPlace{.first_song = 0, .pointers = 0x17C2A80, .samples = 0x17CA780},
    LibmusBankPlace{.first_song = 0x47, .pointers = 0x1C60AD0, .samples = 0x1C63BE0},
};

// The game's music setting starts at 0.55 of the player's full volume. Its code gives each song its byte of the table.
constexpr LibmusProfile kPuzzleLeague = {
    .output_rate = 22047,
    .songs = 0xB5F30,
    .song_count = 72,
    .song_volumes = 0xB4BB0,
    .master_volume = 18021,
    .banks = kPuzzleLeagueBanks,
};

// The volumes that the game's code gives its songs, in a switch at 0x8004B658. Song 0x1D reads a word at 0x80078FB4,
// which holds 112.
constexpr std::array<uint8_t, 76> kStadiumVolumes = {
    152, 104, 52, 68,  56,  52, 56,  120, 160, 80,  96,  56,  56,  72,  100, 96,  104, 96,  46,
    104, 88,  96, 96,  96,  96, 88,  96,  88,  96,  112, 101, 112, 112, 80,  72,  80,  72,  80,
    64,  88,  72, 120, 96,  96, 112, 96,  84,  88,  100, 96,  96,  96,  96,  84,  56,  56,  96,
    56,  104, 60, 96,  136, 96, 96,  80,  96,  128, 128, 144, 96,  96,  136, 112, 104, 168, 96,
};

// The game's code keeps the songs and their bank compressed in a directory. Each song is a file of the library's early
// form, and the library's early player plays it. That player holds a voice's volume at 0x7FFF after the master volume
// (0x8003B18C to 0x8003B1D8), and its envelopes count ticks whatever their speed (0x8003B630). The game gives the songs
// a master volume of 0x6FFF. Its synthesizer runs at 32006 Hz with the SDK's big room on the auxiliary bus.
constexpr LibmusProfile kStadium = {
    .output_rate = 32006,
    .early_form = true,
    .early_player = true,
    .directory = 0x15B5020,
    .song_volume_list = kStadiumVolumes,
    .master_volume = 0x6FFF,
};

constexpr std::array kMarioTennisBanks = {
    LibmusBankPlace{.first_song = 0, .pointers = 0x3C780, .samples = 0x55B20},
};

// The trophy scene in file 0x1CE scales three songs' tempos, at offsets 0x5BCC, 0x5EE8 and 0x5FB4. A run of the game's
// code shows the calls at 0x802ECBCC, four frames after each song starts: TRO-MARIO for character 2, TRO-WALUI for
// character 8 and TRO-BABYM for character 0x11. Character 6 plays TRO-BABYM without a change.
constexpr std::array kMarioTennisTempos = {
    LibmusTempoChange{.song = 0x2A, .scale = 0x8E, .beside = "faster"},
    LibmusTempoChange{.song = 0x2F, .scale = 0x7C},
    LibmusTempoChange{.song = 0x3A, .scale = 0x78},
};

// The game's code loads its sound code from file 0x13 to 0x802EC000. The function at 0x802ECC60 starts a song and gives
// its channels a volume of 0x32, the word at 0x802F4AB0. The master volume stays at 0x7FFF. The synthesizer runs at
// 32006 Hz.
constexpr LibmusProfile kMarioTennis = {
    .output_rate = 32006,
    .song_volume = 0x32,
    .banks = kMarioTennisBanks,
    .files = {.directory = 0x116C0, .table_file = 0x13, .table = 0x8ABC, .table_count = 66, .file_address = 0x802EC000},
    .tempo_changes = kMarioTennisTempos,
};

// The game's scripts slow some cues through 0x8011D768, which passes a rate to 0x800231F4. A run of the game's code in
// those areas shows where each slide stops: 0.798 past an area's time limit, and 0.79 in the fishing areas. The rate
// reaches a cue through 0x8001FF84, which leaves out cue 2, Mamma mia!. Each track holds the rate where the slide
// stops.
constexpr std::array kHeyYouPikachuRates = {
    LibultraRateChange{.song = 5, .rate = 0.798, .beside = "slower"},
    LibultraRateChange{.song = 6, .rate = 0.798, .beside = "slower"},
    LibultraRateChange{.song = 11, .rate = 0.798, .beside = "slower"},
    LibultraRateChange{.song = 14, .rate = 0.798, .beside = "slower"},
    LibultraRateChange{.song = 18, .rate = 0.798, .beside = "slower"},
    LibultraRateChange{.song = 19, .rate = 0.79, .beside = "slower"},
    LibultraRateChange{.song = 24, .rate = 0.798, .beside = "slower"},
};

// Lists of a track's number and a level. The game's code at 0x80055EB4 gives them by number.
constexpr std::size_t kPaperMarioVillageLevels = 0x541B8;
constexpr std::size_t kPaperMarioTunnelLevels = 0x541CC;
constexpr std::size_t kPaperMarioCastleFirst = 0x541F8;
constexpr std::size_t kPaperMarioCastleMiddle = 0x54214;
constexpr std::size_t kPaperMarioCastleFinal = 0x54230;

// The game's map scripts change five songs while they play. Toad Town's triggers pick a mix by the player's position,
// at the addresses below in the image. No trigger holds mix 6. Each script that uses them starts the song's first
// version: the calls at 0x7F9138, 0x809130, 0x831418 and 0x8520C0. The code at 0x80056044 gives a mix its near level,
// and the code at 0x80056068 its full level. A building's script gives the mixes with names their full level. The code
// at 0x954004 swaps Dry Dry Desert's linked tracks at the oasis. The code at 0xB2C47C and at 0x892A2C and 0x89FECC sets
// track levels for Yoshi's Village and two rooms of the tunnels. Bowser's Castle sets them in scripts at 0xA4C178 to
// 0xAA9768.
constexpr std::array kPaperMarioChanges = {
    SbnChange{.song = 0x00, .mix = 1, .name = "Tayce T.'s House"}, // 0x8313E0
    SbnChange{.song = 0x00, .mix = 2, .name = "Merlon's House"},   // 0x8090E0
    SbnChange{.song = 0x00, .mix = 3, .name = "the Dojo"},         // 0x7F9118
    SbnChange{.song = 0x00, .mix = 4, .near = true},               // 0x8313F8
    SbnChange{.song = 0x00, .mix = 5, .near = true},               // 0x809110
    SbnChange{.song = 0x00, .mix = 7, .name = "Russ T.'s House"},  // 0x7F9100
    SbnChange{.song = 0x00, .mix = 8, .near = true},               // 0x8090F8
    SbnChange{.song = 0x00, .mix = 9, .name = "the Warehouse"},    // 0x8520A0
    SbnChange{.song = 0x17, .linked = true, .name = "the Oasis"},
    SbnChange{.song = 0x26, .levels = kPaperMarioVillageLevels, .name = "full levels"},
    SbnChange{.song = 0x4B, .levels = kPaperMarioTunnelLevels, .name = "cold rooms"},
    SbnChange{.song = 0x64, .levels = kPaperMarioCastleFirst, .name = "first areas"},
    SbnChange{.song = 0x64, .levels = kPaperMarioCastleMiddle, .name = "middle areas"},
    SbnChange{.song = 0x64, .levels = kPaperMarioCastleFinal, .name = "final areas"},
};

// Paper Mario's tables, in its image.
constexpr SbnProfile kPaperMario = {.output_rate = 32006,
                                    .ticks_per_beat = 0x53910,
                                    .envelope_times = 0x539A0,
                                    .envelope_time_count = 95,
                                    .custom_times = 0x53958,
                                    .custom_time_count = 40,
                                    .default_envelope = 0x5394C,
                                    .names = ultralib::n64::kPaperMarioSongNames,
                                    .changes = kPaperMarioChanges};

// Hi Pai Paradise (Aleck64). The game's code runs from 0x80025C00, from 0x1000 in the image. The function at
// 0x8002E870 reads a song's file from the cartridge through the table of files at 0x80091CA0, from 0x140000 in the
// image. The halfwords at 0x800918A0 give the files of songs 1 to 13. At boot, the code at 0x8002E334 reads the table
// of waves from file 0x3D4 and gives it the samples of file 0x3D3. The function at 0x8002E870 gives each song a volume
// of 0xAC, from the setting at 0x800918E8 and the table at 0x800918E0. The other value of the setting gives 0xDC. The
// game asks the library for 22050 Hz.
constexpr std::array kHiPaiParadiseBanks = {LibmusBankPlace{.pointers = 0xCC4660, .samples = 0xB12F60}};

constexpr LibmusProfile kHiPaiParadise = {
    .output_rate = 22047,
    .song_count = 13,
    .cartridge_files = {.table = 0x6D0A0, .base = 0x140000, .song_files = 0x6CCA0, .first_song = 1},
    .song_volume = 0xAC,
    .banks = kHiPaiParadiseBanks,
};

// Tower & Shaft (Aleck64). The game's code runs from 0x80025C00, from 0x1000 in the image. The function at 0x8003919C
// reads a song from the cartridge through the tables of starts at 0x8010EB90 and of ends at 0x8010EBDC. It gives the
// song the volume in the halfword at 0x8009F0F6. The game sets that halfword to 0x78. At boot, the code at 0x800390E4
// reads the table of waves from 0x213F40 in the image and gives it the samples at 0x217C90. The game asks the library
// for 32000 Hz.
constexpr std::array kTowerAndShaftBanks = {LibmusBankPlace{.pointers = 0x213F40, .samples = 0x217C90}};

constexpr LibmusProfile kTowerAndShaft = {
    .output_rate = 32006,
    .songs = 0xE9F90,
    .song_count = 19,
    .song_ends = 0xE9FDC,
    .song_volume = 0x78,
    .banks = kTowerAndShaftBanks,
};

// A song of Gex 64 that the image holds as a raw DEFLATE stream.
constexpr LibmusSong PackedSong(std::size_t start, std::size_t end, int volume, int reverb_base, const char* name)
{
    return {.start = start, .end = end, .deflated = true, .volume = volume, .reverb_base = reverb_base, .name = name};
}

// Gex 64: Enter the Gecko (USA). The game's code runs from 0x80000400, from 0x1000 in the image. The songs have the
// library's early form, and the later player plays them. A channel keeps one voice (0x80054988), an envelope's times
// count units of its speed (0x80053048), and a voice's volume stops at 0x7FFF before the master volume (0x80054BD4).
// The songs of looney30 and looney69 reach that top. The profile therefore clips the envelopes. The table of levels at
// 0x8006F5A8 gives each level a bank, a volume, a reverb base and one song or a list of songs. A song's name lists its
// levels by their names in the table at 0x8006FCE0. That table has 32 bytes for each level, with the name at byte 20.
// The level mooshu1 has the songs of circuit5 and no triggers (0x8006F3C0). It never plays the second song. Each song
// is a raw DEFLATE stream. The function at 0x800327C0 unpacks a level's songs and starts the first at the level's
// volume (0x80054164) and reverb base (0x80054330). The level's triggers start the others in a list. A trigger fades
// its song in by the trigger's step (0x800329DC). The fade stops once the volume reaches the volume of the list's first
// song, and it never goes past the level's volume. Each song below takes the volume where the fade of its track's
// trigger ends, as runs of the game's code show. Song 2 of looney30 and looney69 ends at 0x70 in looney30 and at 0xA0
// in looney69. The function at 0x8003335C starts a short song of EXTRAS.WBK from the table at 0x8006FA0C, at the
// table's volume, with a reverb base of 0. Slot 2 of looney69 (0x459FF0) and the song at 0x47B670 are never played. The
// songs take a master volume of 0x3FFF, from the game's settings. The game asks the library for 22050 Hz.
constexpr std::array kGexSongs = {
    PackedSong(0x4918E0, 0x4931C0, 0x60, 0x19, "scifi10 and scifi14"),
    PackedSong(0x4931C0, 0x4958F0, 0x60, 0x19, "spy2 and map5"),
    PackedSong(0x3EB590, 0x3EC2A0, 0x40, 0x19, "aztec16"),
    PackedSong(0x403DD0, 0x405C60, 0x60, 0x19, "gillig1"),
    PackedSong(0x4030B0, 0x403DD0, 0x60, 0x19, "gexzil9"),
    PackedSong(0x45CBC0, 0x45D100, 0xA0, 0x19, "looney30 and looney69, song 1"),
    PackedSong(0x458B70, 0x459FF0, 0x70, 0x19, "looney30 and looney69, song 2"),
    PackedSong(0x45B420, 0x45CBC0, 0xA0, 0x19, "looney30 and looney69, song 3"),
    PackedSong(0x45AE70, 0x45B420, 0xA0, 0x19, "looney30, song 4"),
    PackedSong(0x45A5C0, 0x45AE70, 0xA0, 0x19, "looney30 and looney69, song 5"),
    PackedSong(0x45D100, 0x45D6D0, 0xA0, 0x19, "looney69, song 6"),
    PackedSong(0x45D6D0, 0x45DC50, 0xA0, 0x19, "looney69, song 7"),
    PackedSong(0x459FF0, 0x45A5C0, 0xA0, 0x19, "looney69, song 8, never played"),
    PackedSong(0x401F70, 0x402880, 0x80, 0x19, "circuit5, circuit9, circuit0 and mooshu1"),
    PackedSong(0x402880, 0x4030B0, 0x80, 0x19, "circuit5, circuit9 and circuit0, second song"),
    PackedSong(0x41B390, 0x41CA80, 0x60, 0x19, "horror4, horror2, horror6 and rta1"),
    PackedSong(0x41CA80, 0x41CE00, 0x60, 0x19, "rta1, second song"),
    PackedSong(0x439330, 0x43B180, 0x60, 0x19, "kungfu02, kungfu1 and kungfu4"),
    PackedSong(0x457A80, 0x458B70, 0x60, 0x19, "nypd01"),
    PackedSong(0x47A340, 0x47B670, 0x60, 0x14, "prehst1 and prehst2"),
    PackedSong(0x47B670, 0x47CCB0, 0x60, 0x14, "prehismx.wbk, second song, never played"),
    PackedSong(0x490EC0, 0x4918E0, 0x60, 0x19, "rezop3 and rezop2"),
    PackedSong(0x405C60, 0x4066F0, 0x60, 0x19, "final01"),
    LibmusSong{.start = 0x497E00, .end = 0x498300, .volume = 0x78, .name = "EXTRAS.WBK, short song 1"},
    LibmusSong{.start = 0x498300, .end = 0x498A50, .volume = 0x50, .name = "EXTRAS.WBK, short song 2"},
    LibmusSong{.start = 0x498A50, .end = 0x499550, .volume = 0x50, .name = "EXTRAS.WBK, short song 3"},
    LibmusSong{.start = 0x499550, .end = 0x49A200, .volume = 0x64, .name = "EXTRAS.WBK, short song 4"},
};

// The banks of the songs above: orchestr.wbk, CIRCUIT.WBK, horrormx.wbk, KungFu.wbk, NYPD.wbk, prehismx.wbk,
// REZOP.WBK and EXTRAS.WBK. Each table of waves follows its samples.
constexpr std::array kGexBanks = {
    LibmusBankPlace{.first_song = 0, .pointers = 0x3E7060, .samples = 0x386450},
    LibmusBankPlace{.first_song = 13, .pointers = 0x401250, .samples = 0x3EC2A0},
    LibmusBankPlace{.first_song = 15, .pointers = 0x41AC70, .samples = 0x4066F0},
    LibmusBankPlace{.first_song = 17, .pointers = 0x4382E0, .samples = 0x41CE00},
    LibmusBankPlace{.first_song = 18, .pointers = 0x456C70, .samples = 0x43B180},
    LibmusBankPlace{.first_song = 19, .pointers = 0x4795F0, .samples = 0x45DC50},
    LibmusBankPlace{.first_song = 21, .pointers = 0x490000, .samples = 0x47CCB0},
    LibmusBankPlace{.first_song = 23, .pointers = 0x4973A0, .samples = 0x4958F0},
};

constexpr LibmusProfile kGex = {
    .output_rate = 22047,
    .early_form = true,
    .clipped_envelopes = true,
    .loop_on_later_passes = true,
    .master_volume = 0x3FFF,
    .banks = kGexBanks,
    .song_list = kGexSongs,
};

// The stereo songs of Super Real Mahjong VS.
constexpr std::array kSuperRealMahjongVsSets = {
    LibultraSongSet{.name = "stereo", .first_song = 0, .song_count = 13, .bank = 0x8E040},
};

const std::array kProfiles = {
    // Chameleon Twist (USA, Rev 1). The game's code gives the sequence player its volume from a table of 8 bytes for
    // each song at 0x800FB0B0 (0xD64B0 in the image), through 0x8008B4F4. Its player has the SDK's oscillators at
    // 0x80084CA0. Its synthesizer has the SDK's echo on the auxiliary bus.
    GameProfile{.sha1 = "4ca6d563131b4809fe1748335182816a024999d4",
                .libultra = {.output_rate = 44095,
                             .sequences = 0x27E930,
                             .bank = 0x143210,
                             .volumes = {.offset = 0xD64B2, .stride = 8, .count = 30},
                             .oscillators = true,
                             .effect = BusEffect::kEcho,
                             .names = ultralib::n64::kChameleonTwistSongNames}},

    // Chameleon Twist 2 (USA). A run of the game's code gives its player a volume of 0x3FFF for every song. Its player
    // has no oscillators. Its synthesizer has the SDK's big room on the auxiliary bus. The game's code at 0x800CD70C
    // sets every program's bend range to 1216 cents.
    GameProfile{.sha1 = "9fa379d66b218228da3cbf386c91d857c677f489",
                .libultra = {.output_rate = 22047,
                             .sequences = 0x120190,
                             .bank = 0xD7B90,
                             .line_ramps = true,
                             .volume = 0x3FFF,
                             .program_bend_range = 1216,
                             .effect = BusEffect::kBigRoom,
                             .names = ultralib::n64::kChameleonTwist2SongNames}},

    // Cruis'n World (USA). A run of the game's code gives its player a volume of 0x42D7 for every song. Its player has
    // no oscillators. Its synthesizer has the SDK's small room on the auxiliary bus.
    GameProfile{.sha1 = "6da1a6a2bda687d50e798d80c342948ad1738202",
                .libultra = {.output_rate = 22047,
                             .sequences = 0x82CF10,
                             .bank = 0x835C10,
                             .volume = 0x42D7,
                             .effect = BusEffect::kSmallRoom,
                             .names = ultralib::n64::kCruisnWorldSongNames}},

    // GoldenEye 007 (USA). The game's code gives its player a volume for each song, from a table in its data. Its
    // players have no oscillators. Its synthesizer has a custom effect of six sections on the auxiliary bus. The
    // stages' second tracks play on a second player at the same time, when the game turns them up.
    GameProfile{.sha1 = "abe01e4aeb033b6c0836819f549c791b26cfde83",
                .libultra = {.output_rate = 22047,
                             .bank = 0x3B4450,
                             .rare_sequences = true,
                             .line_ramps = true,
                             .volume_list = kGoldenEyeVolumes,
                             .effect = BusEffect::kCustom,
                             .names = ultralib::n64::kGoldenEyeSongNames}},

    // Pokémon Snap (USA). The game's code gives each player a volume of 0x7F00 at the start of a scene, and fades it
    // from there. HAL's player has the SDK's oscillators at 0x80020364, and its volume arithmetic at 0x8003144C. Its
    // synthesizer has a custom effect of eight sections.
    GameProfile{.sha1 = "edc7c49cc568c045fe48be0d18011c30f393cbaf",
                .libultra = {.output_rate = 32006,
                             .sequences = 0xAEFC10,
                             .bank = 0xAFEEE0,
                             .player = LibultraPlayer::kHal,
                             .volume = 0x7F00,
                             .oscillators = true,
                             .effect = BusEffect::kCustom,
                             .names = ultralib::n64::kPokemonSnapSongNames}},

    // Hey You, Pikachu! (USA). The game's code at 0x800059E4 reads its 45 standard MIDI files on a player of its
    // developer. The synthesizer's bus has a custom effect of six sections.
    GameProfile{.sha1 = "da1a1e47a86720f9d54fb2d2d247480041bda824",
                .libultra = {.output_rate = 32006,
                             .sequences = 0xB1422C,
                             .bank = 0x84F5F0,
                             .midi_files = 45,
                             .player = LibultraPlayer::kAmbrella,
                             .line_ramps = true,
                             .effect = BusEffect::kCustom,
                             .names = ultralib::n64::kHeyYouPikachuSongNames,
                             .rate_changes = kHeyYouPikachuRates}},
    // Pokémon Puzzle League.
    GameProfile{.sha1 = "8173866fc8c7652abd44c48efcab85441c6806a1", .libmus = &kPuzzleLeague},

    // Pokémon Stadium (USA, Rev 2).
    GameProfile{.sha1 = "0d3b1d740c6ee6da6923550bf24ad827262fd8c0", .libmus = &kStadium},

    // Mario Tennis (USA).
    GameProfile{.sha1 = "999047f07cec931ffbdcc7b33b8502ef602807ee", .libmus = &kMarioTennis},

    // Paper Mario (USA).
    GameProfile{.sha1 = "3837f44cda784b466c9a2d99df70d77c322b97a0", .sbn = &kPaperMario},

    // Pokémon Stadium 2 (USA).
    GameProfile{.sha1 = "d8343e69a7dc63b869cf6361d87cde64444281d3", .ead = &kStadium2},
    // Super Mario 64.
    GameProfile{.sha1 = "9bef1128717f958171a4afac3ed78ee2bb4e86ce", .ead = &kSuperMario64},
    // Mario Kart 64.
    GameProfile{.sha1 = "579c48e211ae952530ffc8738709f078d5dd215e", .ead = &kMarioKart64},
    // The Legend of Zelda: Ocarina of Time.
    GameProfile{.sha1 = "41b3bdc48d98c48529219919015a1af22f5057c2", .ead = &kOcarinaOfTime},
    // The Legend of Zelda: Majora's Mask.
    GameProfile{.sha1 = "d6133ace5afaa0882cf214cf88daba39e266c078", .ead = &kMajorasMask},
    // Wave Race 64.
    GameProfile{.sha1 = "508dfc2d4caa42b6f6de5263d0aed5e44ac7966a", .ead = &kWaveRace64},
    // Gex 64: Enter the Gecko.
    GameProfile{.sha1 = "16042cd0dfa5439ca436f1bf05ebfb7e9f730cda", .libmus = &kGex},

    // Eleven Beat (Aleck64). The songs play on libultra's compressed sequence player, under Hudson's sound code. The
    // game gives its synthesizer a rate of 32006 and the SDK's small room on the auxiliary bus, in the configuration
    // at 0x800A08E4. Its player has oscillators at 0x800049B4, in the place of the SDK's oscillators.
    GameProfile{.sha1 = "5432056f34c378b5862009e5b79a256c10c5a20e",
                .libultra = {.output_rate = 32006,
                             .sequences = 0x36D7D0,
                             .bank = 0x36D9B8,
                             .hudson_sequences = true,
                             .oscillators = true,
                             .effect = BusEffect::kSmallRoom}},

    // Mayjinsen 3 (Aleck64). The game's code at 0x800288C4 gives the compressed sequence player the volume at
    // 0x80108DAC. The volume there is 0x3FFF. Its player has no oscillators. The code at 0x80029124 gives its
    // synthesizer the SDK's small room on the auxiliary bus.
    GameProfile{.sha1 = "da401fc96a2b69db81970d879a2255e54db88e34",
                .libultra = {.output_rate = 44095,
                             .sequences = 0x600660,
                             .bank = 0x444250,
                             .volume = 0x3FFF,
                             .effect = BusEffect::kSmallRoom}},

    // Star Soldier: Vanishing Earth (Aleck64). The songs play on libultra's compressed sequence player, under Hudson's
    // sound code. The function at 0x80002B84 reads the record of a song. The code at 0x80002D78 gives the player the
    // bank that the record names. The code at 0x8000205C gives the synthesizer a custom effect on the auxiliary bus.
    // The player has oscillators at 0x80003740, in the place of the SDK's oscillators.
    GameProfile{.sha1 = "a29348632ed7726d75d693dfb007f277c1f6a563",
                .libultra = {.output_rate = 32006,
                             .sequences = 0x9B180,
                             .bank = 0x9B3F8,
                             .hudson_sequences = true,
                             .oscillators = true,
                             .effect = BusEffect::kCustom}},

    // Super Real Mahjong VS (Aleck64). The sequence file holds 40 sequences. The first 20 play in stereo on the bank
    // file at 0x8E040. The next 20 hold the same songs for mono, on the bank file at 0x81F90. Sequences 13 to 19 and 33
    // to 39 are empty. A setting of the service menu picks the mode. The setting is bit 0 of the byte at 0x800DE6C8,
    // among the saved settings, and the function at 0x80005058 returns it. At boot, the code at 0x800337A8 plays stereo
    // when the bit is set. The function at 0x80005388 clears the settings when their checksum fails. The game then
    // plays mono. Mono is also the factory setting. The menu code at 0x80033B0C reads the setting. A change to it calls
    // the function at 0x80001CE0. That function sets the mode and gives the player the bank file of the mode. In mono,
    // the function at 0x80001E38 adds 20 to the number of a song. The mono songs come first, and the stereo set
    // follows. The code at 0x80030180 gives the compressed sequence player a volume of 0x7FFF. The player has no
    // oscillators. The code at 0x80002920 gives the synthesizer no effect.
    GameProfile{.sha1 = "a590216a171a61517cd07ff629d8215631c8be87",
                .libultra = {.output_rate = 44095,
                             .sequences = 0x268770,
                             .bank = 0x81F90,
                             .first_song = 20,
                             .song_count = 13,
                             .other_sets = kSuperRealMahjongVsSets,
                             .volume = 0x7FFF}},

    // Vivid Dolls (Aleck64). The game's code at 0x8000D344 gives the compressed sequence player the volume at
    // 0x800A236C. The volume there is 0x7800. Its player has no oscillators. The code at 0x8000DB84 gives its
    // synthesizer the SDK's small room on the auxiliary bus.
    GameProfile{.sha1 = "747e9420cce23ff82b5922cd91ec43272d03fa4f",
                .libultra = {.output_rate = 44095,
                             .sequences = 0x1D7720,
                             .bank = 0x196CC0,
                             .volume = 0x7800,
                             .effect = BusEffect::kSmallRoom}},

    // Hi Pai Paradise.
    GameProfile{.sha1 = "68329c94e1b9342ee60713e9e0945ea2663210c7", .libmus = &kHiPaiParadise},
    // Tower & Shaft.
    GameProfile{.sha1 = "ba4474dc4967adb2a41cf3b0022c6215990c0e64", .libmus = &kTowerAndShaft},

    // Four games of the Aleck64 stream each song from the cartridge as one wave, through Software Creations' music
    // library. Hi Pai Paradise 2 gives the library a bank of 17 waves, one for each song, from the table of waves at
    // 0x13D7970 in the image.

    // Hanabi de Doon! - Don-chan Puzzle.
    GameProfile{.sha1 = "2a77e47d34b8d9d89eb37ffc062e9ec0c2121abc", .streamed_music = true},
    // Hi Pai Paradise 2.
    GameProfile{.sha1 = "47fdc89620ddf68972e861dee1536a26a309e35c", .streamed_music = true},
    // Kurukuru Fever.
    GameProfile{.sha1 = "f59c121e4d4c2c57c2ea4728919775a8d5d5e02e", .streamed_music = true},
    // Magical Tetris Challenge.
    GameProfile{.sha1 = "7e357995def4078a54ba8e4486f361d631ff3329", .streamed_music = true},
};

} // namespace

std::string SequenceLabel(std::size_t index)
{
    return "Sequence " + xsflib::Hex(static_cast<uint32_t>(index), 2, true);
}

std::string SequenceName(std::size_t index, std::span<const SongName> names, const char* beside)
{
    std::string inside;
    for (const SongName& song : names)
    {
        if (song.index == index)
        {
            inside = song.name;
            break;
        }
    }

    if (beside != nullptr)
    {
        inside += (inside.empty() ? "" : ", ") + std::string(beside);
    }

    return inside.empty() ? SequenceLabel(index) : SequenceLabel(index) + " (" + inside + ")";
}

std::span<const GameProfile> Profiles()
{
    return kProfiles;
}

const GameProfile* FindProfile(const std::string& sha1)
{
    for (const GameProfile& profile : kProfiles)
    {
        if (sha1 == profile.sha1)
        {
            return &profile;
        }
    }

    return nullptr;
}

} // namespace ultralib::sequence_export::n64
