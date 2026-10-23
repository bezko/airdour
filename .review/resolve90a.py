from pathlib import Path

def resolve_file(path, replacements):
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    for old, new in replacements:
        if old not in text:
            print(f"MISSING in {path}:\n---\n{old[:200]}\n---")
            return False
        text = text.replace(old, new, 1)
    p.write_bytes(text.encode("utf-8"))
    print(f"OK {path}")
    return True

# engine_host.h: union both field groups
resolve_file("win_ardour/engine_host.h", [(
"""<<<<<<< HEAD
	/** Pan AutoState: 0=Off 1=Play 2=Write 3=Touch (Latch unused in shell). */
	int          track_pan_auto_state[kMeters];
=======
	float        track_pan_width[kMeters]; /* stereo width −1..+1 (1=full) */
	bool         track_pan_width_ok[kMeters]; /* pan_width_control present */
>>>>>>> 6426f264be (Expose strip stereo pan width (Shift+drag pan).)""",
"""	/** Pan AutoState: 0=Off 1=Play 2=Write 3=Touch (Latch unused in shell). */
	int          track_pan_auto_state[kMeters];
	float        track_pan_width[kMeters]; /* stereo width −1..+1 (1=full) */
	bool         track_pan_width_ok[kMeters]; /* pan_width_control present */"""
)])

# engine_host.cc: union both inits
resolve_file("win_ardour/engine_host.cc", [(
"""<<<<<<< HEAD
		_snap.track_pan_auto_state[i] = 0;
=======
		_snap.track_pan_width[i] = 1.f;
		_snap.track_pan_width_ok[i] = false;
>>>>>>> 6426f264be (Expose strip stereo pan width (Shift+drag pan).)""",
"""		_snap.track_pan_auto_state[i] = 0;
		_snap.track_pan_width[i] = 1.f;
		_snap.track_pan_width_ok[i] = false;"""
)])

# mixer_strip.cc: keep HEAD trim + narrow structure, use theirs pan comment
resolve_file("win_ardour/mixer_strip.cc", [(
"""<<<<<<< HEAD
		/* Trim track — −20 · 0 · +20 dB (knob at unity when centered). */
		rt->FillRectangle (_trim_track, edge);
		{
			float tw = _trim_track.right - _trim_track.left;
			float kn_cx = _trim_track.left + tw * _trim;
			float kn_w = 7.f * dpi_scale;
			D2D1_RECT_F kn = D2D1::RectF (
				kn_cx - kn_w * 0.5f, _trim_track.top,
				kn_cx + kn_w * 0.5f, _trim_track.bottom);
			rt->FillRectangle (kn, accent);
			float mid_x = (_trim_track.left + _trim_track.right) * 0.5f;
			rt->DrawLine (
				D2D1::Point2F (mid_x, _trim_track.top),
				D2D1::Point2F (mid_x, _trim_track.bottom),
				body, 1.f);
		}

		if (!_narrow) {
			/* Pan azimuth track — L · C · R */
=======
		/* Pan azimuth track — L · C · R; stereo width band when available. */
>>>>>>> 6426f264be (Expose strip stereo pan width (Shift+drag pan).)""",
"""		/* Trim track — −20 · 0 · +20 dB (knob at unity when centered). */
		rt->FillRectangle (_trim_track, edge);
		{
			float tw = _trim_track.right - _trim_track.left;
			float kn_cx = _trim_track.left + tw * _trim;
			float kn_w = 7.f * dpi_scale;
			D2D1_RECT_F kn = D2D1::RectF (
				kn_cx - kn_w * 0.5f, _trim_track.top,
				kn_cx + kn_w * 0.5f, _trim_track.bottom);
			rt->FillRectangle (kn, accent);
			float mid_x = (_trim_track.left + _trim_track.right) * 0.5f;
			rt->DrawLine (
				D2D1::Point2F (mid_x, _trim_track.top),
				D2D1::Point2F (mid_x, _trim_track.bottom),
				body, 1.f);
		}

		if (!_narrow) {
			/* Pan azimuth track — L · C · R; stereo width band when available. */"""
)])
