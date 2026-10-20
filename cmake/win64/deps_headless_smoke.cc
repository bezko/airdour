/* Headless deps smoke: prove MSVC + vcpkg C stack (glib, xml, sndfile, curl, archive, pthread).
 * Does not pull glibmm/sigc++ — those need a 2.4/2.0 overlay before libpbd.
 */
#include <cstdio>
#include <string>

#include <archive.h>
#include <curl/curl.h>
#include <glib.h>
#include <libxml/parser.h>
#include <pthread.h>
#include <sndfile.h>

int main()
{
	const char* glib_ver = glib_check_version (2, 0, 0);
	std::printf ("airdour deps_headless_smoke\n");
	std::printf ("  glib:     %u.%u.%u%s\n",
	             glib_major_version, glib_minor_version, glib_micro_version,
	             glib_ver ? " (version check reported mismatch)" : "");
	std::printf ("  libxml:   %s\n", LIBXML_DOTTED_VERSION);
	std::printf ("  sndfile:  %s\n", sf_version_string ());
	std::printf ("  curl:     %s\n", curl_version ());
	std::printf ("  libarchive: %s\n", archive_version_string ());

	pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
	pthread_mutex_lock (&mu);
	pthread_mutex_unlock (&mu);
	std::printf ("  pthread:  ok\n");
	std::printf ("PASS\n");
	return 0;
}
