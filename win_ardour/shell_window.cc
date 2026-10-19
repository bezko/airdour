/*
 * Copyright (C) 2026 Airdour contributors
 */

#include "shell_window.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <commdlg.h>

namespace {

constexpr wchar_t kClassName[] = L"AirdourWinShell";
constexpr wchar_t kWindowTitle[] = L"Airdour";

constexpr D2D1_COLOR_F kBgTop      = { 0.055f, 0.075f, 0.095f, 1.f };
constexpr D2D1_COLOR_F kBgBottom   = { 0.090f, 0.140f, 0.165f, 1.f };
constexpr D2D1_COLOR_F kBrand      = { 0.930f, 0.955f, 0.970f, 1.f };
constexpr D2D1_COLOR_F kBody       = { 0.620f, 0.720f, 0.760f, 1.f };
constexpr D2D1_COLOR_F kAccent     = { 0.180f, 0.820f, 0.720f, 1.f };
constexpr D2D1_COLOR_F kPaneFill   = { 0.070f, 0.100f, 0.120f, 0.92f };
constexpr D2D1_COLOR_F kPaneEdge   = { 0.160f, 0.220f, 0.250f, 1.f };
constexpr D2D1_COLOR_F kPeak       = { 0.220f, 0.920f, 0.820f, 1.f };
constexpr D2D1_COLOR_F kRms        = { 0.120f, 0.420f, 0.400f, 0.85f };
constexpr D2D1_COLOR_F kPlayhead   = { 0.980f, 0.780f, 0.220f, 1.f };
constexpr D2D1_COLOR_F kSelection  = { 0.350f, 0.780f, 0.920f, 1.f };
constexpr D2D1_COLOR_F kLoop       = { 0.920f, 0.620f, 0.220f, 1.f };
constexpr D2D1_COLOR_F kPunch      = { 0.880f, 0.320f, 0.280f, 1.f };
constexpr D2D1_COLOR_F kMarker     = { 0.780f, 0.860f, 0.420f, 1.f };
constexpr D2D1_COLOR_F kMidi       = { 0.720f, 0.480f, 0.920f, 1.f };
constexpr D2D1_COLOR_F kMeterHot   = { 0.95f, 0.35f, 0.22f, 1.f };

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

UINT
query_dpi_for_hwnd (HWND hwnd)
{
	HMODULE user32 = GetModuleHandleW (L"user32.dll");
	if (user32) {
		typedef UINT (WINAPI *GetDpiForWindowFn) (HWND);
		auto fn = (GetDpiForWindowFn)GetProcAddress (user32, "GetDpiForWindow");
		if (fn && hwnd) {
			UINT d = fn (hwnd);
			if (d > 0) {
				return d;
			}
		}
	}
	HDC dc = GetDC (nullptr);
	UINT d = dc ? (UINT)GetDeviceCaps (dc, LOGPIXELSX) : 96;
	if (dc) {
		ReleaseDC (nullptr, dc);
	}
	return d ? d : 96;
}

void
enable_dpi_awareness ()
{
	HMODULE user32 = LoadLibraryW (L"user32.dll");
	if (user32) {
		typedef BOOL (WINAPI *SetCtxFn) (HANDLE);
		auto set_ctx = (SetCtxFn)GetProcAddress (user32, "SetProcessDpiAwarenessContext");
		if (set_ctx) {
			set_ctx ((HANDLE)(intptr_t)-4);
			FreeLibrary (user32);
			return;
		}
		FreeLibrary (user32);
	}
	HMODULE shcore = LoadLibraryW (L"Shcore.dll");
	if (shcore) {
		typedef HRESULT (WINAPI *SetAwareFn) (int);
		auto set_aware = (SetAwareFn)GetProcAddress (shcore, "SetProcessDpiAwareness");
		if (set_aware) {
			set_aware (2);
		}
		FreeLibrary (shcore);
	}
}

} // namespace

ShellWindow::ShellWindow ()
	: _hwnd (nullptr)
	, _width (1180)
	, _height (760)
	, _dpi (96)
	, _dpi_scale (1.f)
	, _running (false)
	, _start_tick (0)
	, _timeout_secs (0)
	, _drag (DragMode::None)
	, _drag_index (-1)
	, _focus_audio_track (0)
	, _pan_last_x (0.f)
	, _pan_down_x (0.f)
	, _pan_down_y (0.f)
	, _region_move_grab_offset_sec (0.f)
	, _midi_draw_start_sec (0.f)
	, _midi_draw_pitch (60)
	, _transport_playing (false)
	, _playhead_sec (0.f)
	, _last_tick_ms (0)
	, _smoke_record (SmokeRecordPhase::Off)
	, _smoke_record_start_ms (0)
	, _smoke_failed (false)
	, _brand_bar ()
	, _transport_bounds ()
	, _editor_pane ()
	, _editor_ruler ()
	, _midi_lane ()
	, _auto_lane_bounds ()
	, _mixer_pane ()
	, _status_bar ()
	, _d2d_factory (nullptr)
	, _dwrite_factory (nullptr)
	, _rt (nullptr)
	, _brush_brand (nullptr)
	, _brush_body (nullptr)
	, _brush_accent (nullptr)
	, _brush_pane (nullptr)
	, _brush_pane_edge (nullptr)
	, _brush_peak (nullptr)
	, _brush_rms (nullptr)
	, _brush_playhead (nullptr)
	, _brush_selection (nullptr)
	, _brush_loop (nullptr)
	, _brush_punch (nullptr)
	, _brush_marker (nullptr)
	, _brush_midi (nullptr)
	, _brush_meter_hot (nullptr)
	, _brush_bg (nullptr)
	, _fmt_brand (nullptr)
	, _fmt_body (nullptr)
	, _fmt_meta (nullptr)
	, _fmt_pane (nullptr)
	, _pulse (0.f)
	, _engine_snap ()
	, _auto_last_write_sec (-1.f)
{
	enable_dpi_awareness ();
	_engine_snap = _engine.snapshot ();

	for (int i = 0; i < kTracksMax; ++i) {
		wchar_t lab[24];
		_snwprintf_s (lab, _TRUNCATE, L"Audio %d", i + 1);
		wcsncpy_s (_lanes[i].label, lab, _TRUNCATE);
		_lanes[i].bounds = D2D1::RectF ();
		_mixers[i].set_label (lab);
		_mixers[i].set_gain (0.85f - 0.04f * (float)i);
		_mixers[i].set_midi_strip (false);
		_group_pick[i] = false;
	}
	for (int i = 0; i < kMixerStripsMax; ++i) {
		if (i >= kTracksMax) {
			_mixers[i].set_label (L"MIDI 1");
			_mixers[i].set_gain (0.85f);
			_mixers[i].set_midi_strip (true);
		}
	}
	/* Default MIDI strip after 3 audio (session default). */
	_mixers[3].set_label (L"MIDI 1");
	_mixers[3].set_gain (0.85f);
	_mixers[3].set_midi_strip (true);
}

ShellWindow::~ShellWindow ()
{
	_engine.stop ();
	discard_device_resources ();
	if (_fmt_pane) { _fmt_pane->Release (); _fmt_pane = nullptr; }
	if (_fmt_meta) { _fmt_meta->Release (); _fmt_meta = nullptr; }
	if (_fmt_body) { _fmt_body->Release (); _fmt_body = nullptr; }
	if (_fmt_brand) { _fmt_brand->Release (); _fmt_brand = nullptr; }
	if (_dwrite_factory) { _dwrite_factory->Release (); _dwrite_factory = nullptr; }
	if (_d2d_factory) { _d2d_factory->Release (); _d2d_factory = nullptr; }
	if (_hwnd) { DestroyWindow (_hwnd); _hwnd = nullptr; }
}

bool
ShellWindow::attach_engine (const std::string& backend_name,
                            unsigned smoke_timeout_secs,
                            bool smoke_import,
                            bool smoke_record,
                            bool smoke_save_load,
                            bool smoke_export,
                            bool smoke_undo,
                            bool smoke_loop,
                            bool smoke_midi,
                            bool smoke_midi_edit,
                            bool smoke_plugin,
                            bool smoke_perf,
                            const std::string& open_session_path,
                            unsigned period_frames,
                            bool smoke_vst3,
                            bool smoke_mute_solo,
                            bool smoke_pan,
                            bool smoke_send,
                            bool smoke_automation,
                            bool smoke_edit,
                            bool smoke_snap,
                            bool smoke_punch,
                            bool smoke_tempo,
                            bool smoke_click,
                            bool smoke_tracks,
                            bool smoke_rename,
                            bool smoke_group,
                            bool smoke_monitor,
                            bool smoke_locate,
                            bool smoke_zoom)
{
	if (!_engine.is_linked ()) {
		_engine.prepare (backend_name);
		_engine_snap = _engine.snapshot ();
		return false;
	}
	if (period_frames > 0) {
		_engine.set_period_frames (period_frames);
		std::fprintf (stderr, "airdour_win_shell: --period %u\n", period_frames);
		std::fflush (stderr);
	}
	bool ok = _engine.start (backend_name);
	/* Unattended --timeout: keep Session + PeakMeter (not stubs) but skip
	 * auto-roll — rolling under Debug has intermittent AVs on Dummy/WASAPI
	 * that prevent ExitProcess(0). Interactive runs still auto-play.
	 * --smoke-record / --smoke-perf drive their own short roll. */
	const bool autoplay = ok && (smoke_timeout_secs == 0) && !smoke_record && !smoke_perf;

	if (ok && !open_session_path.empty ()) {
		if (_engine.open_session (open_session_path)) {
			std::fprintf (stderr, "airdour_win_shell: opened --session path\n");
		} else {
			std::fprintf (stderr, "airdour_win_shell: --session open failed\n");
			ok = false;
		}
		std::fflush (stderr);
	}

	if (ok && smoke_import && _engine.snapshot ().session_ok) {
		if (_engine.import_test_tone (0, 220.f, 2.f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-import ok (track 0)\n");
		} else {
			std::fprintf (stderr, "airdour_win_shell: smoke-import failed\n");
		}
		std::fflush (stderr);
	}

	if (ok && smoke_save_load && _engine.snapshot ().session_ok) {
		if (!_engine.snapshot ().track_has_regions[0]) {
			_engine.import_test_tone (0, 330.f, 1.5f);
		}
		if (_engine.reload_session () && _engine.snapshot ().track_has_regions[0]) {
			std::fprintf (stderr, "airdour_win_shell: smoke-save-load ok (regions restored)\n");
		} else {
			std::fprintf (stderr, "airdour_win_shell: smoke-save-load FAILED (saved=%d regions0=%d)\n",
			              _engine.snapshot ().session_saved ? 1 : 0,
			              _engine.snapshot ().track_has_regions[0] ? 1 : 0);
		}
		std::fflush (stderr);
	}

	if (ok && smoke_export && _engine.snapshot ().session_ok) {
		std::string exported;
		if (_engine.smoke_export_wav (&exported)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-export ok → %s\n", exported.c_str ());
		} else {
			std::fprintf (stderr, "airdour_win_shell: smoke-export FAILED\n");
		}
		std::fflush (stderr);
	}

	if (ok && smoke_undo && _engine.snapshot ().session_ok) {
		if (_engine.smoke_undo ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-undo ok\n");
		} else {
			std::fprintf (stderr, "airdour_win_shell: smoke-undo FAILED\n");
		}
		std::fflush (stderr);
	}

	if (ok && smoke_loop && _engine.snapshot ().session_ok) {
		if (_engine.smoke_loop_markers ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-loop ok\n");
		} else {
			std::fprintf (stderr, "airdour_win_shell: smoke-loop FAILED\n");
		}
		std::fflush (stderr);
	}

	if (ok && smoke_midi_edit && _engine.snapshot ().session_ok) {
		if (_engine.smoke_midi_edit ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-edit ok\n");
		} else {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-edit FAILED\n");
		}
		std::fflush (stderr);
	} else if (ok && smoke_midi && _engine.snapshot ().session_ok) {
		if (_engine.smoke_midi ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi ok\n");
		} else {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi FAILED\n");
		}
		std::fflush (stderr);
	} else if (ok && _engine.snapshot ().session_ok) {
		/* Always scaffold a MIDI track + empty region so the piano-roll is live. */
		_engine.ensure_midi_track ();
		_engine.ensure_midi_region ();
	}

	if (ok && smoke_plugin && _engine.snapshot ().session_ok) {
		if (_engine.smoke_plugin_insert ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-plugin ok\n");
		} else {
			std::fprintf (stderr, "airdour_win_shell: smoke-plugin FAILED\n");
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_plugin) {
		std::fprintf (stderr, "airdour_win_shell: smoke-plugin FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_vst3 && _engine.snapshot ().session_ok) {
		if (!_engine.smoke_vst3 ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_vst3) {
		std::fprintf (stderr, "airdour_win_shell: smoke-vst3 FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_mute_solo && _engine.snapshot ().session_ok) {
		if (!_engine.smoke_mute_solo ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_mute_solo_from_engine ();
		}
		std::fflush (stderr);
	} else if (smoke_mute_solo) {
		std::fprintf (stderr, "airdour_win_shell: smoke-mute-solo FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_pan && _engine.snapshot ().session_ok) {
		if (!_engine.smoke_pan ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_pan_from_engine ();
		}
		std::fflush (stderr);
	} else if (smoke_pan) {
		std::fprintf (stderr, "airdour_win_shell: smoke-pan FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_send && _engine.snapshot ().session_ok) {
		if (!_engine.smoke_send ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_send_from_engine ();
		}
		std::fflush (stderr);
	} else if (smoke_send) {
		std::fprintf (stderr, "airdour_win_shell: smoke-send FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_automation && _engine.snapshot ().session_ok) {
		if (!_engine.smoke_automation ()) {
			_smoke_failed = true;
		} else {
			refresh_automation_lane ();
		}
		std::fflush (stderr);
	} else if (smoke_automation) {
		std::fprintf (stderr, "airdour_win_shell: smoke-automation FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_edit && _engine.snapshot ().session_ok) {
		if (!_engine.smoke_edit ()) {
			_smoke_failed = true;
		} else {
			apply_region_selection_to_lanes ();
		}
		std::fflush (stderr);
	} else if (smoke_edit) {
		std::fprintf (stderr, "airdour_win_shell: smoke-edit FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_snap && _engine.snapshot ().session_ok) {
		if (!_engine.smoke_snap ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_snap) {
		std::fprintf (stderr, "airdour_win_shell: smoke-snap FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_punch && _engine.snapshot ().session_ok) {
		if (!_engine.smoke_punch ()) {
			_smoke_failed = true;
		} else {
			sync_transport_punch_from_engine ();
		}
		std::fflush (stderr);
	} else if (smoke_punch) {
		std::fprintf (stderr, "airdour_win_shell: smoke-punch FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_tempo && _engine.snapshot ().session_ok) {
		if (!_engine.smoke_tempo ()) {
			_smoke_failed = true;
		} else {
			sync_transport_tempo_from_engine ();
		}
		std::fflush (stderr);
	} else if (smoke_tempo) {
		std::fprintf (stderr, "airdour_win_shell: smoke-tempo FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_click && _engine.snapshot ().session_ok) {
		if (!_engine.smoke_click ()) {
			_smoke_failed = true;
		} else {
			sync_transport_tempo_from_engine ();
		}
		std::fflush (stderr);
	} else if (smoke_click) {
		std::fprintf (stderr, "airdour_win_shell: smoke-click FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_tracks && _engine.snapshot ().session_ok) {
		if (!_engine.smoke_tracks ()) {
			_smoke_failed = true;
		} else {
			sync_track_labels_from_engine ();
			layout_panes ();
		}
		std::fflush (stderr);
	} else if (smoke_tracks) {
		std::fprintf (stderr, "airdour_win_shell: smoke-tracks FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_rename && _engine.snapshot ().session_ok) {
		if (!_engine.smoke_rename ()) {
			_smoke_failed = true;
		} else {
			sync_track_labels_from_engine ();
			sync_mixer_gains_from_engine ();
			layout_panes ();
		}
		std::fflush (stderr);
	} else if (smoke_rename) {
		std::fprintf (stderr, "airdour_win_shell: smoke-rename FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_group && _engine.snapshot ().session_ok) {
		if (!_engine.smoke_group ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_group_from_engine ();
			sync_mixer_gains_from_engine ();
			sync_mixer_mute_solo_from_engine ();
			layout_panes ();
		}
		std::fflush (stderr);
	} else if (smoke_group) {
		std::fprintf (stderr, "airdour_win_shell: smoke-group FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_monitor && _engine.snapshot ().session_ok) {
		if (!_engine.smoke_monitor ()) {
			_smoke_failed = true;
		} else {
			sync_monitor_from_engine ();
			layout_panes ();
		}
		std::fflush (stderr);
	} else if (smoke_monitor) {
		std::fprintf (stderr, "airdour_win_shell: smoke-monitor FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_locate && _engine.snapshot ().session_ok) {
		if (!_engine.smoke_locate ()) {
			_smoke_failed = true;
		} else {
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			_playhead_sec = _engine_snap.transport_sec;
			sync_transport_clock_from_engine ();
		}
		std::fflush (stderr);
	} else if (smoke_locate) {
		std::fprintf (stderr, "airdour_win_shell: smoke-locate FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_zoom && _engine.snapshot ().session_ok) {
		if (!_engine.smoke_zoom ()) {
			_smoke_failed = true;
		} else {
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			apply_region_selection_to_lanes ();
			float before = _lanes[0].strip.view_span_sec ();
			do_fit_session ();
			float span_sess = _lanes[0].strip.view_span_sec ();
			do_fit_selection ();
			float span_sel = _lanes[0].strip.view_span_sec ();
			do_fit_all_regions ();
			float span_all = _lanes[0].strip.view_span_sec ();
			if (span_sess < 0.2f || span_sel < 0.05f || span_all < 0.2f
			    || span_sel > span_sess * 1.05f + 0.5f) {
				std::fprintf (stderr,
				              "airdour_win_shell: smoke-zoom FAILED (UI fit spans sess=%.2f sel=%.2f all=%.2f before=%.2f)\n",
				              span_sess, span_sel, span_all, before);
				_smoke_failed = true;
			} else {
				std::fprintf (stderr,
				              "airdour_win_shell: smoke-zoom UI fit ok (sess=%.2fs sel=%.2fs all=%.2fs)\n",
				              span_sess, span_sel, span_all);
			}
			layout_panes ();
		}
		std::fflush (stderr);
	} else if (smoke_zoom) {
		std::fprintf (stderr, "airdour_win_shell: smoke-zoom FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_perf) {
		unsigned secs = smoke_timeout_secs > 0 ? smoke_timeout_secs : 4u;
		if (secs > 12) {
			secs = 12;
		}
		if (!_engine.smoke_perf (secs)) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	}

	if (ok && smoke_record && _engine.snapshot ().session_ok) {
		_smoke_record = SmokeRecordPhase::NeedArm;
		std::fprintf (stderr, "airdour_win_shell: smoke-record scheduled (arm → ~1.2s take)\n");
		std::fflush (stderr);
	}
	if (autoplay) {
		_engine.transport_play ();
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_mixer_arm_from_engine ();
	sync_mixer_fx_from_engine ();
	sync_transport_loop_from_engine ();
	sync_transport_punch_from_engine ();
	sync_transport_tempo_from_engine ();
	sync_track_labels_from_engine ();
	sync_mixer_gains_from_engine ();
	sync_mixer_group_from_engine ();
	if (_engine.is_linked () && _engine_snap.session_ok) {
		_engine.ensure_monitor_section ();
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	sync_monitor_from_engine ();
	layout_panes ();
	apply_loop_to_lanes ();
	apply_region_selection_to_lanes ();
	if (autoplay) {
		_transport_playing = true;
		_transport.set_playing (true);
		if (_engine_snap.session_ok) {
			_playhead_sec = _engine_snap.transport_sec;
		}
	}
	return ok;
}

void
ShellWindow::sync_transport_loop_from_engine ()
{
	_transport.set_loop_active (_engine_snap.loop_active);
	_transport.set_loop_playing (_engine_snap.loop_playing);
}

void
ShellWindow::sync_transport_punch_from_engine ()
{
	_transport.set_punch_enabled (_engine_snap.punch_enabled);
}

void
ShellWindow::sync_transport_tempo_from_engine ()
{
	_transport.set_tempo_bpm (_engine_snap.tempo_bpm);
	_transport.set_count_in_enabled (_engine_snap.count_in_enabled);
	_transport.set_click_enabled (_engine_snap.click_enabled);
}

void
ShellWindow::sync_transport_clock_from_engine ()
{
	_transport.set_clock_bbt (_engine_snap.bbt_bars, _engine_snap.bbt_beats, _engine_snap.bbt_ticks);
	_transport.set_clock_timecode (_engine_snap.tc_hours, _engine_snap.tc_minutes,
	                               _engine_snap.tc_seconds, _engine_snap.tc_frames);
	float sn = 0.f;
	if (std::fabs (_engine_snap.transport_speed) > 0.05f) {
		/* Map ±8 → ±1 for thumb. */
		sn = _engine_snap.transport_speed / 8.f;
		if (sn > 1.f) {
			sn = 1.f;
		}
		if (sn < -1.f) {
			sn = -1.f;
		}
	}
	if (_drag != DragMode::Shuttle) {
		_transport.set_shuttle_norm (sn);
	}
}

int
ShellWindow::n_audio_lanes () const
{
	int n = _engine_snap.audio_track_count;
	if (!_engine_snap.session_ok || n < 1) {
		n = 3;
	}
	if (n > kTracksMax) {
		n = kTracksMax;
	}
	return n;
}

int
ShellWindow::n_mixer_strips () const
{
	int n = n_audio_lanes ();
	if (_engine_snap.midi_track_ok || _engine_snap.midi_track_count > 0 || !_engine_snap.session_ok) {
		++n;
	}
	if (n > kMixerStripsMax) {
		n = kMixerStripsMax;
	}
	return n;
}

int
ShellWindow::midi_mixer_index () const
{
	return n_audio_lanes ();
}

void
ShellWindow::sync_track_labels_from_engine ()
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < kTracksMax; ++i) {
		wchar_t lab[32];
		if (i < na && _engine_snap.track_name[i][0] != L'\0') {
			wcsncpy_s (lab, _engine_snap.track_name[i], _TRUNCATE);
		} else {
			_snwprintf_s (lab, _TRUNCATE, L"Audio %d", i + 1);
		}
		wcsncpy_s (_lanes[i].label, lab, _TRUNCATE);
		_mixers[i].set_label (lab);
		_mixers[i].set_midi_strip (false);
	}
	const int mi = midi_mixer_index ();
	if (mi >= 0 && mi < kMixerStripsMax) {
		if (_engine_snap.midi_track_name[0] != L'\0') {
			_mixers[mi].set_label (_engine_snap.midi_track_name);
		} else {
			_mixers[mi].set_label (L"MIDI 1");
		}
		_mixers[mi].set_midi_strip (true);
	}
	if (na > 0 && _engine_snap.track_name[0][0] != L'\0') {
		wchar_t auto_lab[48];
		_snwprintf_s (auto_lab, _TRUNCATE, L"Gain · %s", _engine_snap.track_name[0]);
		_auto_lane.set_label (auto_lab);
	} else {
		_auto_lane.set_label (L"Gain · Audio 1");
	}
}

void
ShellWindow::sync_mixer_gains_from_engine ()
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < na && i < EngineSnapshot::kMeters; ++i) {
		_mixers[i].set_gain (_engine_snap.track_gain[i]);
	}
}

void
ShellWindow::sync_mixer_group_from_engine ()
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < kTracksMax; ++i) {
		bool in = (i < na && i < EngineSnapshot::kMeters && _engine_snap.track_in_group[i]);
		_mixers[i].set_in_group (in);
		_mixers[i].set_group_picked (_group_pick[i]);
	}
	_transport.set_group_active (_engine_snap.group_member_count >= 2
	                             && _engine_snap.group_active);
}

void
ShellWindow::sync_mixer_arm_from_engine ()
{
	const int na = n_audio_lanes ();
	const int mi = midi_mixer_index ();
	for (int i = 0; i < kMixerStripsMax; ++i) {
		_mixers[i].set_armed (false);
	}
	for (int i = 0; i < na && i < EngineSnapshot::kMeters; ++i) {
		_mixers[i].set_armed (_engine_snap.track_armed[i]);
	}
	if (mi >= 0 && mi < kMixerStripsMax) {
		_mixers[mi].set_armed (false);
	}
	_transport.set_record_armed (_engine_snap.session_record_armed);
}

void
ShellWindow::sync_mixer_mute_solo_from_engine ()
{
	const int na = n_audio_lanes ();
	const int mi = midi_mixer_index ();
	for (int i = 0; i < na && i < EngineSnapshot::kMeters; ++i) {
		_mixers[i].set_muted (_engine_snap.track_muted[i]);
		_mixers[i].set_soloed (_engine_snap.track_soloed[i]);
	}
	if (mi >= 0 && mi < kMixerStripsMax) {
		_mixers[mi].set_muted (_engine_snap.midi_muted);
		_mixers[mi].set_soloed (_engine_snap.midi_soloed);
	}
}

void
ShellWindow::sync_mixer_pan_from_engine ()
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < na && i < EngineSnapshot::kMeters; ++i) {
		_mixers[i].set_pan (_engine_snap.track_pan[i]);
	}
	_transport.set_recording (_engine_snap.session_recording);
}

void
ShellWindow::sync_mixer_send_from_engine ()
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < na && i < EngineSnapshot::kMeters; ++i) {
		_mixers[i].set_send (_engine_snap.track_send[i]);
	}
}

void
ShellWindow::refresh_automation_lane ()
{
	if (_engine_snap.track_name[0][0] != L'\0') {
		wchar_t auto_lab[48];
		_snwprintf_s (auto_lab, _TRUNCATE, L"Gain · %s", _engine_snap.track_name[0]);
		_auto_lane.set_label (auto_lab);
	} else {
		_auto_lane.set_label (L"Gain · Audio 1");
	}
	float view0 = _lanes[0].strip.view_start_sec ();
	float span = std::max (0.001f, _lanes[0].strip.view_span_sec ());
	float curve[AutomationLane::kMaxColumns];
	size_t n = AutomationLane::kMaxColumns;
	if (_engine.is_linked () && _engine_snap.session_ok) {
		_engine.fill_gain_automation_curve (0, view0, span, curve, n);
	} else {
		for (size_t i = 0; i < n; ++i) {
			curve[i] = _mixers[0].gain ();
		}
	}
	_auto_lane.set_curve (curve, n);

	float px[AutomationLane::kMaxPoints];
	float py[AutomationLane::kMaxPoints];
	int npt = 0;
	if (_engine.is_linked () && _engine_snap.session_ok) {
		npt = _engine.fill_gain_automation_points (
			0, view0, span, px, py, (int)AutomationLane::kMaxPoints);
	}
	_auto_lane.set_points (px, py, (size_t)std::max (0, npt));
	_auto_lane.set_playhead_norm (_lanes[0].strip.playhead_norm ());
}

void
ShellWindow::write_automation_at (float x, float y)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	float time_sec = _lanes[0].strip.time_at_x (_auto_lane_bounds, x);
	float value = _auto_lane.value_at_y (_auto_lane_bounds, y);
	/* Thin writes while dragging — skip near-duplicate times. */
	if (_drag == DragMode::AutoWrite && _auto_last_write_sec >= 0.f
	    && std::fabs (time_sec - _auto_last_write_sec) < 0.035f) {
		return;
	}
	if (_engine.write_track_gain_automation (0, time_sec, value)) {
		_auto_last_write_sec = time_sec;
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		refresh_automation_lane ();
	}
}

void
ShellWindow::draw_automation_lane ()
{
	if (_auto_lane_bounds.right <= _auto_lane_bounds.left) {
		return;
	}
	_auto_lane.draw (_rt, _auto_lane_bounds, _brush_accent, _brush_body,
	                 _brush_pane_edge, _brush_playhead, _fmt_meta, _dpi_scale);
}

void
ShellWindow::sync_mixer_fx_from_engine ()
{
	for (int i = 0; i < kTracks; ++i) {
		if (i == 0 && _engine_snap.track_fx_ok && !_engine_snap.track_fx_name.empty ()) {
			_mixers[i].set_fx_label (_engine_snap.track_fx_name.c_str ());
			_mixers[i].set_fx_bypassed (_engine_snap.track_fx_bypassed);
			_mixers[i].set_fx_param (_engine_snap.track_fx_param);
		} else {
			_mixers[i].set_fx_label (nullptr);
			_mixers[i].set_fx_bypassed (false);
			_mixers[i].set_fx_param (0.3f);
		}
	}
}

void
ShellWindow::transport_toggle_record ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (!_engine.start ()) {
		_engine_snap = _engine.snapshot ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	}
	_engine.toggle_session_record ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_mixer_arm_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::prompt_export_wav ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (!_engine.start ()) {
		_engine_snap = _engine.snapshot ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	}

	wchar_t path[MAX_PATH] = L"airdour_export.wav";
	OPENFILENAMEW ofn = {};
	ofn.lStructSize = sizeof (ofn);
	ofn.hwndOwner = _hwnd;
	ofn.lpstrFilter = L"WAV audio (*.wav)\0*.wav\0All files (*.*)\0*.*\0";
	ofn.lpstrFile = path;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
	ofn.lpstrTitle = L"Airdour — export WAV";
	ofn.lpstrDefExt = L"wav";
	if (!GetSaveFileNameW (&ofn)) {
		return;
	}

	char utf8[MAX_PATH * 4];
	WideCharToMultiByte (CP_UTF8, 0, path, -1, utf8, sizeof (utf8), nullptr, nullptr);
	if (_engine.export_session_wav (utf8)) {
		std::fprintf (stderr, "airdour_win_shell: export dialog ok\n");
	} else {
		std::fprintf (stderr, "airdour_win_shell: export dialog failed\n");
	}
	std::fflush (stderr);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_save_session ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (!_engine.start ()) {
		_engine_snap = _engine.snapshot ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	}
	_engine.save_session ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::prompt_open_session ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (!_engine.start ()) {
		_engine_snap = _engine.snapshot ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	}

	wchar_t path[MAX_PATH] = {};
	OPENFILENAMEW ofn = {};
	ofn.lStructSize = sizeof (ofn);
	ofn.hwndOwner = _hwnd;
	ofn.lpstrFilter = L"Airdour session (*.ardour)\0*.ardour\0All files (*.*)\0*.*\0";
	ofn.lpstrFile = path;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
	ofn.lpstrTitle = L"Airdour — open session";
	if (!GetOpenFileNameW (&ofn)) {
		return;
	}

	/* path = ...\session_dir\snapshot.ardour → dir + snapshot stem */
	wchar_t* slash = wcsrchr (path, L'\\');
	if (!slash) {
		slash = wcsrchr (path, L'/');
	}
	if (!slash) {
		return;
	}
	*slash = L'\0';
	const wchar_t* file = slash + 1;
	std::wstring snap_w (file);
	size_t dot = snap_w.find_last_of (L'.');
	if (dot != std::wstring::npos) {
		snap_w = snap_w.substr (0, dot);
	}

	char dir_utf8[MAX_PATH * 4];
	char snap_utf8[MAX_PATH];
	WideCharToMultiByte (CP_UTF8, 0, path, -1, dir_utf8, sizeof (dir_utf8), nullptr, nullptr);
	WideCharToMultiByte (CP_UTF8, 0, snap_w.c_str (), -1, snap_utf8, sizeof (snap_utf8), nullptr, nullptr);

	if (_engine.open_session (dir_utf8, snap_utf8)) {
		std::fprintf (stderr, "airdour_win_shell: open dialog ok\n");
	} else {
		std::fprintf (stderr, "airdour_win_shell: open dialog failed\n");
	}
	std::fflush (stderr);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_mixer_arm_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::prompt_import_audio ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (!_engine.start ()) {
		_engine_snap = _engine.snapshot ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	}

	wchar_t path[MAX_PATH] = {};
	OPENFILENAMEW ofn = {};
	ofn.lStructSize = sizeof (ofn);
	ofn.hwndOwner = _hwnd;
	ofn.lpstrFilter = L"WAV audio (*.wav)\0*.wav\0All files (*.*)\0*.*\0";
	ofn.lpstrFile = path;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
	ofn.lpstrTitle = L"Airdour — import audio";
	if (!GetOpenFileNameW (&ofn)) {
		return;
	}

	char utf8[MAX_PATH * 4];
	WideCharToMultiByte (CP_UTF8, 0, path, -1, utf8, sizeof (utf8), nullptr, nullptr);
	if (_engine.import_audio_file (0, utf8)) {
		std::fprintf (stderr, "airdour_win_shell: import dialog ok\n");
	} else {
		std::fprintf (stderr, "airdour_win_shell: import dialog failed\n");
	}
	std::fflush (stderr);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::tick_smoke_record (DWORD now)
{
	if (_smoke_record == SmokeRecordPhase::Off || _smoke_record == SmokeRecordPhase::Done) {
		return;
	}
	if (!_engine_snap.session_ok) {
		return;
	}

	if (_smoke_record == SmokeRecordPhase::NeedArm) {
		_engine.set_track_armed (0, true);
		_smoke_record_start_ms = now;
		_smoke_record = SmokeRecordPhase::ArmWait;
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		sync_mixer_arm_from_engine ();
		std::fprintf (stderr, "airdour_win_shell: smoke-record track0 arm queued\n");
		std::fflush (stderr);
		return;
	}

	if (_smoke_record == SmokeRecordPhase::ArmWait
	    && (now - _smoke_record_start_ms) >= 250u) {
		_engine.set_session_record (true);
		_engine.transport_play ();
		_transport_playing = true;
		_transport.set_playing (true);
		_smoke_record_start_ms = now;
		_smoke_record = SmokeRecordPhase::Rolling;
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		sync_mixer_arm_from_engine ();
		std::fprintf (stderr, "airdour_win_shell: smoke-record rolling (armed=%d session_rec=%d)\n",
		              _engine_snap.track_armed[0] ? 1 : 0,
		              _engine_snap.session_record_armed ? 1 : 0);
		std::fflush (stderr);
		return;
	}

	if (_smoke_record == SmokeRecordPhase::Rolling
	    && (now - _smoke_record_start_ms) >= 1200u) {
		transport_stop ();
		_smoke_record_start_ms = now;
		_smoke_record = SmokeRecordPhase::FinalizeWait;
		std::fprintf (stderr, "airdour_win_shell: smoke-record transport stopped — waiting finalize\n");
		std::fflush (stderr);
		return;
	}

	if (_smoke_record == SmokeRecordPhase::FinalizeWait
	    && (now - _smoke_record_start_ms) >= 400u) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		sync_mixer_arm_from_engine ();
		_smoke_record = SmokeRecordPhase::Done;
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-record done — track0 regions=%d\n",
		              _engine_snap.track_has_regions[0] ? 1 : 0);
		std::fflush (stderr);
		/* Leave early so ExitProcess is less likely to race Butler after a take. */
		PostQuitMessage (0);
	}
}

void
ShellWindow::transport_play ()
{
	if (_engine.is_linked ()) {
		if (!_engine.start ()) {
			_engine_snap = _engine.snapshot ();
			InvalidateRect (_hwnd, nullptr, FALSE);
			return;
		}
		_engine.transport_play ();
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		sync_mixer_arm_from_engine ();
	}
	_transport_playing = true;
	_transport.set_playing (true);
	_last_tick_ms = GetTickCount ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::transport_stop ()
{
	_transport_playing = false;
	_transport.set_playing (false);
	_playhead_sec = 0.f;
	if (_engine.is_linked ()) {
		_engine.transport_stop ();
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		sync_mixer_arm_from_engine ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::transport_toggle ()
{
	if (_transport_playing) {
		transport_stop ();
	} else {
		transport_play ();
	}
}

void
ShellWindow::apply_shared_view (int source_lane)
{
	if (source_lane < 0 || source_lane >= kTracks) {
		return;
	}
	for (int i = 0; i < kTracks; ++i) {
		if (i != source_lane) {
			_lanes[i].strip.sync_view_from (_lanes[source_lane].strip);
		}
	}
}

void
ShellWindow::reset_shared_view ()
{
	for (int i = 0; i < kTracks; ++i) {
		_lanes[i].strip.reset_view ();
	}
}

void
ShellWindow::fit_shared_range (float start_sec, float end_sec)
{
	_lanes[0].strip.fit_range (start_sec, end_sec);
	apply_shared_view (0);
	refresh_editor_view_after_nav ();
}

void
ShellWindow::do_fit_session ()
{
	float s = 0.f, e = 0.f;
	if (_engine.is_linked () && _engine.session_extent_sec (&s, &e) && e > s) {
		fit_shared_range (s, e);
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	}
	/* Fallback: default 0..8s window. */
	fit_shared_range (0.f, WaveStrip::kDefaultSpanSec);
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_fit_selection ()
{
	if (_engine_snap.has_selection
	    && _engine_snap.selection_end_sec > _engine_snap.selection_start_sec) {
		fit_shared_range (_engine_snap.selection_start_sec, _engine_snap.selection_end_sec);
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	}
	/* No selection → fit all regions (honest fallback). */
	do_fit_all_regions ();
}

void
ShellWindow::do_fit_all_regions ()
{
	float s = 0.f, e = 0.f;
	if (_engine.is_linked () && _engine.regions_extent_sec (&s, &e) && e > s) {
		fit_shared_range (s, e);
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	}
	do_fit_session ();
}

void
ShellWindow::do_scroll_editor (float delta_widths)
{
	if (delta_widths == 0.f) {
		return;
	}
	_lanes[0].strip.pan_by (delta_widths);
	apply_shared_view (0);
	refresh_editor_view_after_nav ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::refresh_editor_view_after_nav ()
{
	/* Keep snap grid + playhead overlay consistent with the new window. */
	if (_engine.is_linked () && _engine_snap.session_ok && _engine_snap.snap_enabled) {
		_engine.refresh_grid_lines (
			_lanes[0].strip.view_start_sec (),
			_lanes[0].strip.view_span_sec ());
		_engine_snap = _engine.snapshot ();
	}
}

void
ShellWindow::apply_region_selection_to_lanes ()
{
	for (int i = 0; i < kTracks; ++i) {
		bool active = _engine_snap.has_selection && _engine_snap.selection_track == i;
		_lanes[i].strip.set_selection (
			active,
			_engine_snap.selection_start_sec,
			_engine_snap.selection_end_sec,
			active ? _engine_snap.selection_fade_in_sec : 0.f,
			active ? _engine_snap.selection_fade_out_sec : 0.f);
	}
}

void
ShellWindow::apply_loop_to_lanes ()
{
	for (int i = 0; i < kTracks; ++i) {
		_lanes[i].strip.set_loop_range (
			_engine_snap.loop_active,
			_engine_snap.loop_start_sec,
			_engine_snap.loop_end_sec);
	}
}

void
ShellWindow::do_toggle_loop ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.toggle_loop_play ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_transport_loop_from_engine ();
	apply_loop_to_lanes ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_toggle_punch ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.toggle_punch_enabled ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_transport_punch_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_toggle_count_in ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.toggle_count_in ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_transport_tempo_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_toggle_click ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.toggle_clicking ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_transport_tempo_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_add_audio_track ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.add_audio_track ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_track_labels_from_engine ();
	sync_mixer_gains_from_engine ();
	sync_mixer_group_from_engine ();
	sync_mixer_arm_from_engine ();
	sync_mixer_mute_solo_from_engine ();
	sync_mixer_pan_from_engine ();
	sync_mixer_send_from_engine ();
	layout_panes ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_add_midi_track ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.add_midi_track ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_track_labels_from_engine ();
	sync_mixer_mute_solo_from_engine ();
	layout_panes ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_remove_track ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	/* Shift+−T removes MIDI when present; else selected/empty audio. */
	if ((GetKeyState (VK_SHIFT) & 0x8000)
	    && (_engine_snap.midi_track_ok || _engine_snap.midi_track_count > 0)) {
		_engine.remove_midi_track ();
	} else {
		_engine.remove_selected_or_empty_audio_track ();
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_track_labels_from_engine ();
	sync_mixer_gains_from_engine ();
	sync_mixer_arm_from_engine ();
	sync_mixer_mute_solo_from_engine ();
	sync_mixer_pan_from_engine ();
	sync_mixer_send_from_engine ();
	layout_panes ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_move_audio_track (int delta)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	int idx = _focus_audio_track;
	if (_engine_snap.has_selection && _engine_snap.selection_track >= 0
	    && _engine_snap.selection_track < n_audio_lanes ()) {
		idx = _engine_snap.selection_track;
	}
	if (idx < 0 || idx >= n_audio_lanes ()) {
		idx = 0;
	}
	if (!_engine.move_audio_track (idx, delta)) {
		_engine_snap = _engine.snapshot ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	_focus_audio_track = idx + delta;
	if (_focus_audio_track < 0) {
		_focus_audio_track = 0;
	}
	if (_focus_audio_track >= n_audio_lanes ()) {
		_focus_audio_track = n_audio_lanes () - 1;
	}
	sync_track_labels_from_engine ();
	sync_mixer_gains_from_engine ();
	sync_mixer_arm_from_engine ();
	sync_mixer_mute_solo_from_engine ();
	sync_mixer_pan_from_engine ();
	sync_mixer_send_from_engine ();
	apply_region_selection_to_lanes ();
	layout_panes ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

namespace {

struct RenameDlgCtx {
	HWND    edit;
	wchar_t name[32];
	bool    done;
	bool    ok;
};

LRESULT CALLBACK
rename_wnd_proc (HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	RenameDlgCtx* ctx = reinterpret_cast<RenameDlgCtx*> (
		GetWindowLongPtrW (hwnd, GWLP_USERDATA));
	switch (msg) {
	case WM_CREATE: {
		CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*> (lparam);
		ctx = reinterpret_cast<RenameDlgCtx*> (cs->lpCreateParams);
		SetWindowLongPtrW (hwnd, GWLP_USERDATA, (LONG_PTR)ctx);
		ctx->edit = CreateWindowExW (
			WS_EX_CLIENTEDGE, L"EDIT", ctx->name,
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
			12, 12, 260, 24, hwnd, (HMENU)1001, cs->hInstance, nullptr);
		CreateWindowW (L"BUTTON", L"OK",
		               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
		               80, 48, 72, 26, hwnd, (HMENU)IDOK, cs->hInstance, nullptr);
		CreateWindowW (L"BUTTON", L"Cancel",
		               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
		               164, 48, 72, 26, hwnd, (HMENU)IDCANCEL, cs->hInstance, nullptr);
		if (ctx->edit) {
			SendMessageW (ctx->edit, EM_SETSEL, 0, -1);
			SetFocus (ctx->edit);
		}
		return 0;
	}
	case WM_COMMAND:
		if (LOWORD (wparam) == IDOK && ctx) {
			GetWindowTextW (ctx->edit, ctx->name, 32);
			ctx->ok = (ctx->name[0] != L'\0');
			ctx->done = true;
			DestroyWindow (hwnd);
			return 0;
		}
		if (LOWORD (wparam) == IDCANCEL && ctx) {
			ctx->ok = false;
			ctx->done = true;
			DestroyWindow (hwnd);
			return 0;
		}
		break;
	case WM_CLOSE:
		if (ctx) {
			ctx->ok = false;
			ctx->done = true;
		}
		DestroyWindow (hwnd);
		return 0;
	default:
		break;
	}
	return DefWindowProcW (hwnd, msg, wparam, lparam);
}

bool
run_rename_dialog (HWND parent, wchar_t* name, size_t name_chars)
{
	if (!name || name_chars < 2) {
		return false;
	}
	static const wchar_t* kCls = L"AirdourRenameDlg";
	static bool registered = false;
	HINSTANCE inst = GetModuleHandleW (nullptr);
	if (!registered) {
		WNDCLASSEXW wc = {};
		wc.cbSize = sizeof (wc);
		wc.lpfnWndProc = rename_wnd_proc;
		wc.hInstance = inst;
		wc.hCursor = LoadCursorW (nullptr, IDC_ARROW);
		wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
		wc.lpszClassName = kCls;
		if (!RegisterClassExW (&wc) && GetLastError () != ERROR_CLASS_ALREADY_EXISTS) {
			return false;
		}
		registered = true;
	}

	RenameDlgCtx ctx = {};
	wcsncpy_s (ctx.name, name, _TRUNCATE);
	ctx.done = false;
	ctx.ok = false;

	RECT pr = {};
	if (parent) {
		GetWindowRect (parent, &pr);
	}
	int wx = pr.left + 80;
	int wy = pr.top + 120;
	HWND dlg = CreateWindowExW (
		WS_EX_DLGMODALFRAME, kCls, L"Rename track — Airdour",
		WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
		wx, wy, 300, 120, parent, nullptr, inst, &ctx);
	if (!dlg) {
		return false;
	}
	if (parent) {
		EnableWindow (parent, FALSE);
	}
	MSG msg;
	while (!ctx.done && GetMessageW (&msg, nullptr, 0, 0) > 0) {
		if (!IsDialogMessageW (dlg, &msg)) {
			TranslateMessage (&msg);
			DispatchMessageW (&msg);
		}
	}
	if (parent) {
		EnableWindow (parent, TRUE);
		SetForegroundWindow (parent);
	}
	if (!ctx.ok) {
		return false;
	}
	wcsncpy_s (name, name_chars, ctx.name, _TRUNCATE);
	return name[0] != L'\0';
}

} // namespace

void
ShellWindow::prompt_rename_track (int audio_index, bool midi)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	wchar_t name[32];
	if (midi) {
		if (_engine_snap.midi_track_name[0]) {
			wcsncpy_s (name, _engine_snap.midi_track_name, _TRUNCATE);
		} else {
			wcsncpy_s (name, L"MIDI 1", _TRUNCATE);
		}
	} else {
		if (audio_index < 0 || audio_index >= n_audio_lanes ()) {
			return;
		}
		if (_engine_snap.track_name[audio_index][0]) {
			wcsncpy_s (name, _engine_snap.track_name[audio_index], _TRUNCATE);
		} else {
			_snwprintf_s (name, _TRUNCATE, L"Audio %d", audio_index + 1);
		}
	}
	if (!run_rename_dialog (_hwnd, name, 32)) {
		return;
	}
	char utf8[128];
	WideCharToMultiByte (CP_UTF8, 0, name, -1, utf8, sizeof (utf8), nullptr, nullptr);
	bool ok = false;
	if (midi) {
		ok = _engine.set_midi_track_name (utf8);
	} else {
		ok = _engine.set_track_name (audio_index, utf8);
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_track_labels_from_engine ();
	if (!ok) {
		MessageBoxW (_hwnd, L"Could not rename track (empty name or rec-armed).",
		             L"Airdour", MB_OK | MB_ICONWARNING);
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_rename_focused_track ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	int idx = _focus_audio_track;
	if (_engine_snap.has_selection && _engine_snap.selection_track >= 0) {
		idx = _engine_snap.selection_track;
	}
	if (idx < 0 || idx >= n_audio_lanes ()) {
		idx = 0;
	}
	prompt_rename_track (idx, false);
}

void
ShellWindow::do_toggle_group_pick (int audio_index)
{
	if (audio_index < 0 || audio_index >= kTracksMax || audio_index >= n_audio_lanes ()) {
		return;
	}
	_group_pick[audio_index] = !_group_pick[audio_index];
	_focus_audio_track = audio_index;
	sync_mixer_group_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::sync_monitor_from_engine ()
{
	_monitor.set_available (_engine_snap.monitor_ok);
	_monitor.set_dim (_engine_snap.monitor_dim);
	_monitor.set_mono (_engine_snap.monitor_mono);
	_monitor.set_mute (_engine_snap.monitor_cut);
}

void
ShellWindow::do_toggle_monitor_dim ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.toggle_monitor_dim ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_monitor_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_toggle_monitor_mono ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.toggle_monitor_mono ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_monitor_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_toggle_monitor_cut ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.toggle_monitor_cut ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_monitor_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_apply_or_clear_group ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	/* Shift+Grp clears; else build from picks (≥2) or focus+neighbour. */
	if (GetKeyState (VK_SHIFT) & 0x8000) {
		_engine.clear_route_group ();
		for (int i = 0; i < kTracksMax; ++i) {
			_group_pick[i] = false;
		}
	} else {
		int idxs[EngineSnapshot::kMeters];
		int n = 0;
		for (int i = 0; i < n_audio_lanes () && i < EngineSnapshot::kMeters; ++i) {
			if (_group_pick[i]) {
				idxs[n++] = i;
			}
		}
		if (n < 2) {
			int a = _focus_audio_track;
			if (a < 0 || a >= n_audio_lanes ()) {
				a = 0;
			}
			int b = a + 1;
			if (b >= n_audio_lanes ()) {
				b = a - 1;
			}
			if (b < 0 || b == a) {
				_engine_snap = _engine.snapshot ();
				InvalidateRect (_hwnd, nullptr, FALSE);
				return;
			}
			idxs[0] = a;
			idxs[1] = b;
			n = 2;
		}
		_engine.create_route_group (idxs, n);
		for (int i = 0; i < kTracksMax; ++i) {
			_group_pick[i] = false;
		}
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_mixer_group_from_engine ();
	sync_mixer_gains_from_engine ();
	sync_mixer_mute_solo_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_nudge_tempo (float delta)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (GetKeyState (VK_SHIFT) & 0x8000) {
		delta *= 5.f;
	}
	_engine.nudge_tempo_bpm (delta);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_transport_tempo_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_add_marker_at_playhead ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.add_marker_at (_playhead_sec);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_remove_marker_at_playhead ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.remove_marker_near (_playhead_sec);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_undo ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.undo (1);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	apply_region_selection_to_lanes ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_redo ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.redo (1);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	apply_region_selection_to_lanes ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_delete_selection ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	/* Prefer MIDI note selection when present. */
	if (_engine_snap.midi_selected_note_id >= 0) {
		if (_engine.delete_selected_midi_note ()) {
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			InvalidateRect (_hwnd, nullptr, FALSE);
			return;
		}
	}
	if (!_engine.delete_selected_region ()) {
		_engine_snap = _engine.snapshot ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	apply_region_selection_to_lanes ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_split_at_playhead ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	float t = _playhead_sec;
	if (_engine_snap.transport_rolling) {
		t = _engine_snap.transport_sec;
	}
	int track = _engine_snap.has_selection ? _engine_snap.selection_track : 0;
	if (_engine.split_at (t, track)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_apply_crossfade ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.apply_selected_crossfade (0.08f)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_toggle_snap ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	_engine.toggle_snap ();
	_engine_snap = _engine.snapshot ();
	_transport.set_snap_enabled (_engine_snap.snap_enabled);
	if (_engine_snap.snap_enabled) {
		_engine.refresh_grid_lines (
			_lanes[0].strip.view_start_sec (),
			_lanes[0].strip.view_span_sec ());
		_engine_snap = _engine.snapshot ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_cycle_snap_density ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	_engine.cycle_snap_density ();
	if (!_engine.snap_enabled ()) {
		_engine.set_snap_enabled (true);
	}
	_engine.refresh_grid_lines (
		_lanes[0].strip.view_start_sec (),
		_lanes[0].strip.view_span_sec ());
	_engine_snap = _engine.snapshot ();
	_transport.set_snap_enabled (_engine_snap.snap_enabled);
	InvalidateRect (_hwnd, nullptr, FALSE);
}

float
ShellWindow::stub_meter_peak (int track, float dt) const
{
	(void)dt;
	if (!_transport_playing) {
		return 0.f;
	}
	float g = _mixers[track].gain ();
	/* Fallback only when Session PeakMeter is unavailable. */
	float t = _playhead_sec * (3.1f + track * 0.7f) + track * 0.4f;
	float pulse = 0.55f + 0.45f * std::sin (t);
	float beat = std::fmod (_playhead_sec * 2.f + track * 0.17f, 1.f);
	if (beat < 0.f) {
		beat += 1.f;
	}
	float hit = (beat < 0.08f) ? (1.f - beat / 0.08f) : 0.f;
	return std::min (1.f, g * (0.35f + 0.4f * pulse + 0.45f * hit));
}

bool
ShellWindow::init_d2d ()
{
	HRESULT hr = D2D1CreateFactory (D2D1_FACTORY_TYPE_SINGLE_THREADED, &_d2d_factory);
	if (FAILED (hr)) {
		return false;
	}
	hr = DWriteCreateFactory (DWRITE_FACTORY_TYPE_SHARED,
	                          __uuidof (IDWriteFactory),
	                          reinterpret_cast<IUnknown**> (&_dwrite_factory));
	if (FAILED (hr)) {
		return false;
	}

	auto make_fmt = [&] (const wchar_t* preferred, const wchar_t* fallback,
	                     DWRITE_FONT_WEIGHT weight, float size, IDWriteTextFormat** out) -> bool {
		HRESULT h = _dwrite_factory->CreateTextFormat (
			preferred, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
			DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", out);
		if (FAILED (h)) {
			h = _dwrite_factory->CreateTextFormat (
				fallback, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
				DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", out);
		}
		return SUCCEEDED (h);
	};

	if (!make_fmt (L"Segoe UI Variable Display", L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 48.f, &_fmt_brand)) {
		return false;
	}
	_fmt_brand->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_LEADING);
	_fmt_brand->SetParagraphAlignment (DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
	if (!make_fmt (L"Segoe UI Variable Text", L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, 16.f, &_fmt_body)) {
		return false;
	}
	if (!make_fmt (L"Cascadia Mono", L"Consolas", DWRITE_FONT_WEIGHT_NORMAL, 13.f, &_fmt_meta)) {
		return false;
	}
	if (!make_fmt (L"Segoe UI Variable Text", L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 14.f, &_fmt_pane)) {
		return false;
	}
	return true;
}

void
ShellWindow::discard_device_resources ()
{
	if (_brush_bg) { _brush_bg->Release (); _brush_bg = nullptr; }
	if (_brush_meter_hot) { _brush_meter_hot->Release (); _brush_meter_hot = nullptr; }
	if (_brush_midi) { _brush_midi->Release (); _brush_midi = nullptr; }
	if (_brush_marker) { _brush_marker->Release (); _brush_marker = nullptr; }
	if (_brush_punch) { _brush_punch->Release (); _brush_punch = nullptr; }
	if (_brush_loop) { _brush_loop->Release (); _brush_loop = nullptr; }
	if (_brush_selection) { _brush_selection->Release (); _brush_selection = nullptr; }
	if (_brush_playhead) { _brush_playhead->Release (); _brush_playhead = nullptr; }
	if (_brush_rms) { _brush_rms->Release (); _brush_rms = nullptr; }
	if (_brush_peak) { _brush_peak->Release (); _brush_peak = nullptr; }
	if (_brush_pane_edge) { _brush_pane_edge->Release (); _brush_pane_edge = nullptr; }
	if (_brush_pane) { _brush_pane->Release (); _brush_pane = nullptr; }
	if (_brush_accent) { _brush_accent->Release (); _brush_accent = nullptr; }
	if (_brush_body) { _brush_body->Release (); _brush_body = nullptr; }
	if (_brush_brand) { _brush_brand->Release (); _brush_brand = nullptr; }
	if (_rt) { _rt->Release (); _rt = nullptr; }
}

bool
ShellWindow::ensure_device_resources ()
{
	if (_rt) {
		return true;
	}
	if (!_d2d_factory || !_hwnd) {
		return false;
	}

	RECT rc = {};
	GetClientRect (_hwnd, &rc);
	D2D1_SIZE_U size = D2D1::SizeU (
		rc.right > 0 ? (UINT)rc.right : _width,
		rc.bottom > 0 ? (UINT)rc.bottom : _height);

	HRESULT hr = _d2d_factory->CreateHwndRenderTarget (
		D2D1::RenderTargetProperties (),
		D2D1::HwndRenderTargetProperties (_hwnd, size),
		&_rt);
	if (FAILED (hr)) {
		return false;
	}

	_rt->CreateSolidColorBrush (kBrand, &_brush_brand);
	_rt->CreateSolidColorBrush (kBody, &_brush_body);
	_rt->CreateSolidColorBrush (kAccent, &_brush_accent);
	_rt->CreateSolidColorBrush (kPaneFill, &_brush_pane);
	_rt->CreateSolidColorBrush (kPaneEdge, &_brush_pane_edge);
	_rt->CreateSolidColorBrush (kPeak, &_brush_peak);
	_rt->CreateSolidColorBrush (kRms, &_brush_rms);
	_rt->CreateSolidColorBrush (kPlayhead, &_brush_playhead);
	_rt->CreateSolidColorBrush (kSelection, &_brush_selection);
	_rt->CreateSolidColorBrush (kLoop, &_brush_loop);
	_rt->CreateSolidColorBrush (kPunch, &_brush_punch);
	_rt->CreateSolidColorBrush (kMarker, &_brush_marker);
	_rt->CreateSolidColorBrush (kMidi, &_brush_midi);
	_rt->CreateSolidColorBrush (kMeterHot, &_brush_meter_hot);

	ID2D1GradientStopCollection* stops = nullptr;
	D2D1_GRADIENT_STOP gs[2];
	gs[0].position = 0.f; gs[0].color = kBgTop;
	gs[1].position = 1.f; gs[1].color = kBgBottom;
	hr = _rt->CreateGradientStopCollection (gs, 2, &stops);
	if (SUCCEEDED (hr)) {
		_rt->CreateLinearGradientBrush (
			D2D1::LinearGradientBrushProperties (D2D1::Point2F (0, 0), D2D1::Point2F (0, (float)size.height)),
			stops, &_brush_bg);
		stops->Release ();
	}

	return _brush_brand && _brush_body && _brush_accent && _brush_pane
	    && _brush_peak && _brush_rms && _brush_playhead && _brush_selection
	    && _brush_loop && _brush_punch && _brush_marker && _brush_midi && _brush_meter_hot && _brush_bg;
}

void
ShellWindow::layout_panes ()
{
	const float w = (float)_width;
	const float h = (float)_height;
	const float pad = 16.f * _dpi_scale;
	const float brand_h = 68.f * _dpi_scale;
	const float transport_h = 44.f * _dpi_scale;
	const float status_h = 40.f * _dpi_scale;
	const float gap = 12.f * _dpi_scale;

	_brand_bar = D2D1::RectF (0, 0, w, brand_h);
	_transport_bounds = D2D1::RectF (0, brand_h, w, brand_h + transport_h);
	_transport.layout (_transport_bounds, _dpi_scale);
	_status_bar = D2D1::RectF (0, h - status_h, w, h);

	float body_top = brand_h + transport_h + pad;
	float body_bot = h - status_h - pad;
	float left = pad;
	float right = w - pad;
	float split = left + (right - left) * 0.64f;

	_editor_pane = D2D1::RectF (left, body_top, split - gap * 0.5f, body_bot);
	_mixer_pane = D2D1::RectF (split + gap * 0.5f, body_top, right, body_bot);

	float inset = 12.f * _dpi_scale;
	float title_h = 32.f * _dpi_scale;
	float ruler_h = 22.f * _dpi_scale;
	float label_w = 64.f * _dpi_scale;
	float lane_gap = 6.f * _dpi_scale;
	float wave_left = _editor_pane.left + inset + label_w;
	float wave_right = _editor_pane.right - inset;
	_editor_ruler = D2D1::RectF (wave_left, _editor_pane.top + title_h,
	                             wave_right, _editor_pane.top + title_h + ruler_h);
	float midi_h = 96.f * _dpi_scale; /* ~24-key piano-roll */
	float lanes_top = _editor_ruler.bottom + 4.f * _dpi_scale;
	float lanes_bot = _editor_pane.bottom - inset - midi_h - lane_gap;
	const int n_lanes = std::max (1, n_audio_lanes ());
	float lane_h = (lanes_bot - lanes_top - lane_gap * (float)(n_lanes - 1)) / (float)n_lanes;

	for (int i = 0; i < kTracksMax; ++i) {
		_lanes[i].bounds = D2D1::RectF ();
	}
	_auto_lane_bounds = D2D1::RectF ();
	for (int i = 0; i < n_lanes; ++i) {
		float y0 = lanes_top + (float)i * (lane_h + lane_gap);
		if (i == 0) {
			/* Split Audio 1: waveform on top, gain automation underneath. */
			float auto_h = std::max (36.f * _dpi_scale, lane_h * 0.32f);
			float wave_h = lane_h - auto_h - 3.f * _dpi_scale;
			if (wave_h < 28.f * _dpi_scale) {
				wave_h = lane_h * 0.62f;
				auto_h = lane_h - wave_h - 3.f * _dpi_scale;
			}
			_lanes[0].bounds = D2D1::RectF (wave_left, y0, wave_right, y0 + wave_h);
			_auto_lane_bounds = D2D1::RectF (
				wave_left, y0 + wave_h + 3.f * _dpi_scale, wave_right, y0 + lane_h);
		} else {
			_lanes[i].bounds = D2D1::RectF (wave_left, y0, wave_right, y0 + lane_h);
		}
		_lanes[i].strip.resize_for_width (wave_right - wave_left);
	}
	_midi_lane = D2D1::RectF (wave_left, lanes_bot + lane_gap, wave_right,
	                          lanes_bot + lane_gap + midi_h);

	float mix_inset = 10.f * _dpi_scale;
	float mix_top = _mixer_pane.top + 36.f * _dpi_scale;
	float mix_bot = _mixer_pane.bottom - mix_inset;
	float mix_inner = _mixer_pane.right - _mixer_pane.left - mix_inset * 2.f;
	float strip_gap = 6.f * _dpi_scale;
	const int n_mix = std::max (1, n_mixer_strips ());
	/* Reserve a narrow Monitor strip on the right. */
	float mon_w = 56.f * _dpi_scale;
	float strips_inner = mix_inner - mon_w - strip_gap;
	if (strips_inner < 40.f * _dpi_scale) {
		mon_w = 48.f * _dpi_scale;
		strips_inner = mix_inner - mon_w - strip_gap;
	}
	float strip_w = (strips_inner - strip_gap * (float)(n_mix - 1)) / (float)n_mix;
	for (int i = 0; i < kMixerStripsMax; ++i) {
		_mixers[i].layout (D2D1::RectF (0, 0, 0, 0), _dpi_scale);
	}
	for (int i = 0; i < n_mix; ++i) {
		float x0 = _mixer_pane.left + mix_inset + (float)i * (strip_w + strip_gap);
		_mixers[i].layout (D2D1::RectF (x0, mix_top, x0 + strip_w, mix_bot), _dpi_scale);
	}
	float mon_x = _mixer_pane.right - mix_inset - mon_w;
	_monitor.layout (D2D1::RectF (mon_x, mix_top, mon_x + mon_w, mix_bot), _dpi_scale);
}

bool
ShellWindow::create (HINSTANCE instance, int show_cmd)
{
	if (!init_d2d ()) {
		return false;
	}

	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof (wc);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = ShellWindow::wnd_proc;
	wc.hInstance = instance;
	wc.hCursor = LoadCursorW (nullptr, IDC_ARROW);
	wc.hbrBackground = nullptr;
	wc.lpszClassName = kClassName;
	wc.hIcon = LoadIconW (nullptr, IDI_APPLICATION);
	if (!RegisterClassExW (&wc) && GetLastError () != ERROR_CLASS_ALREADY_EXISTS) {
		return false;
	}

	DWORD style = WS_OVERLAPPEDWINDOW;
	RECT wr = { 0, 0, (LONG)_width, (LONG)_height };
	AdjustWindowRect (&wr, style, FALSE);

	_hwnd = CreateWindowExW (
		0, kClassName, kWindowTitle, style,
		CW_USEDEFAULT, CW_USEDEFAULT,
		wr.right - wr.left, wr.bottom - wr.top,
		nullptr, nullptr, instance, this);
	if (!_hwnd) {
		return false;
	}

	_dpi = query_dpi_for_hwnd (_hwnd);
	_dpi_scale = (float)_dpi / 96.f;
	layout_panes ();
	ShowWindow (_hwnd, show_cmd);
	UpdateWindow (_hwnd);
	/* 30 Hz UI clock — enough for meters; saves paint vs 60 Hz while playing. */
	SetTimer (_hwnd, 1, 33, nullptr);
	return true;
}

LRESULT CALLBACK
ShellWindow::wnd_proc (HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	ShellWindow* self = nullptr;
	if (msg == WM_NCCREATE) {
		auto* cs = reinterpret_cast<CREATESTRUCTW*> (lparam);
		self = static_cast<ShellWindow*> (cs->lpCreateParams);
		SetWindowLongPtrW (hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR> (self));
		self->_hwnd = hwnd;
	} else {
		self = reinterpret_cast<ShellWindow*> (GetWindowLongPtrW (hwnd, GWLP_USERDATA));
	}
	if (self) {
		return self->handle_message (msg, wparam, lparam);
	}
	return DefWindowProcW (hwnd, msg, wparam, lparam);
}

LRESULT
ShellWindow::handle_message (UINT msg, WPARAM wparam, LPARAM lparam)
{
	switch (msg) {
	case WM_SIZE:
		on_resize (LOWORD (lparam), HIWORD (lparam));
		return 0;
	case WM_DPICHANGED: {
		UINT dpi = HIWORD (wparam);
		RECT* suggested = reinterpret_cast<RECT*> (lparam);
		on_dpi_changed (dpi, suggested);
		return 0;
	}
	case WM_DISPLAYCHANGE:
		InvalidateRect (_hwnd, nullptr, FALSE);
		return 0;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		BeginPaint (_hwnd, &ps);
		on_paint ();
		EndPaint (_hwnd, &ps);
		return 0;
	}
	case WM_TIMER:
		on_tick ();
		return 0;
	case WM_KEYDOWN:
		if (wparam == VK_ESCAPE) {
			PostQuitMessage (0);
		} else if (wparam == VK_SPACE) {
			transport_toggle ();
		} else if (wparam == VK_DELETE || wparam == VK_BACK) {
			do_delete_selection ();
		} else if (wparam == VK_HOME || wparam == '0') {
			reset_shared_view ();
			refresh_editor_view_after_nav ();
			InvalidateRect (_hwnd, nullptr, FALSE);
		} else if (wparam == VK_LEFT) {
			float step = (GetKeyState (VK_SHIFT) & 0x8000) ? 0.5f : 0.15f;
			do_scroll_editor (-step);
		} else if (wparam == VK_RIGHT) {
			float step = (GetKeyState (VK_SHIFT) & 0x8000) ? 0.5f : 0.15f;
			do_scroll_editor (step);
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'A' || wparam == 'a')) {
			if (GetKeyState (VK_SHIFT) & 0x8000) {
				do_fit_session ();
			} else {
				do_fit_all_regions ();
			}
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'E' || wparam == 'e')
		           && !(GetKeyState (VK_MENU) & 0x8000)) {
			/* E = fit selection (Ctrl+E remains export). */
			do_fit_selection ();
		} else if (wparam == 'L' || wparam == 'l') {
			do_toggle_loop ();
		} else if (wparam == 'P' || wparam == 'p') {
			if (!(GetKeyState (VK_CONTROL) & 0x8000)) {
				do_toggle_punch ();
			}
		} else if (wparam == VK_OEM_4) { /* [ */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				if (GetKeyState (VK_SHIFT) & 0x8000) {
					_engine.set_punch_start (_playhead_sec);
					_engine.poll ();
					_engine_snap = _engine.snapshot ();
					sync_transport_punch_from_engine ();
				} else {
					_engine.set_loop_start (_playhead_sec);
					_engine.poll ();
					_engine_snap = _engine.snapshot ();
					sync_transport_loop_from_engine ();
					apply_loop_to_lanes ();
				}
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
		} else if (wparam == VK_OEM_6) { /* ] */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				if (GetKeyState (VK_SHIFT) & 0x8000) {
					_engine.set_punch_end (_playhead_sec);
					_engine.poll ();
					_engine_snap = _engine.snapshot ();
					sync_transport_punch_from_engine ();
				} else {
					_engine.set_loop_end (_playhead_sec);
					_engine.poll ();
					_engine_snap = _engine.snapshot ();
					sync_transport_loop_from_engine ();
					apply_loop_to_lanes ();
				}
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
		} else if (wparam == 'M' || wparam == 'm') {
			if (GetKeyState (VK_SHIFT) & 0x8000) {
				do_remove_marker_at_playhead ();
			} else {
				do_add_marker_at_playhead ();
			}
		} else if ((GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'Z' || wparam == 'z')) {
			do_undo ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'Y' || wparam == 'y')) {
			do_redo ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'S' || wparam == 's')) {
			do_save_session ();
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'S' || wparam == 's')) {
			do_split_at_playhead ();
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'F' || wparam == 'f')) {
			do_apply_crossfade ();
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'G' || wparam == 'g')) {
			if (GetKeyState (VK_SHIFT) & 0x8000) {
				do_cycle_snap_density ();
			} else {
				do_toggle_snap ();
			}
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'C' || wparam == 'c')) {
			do_toggle_count_in ();
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'K' || wparam == 'k')) {
			do_toggle_click ();
		} else if (wparam == VK_OEM_MINUS || wparam == VK_SUBTRACT) {
			do_nudge_tempo (-1.f);
		} else if (wparam == VK_OEM_PLUS || wparam == VK_ADD) {
			do_nudge_tempo (1.f);
		} else if ((GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'O' || wparam == 'o')) {
			prompt_open_session ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'E' || wparam == 'e')) {
			prompt_export_wav ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000) && (wparam == VK_OEM_PLUS || wparam == VK_ADD)) {
			_lanes[0].strip.zoom_at (1.15f, 0.5f);
			apply_shared_view (0);
			refresh_editor_view_after_nav ();
			InvalidateRect (_hwnd, nullptr, FALSE);
		} else if ((GetKeyState (VK_CONTROL) & 0x8000) && (wparam == VK_OEM_MINUS || wparam == VK_SUBTRACT)) {
			_lanes[0].strip.zoom_at (1.f / 1.15f, 0.5f);
			apply_shared_view (0);
			refresh_editor_view_after_nav ();
			InvalidateRect (_hwnd, nullptr, FALSE);
		} else if (wparam == VK_F2) {
			do_rename_focused_track ();
		} else if ((GetKeyState (VK_MENU) & 0x8000) && wparam == VK_UP) {
			do_move_audio_track (-1);
		} else if ((GetKeyState (VK_MENU) & 0x8000) && wparam == VK_DOWN) {
			do_move_audio_track (+1);
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'U' || wparam == 'u')) {
			do_apply_or_clear_group ();
		}
		return 0;
	case WM_MOUSEWHEEL:
		if (on_mouse_wheel (wparam, lparam)) {
			return 0;
		}
		break;
	case WM_LBUTTONDOWN:
		if (on_mouse_down (lparam)) {
			return 0;
		}
		break;
	case WM_LBUTTONDBLCLK: {
		float x, y;
		client_from_lparam (lparam, &x, &y);
		int lab = hit_mixer_label (x, y);
		if (lab >= 0) {
			const int mi = midi_mixer_index ();
			if (lab == mi) {
				prompt_rename_track (0, true);
			} else if (lab < n_audio_lanes ()) {
				_focus_audio_track = lab;
				prompt_rename_track (lab, false);
			}
			return 0;
		}
		int ed = hit_editor_lane_label (x, y);
		if (ed >= 0) {
			_focus_audio_track = ed;
			prompt_rename_track (ed, false);
			return 0;
		}
		break;
	}
	case WM_MOUSEMOVE:
		if (on_mouse_move (lparam)) {
			return 0;
		}
		break;
	case WM_LBUTTONUP:
	case WM_CAPTURECHANGED:
		on_mouse_up ();
		return 0;
	case WM_DESTROY:
		KillTimer (_hwnd, 1);
		_engine.stop ();
		discard_device_resources ();
		PostQuitMessage (0);
		return 0;
	default:
		return DefWindowProcW (_hwnd, msg, wparam, lparam);
	}
	return DefWindowProcW (_hwnd, msg, wparam, lparam);
}

void
ShellWindow::client_from_lparam (LPARAM lparam, float* out_x, float* out_y) const
{
	*out_x = (float)(short)LOWORD (lparam);
	*out_y = (float)(short)HIWORD (lparam);
}

bool
ShellWindow::hit_editor_ruler (float x, float y) const
{
	const D2D1_RECT_F& r = _editor_ruler;
	return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

bool
ShellWindow::hit_midi_lane (float x, float y) const
{
	const D2D1_RECT_F& r = _midi_lane;
	return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

uint8_t
ShellWindow::midi_pitch_at_y (float y) const
{
	const D2D1_RECT_F& r = _midi_lane;
	float h = r.bottom - r.top;
	if (h <= 1.f) {
		return 60;
	}
	float t = (y - r.top) / h;
	if (t < 0.f) {
		t = 0.f;
	}
	if (t > 0.999f) {
		t = 0.999f;
	}
	/* Top of lane = high pitch (piano-roll convention). */
	int row = (int)(t * (float)kMidiPitchRows);
	if (row < 0) {
		row = 0;
	}
	if (row >= kMidiPitchRows) {
		row = kMidiPitchRows - 1;
	}
	return (uint8_t)(kMidiPitchHi - row);
}

float
ShellWindow::midi_time_at_x (float x) const
{
	return _lanes[0].strip.time_at_x (_midi_lane, x);
}

int
ShellWindow::hit_editor_lane (float x, float y) const
{
	const int n = n_audio_lanes ();
	for (int i = 0; i < n; ++i) {
		const D2D1_RECT_F& r = _lanes[i].bounds;
		if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) {
			return i;
		}
	}
	return -1;
}

int
ShellWindow::hit_mixer_arm (float x, float y) const
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < na; ++i) {
		if (_mixers[i].hit_arm (x, y)) {
			return i;
		}
	}
	return -1;
}

int
ShellWindow::hit_mixer_mute (float x, float y) const
{
	const int n = n_mixer_strips ();
	for (int i = 0; i < n; ++i) {
		if (_mixers[i].hit_mute (x, y)) {
			return i;
		}
	}
	return -1;
}

int
ShellWindow::hit_mixer_solo (float x, float y) const
{
	const int n = n_mixer_strips ();
	for (int i = 0; i < n; ++i) {
		if (_mixers[i].hit_solo (x, y)) {
			return i;
		}
	}
	return -1;
}

int
ShellWindow::hit_mixer_pan (float x, float y) const
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < na; ++i) {
		if (_mixers[i].hit_pan (x, y)) {
			return i;
		}
	}
	return -1;
}

int
ShellWindow::hit_mixer_send (float x, float y) const
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < na; ++i) {
		if (_mixers[i].hit_send (x, y)) {
			return i;
		}
	}
	return -1;
}

int
ShellWindow::hit_mixer_meter (float x, float y) const
{
	const int n = n_mixer_strips ();
	for (int i = 0; i < n; ++i) {
		if (_mixers[i].hit_meter (x, y)) {
			return i;
		}
	}
	return -1;
}

int
ShellWindow::hit_mixer_fx (float x, float y) const
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < na; ++i) {
		if (_mixers[i].hit_fx (x, y)) {
			return i;
		}
	}
	return -1;
}

int
ShellWindow::hit_mixer_label (float x, float y) const
{
	const int n = n_mixer_strips ();
	for (int i = 0; i < n; ++i) {
		if (_mixers[i].hit_label (x, y)) {
			return i;
		}
	}
	return -1;
}

int
ShellWindow::hit_editor_lane_label (float x, float y) const
{
	const int n = n_audio_lanes ();
	float inset = 12.f * _dpi_scale;
	for (int i = 0; i < n; ++i) {
		const D2D1_RECT_F& lb = _lanes[i].bounds;
		if (lb.right <= lb.left) {
			continue;
		}
		float label_bot = lb.bottom;
		if (i == 0 && _auto_lane_bounds.bottom > label_bot) {
			label_bot = _auto_lane_bounds.bottom;
		}
		D2D1_RECT_F label_rc = D2D1::RectF (
			_editor_pane.left + inset, lb.top, lb.left - 4.f * _dpi_scale, label_bot);
		if (x >= label_rc.left && x <= label_rc.right
		    && y >= label_rc.top && y <= label_rc.bottom) {
			return i;
		}
	}
	return -1;
}

int
ShellWindow::hit_mixer_fx_param (float x, float y) const
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < na; ++i) {
		if (_mixers[i].hit_fx_param (x, y)) {
			return i;
		}
	}
	return -1;
}

int
ShellWindow::hit_mixer_fader (float x, float y) const
{
	const int n = n_mixer_strips ();
	for (int i = 0; i < n; ++i) {
		if (_mixers[i].hit_fader (x, y)) {
			return i;
		}
	}
	return -1;
}

bool
ShellWindow::on_mouse_wheel (WPARAM wparam, LPARAM lparam)
{
	POINT pt;
	pt.x = (short)LOWORD (lparam);
	pt.y = (short)HIWORD (lparam);
	ScreenToClient (_hwnd, &pt);
	float x = (float)pt.x;
	float y = (float)pt.y;
	int lane = hit_editor_lane (x, y);
	bool over_editor = (lane >= 0) || hit_editor_ruler (x, y) || hit_midi_lane (x, y);
	if (!over_editor) {
		return false;
	}
	if (lane < 0) {
		lane = 0;
	}

	int delta = GET_WHEEL_DELTA_WPARAM (wparam);
	/* Shift+wheel → horizontal scroll (keep zoom / snap / playhead). */
	if (GetKeyState (VK_SHIFT) & 0x8000) {
		float widths = (delta > 0) ? -0.12f : 0.12f;
		if (GetKeyState (VK_CONTROL) & 0x8000) {
			widths *= 0.35f;
		}
		do_scroll_editor (widths);
		return true;
	}

	const D2D1_RECT_F& r = _lanes[lane].bounds;
	float w = r.right - r.left;
	float norm_x = w > 1.f ? (x - r.left) / w : 0.5f;
	float step = (GetKeyState (VK_CONTROL) & 0x8000) ? 1.05f : 1.12f;
	float factor = (delta > 0) ? step : (1.f / step);
	_lanes[lane].strip.zoom_at (factor, norm_x);
	apply_shared_view (lane);
	refresh_editor_view_after_nav ();
	InvalidateRect (_hwnd, nullptr, FALSE);
	return true;
}

bool
ShellWindow::on_mouse_down (LPARAM lparam)
{
	float x, y;
	client_from_lparam (lparam, &x, &y);

	TransportHit th = _transport.hit_test (x, y);
	if (th == TransportHit::Play) {
		transport_play ();
		return true;
	}
	if (th == TransportHit::Stop) {
		transport_stop ();
		return true;
	}
	if (th == TransportHit::Rec) {
		transport_toggle_record ();
		return true;
	}
	if (th == TransportHit::Import) {
		prompt_import_audio ();
		return true;
	}
	if (th == TransportHit::Save) {
		do_save_session ();
		return true;
	}
	if (th == TransportHit::Open) {
		prompt_open_session ();
		return true;
	}
	if (th == TransportHit::Export) {
		prompt_export_wav ();
		return true;
	}
	if (th == TransportHit::Loop) {
		do_toggle_loop ();
		return true;
	}
	if (th == TransportHit::Punch) {
		do_toggle_punch ();
		return true;
	}
	if (th == TransportHit::Mark) {
		do_add_marker_at_playhead ();
		return true;
	}
	if (th == TransportHit::Split) {
		do_split_at_playhead ();
		return true;
	}
	if (th == TransportHit::Fade) {
		do_apply_crossfade ();
		return true;
	}
	if (th == TransportHit::Snap) {
		if (GetKeyState (VK_SHIFT) & 0x8000) {
			do_cycle_snap_density ();
		} else {
			do_toggle_snap ();
		}
		return true;
	}
	if (th == TransportHit::CountIn) {
		do_toggle_count_in ();
		return true;
	}
	if (th == TransportHit::Click) {
		do_toggle_click ();
		return true;
	}
	if (th == TransportHit::TempoDown) {
		do_nudge_tempo (-1.f);
		return true;
	}
	if (th == TransportHit::TempoUp) {
		do_nudge_tempo (1.f);
		return true;
	}
	if (th == TransportHit::AddAudio) {
		do_add_audio_track ();
		return true;
	}
	if (th == TransportHit::AddMidi) {
		do_add_midi_track ();
		return true;
	}
	if (th == TransportHit::RemoveTrack) {
		do_remove_track ();
		return true;
	}
	if (th == TransportHit::TrackUp) {
		do_move_audio_track (-1);
		return true;
	}
	if (th == TransportHit::TrackDown) {
		do_move_audio_track (+1);
		return true;
	}
	if (th == TransportHit::Group) {
		do_apply_or_clear_group ();
		return true;
	}
	if (th == TransportHit::FitSession) {
		do_fit_session ();
		return true;
	}
	if (th == TransportHit::FitSelection) {
		do_fit_selection ();
		return true;
	}
	if (th == TransportHit::FitRegions) {
		do_fit_all_regions ();
		return true;
	}
	if (th == TransportHit::Shuttle) {
		_drag = DragMode::Shuttle;
		_drag_index = -1;
		_pan_down_x = x;
		_pan_down_y = y;
		_pan_last_x = x;
		float speed = _transport.shuttle_speed_at_x (x);
		float norm = 0.f;
		float w = _transport.shuttle_btn ().right - _transport.shuttle_btn ().left;
		if (w > 1.f) {
			norm = ((x - _transport.shuttle_btn ().left) / w - 0.5f) * 2.f;
			norm = std::max (-1.f, std::min (1.f, norm));
		}
		_transport.set_shuttle_norm (norm);
		if (_engine.is_linked () && _engine_snap.session_ok) {
			_engine.set_transport_speed ((double)speed);
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			_playhead_sec = _engine_snap.transport_sec;
			_transport_playing = _engine_snap.transport_rolling;
			_transport.set_playing (_transport_playing);
			sync_transport_clock_from_engine ();
		}
		SetCapture (_hwnd);
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	MonitorHit mh = _monitor.hit_test (x, y);
	if (mh == MonitorHit::Dim) {
		do_toggle_monitor_dim ();
		return true;
	}
	if (mh == MonitorHit::Mono) {
		do_toggle_monitor_mono ();
		return true;
	}
	if (mh == MonitorHit::Mute) {
		do_toggle_monitor_cut ();
		return true;
	}

	if (hit_editor_ruler (x, y) && _engine.is_linked () && _engine_snap.session_ok) {
		float time_sec = _lanes[0].strip.time_at_x (_editor_ruler, x);
		_drag = DragMode::RulerScrub;
		_drag_index = -1;
		_pan_down_x = x;
		_pan_down_y = y;
		_pan_last_x = x;
		/* Click locate with mark snap; drag continues via scrub_to. */
		_engine.locate_to (time_sec, true /* snap to marker */, 0.4f);
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		_playhead_sec = _engine_snap.transport_sec;
		sync_transport_clock_from_engine ();
		SetCapture (_hwnd);
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	if (hit_midi_lane (x, y) && _engine.is_linked () && _engine_snap.session_ok) {
		_engine.ensure_midi_region ();
		float time_sec = midi_time_at_x (x);
		uint8_t pitch = midi_pitch_at_y (y);
		if (_engine.select_midi_note_at (time_sec, pitch)) {
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			InvalidateRect (_hwnd, nullptr, FALSE);
			return true;
		}
		/* Empty cell → start draw (finalize length on mouse-up). */
		_engine.clear_midi_note_selection ();
		_drag = DragMode::MidiDraw;
		_drag_index = -1;
		_midi_draw_start_sec = time_sec;
		_midi_draw_pitch = pitch;
		_pan_down_x = x;
		_pan_down_y = y;
		_pan_last_x = x;
		SetCapture (_hwnd);
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	int lab = hit_mixer_label (x, y);
	if (lab >= 0) {
		const int mi = midi_mixer_index ();
		if (lab == mi) {
			/* Double-click rename is handled in WM_LBUTTONDBLCLK. */
			return true;
		}
		if (lab < n_audio_lanes ()) {
			if (GetKeyState (VK_SHIFT) & 0x8000) {
				do_toggle_group_pick (lab);
				return true;
			}
			_focus_audio_track = lab;
			_drag = DragMode::TrackReorderPending;
			_drag_index = lab;
			_pan_down_x = x;
			_pan_down_y = y;
			_pan_last_x = x;
			SetCapture (_hwnd);
			return true;
		}
	}

	int ed_lab = hit_editor_lane_label (x, y);
	if (ed_lab >= 0) {
		_focus_audio_track = ed_lab;
		return true;
	}

	int arm = hit_mixer_arm (x, y);
	if (arm >= 0) {
		if (_engine.is_linked ()) {
			if (!_engine.start ()) {
				_engine_snap = _engine.snapshot ();
			} else {
				_engine.toggle_track_armed (arm);
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_arm_from_engine ();
			}
		}
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	int mute_i = hit_mixer_mute (x, y);
	if (mute_i >= 0) {
		if (_engine.is_linked () && _engine.snapshot ().session_ok) {
			if (mute_i == midi_mixer_index ()) {
				_engine.toggle_midi_muted ();
			} else {
				_engine.toggle_track_muted (mute_i);
			}
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_mute_solo_from_engine ();
		}
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	int solo_i = hit_mixer_solo (x, y);
	if (solo_i >= 0) {
		if (_engine.is_linked () && _engine.snapshot ().session_ok) {
			if (solo_i == midi_mixer_index ()) {
				_engine.toggle_midi_soloed ();
			} else {
				_engine.toggle_track_soloed (solo_i);
			}
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_mute_solo_from_engine ();
		}
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	int pan_i = hit_mixer_pan (x, y);
	if (pan_i >= 0) {
		_drag = DragMode::Pan;
		_drag_index = pan_i;
		if (_mixers[pan_i].drag_pan_to (x)) {
			_engine.set_track_pan (pan_i, _mixers[pan_i].pan ());
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_pan_from_engine ();
		}
		SetCapture (_hwnd);
		SetCursor (LoadCursorW (nullptr, IDC_SIZEWE));
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	int send_i = hit_mixer_send (x, y);
	if (send_i >= 0) {
		_drag = DragMode::Send;
		_drag_index = send_i;
		if (_mixers[send_i].drag_send_to (x)) {
			_engine.set_track_send (send_i, _mixers[send_i].send ());
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_send_from_engine ();
		}
		SetCapture (_hwnd);
		SetCursor (LoadCursorW (nullptr, IDC_SIZEWE));
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	/* Click meter → clear peak-hold clip latch (PeakMeter::reset_max). */
	int meter_i = hit_mixer_meter (x, y);
	if (meter_i >= 0 && _mixers[meter_i].meter_clipped ()) {
		_mixers[meter_i].clear_meter_clip ();
		if (meter_i < EngineSnapshot::kMeters && _engine.is_linked ()
		    && _engine.snapshot ().session_ok) {
			_engine.clear_track_meter_clip (meter_i);
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
		}
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	int fxpi = hit_mixer_fx_param (x, y);
	if (fxpi >= 0 && _mixers[fxpi].has_fx ()) {
		_drag = DragMode::FxParam;
		_drag_index = fxpi;
		if (_mixers[fxpi].drag_fx_param_to (x)) {
			_engine.set_track_fx_param (fxpi, _mixers[fxpi].fx_param ());
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_fx_from_engine ();
		}
		SetCapture (_hwnd);
		SetCursor (LoadCursorW (nullptr, IDC_SIZEWE));
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	int fxi = hit_mixer_fx (x, y);
	if (fxi >= 0) {
		if (_engine.is_linked () && _engine.snapshot ().session_ok) {
			if (_mixers[fxi].has_fx ()) {
				_engine.toggle_track_fx_bypass (fxi);
			} else {
				_engine.insert_track_fx (fxi);
			}
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_fx_from_engine ();
		}
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	int mi = hit_mixer_fader (x, y);
	if (mi >= 0) {
		_drag = DragMode::Fader;
		_drag_index = mi;
		_mixers[mi].drag_to (y);
		if (mi == midi_mixer_index ()) {
			_engine.set_midi_gain (_mixers[mi].gain ());
		} else {
			_engine.set_track_gain (mi, _mixers[mi].gain ());
			if (mi == 0) {
				_engine_snap.gain = _mixers[0].gain ();
			}
		}
		SetCapture (_hwnd);
		SetCursor (LoadCursorW (nullptr, IDC_SIZENS));
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	if (_auto_lane.hit (_auto_lane_bounds, x, y)
	    && _engine.is_linked () && _engine_snap.session_ok) {
		_drag = DragMode::AutoWrite;
		_drag_index = 0;
		_auto_last_write_sec = -1.f;
		_engine.ensure_track_gain_automation (0);
		write_automation_at (x, y);
		SetCapture (_hwnd);
		SetCursor (LoadCursorW (nullptr, IDC_CROSS));
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	int lane = hit_editor_lane (x, y);
	if (lane >= 0) {
		const bool shift = (GetKeyState (VK_SHIFT) & 0x8000) != 0;
		const bool ctrl = (GetKeyState (VK_CONTROL) & 0x8000) != 0;
		const bool alt = (GetKeyState (VK_MENU) & 0x8000) != 0;
		if (_engine.is_linked () && _engine_snap.session_ok
		    && (shift || ctrl || alt)) {
			float time_sec = _lanes[lane].strip.time_at_x (_lanes[lane].bounds, x);
			if (shift) {
				_engine.set_loop_start (time_sec);
			} else if (ctrl) {
				_engine.set_loop_end (time_sec);
			} else {
				_engine.add_marker_at (time_sec);
			}
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_transport_loop_from_engine ();
			apply_loop_to_lanes ();
			InvalidateRect (_hwnd, nullptr, FALSE);
			return true;
		}
		/* Selected region: trim edges → fade wedges → body move → else pan/select. */
		if (_engine.is_linked () && _engine_snap.has_selection
		    && _engine_snap.selection_track == lane) {
			int te = _lanes[lane].strip.hit_trim_edge (
				_lanes[lane].bounds, x, y, _dpi_scale);
			if (te == 0 || te == 1) {
				_drag = (te == 0) ? DragMode::TrimFront : DragMode::TrimEnd;
				_drag_index = lane;
				SetCapture (_hwnd);
				SetCursor (LoadCursorW (nullptr, IDC_SIZEWE));
				InvalidateRect (_hwnd, nullptr, FALSE);
				return true;
			}
			int fh = _lanes[lane].strip.hit_fade_handle (
				_lanes[lane].bounds, x, y, _dpi_scale);
			if (fh == 0 || fh == 1) {
				_drag = (fh == 0) ? DragMode::FadeIn : DragMode::FadeOut;
				_drag_index = lane;
				SetCapture (_hwnd);
				SetCursor (LoadCursorW (nullptr, IDC_SIZEWE));
				InvalidateRect (_hwnd, nullptr, FALSE);
				return true;
			}
			if (_lanes[lane].strip.hit_selection_body (
				    _lanes[lane].bounds, x, y, _dpi_scale)) {
				float t = _lanes[lane].strip.time_at_x (_lanes[lane].bounds, x);
				_region_move_grab_offset_sec = t - _engine_snap.selection_start_sec;
				_drag = DragMode::RegionMovePending;
				_drag_index = lane;
				_pan_last_x = x;
				_pan_down_x = x;
				_pan_down_y = y;
				SetCapture (_hwnd);
				return true;
			}
		}
		/* Click selects a region; drag past threshold pans the timeline. */
		_drag = DragMode::WavePending;
		_drag_index = lane;
		_pan_last_x = x;
		_pan_down_x = x;
		_pan_down_y = y;
		SetCapture (_hwnd);
		return true;
	}
	return false;
}

bool
ShellWindow::on_mouse_move (LPARAM lparam)
{
	if (_drag == DragMode::None) {
		return false;
	}
	float x, y;
	client_from_lparam (lparam, &x, &y);

	if (_drag == DragMode::Fader && _drag_index >= 0 && _drag_index < n_mixer_strips ()) {
		if (_mixers[_drag_index].drag_to (y)) {
			if (_drag_index == midi_mixer_index ()) {
				_engine.set_midi_gain (_mixers[_drag_index].gain ());
			} else {
				_engine.set_track_gain (_drag_index, _mixers[_drag_index].gain ());
				_engine_snap = _engine.snapshot ();
				sync_mixer_gains_from_engine ();
			}
			InvalidateRect (_hwnd, nullptr, FALSE);
		}
		return true;
	}

	if (_drag == DragMode::FxParam && _drag_index >= 0 && _drag_index < kTracks) {
		if (_mixers[_drag_index].drag_fx_param_to (x)) {
			_engine.set_track_fx_param (_drag_index, _mixers[_drag_index].fx_param ());
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_fx_from_engine ();
			InvalidateRect (_hwnd, nullptr, FALSE);
		}
		return true;
	}

	if (_drag == DragMode::Pan && _drag_index >= 0 && _drag_index < kTracks) {
		if (_mixers[_drag_index].drag_pan_to (x)) {
			_engine.set_track_pan (_drag_index, _mixers[_drag_index].pan ());
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_pan_from_engine ();
			InvalidateRect (_hwnd, nullptr, FALSE);
		}
		return true;
	}

	if (_drag == DragMode::Send && _drag_index >= 0 && _drag_index < kTracks) {
		if (_mixers[_drag_index].drag_send_to (x)) {
			_engine.set_track_send (_drag_index, _mixers[_drag_index].send ());
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_send_from_engine ();
			InvalidateRect (_hwnd, nullptr, FALSE);
		}
		return true;
	}

	if (_drag == DragMode::AutoWrite) {
		write_automation_at (x, y);
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	if ((_drag == DragMode::FadeIn || _drag == DragMode::FadeOut)
	    && _drag_index >= 0 && _drag_index < kTracks
	    && _engine_snap.has_selection) {
		float t = _lanes[_drag_index].strip.time_at_x (_lanes[_drag_index].bounds, x);
		if (_drag == DragMode::FadeIn) {
			float len = t - _engine_snap.selection_start_sec;
			_engine.set_selected_fade_in_sec (len);
		} else {
			float len = _engine_snap.selection_end_sec - t;
			_engine.set_selected_fade_out_sec (len);
		}
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	if ((_drag == DragMode::TrimFront || _drag == DragMode::TrimEnd)
	    && _drag_index >= 0 && _drag_index < kTracks
	    && _engine_snap.has_selection) {
		float t = _lanes[_drag_index].strip.time_at_x (_lanes[_drag_index].bounds, x);
		if (_drag == DragMode::TrimFront) {
			_engine.trim_selected_front_to (t);
		} else {
			_engine.trim_selected_end_to (t);
		}
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	if (_drag == DragMode::RegionMovePending && _drag_index >= 0 && _drag_index < kTracks) {
		float dx = x - _pan_down_x;
		float dy = y - _pan_down_y;
		float thresh = 4.f * _dpi_scale;
		if ((dx * dx + dy * dy) >= (thresh * thresh)) {
			_drag = DragMode::RegionMove;
			SetCursor (LoadCursorW (nullptr, IDC_SIZEALL));
		} else {
			return true;
		}
	}

	if (_drag == DragMode::RegionMove && _drag_index >= 0 && _drag_index < kTracks
	    && _engine_snap.has_selection) {
		float t = _lanes[_drag_index].strip.time_at_x (_lanes[_drag_index].bounds, x);
		float new_start = t - _region_move_grab_offset_sec;
		_engine.move_selected_to (new_start);
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	if (_drag == DragMode::TrackReorderPending && _drag_index >= 0) {
		float dx = x - _pan_down_x;
		float dy = y - _pan_down_y;
		float thresh = 6.f * _dpi_scale;
		if ((dx * dx + dy * dy) >= (thresh * thresh)) {
			_drag = DragMode::TrackReorder;
			SetCursor (LoadCursorW (nullptr, IDC_SIZENS));
		}
		return true;
	}

	if (_drag == DragMode::TrackReorder && _drag_index >= 0) {
		_pan_last_x = x;
		_pan_down_y = y; /* reuse as last y for visual only */
		return true;
	}

	if (_drag == DragMode::WavePending && _drag_index >= 0 && _drag_index < kTracks) {
		float dx = x - _pan_down_x;
		float dy = y - _pan_down_y;
		float thresh = 4.f * _dpi_scale;
		if ((dx * dx + dy * dy) >= (thresh * thresh)) {
			_drag = DragMode::WavePan;
			SetCursor (LoadCursorW (nullptr, IDC_SIZEWE));
		} else {
			return true;
		}
	}

	if (_drag == DragMode::WavePan && _drag_index >= 0 && _drag_index < kTracks) {
		const D2D1_RECT_F& r = _lanes[_drag_index].bounds;
		float w = r.right - r.left;
		if (w > 1.f) {
			float delta_widths = (_pan_last_x - x) / w;
			if (delta_widths != 0.f) {
				_lanes[_drag_index].strip.pan_by (delta_widths);
				apply_shared_view (_drag_index);
				refresh_editor_view_after_nav ();
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
		}
		_pan_last_x = x;
		return true;
	}

	if (_drag == DragMode::MidiDraw) {
		_pan_last_x = x;
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	if (_drag == DragMode::RulerScrub && _engine.is_linked () && _engine_snap.session_ok) {
		_pan_last_x = x;
		float time_sec = _lanes[0].strip.time_at_x (_editor_ruler, x);
		_engine.scrub_to (time_sec);
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		_playhead_sec = _engine_snap.transport_sec;
		sync_transport_clock_from_engine ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	if (_drag == DragMode::Shuttle) {
		_pan_last_x = x;
		float speed = _transport.shuttle_speed_at_x (x);
		float w = _transport.shuttle_btn ().right - _transport.shuttle_btn ().left;
		float norm = 0.f;
		if (w > 1.f) {
			norm = ((x - _transport.shuttle_btn ().left) / w - 0.5f) * 2.f;
			norm = std::max (-1.f, std::min (1.f, norm));
		}
		_transport.set_shuttle_norm (norm);
		if (_engine.is_linked () && _engine_snap.session_ok) {
			_engine.set_transport_speed ((double)speed);
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			_playhead_sec = _engine_snap.transport_sec;
			_transport_playing = _engine_snap.transport_rolling;
			_transport.set_playing (_transport_playing);
			sync_transport_clock_from_engine ();
		}
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}
	return false;
}

void
ShellWindow::on_mouse_up ()
{
	if (_drag == DragMode::MidiDraw && _engine.is_linked () && _engine_snap.session_ok) {
		float end_sec = midi_time_at_x (_pan_last_x);
		float start = _midi_draw_start_sec;
		float end = end_sec;
		if (end < start) {
			float tmp = start;
			start = end;
			end = tmp;
		}
		float len = end - start;
		if (len < 0.12f) {
			len = 0.25f; /* click without drag → quarter-ish note */
		}
		_engine.add_midi_note (_midi_draw_pitch, start, len, 100);
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
	if (_drag == DragMode::WavePending && _drag_index >= 0 && _drag_index < kTracks) {
		int lane = _drag_index;
		float time_sec = _lanes[lane].strip.time_at_x (_lanes[lane].bounds, _pan_down_x);
		if (_engine.is_linked () && _engine_snap.session_ok) {
			if (!_engine.select_region_at (lane, time_sec)) {
				_engine.clear_region_selection ();
			}
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			apply_region_selection_to_lanes ();
			InvalidateRect (_hwnd, nullptr, FALSE);
		}
	}
	if (_drag == DragMode::TrackReorderPending) {
		/* Click without drag — keep focus; F2 renames. */
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
	if (_drag == DragMode::TrackReorder && _drag_index >= 0
	    && _engine.is_linked () && _engine_snap.session_ok) {
		float drop_x = _pan_last_x;
		float drop_y = _pan_down_y;
		POINT pt;
		if (GetCursorPos (&pt) && ScreenToClient (_hwnd, &pt)) {
			drop_x = (float)pt.x;
			drop_y = (float)pt.y;
		}
		int from = _drag_index;
		int to = hit_mixer_label (drop_x, drop_y);
		if (to < 0) {
			/* Fall back to strip bounds under cursor. */
			const int na = n_audio_lanes ();
			for (int i = 0; i < na; ++i) {
				const D2D1_RECT_F& b = _mixers[i].bounds ();
				if (drop_x >= b.left && drop_x <= b.right
				    && drop_y >= b.top && drop_y <= b.bottom) {
					to = i;
					break;
				}
			}
		}
		if (to >= 0 && to < n_audio_lanes () && to != from) {
			int step = (to > from) ? +1 : -1;
			int cur = from;
			while (cur != to) {
				if (!_engine.move_audio_track (cur, step)) {
					break;
				}
				cur += step;
			}
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			_focus_audio_track = cur;
			sync_track_labels_from_engine ();
			sync_mixer_gains_from_engine ();
			sync_mixer_arm_from_engine ();
			sync_mixer_mute_solo_from_engine ();
			sync_mixer_pan_from_engine ();
			sync_mixer_send_from_engine ();
			apply_region_selection_to_lanes ();
			layout_panes ();
			InvalidateRect (_hwnd, nullptr, FALSE);
		}
	}
	/* RegionMovePending click (no drag): keep selection — no timeline pan. */
	if (_drag == DragMode::RegionMovePending) {
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
	if (_drag == DragMode::Shuttle && _engine.is_linked () && _engine_snap.session_ok) {
		_engine.stop_shuttle ();
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		_playhead_sec = _engine_snap.transport_sec;
		_transport_playing = false;
		_transport.set_playing (false);
		_transport.set_shuttle_norm (0.f);
		sync_transport_clock_from_engine ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
	if (_drag == DragMode::RulerScrub) {
		sync_transport_clock_from_engine ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
	if (_drag != DragMode::None) {
		_drag = DragMode::None;
		_drag_index = -1;
		if (GetCapture () == _hwnd) {
			ReleaseCapture ();
		}
	}
}

void
ShellWindow::on_dpi_changed (UINT dpi, const RECT* suggested)
{
	_dpi = dpi ? dpi : 96;
	_dpi_scale = (float)_dpi / 96.f;
	if (suggested) {
		SetWindowPos (_hwnd, nullptr,
		              suggested->left, suggested->top,
		              suggested->right - suggested->left,
		              suggested->bottom - suggested->top,
		              SWP_NOZORDER | SWP_NOACTIVATE);
	}
	layout_panes ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::on_resize (UINT width, UINT height)
{
	_width = width ? width : 1;
	_height = height ? height : 1;
	layout_panes ();
	if (_rt) {
		_rt->Resize (D2D1::SizeU (_width, _height));
		if (_brush_bg) {
			_brush_bg->Release ();
			_brush_bg = nullptr;
		}
		ID2D1GradientStopCollection* stops = nullptr;
		D2D1_GRADIENT_STOP gs[2];
		gs[0].position = 0.f; gs[0].color = kBgTop;
		gs[1].position = 1.f; gs[1].color = kBgBottom;
		if (SUCCEEDED (_rt->CreateGradientStopCollection (gs, 2, &stops))) {
			_rt->CreateLinearGradientBrush (
				D2D1::LinearGradientBrushProperties (D2D1::Point2F (0, 0), D2D1::Point2F (0, (float)_height)),
				stops, &_brush_bg);
			stops->Release ();
		}
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::on_tick ()
{
	DWORD now = GetTickCount ();
	if (_last_tick_ms == 0) {
		_last_tick_ms = now;
	}
	float dt = (now - _last_tick_ms) * 0.001f;
	_last_tick_ms = now;

	float t = (now - _start_tick) * 0.001f;
	_pulse = 0.5f + 0.5f * std::sin (t * 1.6f);

	/* Poll engine often enough for meters while playing. */
	static DWORD last_poll = 0;
	static DWORD last_wave = 0;
	const bool rolling = _transport_playing || _engine_snap.transport_rolling;
	DWORD poll_ms = rolling ? 50u : 250u;
	if (now - last_poll > poll_ms) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		sync_mixer_arm_from_engine ();
		sync_mixer_mute_solo_from_engine ();
		sync_mixer_pan_from_engine ();
		sync_mixer_send_from_engine ();
		sync_transport_loop_from_engine ();
		sync_transport_punch_from_engine ();
		sync_transport_tempo_from_engine ();
		sync_transport_clock_from_engine ();
		apply_region_selection_to_lanes ();
		apply_loop_to_lanes ();
		refresh_automation_lane ();
		_transport.set_snap_enabled (_engine_snap.snap_enabled);
		if (_engine_snap.snap_enabled) {
			_engine.refresh_grid_lines (
				_lanes[0].strip.view_start_sec (),
				_lanes[0].strip.view_span_sec ());
			_engine_snap = _engine.snapshot ();
		}
		last_poll = now;
	}

	tick_smoke_record (now);

	if (_engine_snap.running && rolling) {
		/* Prefer engine clock (Session transport_sample or processed_samples). */
		_playhead_sec = _engine_snap.transport_sec;
	} else if (_transport_playing) {
		_playhead_sec += dt; /* UI-only fallback */
	}

	WaveStripParams wp;
	wp.engine_running = _engine_snap.running;
	wp.transport_playing = _transport_playing;
	wp.sample_rate = _engine_snap.sample_rate;
	wp.period_frames = _engine_snap.period_frames;
	wp.time_sec = t;
	wp.playhead_sec = _playhead_sec;

	float wave_peaks[WaveStrip::kMaxColumns];
	float wave_rms[WaveStrip::kMaxColumns];

	/* Waveform peakfile refill is heavy — throttle while transport rolls. */
	const bool refresh_waves = (!rolling && (now - last_wave > 200u))
		|| (rolling && (now - last_wave > 250u))
		|| (last_wave == 0);
	if (refresh_waves) {
		last_wave = now;
	}

	const int n_lanes = n_audio_lanes ();
	for (int i = 0; i < n_lanes; ++i) {
		wp.gain = _mixers[i].gain ();
		_lanes[i].strip.update_view (wp);

		if (refresh_waves) {
			bool has_regions = false;
			size_t n = _lanes[i].strip.column_count ();
			if (n > WaveStrip::kMaxColumns) {
				n = WaveStrip::kMaxColumns;
			}
			if (_engine_snap.waves_from_session && n > 0) {
				has_regions = _engine.fill_track_waveform (
					i,
					_lanes[i].strip.view_start_sec (),
					_lanes[i].strip.view_span_sec (),
					wave_peaks,
					wave_rms,
					n);
				_lanes[i].strip.set_waveform (wave_peaks, wave_rms, n, has_regions);
			} else {
				_lanes[i].strip.set_waveform (nullptr, nullptr, 0, false);
			}
		}

		float peak = 0.f;
		bool  clip = false;
		if (_engine_snap.meters_real && i < EngineSnapshot::kMeters) {
			peak = _engine_snap.meter_peak[i];
			clip = _engine_snap.meter_clip[i];
		} else {
			peak = stub_meter_peak (i, dt);
		}
		_mixers[i].set_meter_peak (peak, clip);
		_mixers[i].meter_tick (dt);
	}
	{
		const int mi = midi_mixer_index ();
		if (mi >= 0 && mi < kMixerStripsMax
		    && (_engine_snap.midi_track_ok || _engine_snap.midi_track_count > 0)) {
			_mixers[mi].set_meter_peak (_engine_snap.midi_activity, false);
			_mixers[mi].meter_tick (dt);
		}
	}
	sync_mixer_fx_from_engine ();
	sync_mixer_mute_solo_from_engine ();
	sync_mixer_pan_from_engine ();
	sync_mixer_send_from_engine ();
	sync_monitor_from_engine ();

	if (_timeout_secs > 0 && (now - _start_tick) >= (_timeout_secs * 1000u)) {
		PostQuitMessage (0);
		return;
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::draw_editor_ruler ()
{
	const D2D1_RECT_F& rr = _editor_ruler;
	if (rr.right <= rr.left) {
		return;
	}
	_rt->FillRectangle (rr, _brush_pane_edge);
	_rt->DrawRectangle (rr, _brush_body, 1.f);

	float view0 = _lanes[0].strip.view_start_sec ();
	float span = std::max (0.001f, _lanes[0].strip.view_span_sec ());
	float w = rr.right - rr.left;

	if (_engine_snap.loop_active && _brush_loop) {
		float n0 = (_engine_snap.loop_start_sec - view0) / span;
		float n1 = (_engine_snap.loop_end_sec - view0) / span;
		n0 = std::max (0.f, std::min (1.f, n0));
		n1 = std::max (0.f, std::min (1.f, n1));
		if (n1 > n0) {
			_brush_loop->SetOpacity (0.35f);
			_rt->FillRectangle (
				D2D1::RectF (rr.left + n0 * w, rr.top, rr.left + n1 * w, rr.bottom),
				_brush_loop);
			_brush_loop->SetOpacity (1.f);
		}
	}

	if (_engine_snap.punch_range_ok && _brush_punch) {
		float n0 = (_engine_snap.punch_start_sec - view0) / span;
		float n1 = (_engine_snap.punch_end_sec - view0) / span;
		n0 = std::max (0.f, std::min (1.f, n0));
		n1 = std::max (0.f, std::min (1.f, n1));
		if (n1 > n0) {
			float op = _engine_snap.punch_enabled ? 0.55f : 0.28f;
			_brush_punch->SetOpacity (op);
			_rt->FillRectangle (
				D2D1::RectF (rr.left + n0 * w, rr.top, rr.left + n1 * w, rr.bottom),
				_brush_punch);
			_brush_punch->SetOpacity (1.f);
		}
	}

	for (int i = 0; i < _engine_snap.marker_count; ++i) {
		float n = (_engine_snap.markers[i].sec - view0) / span;
		if (n < 0.f || n > 1.f) {
			continue;
		}
		float px = rr.left + n * w;
		float thick = std::max (1.5f, 2.f * _dpi_scale);
		_rt->FillRectangle (
			D2D1::RectF (px - thick * 0.5f, rr.top, px + thick * 0.5f, rr.bottom),
			_brush_marker);
		const wchar_t* name = _engine_snap.markers[i].name;
		if (name[0]) {
			_rt->DrawTextW (name, (UINT32)wcslen (name), _fmt_meta,
			                D2D1::RectF (px + 2.f * _dpi_scale, rr.top,
			                             px + 48.f * _dpi_scale, rr.bottom),
			                _brush_marker);
		}
	}

	/* Playhead tick on ruler. */
	float ph = (_playhead_sec - view0) / span;
	if (ph >= 0.f && ph <= 1.f) {
		float px = rr.left + ph * w;
		float thick = std::max (1.5f, 2.f * _dpi_scale);
		_rt->FillRectangle (
			D2D1::RectF (px - thick * 0.5f, rr.top, px + thick * 0.5f, rr.bottom),
			_brush_playhead);
	}
}

void
ShellWindow::draw_punch_overlay ()
{
	if (!_engine_snap.punch_range_ok || !_brush_punch) {
		return;
	}
	float view0 = _lanes[0].strip.view_start_sec ();
	float span = std::max (0.001f, _lanes[0].strip.view_span_sec ());
	float n0 = (_engine_snap.punch_start_sec - view0) / span;
	float n1 = (_engine_snap.punch_end_sec - view0) / span;
	n0 = std::max (0.f, std::min (1.f, n0));
	n1 = std::max (0.f, std::min (1.f, n1));
	if (n1 <= n0) {
		return;
	}
	float op = _engine_snap.punch_enabled ? 0.22f : 0.10f;
	_brush_punch->SetOpacity (op);
	for (int i = 0; i < kTracks; ++i) {
		const D2D1_RECT_F& lb = _lanes[i].bounds;
		float w = lb.right - lb.left;
		if (w < 4.f) {
			continue;
		}
		float bot = (i == 0 && _auto_lane_bounds.bottom > lb.bottom)
		            ? _auto_lane_bounds.bottom : lb.bottom;
		_rt->FillRectangle (
			D2D1::RectF (lb.left + n0 * w, lb.top, lb.left + n1 * w, bot),
			_brush_punch);
	}
	_brush_punch->SetOpacity (1.f);
}

void
ShellWindow::draw_editor_grid ()
{
	if (!_engine_snap.snap_enabled || _engine_snap.grid_line_count <= 0 || !_brush_body) {
		return;
	}
	float view0 = _lanes[0].strip.view_start_sec ();
	float span = std::max (0.001f, _lanes[0].strip.view_span_sec ());
	/* Vertical span: ruler through Audio lanes (+ automation under Audio 1). */
	float top = _editor_ruler.top;
	float bot = _lanes[kTracks - 1].bounds.bottom;
	if (_auto_lane_bounds.bottom > bot) {
		bot = _auto_lane_bounds.bottom;
	}
	if (_midi_lane.bottom > bot) {
		bot = _midi_lane.bottom;
	}
	float left = _lanes[0].bounds.left;
	float right = _lanes[0].bounds.right;
	float w = right - left;
	if (w < 8.f || bot <= top) {
		return;
	}

	for (int i = 0; i < _engine_snap.grid_line_count; ++i) {
		float n = (_engine_snap.grid_line_sec[i] - view0) / span;
		if (n < 0.f || n > 1.f) {
			continue;
		}
		float px = left + n * w;
		bool is_bar = _engine_snap.grid_line_bar[i];
		float thick = is_bar ? std::max (1.25f, 1.5f * _dpi_scale)
		                     : std::max (1.f, 1.f * _dpi_scale);
		_brush_body->SetOpacity (is_bar ? 0.28f : 0.14f);
		_rt->FillRectangle (
			D2D1::RectF (px - thick * 0.5f, top, px + thick * 0.5f, bot),
			_brush_body);
	}
	_brush_body->SetOpacity (1.f);
}

void
ShellWindow::draw_editor_pane ()
{
	const D2D1_RECT_F& r = _editor_pane;
	_rt->FillRectangle (r, _brush_pane);
	_rt->DrawRectangle (r, _brush_pane_edge, 1.f);
	_rt->FillRectangle (D2D1::RectF (r.left, r.top, r.left + 3.f * _dpi_scale, r.bottom), _brush_accent);

	float inset = 12.f * _dpi_scale;
	_rt->DrawTextW (L"Editor", 6, _fmt_pane,
	                D2D1::RectF (r.left + inset, r.top + 4.f * _dpi_scale,
	                             r.left + inset + 80.f * _dpi_scale, r.top + 26.f * _dpi_scale),
	                _brush_brand);

	wchar_t caption[180];
	int with_audio = 0;
	for (int i = 0; i < kTracks; ++i) {
		if (_lanes[i].strip.has_regions ()) {
			++with_audio;
		}
	}
	if (_engine_snap.waves_from_session) {
		if (_engine_snap.loop_active) {
			_snwprintf_s (caption, _TRUNCATE,
			              L"loop [%.1f–%.1f]%s  ·  marks %d  ·  Shift/Ctrl/Alt+click",
			              _engine_snap.loop_start_sec, _engine_snap.loop_end_sec,
			              _engine_snap.loop_playing ? L" ▶" : L"",
			              _engine_snap.marker_count);
		} else if (_engine_snap.has_selection) {
			_snwprintf_s (caption, _TRUNCATE,
			              L"sel T%d [%.1f–%.1f]  ·  trim/move  S/F  G=snap%s",
			              _engine_snap.selection_track + 1,
			              _engine_snap.selection_start_sec, _engine_snap.selection_end_sec,
			              _engine_snap.snap_enabled ? L" ON" : L"");
		} else if (with_audio > 0) {
			_snwprintf_s (caption, _TRUNCATE,
			              L"%d tracks  ·  %d with audio  ·  G=snap%s  ·  marks %d",
			              kTracks, with_audio,
			              _engine_snap.snap_enabled ? L" ON" : L"",
			              _engine_snap.marker_count);
		} else {
			_snwprintf_s (caption, _TRUNCATE,
			              L"%d tracks  ·  empty  ·  [ ]=loop  M=mark  L=loop play",
			              kTracks);
		}
	} else if (_lanes[0].strip.interacting ()) {
		_snwprintf_s (caption, _TRUNCATE,
		              L"%d tracks  ·  zoom %.2fx  ·  Home follows transport",
		              kTracks, _lanes[0].strip.zoom ());
	} else {
		_snwprintf_s (caption, _TRUNCATE,
		              L"%d tracks  ·  empty lanes (no Session)",
		              kTracks);
	}
	_rt->DrawTextW (caption, (UINT32)wcslen (caption), _fmt_meta,
	                D2D1::RectF (r.left + inset + 90.f * _dpi_scale, r.top + 6.f * _dpi_scale,
	                             r.right - inset, r.top + 26.f * _dpi_scale),
	                _brush_body);

	draw_editor_ruler ();

	float label_w = 64.f * _dpi_scale;
	float view0 = _lanes[0].strip.view_start_sec ();
	float span = std::max (0.001f, _lanes[0].strip.view_span_sec ());
	for (int i = 0; i < kTracks; ++i) {
		const D2D1_RECT_F& lb = _lanes[i].bounds;
		float label_bot = lb.bottom;
		if (i == 0 && _auto_lane_bounds.bottom > label_bot) {
			label_bot = _auto_lane_bounds.bottom;
		}
		D2D1_RECT_F label_rc = D2D1::RectF (
			r.left + inset, lb.top, lb.left - 4.f * _dpi_scale, label_bot);
		(void)label_w;
		_rt->DrawTextW (_lanes[i].label, (UINT32)wcslen (_lanes[i].label), _fmt_meta,
		                label_rc, _brush_body);

		_rt->FillRectangle (lb, _brush_pane_edge);
		_lanes[i].strip.draw (_rt, lb, _brush_peak, _brush_rms, _brush_body,
		                      _brush_playhead, _brush_selection, _dpi_scale);

		/* Marker hairlines across lanes. */
		float lane_w = lb.right - lb.left;
		float mark_bot = (i == 0) ? _auto_lane_bounds.bottom : lb.bottom;
		for (int m = 0; m < _engine_snap.marker_count; ++m) {
			float n = (_engine_snap.markers[m].sec - view0) / span;
			if (n < 0.f || n > 1.f) {
				continue;
			}
			float px = lb.left + n * lane_w;
			_brush_marker->SetOpacity (0.55f);
			_rt->DrawLine (D2D1::Point2F (px, lb.top), D2D1::Point2F (px, mark_bot),
			               _brush_marker, 1.f * _dpi_scale);
			_brush_marker->SetOpacity (1.f);
		}
	}

	draw_automation_lane ();
	draw_midi_lane ();
	draw_punch_overlay ();
	draw_editor_grid ();
}

void
ShellWindow::draw_midi_lane ()
{
	const D2D1_RECT_F& r = _midi_lane;
	if (r.right <= r.left) {
		return;
	}
	float inset = 12.f * _dpi_scale;
	D2D1_RECT_F label_rc = D2D1::RectF (
		_editor_pane.left + inset, r.top, r.left - 4.f * _dpi_scale, r.bottom);
	const wchar_t* midi_lab = (_engine_snap.midi_track_name[0] != L'\0')
		? _engine_snap.midi_track_name : L"MIDI 1";
	_rt->DrawTextW (midi_lab, (UINT32)wcslen (midi_lab), _fmt_meta, label_rc, _brush_midi);

	_rt->FillRectangle (r, _brush_pane_edge);
	_rt->DrawRectangle (r, _brush_midi, 1.f);

	const int keys = kMidiPitchRows;
	float h = r.bottom - r.top;
	float row_h = h / (float)keys;
	for (int k = 0; k < keys; ++k) {
		float y0 = r.top + (float)k * row_h;
		uint8_t pitch = (uint8_t)(kMidiPitchHi - k);
		int pc = pitch % 12;
		bool black = (pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10);
		_brush_body->SetOpacity (black ? 0.18f : 0.08f);
		_rt->FillRectangle (D2D1::RectF (r.left, y0, r.right, y0 + row_h - 0.5f), _brush_body);
		_brush_body->SetOpacity (1.f);
	}

	/* Session MIDI notes (real MidiModel contents). */
	for (int i = 0; i < _engine_snap.midi_note_count; ++i) {
		const EngineMidiNoteSnap& n = _engine_snap.midi_notes[i];
		if (n.pitch < kMidiPitchLo || n.pitch > kMidiPitchHi) {
			continue;
		}
		float x0 = _lanes[0].strip.x_at_time (r, n.start_sec);
		float x1 = _lanes[0].strip.x_at_time (r, n.start_sec + n.length_sec);
		if (x1 < r.left || x0 > r.right) {
			continue;
		}
		if (x1 - x0 < 2.f) {
			x1 = x0 + 2.f;
		}
		int row = (int)(kMidiPitchHi - n.pitch);
		float y0 = r.top + (float)row * row_h + 1.f;
		float y1 = y0 + row_h - 2.f;
		ID2D1SolidColorBrush* brush = n.selected ? _brush_accent : _brush_midi;
		if (!brush) {
			continue;
		}
		brush->SetOpacity (n.selected ? 0.95f : (0.45f + 0.4f * (n.velocity / 127.f)));
		_rt->FillRectangle (D2D1::RectF (x0, y0, x1, y1), brush);
		brush->SetOpacity (1.f);
		if (n.selected && _brush_playhead) {
			_rt->DrawRectangle (D2D1::RectF (x0, y0, x1, y1), _brush_playhead, 1.2f);
		}
	}

	/* In-progress draw rubber-band. */
	if (_drag == DragMode::MidiDraw && _brush_midi) {
		float t0 = _midi_draw_start_sec;
		float t1 = midi_time_at_x (_pan_last_x);
		if (t1 < t0) {
			float tmp = t0;
			t0 = t1;
			t1 = tmp;
		}
		if (t1 - t0 < 0.05f) {
			t1 = t0 + 0.25f;
		}
		float x0 = _lanes[0].strip.x_at_time (r, t0);
		float x1 = _lanes[0].strip.x_at_time (r, t1);
		int row = (int)(kMidiPitchHi - _midi_draw_pitch);
		float y0 = r.top + (float)row * row_h + 1.f;
		float y1 = y0 + row_h - 2.f;
		_brush_midi->SetOpacity (0.55f);
		_rt->FillRectangle (D2D1::RectF (x0, y0, x1, y1), _brush_midi);
		_brush_midi->SetOpacity (1.f);
	}

	wchar_t midicap[160];
	if (_engine_snap.midi_track_ok) {
		_snwprintf_s (midicap, _TRUNCATE,
		              L"%ls  ·  %d note%s  ·  %ls%s",
		              _engine_snap.midi_option.empty () ? L"MIDI" : _engine_snap.midi_option.c_str (),
		              _engine_snap.midi_note_count,
		              _engine_snap.midi_note_count == 1 ? L"" : L"s",
		              _engine_snap.midi_synth_ok
		                  ? (_engine_snap.midi_synth_name.empty () ? L"synth" : _engine_snap.midi_synth_name.c_str ())
		                  : L"no synth",
		              _engine_snap.midi_synth_to_master ? L"→master" : L"");
	} else {
		_snwprintf_s (midicap, _TRUNCATE, L"no MIDI track");
	}
	_rt->DrawTextW (midicap, (UINT32)wcslen (midicap), _fmt_meta,
	                D2D1::RectF (r.left + 6.f * _dpi_scale, r.top,
	                             r.right - 4.f * _dpi_scale, r.top + 14.f * _dpi_scale),
	                _brush_brand);
}

void
ShellWindow::draw_mixer_pane ()
{
	const D2D1_RECT_F& r = _mixer_pane;
	_rt->FillRectangle (r, _brush_pane);
	_rt->DrawRectangle (r, _brush_pane_edge, 1.f);
	_rt->FillRectangle (D2D1::RectF (r.left, r.top, r.left + 3.f * _dpi_scale, r.bottom), _brush_accent);

	float inset = 12.f * _dpi_scale;
	_rt->DrawTextW (L"Mixer", 5, _fmt_pane,
	                D2D1::RectF (r.left + inset, r.top + 8.f * _dpi_scale,
	                             r.left + inset + 70.f * _dpi_scale, r.top + 30.f * _dpi_scale),
	                _brush_brand);

	wchar_t hint[160];
	if (_engine_snap.track_fx_ok && !_engine_snap.track_fx_name.empty ()) {
		_snwprintf_s (hint, _TRUNCATE,
		              L"Meters: PeakMeter  ·  %s%s  ·  Blend %.0f%% (click FX=bypass)",
		              _engine_snap.track_fx_name.c_str (),
		              _engine_snap.track_fx_bypassed ? L" BYP" : L"",
		              _engine_snap.track_fx_param * 100.f);
	} else if (_engine_snap.meters_real) {
		wcsncpy_s (hint, L"PeakMeter  ·  hold/clip  ·  M/S  ·  pan  ·  send", _TRUNCATE);
	} else if (_transport_playing) {
		wcsncpy_s (hint, L"Meters: play stub (no Session)", _TRUNCATE);
	} else {
		wcsncpy_s (hint, L"M/S · faders  ·  idle until Play", _TRUNCATE);
	}
	_rt->DrawTextW (hint, (UINT32)wcslen (hint), _fmt_meta,
	                D2D1::RectF (r.left + inset + 70.f * _dpi_scale, r.top + 10.f * _dpi_scale,
	                             r.right - inset, r.top + 30.f * _dpi_scale),
	                _brush_body);

	const int n_mix = n_mixer_strips ();
	for (int i = 0; i < n_mix; ++i) {
		_mixers[i].draw (_rt, _brush_brand, _brush_body, _brush_accent, _brush_pane,
		                 _brush_pane_edge, _brush_meter_hot, _fmt_pane, _fmt_meta, _dpi_scale);
	}
	_monitor.draw (_rt, _brush_brand, _brush_body, _brush_accent, _brush_pane,
	               _brush_pane_edge, _fmt_pane, _fmt_meta, _dpi_scale);
}

void
ShellWindow::on_paint ()
{
	if (!ensure_device_resources ()) {
		return;
	}

	_rt->BeginDraw ();
	_rt->SetTransform (D2D1::Matrix3x2F::Identity ());

	D2D1_SIZE_F sz = _rt->GetSize ();
	_rt->FillRectangle (D2D1::RectF (0, 0, sz.width, sz.height), _brush_bg);

	_brush_brand->SetOpacity (0.94f + 0.06f * _pulse);
	_rt->DrawTextW (L"Airdour", 7, _fmt_brand,
	                D2D1::RectF (_brand_bar.left + 24.f * _dpi_scale, _brand_bar.top,
	                             _brand_bar.right - 24.f * _dpi_scale, _brand_bar.bottom),
	                _brush_brand);
	_brush_brand->SetOpacity (1.f);

	float rule_y = _brand_bar.bottom - 8.f * _dpi_scale;
	float rule_w = (100.f + 36.f * _pulse) * _dpi_scale;
	_rt->FillRectangle (D2D1::RectF (24.f * _dpi_scale, rule_y,
	                                 24.f * _dpi_scale + rule_w, rule_y + 2.f * _dpi_scale),
	                    _brush_accent);

	sync_transport_clock_from_engine ();
	_transport.draw (_rt, _brush_brand, _brush_body, _brush_accent, _brush_pane,
	                 _brush_pane_edge, _fmt_pane, _playhead_sec, _dpi_scale);

	draw_editor_pane ();
	draw_mixer_pane ();

	wchar_t status[384];
	if (_engine_snap.running) {
		_snwprintf_s (status, _TRUNCATE,
		              L"Engine  %s  ·  %u Hz  ·  t=%.1fs  ·  %s  ·  %s",
		              _engine_snap.backend.c_str (),
		              _engine_snap.sample_rate,
		              _playhead_sec,
		              _engine_snap.meters_real ? L"PeakMeter" : L"stub meters",
		              _engine_snap.detail.c_str ());
	} else if (_engine_snap.available) {
		_snwprintf_s (status, _TRUNCATE,
		              L"Engine idle  ·  %d tracks  ·  %s",
		              kTracks, _engine_snap.detail.c_str ());
	} else {
		_snwprintf_s (status, _TRUNCATE,
		              L"UI shell  ·  %d tracks  ·  %s",
		              kTracks, _engine_snap.detail.c_str ());
	}

	_rt->DrawTextW (status, (UINT32)wcslen (status), _fmt_meta,
	                D2D1::RectF (_status_bar.left + 20.f * _dpi_scale, _status_bar.top,
	                             _status_bar.right - 20.f * _dpi_scale, _status_bar.bottom),
	                _brush_body);

	HRESULT hr = _rt->EndDraw ();
	if (hr == D2DERR_RECREATE_TARGET) {
		discard_device_resources ();
	}
}

int
ShellWindow::run_message_loop (unsigned timeout_secs)
{
	_timeout_secs = timeout_secs;
	_start_tick = GetTickCount ();
	_last_tick_ms = _start_tick;
	_running = true;
	_engine_snap = _engine.snapshot ();

	MSG msg = {};
	while (_running && GetMessageW (&msg, nullptr, 0, 0) > 0) {
		TranslateMessage (&msg);
		DispatchMessageW (&msg);
	}

	/* Session/Graph/Butler and often backend->stop() hang under MSVC.
	 * Unattended --timeout smoke must leave immediately without joins.
	 * ExitProcess (not TerminateProcess) so CRT flushes redirected stderr. */
	if (timeout_secs > 0) {
		const unsigned exit_code = _smoke_failed ? 1u : 0u;
		std::fwprintf (stderr, L"airdour_win_shell: timeout smoke — ExitProcess(%u)\n", exit_code);
		std::fflush (stderr);
		std::fflush (stdout);
		ExitProcess (exit_code);
	}

	_engine.stop ();
	return (int)msg.wParam;
}
