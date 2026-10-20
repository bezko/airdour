/*
 * canvas_types_probe — attempt to compile a libs/canvas header on MSVC.
 *
 * Expected: FAIL until gtkmm2ext + ytk/ydk (Ardour GTK2 forks) exist on MSVC.
 * Stock vcpkg gtkmm is 4.x and does not satisfy this include chain.
 *
 * No GTK windows. Documents the blocker for hybrid canvas reuse.
 */
#include <cstdio>

/* This is the first canvas public header many TUs pull in. */
#include "canvas/types.h"

int
main ()
{
	using namespace ArdourCanvas;
	Duple a (1.0, 2.0);
	Duple b (3.0, 4.0);
	Duple c = a + b;
	std::printf ("airdour canvas-types-probe (unexpected PASS)\n");
	std::printf ("  Duple sum=(%.1f,%.1f) COORD_MAX ok\n", c.x, c.y);
	std::printf ("PASS\n");
	return 0;
}
