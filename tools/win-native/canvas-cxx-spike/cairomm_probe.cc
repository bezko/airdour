/*
 * cairomm_probe — prove MSVC can link cairomm (C++ Cairo) without GTK windows.
 * Isolated spike prefix; not product chrome (Airdour stays D2D).
 */
#include <cstdint>
#include <cstdio>

#include <cairomm/cairomm.h>
#include <cairomm/context.h>
#include <cairomm/surface.h>

int
main ()
{
	std::printf ("airdour cairomm-probe (MSVC; no chrome)\n");
	std::printf ("  cairo: %s\n", cairo_version_string ());

	Cairo::RefPtr<Cairo::ImageSurface> surface =
	    Cairo::ImageSurface::create (Cairo::Surface::Format::ARGB32, 64, 64);
	if (!surface) {
		std::fprintf (stderr, "cairomm-probe: ImageSurface::create FAILED\n");
		return 1;
	}

	Cairo::RefPtr<Cairo::Context> cr = Cairo::Context::create (surface);
	if (!cr) {
		std::fprintf (stderr, "cairomm-probe: Context::create FAILED\n");
		return 1;
	}

	cr->set_source_rgb (0.12, 0.55, 0.40);
	cr->paint ();
	cr->set_source_rgba (0.95, 0.55, 0.20, 0.9);
	cr->move_to (8, 32);
	cr->line_to (56, 16);
	cr->line_to (56, 48);
	cr->close_path ();
	cr->fill ();

	surface->flush ();
	const int stride = surface->get_stride ();
	unsigned char* data = surface->get_data ();
	if (!data || stride <= 0) {
		std::fprintf (stderr, "cairomm-probe: surface data FAILED\n");
		return 1;
	}

	unsigned inked = 0;
	for (int y = 0; y < 64; ++y) {
		const uint32_t* row = (const uint32_t*)(data + y * stride);
		for (int x = 0; x < 64; ++x) {
			if ((row[x] >> 24) != 0) {
				++inked;
			}
		}
	}
	if (inked < 64) {
		std::fprintf (stderr, "cairomm-probe: FAILED (inked=%u)\n", inked);
		return 1;
	}

	std::printf ("  cairomm ImageSurface: ok (64x64; inked=%u)\n", inked);
	std::printf ("PASS\n");
	return 0;
}
