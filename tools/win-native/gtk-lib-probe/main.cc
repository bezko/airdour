/*
 * gtk-lib-probe — prove MSVC can link GTK3 + Cairo (libraries only).
 *
 * Does NOT create windows, show widgets, or apply GTK themes.
 * Product UI remains Win32 + Direct2D (airdour_win_shell).
 */
#include <cstdio>

#include <cairo.h>
#include <gtk/gtk.h>

int
main ()
{
	std::printf ("airdour gtk-lib-probe (MSVC libs; no chrome)\n");
	std::printf ("  gtk:   %u.%u.%u\n",
	             gtk_get_major_version (),
	             gtk_get_minor_version (),
	             gtk_get_micro_version ());
	std::printf ("  cairo: %s\n", cairo_version_string ());

	cairo_surface_t* surface =
	    cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 64, 64);
	if (!surface || cairo_surface_status (surface) != CAIRO_STATUS_SUCCESS) {
		std::fprintf (stderr, "gtk-lib-probe: cairo image surface FAILED\n");
		if (surface) {
			cairo_surface_destroy (surface);
		}
		return 1;
	}

	cairo_t* cr = cairo_create (surface);
	if (!cr || cairo_status (cr) != CAIRO_STATUS_SUCCESS) {
		std::fprintf (stderr, "gtk-lib-probe: cairo_create FAILED\n");
		cairo_surface_destroy (surface);
		return 1;
	}

	cairo_set_source_rgb (cr, 0.12, 0.35, 0.55);
	cairo_paint (cr);
	cairo_destroy (cr);
	cairo_surface_destroy (surface);

	/* Touch a GTK type id without gtk_init / windows — link-only smoke. */
	const GType widget_type = gtk_widget_get_type ();
	if (widget_type == G_TYPE_INVALID) {
		std::fprintf (stderr, "gtk-lib-probe: gtk_widget_get_type FAILED\n");
		return 1;
	}

	std::printf ("  cairo image surface: ok (64x64 ARGB32)\n");
	std::printf ("  gtk_widget_get_type:  ok (GType=%llu)\n",
	             (unsigned long long)widget_type);
	std::printf ("PASS\n");
	return 0;
}
