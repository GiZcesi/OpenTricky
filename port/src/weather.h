/*
 * weather -- Graphics.Weather: the snowfall without the fog the Xbox version
 * adds to it, as in the PS2 version (XBOX_WEATHER).
 *
 * The draw (InGameState_LoadLevel 0xAC9B0, both versions): weather = r < p,
 * p = [TrackInfo+0x54] ([[lvl+0x2C]+0x54]: Snowdream 1.0, Merqury 0.25, the
 * other tracks 0), into the byte lvl+0x78 (lvl = [[0x1E3C7C]+0x72C]). The
 * Xbox version alone draws once more on three tracks (table 0xAD464:
 * Garibaldi < 0.03, Elysium < 0.02, Untracked < 0.05). Both cut it in races,
 * with two players and with more than two riders. Then the snow amount
 * d = lo + (hi - lo) r (Xbox 0.1 to 0.3, Garibaldi 0.1 to 0.25), into
 * lvl+0x7C and the snowfall object (SetDensity 0xDE710: its flake count).
 * During the ride each rider's own amount (rider+0x18) follows lvl+0x7C (0
 * where rider+0x3B8 has 0x4000), and the snowfall render takes it.
 *
 * What the weather does on the Xbox, besides the snowfall:
 *   - the per-track fog and light setup (0xFD3E0, at the level's setup)
 *     makes the fog density d x 30 x 2e-5 + 1e-5 instead of 1e-5 (53 to
 *     143 m to half visibility at Snowdream), and its common tail (0xFDD5C)
 *     turns the shadow direction vertical ([0x1C0AA0]) with [0x1C0954] 0.8;
 *   - over 2e-5 the renderer leaves out the sky dome (DrawMesh 0x104060 skips
 *     the meshes drawn with state bit 3: the dome only, so no moon), clears
 *     to the fog colour, draws no lens flare (0xE08C0, 0xE13F0: nor their
 *     visibility queries), paints the fog volumes the fog colour (0xDF410),
 *     and changes three more draws (0x3EFB0, 0x101D00, 0x106300).
 * The PS2 version has the same draw without the three extra chances, and
 * its weather drives the falling snow alone: no fog term, the sky dome
 * always drawn.
 *
 * XBOX_WEATHER=ps2 (Recommended, Steam Deck):
 *   0xFD3E0  runs with the weather byte read as 0, then the byte is put back:
 *            fog density 1e-5, sky, lens flares, fog volumes, clear colour
 *            and shadows as in clear weather. The snowfall keeps its weather
 *            and its amount: it falls as the original game draws it.
 *   0xDEE40  (snowfall render, a virtual call) runs with the weather byte
 *            read as 0 on a track whose own chance p is 0 (Garibaldi,
 *            Elysium, Untracked: their weather comes from the Xbox's extra
 *            draws only, the PS2 never has any there): no snow, the title's
 *            own no-weather path.
 *   No hook draws a random number or skips one: LoadLevel and its draws are
 *   left as they are, and the paths that clear weather turns on reach no
 *   draw through a direct call (read from the code; indirect calls were not
 *   followed). Only display code reads the weather byte. Races never have
 *   weather: nothing changes there.
 * XBOX_WEATHER=original, or unset (tools, test runs): the original game. The
 *   direct calls to 0xFD3E0 reach its hook in every build (fork pass
 *   fix_manual_hook_calls), which calls the original at once; 0xDEE40's hook
 *   is not handed out.
 * Logged ([WEATHER]): the level's weather at its fog setup when it is on,
 * and with ps2 the first snowfall left out after that setup.
 */
#ifndef FORK_WEATHER_H
#define FORK_WEATHER_H

void weather_init(void);                       /* reads XBOX_WEATHER */
int  weather_ps2(void);                        /* 1 = the PS2 version's weather */
void (*weather_lookup(unsigned int xbox_va))(void);

#endif
