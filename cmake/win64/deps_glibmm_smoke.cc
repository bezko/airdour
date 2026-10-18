/* glibmm-2.4 + sigc++-2.0 smoke against vcpkg overlay ports (glibmm24 / libsigcpp2).
 * Proves MSVC can include/link the stack libpbd needs — not a libpbd substitute.
 */
#include <cstdio>

#include <glibmm.h>
#include <sigc++/sigc++.h>

static int on_idle()
{
	return 0;
}

int main()
{
	Glib::init();
	Glib::ustring hello("glibmm-2.4");
	sigc::slot<int> idle = sigc::ptr_fun(&on_idle);
	(void)idle;

	std::printf("airdour deps_glibmm_smoke\n");
	std::printf("  glibmm:  %s\n", hello.c_str());
	std::printf("  sigc++:  ok\n");
	std::printf("PASS\n");
	return 0;
}
