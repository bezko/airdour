from pathlib import Path

p = Path("win_ardour/shell_window.cc")
text = p.read_text(encoding="utf-8")

# Conflict 1: smoke dispatch - insert pan-width smoke using has_smoke style,
# keep strip-trim block from HEAD
old1 = """<<<<<<< HEAD
	if (ok && has_smoke ("strip-trim") && engine_session_ok ()) {
		if (!_engine.smoke_strip_trim ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_trim_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("strip-trim")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-strip-trim FAILED (no session)\\n");
=======
	if (ok && smoke_pan_width && _engine.snapshot ().session_ok) {
		if (!_engine.smoke_pan_width ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_pan_from_engine ();
		}
		std::fflush (stderr);
	} else if (smoke_pan_width) {
		std::fprintf (stderr, "airdour_win_shell: smoke-pan-width FAILED (no session)\\n");
>>>>>>> 6426f264be (Expose strip stereo pan width (Shift+drag pan).)"""

new1 = """	if (ok && has_smoke ("pan-width") && engine_session_ok ()) {
		if (!_engine.smoke_pan_width ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_pan_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("pan-width")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-pan-width FAILED (no session)\\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("strip-trim") && engine_session_ok ()) {
		if (!_engine.smoke_strip_trim ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_trim_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("strip-trim")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-strip-trim FAILED (no session)\\n")"""

if old1 not in text:
    print("MISSING conflict 1")
else:
    text = text.replace(old1, new1, 1)
    print("OK conflict 1")

# Conflict 2: prefer engine_session_ok style
old2 = """<<<<<<< HEAD
	if (ok && smoke_send && engine_session_ok ()) {
=======
	if (ok && smoke_send && _engine.snapshot ().session_ok) {
>>>>>>> 6426f264be (Expose strip stereo pan width (Shift+drag pan).)"""
new2 = """	if (ok && smoke_send && engine_session_ok ()) {"""
if old2 not in text:
    print("MISSING conflict 2")
else:
    text = text.replace(old2, new2, 1)
    print("OK conflict 2")

# Conflict 3: combine Alt+click reset with Shift+drag width
old3 = """<<<<<<< HEAD
		const bool alt = (GetKeyState (VK_MENU) & 0x8000) != 0;
		if (alt && _engine.is_linked () && _engine.snapshot ().session_ok) {
			/* Alt+click pan → center every audio strip. */
			_engine.reset_all_pans ();
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_pan_from_engine ();
			InvalidateRect (_hwnd, nullptr, FALSE);
			return true;
		}
		_drag = DragMode::Pan;
		_drag_index = pan_i;
		if (_mixers[pan_i].drag_pan_to (x)) {
			_engine.set_track_pan (pan_i, _mixers[pan_i].pan ());
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_pan_from_engine ();
=======
		const bool shift = (GetKeyState (VK_SHIFT) & 0x8000) != 0;
		if (shift && _mixers[pan_i].pan_width_ok ()) {
			/* Shift+drag pan → stereo width (−1..+1 via 0..1 iface). */
			_drag = DragMode::PanWidth;
			_drag_index = pan_i;
			if (_mixers[pan_i].drag_pan_width_to (x)) {
				_engine.set_track_pan_width (pan_i, _mixers[pan_i].pan_width ());
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_pan_from_engine ();
			}
		} else {
			_drag = DragMode::Pan;
			_drag_index = pan_i;
			if (_mixers[pan_i].drag_pan_to (x)) {
				_engine.set_track_pan (pan_i, _mixers[pan_i].pan ());
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_pan_from_engine ();
			}
>>>>>>> 6426f264be (Expose strip stereo pan width (Shift+drag pan).)"""

new3 = """		const bool alt = (GetKeyState (VK_MENU) & 0x8000) != 0;
		const bool shift = (GetKeyState (VK_SHIFT) & 0x8000) != 0;
		if (alt && _engine.is_linked () && _engine.snapshot ().session_ok) {
			/* Alt+click pan → center every audio strip. */
			_engine.reset_all_pans ();
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_pan_from_engine ();
			InvalidateRect (_hwnd, nullptr, FALSE);
			return true;
		}
		if (shift && _mixers[pan_i].pan_width_ok ()) {
			/* Shift+drag pan → stereo width (−1..+1 via 0..1 iface). */
			_drag = DragMode::PanWidth;
			_drag_index = pan_i;
			if (_mixers[pan_i].drag_pan_width_to (x)) {
				_engine.set_track_pan_width (pan_i, _mixers[pan_i].pan_width ());
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_pan_from_engine ();
			}
		} else {
			_drag = DragMode::Pan;
			_drag_index = pan_i;
			if (_mixers[pan_i].drag_pan_to (x)) {
				_engine.set_track_pan (pan_i, _mixers[pan_i].pan ());
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_pan_from_engine ();
			}"""

if old3 not in text:
    print("MISSING conflict 3")
else:
    text = text.replace(old3, new3, 1)
    print("OK conflict 3")

p.write_bytes(text.encode("utf-8"))

# verify
import re
left = len(re.findall(r"^<<<<<<< |^>>>>>>> ", text, re.M))
print(f"remaining markers: {left}")
