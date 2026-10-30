/*
 * Copyright (C) 2026 Airdour contributors
 */

#include "shell_window.h"
#include "cairo_wave_blit.h"

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

/* Modern D2D palette — cool slate + mint accent (no purple chrome). */
constexpr D2D1_COLOR_F kBgTop      = { 0.042f, 0.052f, 0.062f, 1.f };
constexpr D2D1_COLOR_F kBgBottom   = { 0.072f, 0.095f, 0.112f, 1.f };
constexpr D2D1_COLOR_F kBrand      = { 0.960f, 0.975f, 0.985f, 1.f };
constexpr D2D1_COLOR_F kBody       = { 0.720f, 0.800f, 0.840f, 1.f };
constexpr D2D1_COLOR_F kAccent     = { 0.120f, 0.860f, 0.740f, 1.f };
constexpr D2D1_COLOR_F kPaneFill   = { 0.058f, 0.078f, 0.092f, 0.94f };
constexpr D2D1_COLOR_F kPaneEdge   = { 0.140f, 0.190f, 0.220f, 1.f };
constexpr D2D1_COLOR_F kPeak       = { 0.200f, 0.940f, 0.820f, 1.f };
constexpr D2D1_COLOR_F kRms        = { 0.100f, 0.380f, 0.360f, 0.88f };
constexpr D2D1_COLOR_F kPlayhead   = { 0.990f, 0.820f, 0.280f, 1.f };
constexpr D2D1_COLOR_F kSelection  = { 0.280f, 0.760f, 0.900f, 1.f };
constexpr D2D1_COLOR_F kLoop       = { 0.940f, 0.640f, 0.200f, 1.f };
constexpr D2D1_COLOR_F kPunch      = { 0.920f, 0.340f, 0.280f, 1.f };
constexpr D2D1_COLOR_F kMarker     = { 0.820f, 0.880f, 0.420f, 1.f };
constexpr D2D1_COLOR_F kMidi       = { 0.320f, 0.780f, 0.920f, 1.f }; /* cyan, not purple */
constexpr D2D1_COLOR_F kMeterHot   = { 0.96f, 0.38f, 0.24f, 1.f };

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

bool run_rename_dialog (HWND parent, wchar_t* name, size_t name_chars);

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
	, _midi_vel_drag (100)
	, _midi_trim_start_sec (0.f)
	, _midi_trim_end_sec (0.f)
	, _step_piano_held_count (0)
	, _transport_playing (false)
	, _playhead_sec (0.f)
	, _last_tick_ms (0)
	, _smoke_record (SmokeRecordPhase::Off)
	, _smoke_record_start_ms (0)
	, _smoke_failed (false)
	, _help_visible (false)
	, _prefs_visible (false)
	, _splash_shown_ms (0)
	, _brand_bar ()
	, _transport_bounds ()
	, _editor_pane ()
	, _editor_ruler ()
	, _midi_lane ()
	, _auto_lane_bounds ()
	, _midi_cc_lane_bounds ()
	, _mixer_pane ()
	, _mixer_strips_clip ()
	, _mixer_scroll_x (0.f)
	, _mixer_scroll_max (0.f)
	, _mixer_force_min_w (0.f)
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
	, _midi_cc_last_write_sec (-1.f)
	, _auto_lane_kind (0)
	, _midi_cc_number (7)
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
	for (int i = 0; i < 128; ++i) {
		_step_piano_held[i] = false;
	}
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

void
ShellWindow::run_smoke_check (bool ok, const char* name, std::function<bool()> fn, bool select_lanes)
{
	bool requested = has_smoke (name);
	if (!requested) {
		return;
	}
	if (ok && _engine.snapshot ().session_ok) {
		if (!fn ()) {
			_smoke_failed = true;
		} else {
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			_playhead_sec = _engine_snap.transport_sec;
			sync_transport_clock_from_engine ();
			if (select_lanes) {
				apply_region_selection_to_lanes ();
			}
		}
		std::fflush (stderr);
	} else {
		std::fprintf (stderr, "airdour_win_shell: smoke-%s FAILED (no session)\n", name);
		_smoke_failed = true;
		std::fflush (stderr);
	}
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
                            bool smoke_stereo_track,
                            bool smoke_rename,
                            bool smoke_group,
                            bool smoke_monitor,
                            bool smoke_locate,
                            bool smoke_zoom,
                            bool smoke_history,
                            bool smoke_prefs,
                            bool smoke_recent,
                            bool smoke_recovery,
                            bool smoke_stretch,
                            bool smoke_pitch,
                            bool smoke_reverse,
                            bool smoke_normalize,
                            bool smoke_strip_silence,
                            bool smoke_consolidate,
                            bool smoke_freeze,
                            bool smoke_comp,
                            bool smoke_comp_sc_hotkey,
                            bool smoke_eq,
                            bool smoke_delay,
                            bool smoke_limiter,
                            bool smoke_gate,
                            bool smoke_spectrum,
                            bool smoke_correlation,
                            bool smoke_loudness,
                            bool smoke_midi_quantize,
                            bool smoke_midi_transpose,
                            bool smoke_midi_velocity,
                            bool smoke_midi_length,
                            bool smoke_midi_clipboard,
                            bool smoke_midi_humanize,
                            bool smoke_midi_step,
                            bool smoke_midi_chord,
                            bool smoke_midi_arp,
                            bool smoke_midi_scale,
                            bool smoke_midi_cc,
                            bool smoke_midi_bend,
                            bool smoke_midi_pressure,
                            bool smoke_locate_selection,
                            bool smoke_pan_automation,
                            bool smoke_auto_lane_unify,
                            bool smoke_auto_state,
                            bool smoke_pan_auto_state,
                            bool smoke_send_auto_state,
                            bool smoke_auto_state_unify)
{
	if (!_engine.is_linked ()) {
		_engine.prepare (backend_name);
		refresh_engine_snap ();
		return false;
	}
	/* --timeout smokes: autosave off (unless --smoke-recovery). */
	_engine.set_autosave_enabled (smoke_recovery || smoke_timeout_secs == 0);
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

	if (ok && smoke_import && engine_session_ok ()) {
		if (_engine.import_test_tone (0, 220.f, 2.f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-import ok (track 0)\n");
		} else {
			std::fprintf (stderr, "airdour_win_shell: smoke-import failed\n");
		}
		std::fflush (stderr);
	}

	if (ok && smoke_save_load && engine_session_ok ()) {
		refresh_engine_snap ();
		if (!_engine_snap.track_has_regions[0]) {
			_engine.import_test_tone (0, 330.f, 1.5f);
		}
		const bool reloaded = _engine.reload_session ();
		refresh_engine_snap ();
		if (reloaded && _engine_snap.track_has_regions[0]) {
			std::fprintf (stderr, "airdour_win_shell: smoke-save-load ok (regions restored)\n");
		} else {
			std::fprintf (stderr, "airdour_win_shell: smoke-save-load FAILED (saved=%d regions0=%d)\n",
			              _engine_snap.session_saved ? 1 : 0,
			              _engine_snap.track_has_regions[0] ? 1 : 0);
		}
		std::fflush (stderr);
	}

	if (ok && smoke_export && engine_session_ok ()) {
		std::string exported;
		if (_engine.smoke_export_wav (&exported)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-export ok → %s\n", exported.c_str ());
		} else {
			std::fprintf (stderr, "airdour_win_shell: smoke-export FAILED\n");
		}
		std::fflush (stderr);
	}

	if (ok && smoke_undo && engine_session_ok ()) {
		if (_engine.smoke_undo ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-undo ok\n");
		} else {
			std::fprintf (stderr, "airdour_win_shell: smoke-undo FAILED\n");
		}
		std::fflush (stderr);
	}

	if (ok && smoke_loop && engine_session_ok ()) {
		if (_engine.smoke_loop_markers ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-loop ok\n");
		} else {
			std::fprintf (stderr, "airdour_win_shell: smoke-loop FAILED\n");
		}
		std::fflush (stderr);
	}

	if (ok && smoke_midi_edit && engine_session_ok ()) {
		if (_engine.smoke_midi_edit ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-edit ok\n");
		} else {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-edit FAILED\n");
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (ok && smoke_midi_quantize && engine_session_ok ()) {
		if (!_engine.smoke_midi_quantize ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_midi_quantize) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-quantize FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	} else if (ok && smoke_midi_transpose && engine_session_ok ()) {
		if (!_engine.smoke_midi_transpose ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_midi_transpose) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-transpose FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	} else if (ok && smoke_midi_velocity && engine_session_ok ()) {
		if (!_engine.smoke_midi_velocity ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_midi_velocity) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-velocity FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	} else if (ok && smoke_midi_length && engine_session_ok ()) {
		if (!_engine.smoke_midi_length ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_midi_length) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-length FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	} else if (ok && smoke_midi_clipboard && engine_session_ok ()) {
		if (!_engine.smoke_midi_clipboard ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_midi_clipboard) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-clipboard FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	} else if (ok && smoke_midi_humanize && engine_session_ok ()) {
		if (!_engine.smoke_midi_humanize ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_midi_humanize) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-humanize FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	} else if (ok && smoke_midi_step && engine_session_ok ()) {
		if (!_engine.smoke_midi_step ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_midi_step) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-step FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	} else if (ok && smoke_midi_chord && engine_session_ok ()) {
		if (!_engine.smoke_midi_chord ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_midi_chord) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-chord FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	} else if (ok && smoke_midi_arp && engine_session_ok ()) {
		if (!_engine.smoke_midi_arp ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_midi_arp) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-arp FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	} else if (ok && smoke_midi_scale && engine_session_ok ()) {
		if (!_engine.smoke_midi_scale ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_midi_scale) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-scale FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	} else if (ok && smoke_midi_cc && engine_session_ok ()) {
		if (!_engine.smoke_midi_cc ()) {
			_smoke_failed = true;
		} else {
			refresh_midi_cc_lane ();
		}
		std::fflush (stderr);
	} else if (smoke_midi_cc) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-cc FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	} else if (ok && smoke_midi_bend && engine_session_ok ()) {
		if (!_engine.smoke_midi_bend ()) {
			_smoke_failed = true;
		} else {
			_midi_cc_number = -1;
			refresh_midi_cc_lane ();
		}
		std::fflush (stderr);
	} else if (smoke_midi_bend) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-bend FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	} else if (ok && smoke_midi_pressure && engine_session_ok ()) {
		if (!_engine.smoke_midi_pressure ()) {
			_smoke_failed = true;
		} else {
			_midi_cc_number = -2;
			refresh_midi_cc_lane ();
		}
		std::fflush (stderr);
	} else if (smoke_midi_pressure) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-pressure FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	} else if (ok && has_smoke ("master-fx") && engine_session_ok ()) {
		if (!_engine.smoke_master_fx ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (has_smoke ("master-fx")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-master-fx FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	} else if (ok && smoke_midi && engine_session_ok ()) {
		if (_engine.smoke_midi ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi ok\n");
		} else {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi FAILED\n");
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (ok && has_smoke ("vca") && engine_session_ok ()) {
		if (!_engine.smoke_vca ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (has_smoke ("vca")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-vca FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	} else if (ok && has_smoke ("group-vca") && engine_session_ok ()) {
		if (!_engine.smoke_group_vca ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (has_smoke ("group-vca")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-group-vca FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	} else if (ok && has_smoke ("vca-status") && engine_session_ok ()) {
		if (!_engine.smoke_vca_status ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (has_smoke ("vca-status")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-vca-status FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	} else if (ok && engine_session_ok ()) {
		/* Always scaffold a MIDI track + empty region so the piano-roll is live. */
		_engine.ensure_midi_track ();
		_engine.ensure_midi_region ();
	}

	if (ok && has_smoke ("pan-reset-all") && engine_session_ok ()) {
		if (!_engine.smoke_pan_reset_all ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_pan_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("pan-reset-all")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-pan-reset-all FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("track-color") && engine_session_ok ()) {
		if (!_engine.smoke_track_color ()) {
			_smoke_failed = true;
		} else {
			sync_track_labels_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("track-color")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-track-color FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("send-reset-all") && engine_session_ok ()) {
		if (!_engine.smoke_send_reset_all ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_send_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("send-reset-all")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-send-reset-all FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("fader-reset-all") && engine_session_ok ()) {
		if (!_engine.smoke_fader_reset_all ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_gains_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("fader-reset-all")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-fader-reset-all FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("disarm-all") && engine_session_ok ()) {
		if (!_engine.smoke_disarm_all ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_arm_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("disarm-all")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-disarm-all FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("clear-meter-clips") && engine_session_ok ()) {
		if (!_engine.smoke_clear_meter_clips ()) {
			_smoke_failed = true;
		} else {
			for (int i = 0; i < kMixerStripsMax; ++i) {
				_mixers[i].clear_meter_clip ();
			}
		}
		std::fflush (stderr);
	} else if (has_smoke ("clear-meter-clips")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-clear-meter-clips FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("fx-bypass-all") && engine_session_ok ()) {
		if (!_engine.smoke_fx_bypass_all ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_fx_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("fx-bypass-all")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-fx-bypass-all FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("fx-activate-all") && engine_session_ok ()) {
		if (!_engine.smoke_fx_activate_all ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_fx_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("fx-activate-all")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-fx-activate-all FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_plugin && engine_session_ok ()) {
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

	if (ok && has_smoke ("fx-param-reset") && engine_session_ok ()) {
		if (!_engine.smoke_fx_param_reset ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_fx_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("fx-param-reset")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-fx-param-reset FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_comp && engine_session_ok ()) {
		if (!_engine.smoke_comp ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_fx_from_engine ();
		}
		std::fflush (stderr);
	} else if (smoke_comp) {
		std::fprintf (stderr, "airdour_win_shell: smoke-comp FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_comp_sc_hotkey && _engine.snapshot ().session_ok) {
		if (!_engine.smoke_comp_sc_hotkey ()) {
			_smoke_failed = true;
		} else {
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_fx_from_engine ();
		}
		std::fflush (stderr);
	} else if (smoke_comp_sc_hotkey) {
		std::fprintf (stderr, "airdour_win_shell: smoke-comp-sc-hotkey FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}


	if (ok && smoke_eq && _engine.snapshot ().session_ok) {
		if (!_engine.smoke_eq ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_fx_from_engine ();
		}
		std::fflush (stderr);
	} else if (smoke_eq) {
		std::fprintf (stderr, "airdour_win_shell: smoke-eq FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_delay && engine_session_ok ()) {
		if (!_engine.smoke_delay ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_fx_from_engine ();
		}
		std::fflush (stderr);
	} else if (smoke_delay) {
		std::fprintf (stderr, "airdour_win_shell: smoke-delay FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_limiter && engine_session_ok ()) {
		if (!_engine.smoke_limiter ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_fx_from_engine ();
		}
		std::fflush (stderr);
	} else if (smoke_limiter) {
		std::fprintf (stderr, "airdour_win_shell: smoke-limiter FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_gate && engine_session_ok ()) {
		if (!_engine.smoke_gate ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_fx_from_engine ();
		}
		std::fflush (stderr);
	} else if (smoke_gate) {
		std::fprintf (stderr, "airdour_win_shell: smoke-gate FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_spectrum && engine_session_ok ()) {
		if (!_engine.smoke_spectrum ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_spectrum) {
		std::fprintf (stderr, "airdour_win_shell: smoke-spectrum FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_correlation && engine_session_ok ()) {
		if (!_engine.smoke_correlation ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_correlation) {
		std::fprintf (stderr, "airdour_win_shell: smoke-correlation FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_loudness && engine_session_ok ()) {
		if (!_engine.smoke_loudness ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_loudness) {
		std::fprintf (stderr, "airdour_win_shell: smoke-loudness FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_vst3 && engine_session_ok ()) {
		if (!_engine.smoke_vst3 ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_vst3) {
		std::fprintf (stderr, "airdour_win_shell: smoke-vst3 FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_mute_solo && engine_session_ok ()) {
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

	if (ok && has_smoke ("solo-isolate") && engine_session_ok ()) {
		if (!_engine.smoke_solo_isolate ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_solo_isolate_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("solo-isolate")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-solo-isolate FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("meter-point") && engine_session_ok ()) {
		if (!_engine.smoke_meter_point ()) {
			_smoke_failed = true;
		} else {
			const int na = n_audio_lanes ();
			for (int i = 0; i < na && i < EngineSnapshot::kMeters; ++i) {
				_mixers[i].set_meter_point (_engine.snapshot ().track_meter_point[i]);
			}
		}
		std::fflush (stderr);
	} else if (has_smoke ("meter-point")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-meter-point FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("meter-type") && engine_session_ok ()) {
		if (!_engine.smoke_meter_type ()) {
			_smoke_failed = true;
		} else {
			const int na = n_audio_lanes ();
			for (int i = 0; i < na && i < EngineSnapshot::kMeters; ++i) {
				_mixers[i].set_meter_type (_engine.snapshot ().track_meter_type[i]);
			}
		}
		std::fflush (stderr);
	} else if (has_smoke ("meter-type")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-meter-type FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("input-monitor") && engine_session_ok ()) {
		if (!_engine.smoke_input_monitor ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_monitor_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("input-monitor")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-input-monitor FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("listen-mode") && engine_session_ok ()) {
		if (!_engine.smoke_listen_mode ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_listen_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("listen-mode")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-listen-mode FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("mon-peer-badge") && engine_session_ok ()) {
		if (!_engine.smoke_mon_peer_badge ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_monitor_from_engine ();
			sync_mixer_group_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("mon-peer-badge")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-mon-peer-badge FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("phase-invert") && engine_session_ok ()) {
		if (!_engine.smoke_phase_invert ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_phase_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("phase-invert")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-phase-invert FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("track-comment") && engine_session_ok ()) {
		if (!_engine.smoke_track_comment ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_comment_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("track-comment")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-track-comment FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("solo-safe") && engine_session_ok ()) {
		if (!_engine.smoke_solo_safe ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_solo_safe_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("solo-safe")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-solo-safe FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("arm-all") && engine_session_ok ()) {
		if (!_engine.smoke_arm_all ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_arm_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("arm-all")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-arm-all FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("record-safe") && engine_session_ok ()) {
		if (!_engine.smoke_record_safe ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_arm_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("record-safe")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-record-safe FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("mute-points") && engine_session_ok ()) {
		if (!_engine.smoke_mute_points ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_mute_solo_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("mute-points")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-mute-points FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("clear-solos") && engine_session_ok ()) {
		if (!_engine.smoke_clear_solos ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_mute_solo_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("clear-solos")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-clear-solos FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("clear-mutes") && engine_session_ok ()) {
		if (!_engine.smoke_clear_mutes ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_mute_solo_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("clear-mutes")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-clear-mutes FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("exclusive-solo") && engine_session_ok ()) {
		if (!_engine.smoke_exclusive_solo ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_mute_solo_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("exclusive-solo")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-exclusive-solo FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_pan && engine_session_ok ()) {
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

	if (ok && has_smoke ("pan-width") && engine_session_ok ()) {
		if (!_engine.smoke_pan_width ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_pan_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("pan-width")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-pan-width FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("mixer-reset") && engine_session_ok ()) {
		if (!_engine.smoke_mixer_reset ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_gains_from_engine ();
			sync_mixer_pan_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("mixer-reset")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-mixer-reset FAILED (no session)\n");
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
		std::fprintf (stderr, "airdour_win_shell: smoke-strip-trim FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_send && engine_session_ok ()) {
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

	if (ok && has_smoke ("strip-mixer-reset") && engine_session_ok ()) {
		if (!_engine.smoke_strip_mixer_reset ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_gains_from_engine ();
			sync_mixer_pan_from_engine ();
			sync_mixer_send_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("strip-mixer-reset")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-strip-mixer-reset FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("aux-return") && engine_session_ok ()) {
		if (!_engine.smoke_aux_return ()) {
			_smoke_failed = true;
		} else {
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_monitor_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("aux-return")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-aux-return FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("send-pan") && engine_session_ok ()) {
		if (!_engine.smoke_send_pan ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_send_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("send-pan")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-send-pan FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("send-reset") && engine_session_ok ()) {
		if (!_engine.smoke_send_reset ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_send_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("send-reset")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-send-reset FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("send-mute") && engine_session_ok ()) {
		if (!_engine.smoke_send_mute ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_send_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("send-mute")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-send-mute FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_automation && engine_session_ok ()) {
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

	if (ok && smoke_pan_automation && engine_session_ok ()) {
		_auto_lane_kind = 1;
		if (!_engine.smoke_pan_automation ()) {
			_smoke_failed = true;
		} else {
			refresh_automation_lane ();
		}
		std::fflush (stderr);
	} else if (smoke_pan_automation) {
		std::fprintf (stderr, "airdour_win_shell: smoke-pan-automation FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("auto-lane-focus") && engine_session_ok ()) {
		/* Focus Audio 2 so the D2D lane label/curve target matches smoke writes. */
		_focus_audio_track = 1;
		if (!_engine.smoke_auto_lane_focus ()) {
			_smoke_failed = true;
		} else {
			refresh_automation_lane ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("auto-lane-focus")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-auto-lane-focus FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_auto_lane_unify && engine_session_ok ()) {
		_auto_lane_kind = 0;
		if (!_engine.smoke_auto_lane_unify ()) {
			_smoke_failed = true;
		} else {
			_auto_lane_kind = 2; /* leave on Send after unify */
			refresh_automation_lane ();
		}
		std::fflush (stderr);
	} else if (smoke_auto_lane_unify) {
		std::fprintf (stderr, "airdour_win_shell: smoke-auto-lane-unify FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_auto_state && engine_session_ok ()) {
		if (!_engine.smoke_auto_state ()) {
			_smoke_failed = true;
		} else {
			refresh_automation_lane ();
		}
		std::fflush (stderr);
	} else if (smoke_auto_state) {
		std::fprintf (stderr, "airdour_win_shell: smoke-auto-state FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_pan_auto_state && engine_session_ok ()) {
		if (!_engine.smoke_pan_auto_state ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_pan_from_engine ();
			refresh_automation_lane ();
		}
		std::fflush (stderr);
	} else if (smoke_pan_auto_state) {
		std::fprintf (stderr, "airdour_win_shell: smoke-pan-auto-state FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_send_auto_state && engine_session_ok ()) {
		if (!_engine.smoke_send_auto_state ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_send_from_engine ();
			refresh_automation_lane ();
		}
		std::fflush (stderr);
	} else if (smoke_send_auto_state) {
		std::fprintf (stderr, "airdour_win_shell: smoke-send-auto-state FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_auto_state_unify && engine_session_ok ()) {
		_auto_lane_kind = 0;
		if (!_engine.smoke_auto_state_unify ()) {
			_smoke_failed = true;
		} else {
			refresh_automation_lane ();
		}
		std::fflush (stderr);
	} else if (smoke_auto_state_unify) {
		std::fprintf (stderr, "airdour_win_shell: smoke-auto-state-unify FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_edit && engine_session_ok ()) {
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

	if (ok && smoke_stretch && engine_session_ok ()) {
		if (!_engine.smoke_stretch ()) {
			_smoke_failed = true;
		} else {
			apply_region_selection_to_lanes ();
		}
		std::fflush (stderr);
	} else if (smoke_stretch) {
		std::fprintf (stderr, "airdour_win_shell: smoke-stretch FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_pitch && engine_session_ok ()) {
		if (!_engine.smoke_pitch ()) {
			_smoke_failed = true;
		} else {
			apply_region_selection_to_lanes ();
		}
		std::fflush (stderr);
	} else if (smoke_pitch) {
		std::fprintf (stderr, "airdour_win_shell: smoke-pitch FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_reverse && engine_session_ok ()) {
		if (!_engine.smoke_reverse ()) {
			_smoke_failed = true;
		} else {
			apply_region_selection_to_lanes ();
		}
		std::fflush (stderr);
	} else if (smoke_reverse) {
		std::fprintf (stderr, "airdour_win_shell: smoke-reverse FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_normalize && engine_session_ok ()) {
		if (!_engine.smoke_normalize ()) {
			_smoke_failed = true;
		} else {
			apply_region_selection_to_lanes ();
		}
		std::fflush (stderr);
	} else if (smoke_normalize) {
		std::fprintf (stderr, "airdour_win_shell: smoke-normalize FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_strip_silence && engine_session_ok ()) {
		if (!_engine.smoke_strip_silence ()) {
			_smoke_failed = true;
		} else {
			apply_region_selection_to_lanes ();
		}
		std::fflush (stderr);
	} else if (smoke_strip_silence) {
		std::fprintf (stderr, "airdour_win_shell: smoke-strip-silence FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_consolidate && engine_session_ok ()) {
		if (!_engine.smoke_consolidate ()) {
			_smoke_failed = true;
		} else {
			apply_region_selection_to_lanes ();
		}
		std::fflush (stderr);
	} else if (smoke_consolidate) {
		std::fprintf (stderr, "airdour_win_shell: smoke-consolidate FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_freeze && engine_session_ok ()) {
		if (!_engine.smoke_freeze ()) {
			_smoke_failed = true;
		} else {
			apply_region_selection_to_lanes ();
			sync_transport_freeze_from_engine ();
		}
		std::fflush (stderr);
	} else if (smoke_freeze) {
		std::fprintf (stderr, "airdour_win_shell: smoke-freeze FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_snap && engine_session_ok ()) {
		if (!_engine.smoke_snap ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_snap) {
		std::fprintf (stderr, "airdour_win_shell: smoke-snap FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_punch && engine_session_ok ()) {
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

	if (ok && smoke_tempo && engine_session_ok ()) {
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

	if (ok && smoke_click && engine_session_ok ()) {
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

	if (ok && has_smoke ("click-gain") && engine_session_ok ()) {
		if (!_engine.smoke_click_gain ()) {
			_smoke_failed = true;
		} else {
			sync_transport_tempo_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("click-gain")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-click-gain FAILED (no session)\n");
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

	if (ok && smoke_stereo_track && engine_session_ok ()) {
		if (!_engine.smoke_stereo_track ()) {
			_smoke_failed = true;
		} else {
			sync_track_labels_from_engine ();
			sync_mixer_gains_from_engine ();
			sync_mixer_pan_from_engine ();
			sync_mixer_send_from_engine ();
			layout_panes ();
		}
		std::fflush (stderr);
	} else if (smoke_stereo_track) {
		std::fprintf (stderr, "airdour_win_shell: smoke-stereo-track FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_rename && engine_session_ok ()) {
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

	if (ok && smoke_group && engine_session_ok ()) {
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

	if (ok && has_smoke ("narrow-mixer")) {
		if (!_engine.smoke_narrow_mixer ()) {
			_smoke_failed = true;
		} else {
			_engine_snap = _engine.snapshot ();
			layout_panes ();
		}
		std::fflush (stderr);
	}

	if (ok && has_smoke ("group-recenable") && engine_session_ok ()) {
		if (!_engine.smoke_group_recenable ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_group_from_engine ();
			sync_mixer_arm_from_engine ();
			layout_panes ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("group-recenable")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-group-recenable FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("group-relative") && engine_session_ok ()) {
		if (!_engine.smoke_group_relative ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_group_from_engine ();
			sync_mixer_gains_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("group-relative")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-group-relative FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("mixer-scroll") && engine_session_ok ()) {
		if (!smoke_mixer_scroll_layout ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (has_smoke ("mixer-scroll")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-mixer-scroll FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("group-color") && engine_session_ok ()) {
		if (!_engine.smoke_group_color ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_group_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("group-color")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-group-color FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("group-monitoring") && engine_session_ok ()) {
		if (!_engine.smoke_group_monitoring ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_group_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("group-monitoring")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-group-monitoring FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("group-route-active") && engine_session_ok ()) {
		if (!_engine.smoke_group_route_active ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_group_from_engine ();
			sync_mixer_mute_solo_from_engine ();
			layout_panes ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("group-route-active")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-group-route-active FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("group-select") && engine_session_ok ()) {
		if (!_engine.smoke_group_select ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_group_from_engine ();
			layout_panes ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("group-select")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-group-select FAILED (no session)\n");
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

	if (ok && has_smoke ("monitor-dim-level") && engine_session_ok ()) {
		if (!_engine.smoke_monitor_dim_level ()) {
			_smoke_failed = true;
		} else {
			sync_monitor_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("monitor-dim-level")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-monitor-dim-level FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("solo-boost") && engine_session_ok ()) {
		if (!_engine.smoke_solo_boost ()) {
			_smoke_failed = true;
		} else {
			sync_monitor_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("solo-boost")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-solo-boost FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_locate && engine_session_ok ()) {
		if (!_engine.smoke_locate ()) {
			_smoke_failed = true;
		} else {
			_engine.poll ();
			refresh_engine_snap ();
			_playhead_sec = _engine_snap.transport_sec;
			sync_transport_clock_from_engine ();
		}
		std::fflush (stderr);
	} else if (smoke_locate) {
		std::fprintf (stderr, "airdour_win_shell: smoke-locate FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_locate_selection && engine_session_ok ()) {
		if (!_engine.smoke_locate_selection ()) {
			_smoke_failed = true;
		} else {
			_engine.poll ();
			refresh_engine_snap ();
			_playhead_sec = _engine_snap.transport_sec;
			sync_transport_clock_from_engine ();
			apply_region_selection_to_lanes ();
		}
		std::fflush (stderr);
	} else if (smoke_locate_selection) {
		std::fprintf (stderr, "airdour_win_shell: smoke-locate-selection FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}
	run_smoke_check (ok, "marker-navigate", [this]() { return _engine.smoke_marker_navigate (); });
	run_smoke_check (ok, "marker-rename", [this]() { return _engine.smoke_marker_rename (); });
	run_smoke_check (ok, "marker-name-ellipsis", [this]() { return smoke_marker_name_ellipsis (); });
	run_smoke_check (ok, "marker-lock", [this]() { return _engine.smoke_marker_lock (); });
	run_smoke_check (ok, "cd-marker", [this]() { return _engine.smoke_cd_marker (); });
	run_smoke_check (ok, "range-marker", [this]() { return _engine.smoke_range_marker (); });
	run_smoke_check (ok, "region-sync", [this]() { return _engine.smoke_region_sync (); });
	run_smoke_check (ok, "region-sync-draw", [this]() { return smoke_region_sync_draw (); }, true);
	run_smoke_check (ok, "scene-channel", [this]() { return _engine.smoke_scene_channel (); });
	run_smoke_check (ok, "cue-navigate", [this]() { return _engine.smoke_cue_navigate (); });
	run_smoke_check (ok, "section-rename", [this]() { return _engine.smoke_section_rename (); });
	run_smoke_check (ok, "punch-navigate", [this]() { return _engine.smoke_punch_navigate (); });
	run_smoke_check (ok, "session-extent-navigate", [this]() { return _engine.smoke_session_extent_navigate (); });
	run_smoke_check (ok, "loop-navigate", [this]() { return _engine.smoke_loop_navigate (); });
	run_smoke_check (ok, "playhead-nudge", [this]() { return _engine.smoke_playhead_nudge (); });
	run_smoke_check (ok, "zoom-punch", [this]() {
		if (!_engine.smoke_zoom_punch ()) return false;
		_engine.poll ();
		refresh_engine_snap ();
		sync_transport_punch_from_engine ();
		float before = _lanes[0].strip.view_span_sec ();
		do_zoom_to_punch ();
		float span = _lanes[0].strip.view_span_sec ();
		if (span < 1.8f || span > 2.8f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-zoom-punch FAILED (span=%.2f before=%.2f)\n", span, before);
			return false;
		}
		std::fprintf (stderr, "airdour_win_shell: smoke-zoom-punch ok (span=%.2fs)\n", span);
		layout_panes ();
		return true;
	});
	run_smoke_check (ok, "zoom-loop", [this]() {
		if (!_engine.smoke_zoom_loop ()) return false;
		_engine.poll ();
		refresh_engine_snap ();
		sync_transport_loop_from_engine ();
		apply_loop_to_lanes ();
		float before = _lanes[0].strip.view_span_sec ();
		do_zoom_to_loop ();
		float span = _lanes[0].strip.view_span_sec ();
		if (span < 1.8f || span > 2.8f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-zoom-loop FAILED (span=%.2f before=%.2f)\n", span, before);
			return false;
		}
		std::fprintf (stderr, "airdour_win_shell: smoke-zoom-loop ok (span=%.2fs)\n", span);
		layout_panes ();
		return true;
	});
	run_smoke_check (ok, "zoom-selection", [this]() {
		if (!_engine.smoke_zoom_selection ()) return false;
		_engine.poll ();
		refresh_engine_snap ();
		apply_region_selection_to_lanes ();
		float before = _lanes[0].strip.view_span_sec ();
		float want = _engine_snap.selection_end_sec - _engine_snap.selection_start_sec;
		do_zoom_to_selection ();
		float span = _lanes[0].strip.view_span_sec ();
		float expect = want * 1.12f;
		if (span < expect * 0.85f || span > expect * 1.25f + 0.3f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-zoom-selection FAILED (span=%.2f want≈%.2f before=%.2f)\n", span, expect, before);
			return false;
		}
		std::fprintf (stderr, "airdour_win_shell: smoke-zoom-selection ok (span=%.2fs)\n", span);
		layout_panes ();
		return true;
	});
	run_smoke_check (ok, "zoom-session", [this]() {
		if (!_engine.smoke_zoom_session ()) return false;
		_engine.poll ();
		refresh_engine_snap ();
		float before = _lanes[0].strip.view_span_sec ();
		float s0 = 0.f, e0 = 0.f;
		float want = 2.f;
		if (_engine.session_extent_sec (&s0, &e0) && e0 > s0) {
			want = e0 - s0;
		}
		do_zoom_to_session ();
		float span = _lanes[0].strip.view_span_sec ();
		float expect = want * 1.12f;
		if (span < expect * 0.85f || span > expect * 1.25f + 0.3f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-zoom-session FAILED (span=%.2f want≈%.2f before=%.2f)\n", span, expect, before);
			return false;
		}
		std::fprintf (stderr, "airdour_win_shell: smoke-zoom-session ok (span=%.2fs)\n", span);
		layout_panes ();
		return true;
	});
	run_smoke_check (ok, "zoom-regions", [this]() {
		if (!_engine.smoke_zoom_regions ()) return false;
		_engine.poll ();
		refresh_engine_snap ();
		float before = _lanes[0].strip.view_span_sec ();
		float s0 = 0.f, e0 = 0.f;
		float want = 2.f;
		if (_engine.regions_extent_sec (&s0, &e0) && e0 > s0) {
			want = e0 - s0;
		}
		do_zoom_to_regions ();
		float span = _lanes[0].strip.view_span_sec ();
		float expect = want * 1.12f;
		if (span < expect * 0.85f || span > expect * 1.25f + 0.3f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-zoom-regions FAILED (span=%.2f want≈%.2f before=%.2f)\n", span, expect, before);
			return false;
		}
		std::fprintf (stderr, "airdour_win_shell: smoke-zoom-regions ok (span=%.2fs)\n", span);
		layout_panes ();
		return true;
	});
	run_smoke_check (ok, "center-playhead", [this]() {
		if (!_engine.smoke_center_playhead ()) return false;
		refresh_engine_snap ();
		_playhead_sec = _engine_snap.transport_sec;
		sync_transport_clock_from_engine ();
		_lanes[0].strip.set_view_window (0.f, 2.f);
		apply_shared_view (0);
		float before_mid = _lanes[0].strip.view_start_sec () + 0.5f * _lanes[0].strip.view_span_sec ();
		do_center_on_playhead ();
		float span = _lanes[0].strip.view_span_sec ();
		float mid = _lanes[0].strip.view_start_sec () + 0.5f * span;
		float ph = _playhead_sec;
		if (std::fabs (mid - ph) > std::max (0.15f, span * 0.12f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-center-playhead FAILED (mid=%.3f ph=%.3f before_mid=%.3f)\n", mid, ph, before_mid);
			return false;
		}
		std::fprintf (stderr, "airdour_win_shell: smoke-center-playhead ok (mid=%.3f ph=%.3f)\n", mid, ph);
		layout_panes ();
		return true;
	});
	run_smoke_check (ok, "zoom-playhead", [this]() {
		if (!_engine.smoke_zoom_playhead ()) return false;
		refresh_engine_snap ();
		_playhead_sec = _engine_snap.transport_sec;
		sync_transport_clock_from_engine ();
		_lanes[0].strip.set_view_window (0.f, 8.f);
		apply_shared_view (0);
		float before = _lanes[0].strip.view_span_sec ();
		do_zoom_around_playhead (1.15f);
		float span = _lanes[0].strip.view_span_sec ();
		float start = _lanes[0].strip.view_start_sec ();
		float mid = start + 0.5f * span;
		if (span >= before * 0.98f || span < before / 1.15f * 0.85f
		    || std::fabs (mid - _playhead_sec) > std::max (0.2f, span * 0.15f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-zoom-playhead FAILED (span=%.3f before=%.3f mid=%.3f ph=%.3f)\n", span, before, mid, _playhead_sec);
			return false;
		}
		std::fprintf (stderr, "airdour_win_shell: smoke-zoom-playhead ok (span=%.3f→%.3f mid=%.3f)\n", before, span, mid);
		layout_panes ();
		return true;
	});
	run_smoke_check (ok, "scroll-playhead", [this]() {
		if (!_engine.smoke_scroll_playhead ()) return false;
		refresh_engine_snap ();
		_playhead_sec = _engine_snap.transport_sec;
		sync_transport_clock_from_engine ();
		_lanes[0].strip.set_view_window (0.f, 2.f);
		apply_shared_view (0);
		do_scroll_playhead_into_view ();
		float start = _lanes[0].strip.view_start_sec ();
		float span = _lanes[0].strip.view_span_sec ();
		float end = start + span;
		float ph = _playhead_sec;
		if (ph < start || ph > end) {
			std::fprintf (stderr, "airdour_win_shell: smoke-scroll-playhead FAILED (ph=%.3f view=[%.3f..%.3f])\n", ph, start, end);
			return false;
		}
		std::fprintf (stderr, "airdour_win_shell: smoke-scroll-playhead ok (ph=%.3f view=[%.3f..%.3f])\n", ph, start, end);
		layout_panes ();
		return true;
	});
	run_smoke_check (ok, "region-mute-gain", [this]() { return _engine.smoke_region_mute_gain (); });
	run_smoke_check (ok, "region-lock", [this]() { return _engine.smoke_region_lock (); });
	run_smoke_check (ok, "fade-nudge", [this]() { return _engine.smoke_fade_nudge (); });
	run_smoke_check (ok, "region-duplicate", [this]() { return _engine.smoke_region_duplicate (); });
	run_smoke_check (ok, "region-nudge", [this]() { return _engine.smoke_region_nudge (); });
	run_smoke_check (ok, "region-layer", [this]() { return _engine.smoke_region_layer (); });
	run_smoke_check (ok, "region-opaque", [this]() { return _engine.smoke_region_opaque (); });
	run_smoke_check (ok, "export-selection", [this]() { return _engine.smoke_export_selection (); });
	run_smoke_check (ok, "punch-selection", [this]() { return _engine.smoke_punch_selection (); });
	run_smoke_check (ok, "region-rename", [this]() { return _engine.smoke_region_rename (); });
	run_smoke_check (ok, "region-caption", [this]() { return _engine.smoke_region_caption (); });
	run_smoke_check (ok, "loop-selection", [this]() { return _engine.smoke_loop_selection (); });
	run_smoke_check (ok, "crossfade-handles", [this]() { return _engine.smoke_crossfade_handles (); });
	run_smoke_check (ok, "crossfade-shape", [this]() { return _engine.smoke_crossfade_shape (); });
	run_smoke_check (ok, "fade-shape-expand", [this]() { return _engine.smoke_fade_shape_expand (); });
	run_smoke_check (ok, "region-envelope", [this]() { return _engine.smoke_region_envelope (); });
	run_smoke_check (ok, "region-envelope-nudge", [this]() { return _engine.smoke_region_envelope_nudge (); });
	run_smoke_check (ok, "region-envelope-drag", [this]() { return _engine.smoke_region_envelope_drag (); });
	run_smoke_check (ok, "envelope-mid-handle", [this]() { return smoke_envelope_mid_handle (); }, true);
	run_smoke_check (ok, "fade-active", [this]() { return _engine.smoke_fade_active (); });
	run_smoke_check (ok, "region-position-lock", [this]() { return _engine.smoke_region_position_lock (); });
	run_smoke_check (ok, "skip-range", [this]() { return _engine.smoke_skip_range (); });
	run_smoke_check (ok, "region-cycle", [this]() { return _engine.smoke_region_cycle (); });
	run_smoke_check (ok, "region-layer-cycle", [this]() { return _engine.smoke_region_layer_cycle (); });
	run_smoke_check (ok, "clear-selection", [this]() { return _engine.smoke_clear_selection (); });
	run_smoke_check (ok, "snap-toggle", [this]() { return _engine.smoke_snap_toggle (); });
	run_smoke_check (ok, "snap-density", [this]() { return _engine.smoke_snap_density (); });
	run_smoke_check (ok, "snap-density-label", [this]() { return smoke_snap_density_label (); });
	run_smoke_check (ok, "grid-refresh", [this]() { return _engine.smoke_grid_lines (); });
	run_smoke_check (ok, "tempo-nudge", [this]() { return _engine.smoke_tempo_nudge (); });
	run_smoke_check (ok, "count-in", [this]() { return _engine.smoke_count_in (); });
	run_smoke_check (ok, "click-toggle", [this]() { return _engine.smoke_click (); });
	run_smoke_check (ok, "punch-toggle", [this]() { return _engine.smoke_punch (); });
	run_smoke_check (ok, "midi-note-pressure", [this]() {
		if (!_engine.smoke_midi_note_pressure ()) {
			return false;
		}
		_midi_cc_number = -3;
		refresh_midi_cc_lane ();
		return true;
	});
	run_smoke_check (ok, "midi-vel-chord", [this]() { return smoke_midi_vel_chord (); });
	run_smoke_check (ok, "midi-duplicate", [this]() { return _engine.smoke_midi_duplicate (); });
	run_smoke_check (ok, "midi-tempo-map", [this]() { return _engine.smoke_midi_tempo_map (); });
	run_smoke_check (ok, "midi-panic", [this]() { return _engine.smoke_midi_panic (); });
	run_smoke_check (ok, "stop-midi-panic", [this]() { return _engine.smoke_stop_midi_panic (); });
	run_smoke_check (ok, "midi-audition", [this]() { return _engine.smoke_midi_audition (); });
	run_smoke_check (ok, "midi-legato", [this]() { return _engine.smoke_midi_legato (); });
	run_smoke_check (ok, "midi-program", [this]() { return _engine.smoke_midi_program (); });
	run_smoke_check (ok, "vst3-params", [this]() { return _engine.smoke_vst3_params (); });
	run_smoke_check (ok, "export-stems", [this]() { return _engine.smoke_export_stems (); });
	run_smoke_check (ok, "mute-all", [this]() { return _engine.smoke_mute_all (); });
	run_smoke_check (ok, "solo-all", [this]() { return _engine.smoke_solo_all (); });
	run_smoke_check (ok, "midi-arm", [this]() { return _engine.smoke_midi_arm (); });
	run_smoke_check (true, "cairo-wave", []() {
		if (!CairoWaveBlit::available ()) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-cairo-wave FAILED (AIRDOUR_HAVE_CAIRO off — run Install-HeadlessDeps -WithGtkLibs)\n");
			return false;
		}
		return CairoWaveBlit::smoke_offline ();
	});

	if (ok && smoke_zoom && engine_session_ok ()) {
		if (!_engine.smoke_zoom ()) {
			_smoke_failed = true;
		} else {
			_engine.poll ();
			refresh_engine_snap ();
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

	if (ok && smoke_history && engine_session_ok ()) {
		if (!_engine.smoke_history ()) {
			_smoke_failed = true;
		} else {
			_engine.poll ();
			refresh_engine_snap ();
			sync_history_from_engine ();
			layout_panes ();
		}
		std::fflush (stderr);
	} else if (smoke_history) {
		std::fprintf (stderr, "airdour_win_shell: smoke-history FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_prefs && engine_session_ok ()) {
		if (!_engine.smoke_prefs ()) {
			_smoke_failed = true;
		} else {
			_engine.poll ();
			refresh_engine_snap ();
			sync_prefs_from_engine ();
		}
		std::fflush (stderr);
	} else if (smoke_prefs) {
		std::fprintf (stderr, "airdour_win_shell: smoke-prefs FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_recent && engine_session_ok ()) {
		if (!_engine.smoke_recent ()) {
			_smoke_failed = true;
		} else {
			_engine.poll ();
			refresh_engine_snap ();
			sync_recent_from_engine ();
		}
		std::fflush (stderr);
	} else if (smoke_recent) {
		std::fprintf (stderr, "airdour_win_shell: smoke-recent FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_recovery && engine_session_ok ()) {
		if (!_engine.smoke_recovery ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_recovery) {
		std::fprintf (stderr, "airdour_win_shell: smoke-recovery FAILED (no session)\n");
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

	if (ok && smoke_record && engine_session_ok ()) {
		_smoke_record = SmokeRecordPhase::NeedArm;
		std::fprintf (stderr, "airdour_win_shell: smoke-record scheduled (arm → ~1.2s take)\n");
		std::fflush (stderr);
	}
	if (autoplay) {
		_engine.transport_play ();
	}
	_engine.poll ();
	refresh_engine_snap ();
	sync_mixer_arm_from_engine ();
	sync_mixer_fx_from_engine ();
	sync_transport_loop_from_engine ();
	sync_transport_punch_from_engine ();
	sync_transport_tempo_from_engine ();
	sync_transport_freeze_from_engine ();
	sync_track_labels_from_engine ();
	sync_mixer_gains_from_engine ();
	sync_mixer_group_from_engine ();
	if (_engine.is_linked () && _engine_snap.session_ok) {
		_engine.ensure_monitor_section ();
		_engine.poll ();
		refresh_engine_snap ();
	}
	sync_monitor_from_engine ();
	sync_history_from_engine ();
	sync_prefs_from_engine ();
	sync_recent_from_engine ();
	_transport.set_snap_enabled (_engine_snap.snap_enabled);
	_transport.set_snap_density (_engine_snap.snap_density);
	_transport.set_midi_step_enabled (_engine_snap.midi_step_enabled);
	_transport.set_midi_chord_quality (_engine_snap.midi_chord_quality);
	_transport.set_midi_arp_enabled (_engine_snap.midi_arp_enabled);
	_transport.set_midi_arp_mode (_engine_snap.midi_arp_mode);
	_transport.set_dsp_load_pct (_engine_snap.dsp_load_pct);
	sync_scale_to_transport ();
	layout_panes ();
	apply_loop_to_lanes ();
	apply_region_selection_to_lanes ();
	if (smoke_timeout_secs == 0 && !smoke_recovery) {
		maybe_show_recovery_prompt ();
	}
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
ShellWindow::refresh_engine_snap ()
{
	_engine_snap = _engine.snapshot ();
}

bool
ShellWindow::engine_session_ok ()
{
	return _engine.snapshot ().session_ok;
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
	_transport.set_click_gain (_engine_snap.click_gain);
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
		_mixers[i].set_color (i < EngineSnapshot::kMeters ? _engine_snap.track_color[i] : 0);
		_mixers[i].set_midi_strip (false);
	}
	const int mi = midi_mixer_index ();
	if (mi >= 0 && mi < kMixerStripsMax) {
		if (_engine_snap.midi_track_name[0] != L'\0') {
			_mixers[mi].set_label (_engine_snap.midi_track_name);
		} else {
			_mixers[mi].set_label (L"MIDI 1");
		}
		_mixers[mi].set_color (0);
		_mixers[mi].set_midi_strip (true);
	}
	refresh_automation_lane ();
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
		uint32_t rgba = 0;
		if (in && _engine_snap.group_shares_color) {
			rgba = _engine_snap.group_rgba;
		}
		_mixers[i].set_group_rgba (rgba);
		bool sel = (i < na && i < EngineSnapshot::kMeters && _engine_snap.track_selected[i]);
		_mixers[i].set_selected (sel);
	}
	_transport.set_group_active (_engine_snap.group_member_count >= 2
	                             && _engine_snap.group_active);
	_transport.set_group_relative (_engine_snap.group_member_count >= 2
	                              && _engine_snap.group_relative);
	_transport.set_spectrum_enabled (_engine_snap.spectrum_enabled);
	_transport.set_correlation_enabled (_engine_snap.correlation_enabled);
	_transport.set_loudness_enabled (_engine_snap.loudness_enabled);
}

void
ShellWindow::sync_mixer_arm_from_engine ()
{
	const int na = n_audio_lanes ();
	const int mi = midi_mixer_index ();
	for (int i = 0; i < kMixerStripsMax; ++i) {
		_mixers[i].set_armed (false);
		_mixers[i].set_record_safe (false);
	}
	for (int i = 0; i < na && i < EngineSnapshot::kMeters; ++i) {
		_mixers[i].set_armed (_engine_snap.track_armed[i]);
		_mixers[i].set_record_safe (_engine_snap.track_record_safe[i]);
	}
	if (mi >= 0 && mi < kMixerStripsMax) {
		_mixers[mi].set_armed (_engine_snap.midi_armed);
		_mixers[mi].set_record_safe (false);
	}
	_transport.set_record_armed (_engine_snap.session_record_armed);
}

void
ShellWindow::sync_mixer_monitor_from_engine ()
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < kMixerStripsMax; ++i) {
		_mixers[i].set_monitor_choice (0);
		_mixers[i].set_monitor_peer_count (0);
	}
	for (int i = 0; i < na && i < EngineSnapshot::kMeters; ++i) {
		_mixers[i].set_monitor_choice (_engine_snap.track_monitor_choice[i]);
		_mixers[i].set_monitor_peer_count (_engine_snap.track_monitor_peer_count[i]);
	}
}

void
ShellWindow::sync_mixer_mute_solo_from_engine ()
{
	const int na = n_audio_lanes ();
	const int mi = midi_mixer_index ();
	for (int i = 0; i < na && i < EngineSnapshot::kMeters; ++i) {
		_mixers[i].set_muted (_engine_snap.track_muted[i]);
		_mixers[i].set_mute_points (_engine_snap.track_mute_points[i]);
		_mixers[i].set_soloed (_engine_snap.track_soloed[i]);
		_mixers[i].set_route_active (_engine_snap.track_active[i]);
	}
	if (mi >= 0 && mi < kMixerStripsMax) {
		_mixers[mi].set_muted (_engine_snap.midi_muted);
		_mixers[mi].set_mute_points (0);
		_mixers[mi].set_soloed (_engine_snap.midi_soloed);
		_mixers[mi].set_route_active (true);
	}
}

void
ShellWindow::sync_mixer_solo_isolate_from_engine ()
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < kMixerStripsMax; ++i) {
		_mixers[i].set_solo_isolated (false);
	}
	for (int i = 0; i < na && i < EngineSnapshot::kMeters; ++i) {
		_mixers[i].set_solo_isolated (_engine_snap.track_solo_isolated[i]);
	}
}

void
ShellWindow::sync_mixer_solo_safe_from_engine ()
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < kMixerStripsMax; ++i) {
		_mixers[i].set_solo_safe (false);
	}
	for (int i = 0; i < na && i < EngineSnapshot::kMeters; ++i) {
		_mixers[i].set_solo_safe (_engine_snap.track_solo_safe[i]);
	}
}

void
ShellWindow::sync_mixer_comment_from_engine ()
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < kMixerStripsMax; ++i) {
		_mixers[i].set_has_comment (false);
	}
	for (int i = 0; i < na && i < EngineSnapshot::kMeters; ++i) {
		_mixers[i].set_has_comment (_engine_snap.track_comment[i][0] != L'\0');
	}
}

void
ShellWindow::sync_mixer_phase_from_engine ()
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < kMixerStripsMax; ++i) {
		_mixers[i].set_phase_inverted (false);
	}
	for (int i = 0; i < na && i < EngineSnapshot::kMeters; ++i) {
		_mixers[i].set_phase_inverted (_engine_snap.track_phase_invert[i]);
	}
}

void
ShellWindow::sync_mixer_trim_from_engine ()
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < na && i < EngineSnapshot::kMeters; ++i) {
		_mixers[i].set_trim (_engine_snap.track_trim_norm[i]);
	}
}

void
ShellWindow::sync_mixer_listen_from_engine ()
{
	const int n = n_mixer_strips ();
	for (int i = 0; i < n; ++i) {
		_mixers[i].set_listen_mode (_engine_snap.listen_mode);
	}
}

void
ShellWindow::sync_mixer_pan_from_engine ()
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < na && i < EngineSnapshot::kMeters; ++i) {
		_mixers[i].set_pan (_engine_snap.track_pan[i]);
		_mixers[i].set_pan_width (_engine_snap.track_pan_width[i]);
		_mixers[i].set_pan_width_ok (_engine_snap.track_pan_width_ok[i]);
	}
	_transport.set_recording (_engine_snap.session_recording);
}

void
ShellWindow::sync_mixer_send_from_engine ()
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < na && i < EngineSnapshot::kMeters; ++i) {
		_mixers[i].set_send (_engine_snap.track_send[i]);
		_mixers[i].set_send_muted (_engine_snap.track_send_muted[i]);
		_mixers[i].set_send_pan (_engine_snap.track_send_pan[i]);
		_mixers[i].set_send_pan_ok (_engine_snap.track_send_pan_ok[i]);
	}
}

int
ShellWindow::auto_lane_track () const
{
	int t = _focus_audio_track;
	const int na = n_audio_lanes ();
	if (t < 0) {
		t = 0;
	}
	if (na > 0 && t >= na) {
		t = na - 1;
	}
	if (t >= EngineSnapshot::kMeters) {
		t = EngineSnapshot::kMeters - 1;
	}
	return t;
}

void
ShellWindow::cycle_auto_lane_kind ()
{
	_auto_lane_kind = (_auto_lane_kind + 1) % 3;
	refresh_automation_lane ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

const wchar_t*
ShellWindow::auto_state_label (int state_0_3)
{
	switch (state_0_3) {
	case 1:
		return L"Play";
	case 2:
		return L"Write";
	case 3:
		return L"Touch";
	default:
		return L"Off";
	}
}

void
ShellWindow::do_cycle_auto_state ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	switch (_auto_lane_kind) {
	case 1:
		_engine.cycle_track_pan_auto_state (0);
		break;
	case 2:
		_engine.cycle_track_send_auto_state (0);
		break;
	default:
		_engine.cycle_track_gain_auto_state (0);
		break;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	refresh_automation_lane ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::refresh_automation_lane ()
{
	const int track = auto_lane_track ();
	const wchar_t* kind = L"Gain";
	int st_i = _engine_snap.track_gain_auto_state[track];
	if (_auto_lane_kind == 1) {
		kind = L"Pan";
		st_i = _engine_snap.track_pan_auto_state[track];
	} else if (_auto_lane_kind == 2) {
		kind = L"Send";
		st_i = _engine_snap.track_send_auto_state[track];
	}
	const wchar_t* st = auto_state_label (st_i);
	if (_engine_snap.track_name[track][0] != L'\0') {
		wchar_t auto_lab[64];
		_snwprintf_s (auto_lab, _TRUNCATE, L"%s · %s · %s", kind, st,
		              _engine_snap.track_name[track]);
		_auto_lane.set_label (auto_lab);
	} else {
		wchar_t auto_lab[48];
		_snwprintf_s (auto_lab, _TRUNCATE, L"%s · %s · Audio %d", kind, st, track + 1);
		_auto_lane.set_label (auto_lab);
	}
	float view0 = _lanes[0].strip.view_start_sec ();
	float span = std::max (0.001f, _lanes[0].strip.view_span_sec ());
	float curve[AutomationLane::kMaxColumns];
	size_t n = AutomationLane::kMaxColumns;
	if (_engine.is_linked () && _engine_snap.session_ok) {
		if (_auto_lane_kind == 1) {
			_engine.fill_pan_automation_curve (track, view0, span, curve, n);
		} else if (_auto_lane_kind == 2) {
			_engine.fill_send_automation_curve (track, view0, span, curve, n);
		} else {
			_engine.fill_gain_automation_curve (track, view0, span, curve, n);
		}
	} else {
		float flat = (track >= 0 && track < kMixerStripsMax) ? _mixers[track].gain () : 0.5f;
		if (_auto_lane_kind == 1) {
			flat = (track >= 0 && track < kMixerStripsMax) ? _mixers[track].pan () : 0.f;
		} else if (_auto_lane_kind == 2) {
			flat = (track >= 0 && track < kMixerStripsMax) ? _mixers[track].send () : 0.f;
		}
		for (size_t i = 0; i < n; ++i) {
			curve[i] = flat;
		}
	}
	_auto_lane.set_curve (curve, n);

	float px[AutomationLane::kMaxPoints];
	float py[AutomationLane::kMaxPoints];
	int npt = 0;
	if (_engine.is_linked () && _engine_snap.session_ok) {
		if (_auto_lane_kind == 1) {
			npt = _engine.fill_pan_automation_points (
				track, view0, span, px, py, (int)AutomationLane::kMaxPoints);
		} else if (_auto_lane_kind == 2) {
			npt = _engine.fill_send_automation_points (
				track, view0, span, px, py, (int)AutomationLane::kMaxPoints);
		} else {
			npt = _engine.fill_gain_automation_points (
				track, view0, span, px, py, (int)AutomationLane::kMaxPoints);
		}
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
	const int track = auto_lane_track ();
	float time_sec = _lanes[0].strip.time_at_x (_auto_lane_bounds, x);
	float value = _auto_lane.value_at_y (_auto_lane_bounds, y);
	/* Thin writes while dragging — skip near-duplicate times. */
	if (_drag == DragMode::AutoWrite && _auto_last_write_sec >= 0.f
	    && std::fabs (time_sec - _auto_last_write_sec) < 0.035f) {
		return;
	}
	bool ok = false;
	if (_auto_lane_kind == 1) {
		ok = _engine.write_track_pan_automation (track, time_sec, value);
	} else if (_auto_lane_kind == 2) {
		ok = _engine.write_track_send_automation (track, time_sec, value);
	} else {
		ok = _engine.write_track_gain_automation (track, time_sec, value);
	}
	if (ok) {
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

const wchar_t*
ShellWindow::midi_cc_label () const
{
	switch (_midi_cc_number) {
	case 1:
		return L"CC1 · Mod";
	case 11:
		return L"CC11 · Expr";
	case 64:
		return L"CC64 · Sustain";
	case -1:
		return L"Bend";
	case -2:
		return L"Pressure";
	case -3:
		return L"PolyAT · C4";
	default:
		return L"CC7 · Volume";
	}
}

void
ShellWindow::cycle_midi_cc_controller ()
{
	if (_midi_cc_number == 7) {
		_midi_cc_number = 1;
	} else if (_midi_cc_number == 1) {
		_midi_cc_number = 11;
	} else if (_midi_cc_number == 11) {
		_midi_cc_number = 64;
	} else if (_midi_cc_number == 64) {
		_midi_cc_number = -1; /* pitch bend */
	} else if (_midi_cc_number == -1) {
		_midi_cc_number = -2; /* channel pressure */
	} else if (_midi_cc_number == -2) {
		_midi_cc_number = -3; /* poly aftertouch · C4 */
	} else {
		_midi_cc_number = 7;
	}
	_midi_cc_last_write_sec = -1.f;
	if (_engine.is_linked () && _engine_snap.session_ok) {
		if (_midi_cc_number == -1) {
			_engine.ensure_midi_bend_automation ();
		} else if (_midi_cc_number == -2) {
			_engine.ensure_midi_pressure_automation ();
		} else if (_midi_cc_number == -3) {
			_engine.ensure_midi_note_pressure_automation ();
		} else {
			_engine.ensure_midi_cc_automation (_midi_cc_number);
		}
	}
	refresh_midi_cc_lane ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::refresh_midi_cc_lane ()
{
	_midi_cc_lane.set_label (midi_cc_label ());
	float view0 = _lanes[0].strip.view_start_sec ();
	float span = std::max (0.001f, _lanes[0].strip.view_span_sec ());
	float curve[AutomationLane::kMaxColumns];
	size_t n = AutomationLane::kMaxColumns;
	if (_engine.is_linked () && _engine_snap.session_ok) {
		if (_midi_cc_number == -1) {
			_engine.fill_midi_bend_automation_curve (view0, span, curve, n);
		} else if (_midi_cc_number == -2) {
			_engine.fill_midi_pressure_automation_curve (view0, span, curve, n);
		} else if (_midi_cc_number == -3) {
			_engine.fill_midi_note_pressure_automation_curve (view0, span, curve, n);
		} else {
			_engine.fill_midi_cc_automation_curve (
				view0, span, curve, n, _midi_cc_number);
		}
	} else {
		float def = 0.f;
		if (_midi_cc_number == 7) {
			def = 100.f / 127.f;
		} else if (_midi_cc_number == -1) {
			def = 0.5f;
		}
		for (size_t i = 0; i < n; ++i) {
			curve[i] = def;
		}
	}
	_midi_cc_lane.set_curve (curve, n);

	float px[AutomationLane::kMaxPoints];
	float py[AutomationLane::kMaxPoints];
	int npt = 0;
	if (_engine.is_linked () && _engine_snap.session_ok) {
		if (_midi_cc_number == -1) {
			npt = _engine.fill_midi_bend_automation_points (
				view0, span, px, py, (int)AutomationLane::kMaxPoints);
		} else if (_midi_cc_number == -2) {
			npt = _engine.fill_midi_pressure_automation_points (
				view0, span, px, py, (int)AutomationLane::kMaxPoints);
		} else if (_midi_cc_number == -3) {
			npt = _engine.fill_midi_note_pressure_automation_points (
				view0, span, px, py, (int)AutomationLane::kMaxPoints);
		} else {
			npt = _engine.fill_midi_cc_automation_points (
				view0, span, px, py, (int)AutomationLane::kMaxPoints,
				_midi_cc_number);
		}
	}
	_midi_cc_lane.set_points (px, py, (size_t)std::max (0, npt));
	_midi_cc_lane.set_playhead_norm (_lanes[0].strip.playhead_norm ());
}

void
ShellWindow::write_midi_cc_at (float x, float y)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	float time_sec = _lanes[0].strip.time_at_x (_midi_cc_lane_bounds, x);
	float value = _midi_cc_lane.value_at_y (_midi_cc_lane_bounds, y);
	if (_drag == DragMode::MidiCcWrite && _midi_cc_last_write_sec >= 0.f
	    && std::fabs (time_sec - _midi_cc_last_write_sec) < 0.035f) {
		return;
	}
	bool ok = false;
	if (_midi_cc_number == -1) {
		ok = _engine.write_midi_bend_automation (time_sec, value);
	} else if (_midi_cc_number == -2) {
		ok = _engine.write_midi_pressure_automation (time_sec, value);
	} else if (_midi_cc_number == -3) {
		ok = _engine.write_midi_note_pressure_automation (time_sec, value);
	} else {
		ok = _engine.write_midi_cc_automation (time_sec, value, _midi_cc_number);
	}
	if (ok) {
		_midi_cc_last_write_sec = time_sec;
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		refresh_midi_cc_lane ();
	}
}

void
ShellWindow::draw_midi_cc_lane ()
{
	if (_midi_cc_lane_bounds.right <= _midi_cc_lane_bounds.left) {
		return;
	}
	_midi_cc_lane.draw (_rt, _midi_cc_lane_bounds, _brush_midi, _brush_body,
	                    _brush_pane_edge, _brush_playhead, _fmt_meta, _dpi_scale);
}

void
ShellWindow::sync_mixer_fx_from_engine ()
{
	for (int i = 0; i < kTracks; ++i) {
		if (i == 0 && _engine_snap.track_fx_ok && !_engine_snap.track_fx_name.empty ()) {
			_mixers[i].set_fx_label (_engine_snap.track_fx_name.c_str ());
			_mixers[i].set_fx_bypassed (_engine_snap.track_fx_bypassed);
			_mixers[i].set_fx_param_count (1);
			_mixers[i].set_fx_param (_engine_snap.track_fx_param);
		} else if (i == 1 && _engine_snap.track_comp_ok
		           && !_engine_snap.track_comp_name.empty ()) {
			wchar_t label[40];
			if (_engine_snap.track_comp_sidechain) {
				_snwprintf_s (label, _TRUNCATE, L"%s SC",
				              _engine_snap.track_comp_name.c_str ());
			} else {
				wcsncpy_s (label, _engine_snap.track_comp_name.c_str (), _TRUNCATE);
			}
			_mixers[i].set_fx_label (label);
			_mixers[i].set_fx_bypassed (_engine_snap.track_comp_bypassed);
			_mixers[i].set_fx_param_count (2);
			_mixers[i].set_fx_param (_engine_snap.track_comp_threshold_norm);
			_mixers[i].set_fx_param2 (_engine_snap.track_comp_ratio_norm);
		} else if (i == 2 && _engine_snap.track_eq_ok
		           && !_engine_snap.track_eq_name.empty ()) {
			_mixers[i].set_fx_label (_engine_snap.track_eq_name.c_str ());
			_mixers[i].set_fx_bypassed (_engine_snap.track_eq_bypassed);
			_mixers[i].set_fx_param_count (3);
			_mixers[i].set_fx_param (_engine_snap.track_eq_gain_l_norm);
			_mixers[i].set_fx_param2 (_engine_snap.track_eq_gain_m_norm);
			_mixers[i].set_fx_param3 (_engine_snap.track_eq_gain_h_norm);
		} else if (i == 3 && _engine_snap.track_delay_ok
		           && !_engine_snap.track_delay_name.empty ()) {
			_mixers[i].set_fx_label (_engine_snap.track_delay_name.c_str ());
			_mixers[i].set_fx_bypassed (_engine_snap.track_delay_bypassed);
			_mixers[i].set_fx_param_count (2);
			_mixers[i].set_fx_param (_engine_snap.track_delay_time_norm);
			_mixers[i].set_fx_param2 (_engine_snap.track_delay_feedback_norm);
		} else if (i == 4 && _engine_snap.track_limiter_ok
		           && !_engine_snap.track_limiter_name.empty ()) {
			_mixers[i].set_fx_label (_engine_snap.track_limiter_name.c_str ());
			_mixers[i].set_fx_bypassed (_engine_snap.track_limiter_bypassed);
			_mixers[i].set_fx_param_count (2);
			_mixers[i].set_fx_param (_engine_snap.track_limiter_ceiling_norm);
			_mixers[i].set_fx_param2 (_engine_snap.track_limiter_release_norm);
		} else if (i == 5 && _engine_snap.track_gate_ok
		           && !_engine_snap.track_gate_name.empty ()) {
			_mixers[i].set_fx_label (_engine_snap.track_gate_name.c_str ());
			_mixers[i].set_fx_bypassed (_engine_snap.track_gate_bypassed);
			_mixers[i].set_fx_param_count (2);
			_mixers[i].set_fx_param (_engine_snap.track_gate_threshold_norm);
			_mixers[i].set_fx_param2 (_engine_snap.track_gate_hold_norm);
		} else {
			_mixers[i].set_fx_label (nullptr);
			_mixers[i].set_fx_bypassed (false);
			_mixers[i].set_fx_param_count (1);
			_mixers[i].set_fx_param (0.3f);
			_mixers[i].set_fx_param2 (0.16f);
			_mixers[i].set_fx_param3 (0.5f);
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
	sync_recent_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::sync_recent_from_engine ()
{
	_recent.set_rows (_engine_snap.recent_count, _engine_snap.recent_label, _engine_snap.recent_path);
}

void
ShellWindow::toggle_recent_menu ()
{
	dismiss_splash ();
	if (_prefs_visible) {
		_prefs_visible = false;
		_prefs.set_visible (false);
	}
	if (_about.about_visible ()) {
		_about.set_about_visible (false);
	}
	if (_help_visible) {
		_help_visible = false;
	}
	bool on = !_recent.visible ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_recent_from_engine ();
	_recent.set_visible (on);
	if (on) {
		_recent.layout_under (_transport.open_btn (), (float)_width, (float)_height, _dpi_scale);
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::open_recent_at (int index)
{
	_recent.set_visible (false);
	if (!_engine.is_linked ()) {
		return;
	}
	if (!_engine.start ()) {
		_engine_snap = _engine.snapshot ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	}
	if (_engine.open_recent (index)) {
		std::fprintf (stderr, "airdour_win_shell: recent open ok (index=%d)\n", index);
	} else {
		std::fprintf (stderr, "airdour_win_shell: recent open FAILED (index=%d)\n", index);
	}
	std::fflush (stderr);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_mixer_arm_from_engine ();
	sync_track_labels_from_engine ();
	sync_recent_from_engine ();
	layout_panes ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::apply_recent_hit (RecentHit hit)
{
	switch (hit) {
	case RecentHit::Outside:
		_recent.set_visible (false);
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	case RecentHit::Browse:
		_recent.set_visible (false);
		prompt_open_session ();
		return;
	case RecentHit::Clear:
		_engine.clear_recent_sessions (true);
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		sync_recent_from_engine ();
		_recent.layout_under (_transport.open_btn (), (float)_width, (float)_height, _dpi_scale);
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	default: {
		int row = RecentPanel::row_index (hit);
		if (row >= 0) {
			open_recent_at (row);
		}
		return;
	}
	}
}

void
ShellWindow::maybe_show_recovery_prompt ()
{
	if (!_engine.recovery_offer_available ()) {
		return;
	}
	std::string path;
	std::string snap;
	if (!_engine.recovery_offer_path (&path, &snap)) {
		return;
	}
	std::wstring wpath;
	int n = MultiByteToWideChar (CP_UTF8, 0, path.c_str (), (int)path.size (), nullptr, 0);
	if (n > 0) {
		wpath.assign ((size_t)n, L'\0');
		MultiByteToWideChar (CP_UTF8, 0, path.c_str (), (int)path.size (), &wpath[0], n);
	}
	std::wstring label = L"session";
	size_t slash = path.find_last_of ("/\\");
	if (slash != std::string::npos && slash + 1 < path.size ()) {
		std::string base = path.substr (slash + 1);
		int nb = MultiByteToWideChar (CP_UTF8, 0, base.c_str (), (int)base.size (), nullptr, 0);
		if (nb > 0) {
			label.assign ((size_t)nb, L'\0');
			MultiByteToWideChar (CP_UTF8, 0, base.c_str (), (int)base.size (), &label[0], nb);
		}
	}
	_recovery.set_session_hint (label.c_str (), wpath.c_str ());
	_recovery.set_visible (true);
	_recovery.layout ((float)_width, (float)_height, _dpi_scale);
	_recent.set_visible (false);
	_prefs_visible = false;
	_prefs.set_visible (false);
	_help_visible = false;
	_about.set_about_visible (false);
	dismiss_splash ();
	std::fprintf (stderr, "airdour_win_shell: recovery prompt shown\n");
	std::fflush (stderr);
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::apply_recovery_hit (RecoveryHit hit)
{
	if (hit == RecoveryHit::Restore) {
		_recovery.set_visible (false);
		if (_engine.restore_recovery_session ()) {
			std::fprintf (stderr, "airdour_win_shell: recovery restored\n");
		} else {
			std::fprintf (stderr, "airdour_win_shell: recovery restore FAILED\n");
		}
		std::fflush (stderr);
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		sync_mixer_arm_from_engine ();
		sync_track_labels_from_engine ();
		sync_recent_from_engine ();
		layout_panes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	}
	if (hit == RecoveryHit::Discard) {
		_engine.discard_recovery_offer ();
		_recovery.set_visible (false);
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
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
ShellWindow::do_locate_selection_start ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok || !_engine_snap.has_selection) {
		return;
	}
	if (_engine.locate_to_selection_start ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		_playhead_sec = _engine_snap.transport_sec;
		sync_transport_clock_from_engine ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_locate_selection_end ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok || !_engine_snap.has_selection) {
		return;
	}
	if (_engine.locate_to_selection_end ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		_playhead_sec = _engine_snap.transport_sec;
		sync_transport_clock_from_engine ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_locate_next_marker ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (_engine.locate_next_marker ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		_playhead_sec = _engine_snap.transport_sec;
		sync_transport_clock_from_engine ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_locate_prev_marker ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (_engine.locate_prev_marker ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		_playhead_sec = _engine_snap.transport_sec;
		sync_transport_clock_from_engine ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_rename_marker_at_playhead ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	wchar_t name[32] = L"mark";
	for (int i = 0; i < _engine_snap.marker_count; ++i) {
		if (std::fabs (_engine_snap.markers[i].sec - _playhead_sec) < 0.35f) {
			wcsncpy_s (name, _engine_snap.markers[i].name, _TRUNCATE);
			break;
		}
	}
	if (!run_rename_dialog (_hwnd, name, 32)) {
		return;
	}
	char utf8[128];
	WideCharToMultiByte (CP_UTF8, 0, name, -1, utf8, sizeof (utf8), nullptr, nullptr);
	if (!_engine.rename_marker_near (_playhead_sec, utf8)) {
		MessageBoxW (_hwnd, L"No marker near playhead to rename.",
		             L"Airdour", MB_OK | MB_ICONWARNING);
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_toggle_marker_lock_at_playhead ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.toggle_marker_locked_near (_playhead_sec)) {
		MessageBoxW (_hwnd, L"No marker near playhead to lock.",
		             L"Airdour", MB_OK | MB_ICONWARNING);
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_toggle_marker_cd_at_playhead ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.toggle_marker_cd_near (_playhead_sec)) {
		MessageBoxW (_hwnd, L"No marker near playhead for CD flag.",
		             L"Airdour", MB_OK | MB_ICONWARNING);
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_range_marker_from_selection ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		MessageBoxW (_hwnd, L"Select a region first for a range marker.",
		             L"Airdour", MB_OK | MB_ICONWARNING);
		return;
	}
	if (!_engine.add_range_marker_from_selection ()) {
		MessageBoxW (_hwnd, L"Could not create range marker from selection.",
		             L"Airdour", MB_OK | MB_ICONWARNING);
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_region_sync_at_playhead ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	bool ok;
	if (GetKeyState (VK_SHIFT) & 0x8000) {
		ok = _engine.clear_selection_sync ();
		if (!ok) {
			MessageBoxW (_hwnd, L"Select a region to clear sync.",
			             L"Airdour", MB_OK | MB_ICONWARNING);
		}
	} else {
		ok = _engine.set_selection_sync_at_playhead ();
		if (!ok) {
			MessageBoxW (_hwnd,
			             L"Select a region and put the playhead inside it to set sync.",
			             L"Airdour", MB_OK | MB_ICONWARNING);
		}
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_cycle_scene_channel ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.cycle_scene_channel_near (_playhead_sec)) {
		MessageBoxW (_hwnd, L"No scene marker near playhead to cycle channel.",
		             L"Airdour", MB_OK | MB_ICONWARNING);
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_locate_next_cue ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.locate_next_cue ()) {
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	_playhead_sec = _engine_snap.transport_sec;
	sync_transport_clock_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_locate_prev_cue ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.locate_prev_cue ()) {
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	_playhead_sec = _engine_snap.transport_sec;
	sync_transport_clock_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_rename_section_at_playhead ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	wchar_t name[32] = L"section";
	for (int i = 0; i < _engine_snap.marker_count; ++i) {
		if (_engine_snap.markers[i].section
		    && std::fabs (_engine_snap.markers[i].sec - _playhead_sec) < 0.35f) {
			wcsncpy_s (name, _engine_snap.markers[i].name, _TRUNCATE);
			break;
		}
	}
	if (!run_rename_dialog (_hwnd, name, 32)) {
		return;
	}
	char utf8[128];
	WideCharToMultiByte (CP_UTF8, 0, name, -1, utf8, sizeof (utf8), nullptr, nullptr);
	if (!_engine.rename_section_near (_playhead_sec, utf8)) {
		MessageBoxW (_hwnd, L"No section marker near playhead to rename.",
		             L"Airdour", MB_OK | MB_ICONWARNING);
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_locate_punch_in ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.locate_punch_in ()) {
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	_playhead_sec = _engine_snap.transport_sec;
	sync_transport_clock_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_locate_punch_out ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.locate_punch_out ()) {
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	_playhead_sec = _engine_snap.transport_sec;
	sync_transport_clock_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_locate_session_start ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.locate_session_start ()) {
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	_playhead_sec = _engine_snap.transport_sec;
	sync_transport_clock_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_locate_session_end ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.locate_session_end ()) {
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	_playhead_sec = _engine_snap.transport_sec;
	sync_transport_clock_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_locate_loop_start ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.locate_loop_start ()) {
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	_playhead_sec = _engine_snap.transport_sec;
	sync_transport_clock_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_locate_loop_end ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.locate_loop_end ()) {
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	_playhead_sec = _engine_snap.transport_sec;
	sync_transport_clock_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_zoom_to_punch ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	if (!_engine_snap.punch_range_ok
	    || _engine_snap.punch_end_sec <= _engine_snap.punch_start_sec) {
		return;
	}
	fit_shared_range (_engine_snap.punch_start_sec, _engine_snap.punch_end_sec);
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_zoom_to_loop ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	if (!_engine_snap.loop_active
	    || _engine_snap.loop_end_sec <= _engine_snap.loop_start_sec) {
		return;
	}
	fit_shared_range (_engine_snap.loop_start_sec, _engine_snap.loop_end_sec);
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_zoom_to_selection ()
{
	do_fit_selection ();
}

void
ShellWindow::do_nudge_playhead (int direction)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.nudge_playhead (direction)) {
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	_playhead_sec = _engine_snap.transport_sec;
	sync_transport_clock_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_zoom_to_session ()
{
	do_fit_session ();
}

void
ShellWindow::do_zoom_to_regions ()
{
	do_fit_all_regions ();
}

void
ShellWindow::do_center_on_playhead ()
{
	float span = _lanes[0].strip.view_span_sec ();
	if (span < 0.05f) {
		span = WaveStrip::kDefaultSpanSec;
	}
	/* Shell playhead is UI source of truth (async Session locate may lag). */
	float ph = _playhead_sec;
	if (ph < 0.f) {
		ph = 0.f;
	}
	float start = ph - span * 0.5f;
	if (start < 0.f) {
		start = 0.f;
	}
	_lanes[0].strip.set_view_window (start, span);
	apply_shared_view (0);
	refresh_editor_view_after_nav ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_zoom_around_playhead (float factor)
{
	if (factor <= 0.f) {
		return;
	}
	float span = _lanes[0].strip.view_span_sec ();
	if (span < 0.05f) {
		span = WaveStrip::kDefaultSpanSec;
	}
	float start = _lanes[0].strip.view_start_sec ();
	float ph = _playhead_sec;
	if (ph < 0.f) {
		ph = 0.f;
	}
	float norm = (ph - start) / span;
	if (norm < 0.f) {
		norm = 0.f;
	}
	if (norm > 1.f) {
		norm = 1.f;
	}
	_lanes[0].strip.zoom_at (factor, norm);
	apply_shared_view (0);
	refresh_editor_view_after_nav ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_scroll_playhead_into_view ()
{
	float span = _lanes[0].strip.view_span_sec ();
	if (span < 0.05f) {
		span = WaveStrip::kDefaultSpanSec;
	}
	float start = _lanes[0].strip.view_start_sec ();
	float ph = _playhead_sec;
	if (ph < 0.f) {
		ph = 0.f;
	}
	float end = start + span;
	/* Already visible with a little margin — no-op. */
	if (ph >= start + span * 0.05f && ph <= end - span * 0.05f) {
		return;
	}
	float new_start;
	if (ph < start) {
		/* Scroll left: playhead at 25% into the window. */
		new_start = ph - span * 0.25f;
	} else {
		/* Scroll right: playhead at 75% into the window. */
		new_start = ph - span * 0.75f;
	}
	if (new_start < 0.f) {
		new_start = 0.f;
	}
	_lanes[0].strip.set_view_window (new_start, span);
	apply_shared_view (0);
	refresh_editor_view_after_nav ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_clear_selection ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	_engine.clear_region_selection ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	apply_region_selection_to_lanes ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_cycle_fade_shape ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.cycle_selected_fade_shape ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_cycle_region_layer ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.cycle_selected_region_layer ()) {
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	apply_region_selection_to_lanes ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_cycle_region_selection (int direction)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.cycle_region_selection (direction)) {
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_duplicate_selection ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.duplicate_selected_region ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_ensure_region_envelope ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.ensure_selected_region_envelope ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_loop_from_selection ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.copy_selection_to_loop ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		sync_transport_loop_from_engine ();
		apply_loop_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_lower_selection (bool to_bottom)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.lower_selected_region (to_bottom)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_nudge_region_envelope_mid (float delta_coeff)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.nudge_selected_region_envelope_mid (delta_coeff)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_nudge_selection (float delta_sec)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.nudge_selected_by_sec (delta_sec)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_nudge_selection_gain_db (float delta_db)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.nudge_selection_gain_db (delta_db)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_punch_from_selection ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.copy_selection_to_punch ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		sync_transport_punch_from_engine ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_raise_selection (bool to_top)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.raise_selected_region (to_top)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_set_region_envelope_mid (float coeff_0_1)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.set_selected_region_envelope_mid (coeff_0_1)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_skip_range_from_selection ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		MessageBoxW (_hwnd, L"Select a region first for a skip range.",
		             L"Airdour", MB_OK | MB_ICONWARNING);
		return;
	}
	if (!_engine.add_skip_range_from_selection ()) {
		MessageBoxW (_hwnd, L"Could not create skip range from selection.",
		             L"Airdour", MB_OK | MB_ICONWARNING);
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_toggle_fade_active ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.toggle_selected_fade_active ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_toggle_region_position_lock ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.toggle_selection_position_locked ()) {
		MessageBoxW (_hwnd, L"Select a region to position-lock.",
		             L"Airdour", MB_OK | MB_ICONWARNING);
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_toggle_selection_locked ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.toggle_selection_locked ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_toggle_selection_muted ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.toggle_selection_muted ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_toggle_selection_opaque ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.toggle_selection_opaque ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

bool
ShellWindow::envelope_mid_from_y (int lane, float y, float* out_coeff) const
{
	if (!out_coeff || lane < 0 || lane >= kTracks) {
		return false;
	}
	const D2D1_RECT_F& lb = _lanes[lane].bounds;
	float pad = 3.f * _dpi_scale;
	float h = lb.bottom - lb.top - 2.f * pad;
	if (h < 1.f) {
		return false;
	}
	float t = (lb.bottom - pad - y) / h;
	*out_coeff = std::max (0.02f, std::min (1.f, t));
	return true;
}

bool
ShellWindow::hit_region_envelope_mid (float x, float y, int* out_lane) const
{
	if (!_engine_snap.region_envelope_ok || !_engine_snap.has_selection
	    || _engine_snap.selection_end_sec <= _engine_snap.selection_start_sec) {
		return false;
	}
	int lane = _engine_snap.selection_track;
	if (lane < 0 || lane >= kTracks || lane >= n_audio_lanes ()) {
		return false;
	}
	const D2D1_RECT_F& lb = _lanes[lane].bounds;
	float view0 = _lanes[0].strip.view_start_sec ();
	float span = _lanes[0].strip.view_span_sec ();
	if (span < 1e-6f) {
		return false;
	}
	float mid_sec = 0.5f * (_engine_snap.selection_start_sec + _engine_snap.selection_end_sec);
	float nx = (mid_sec - view0) / span;
	if (nx < 0.f || nx > 1.f) {
		return false;
	}
	float mid_gain = _engine.selected_region_envelope_mid ();
	if (mid_gain < 0.f) {
		return false;
	}
	float pad = 3.f * _dpi_scale;
	float lane_w = lb.right - lb.left;
	float lane_h = lb.bottom - lb.top;
	float px = lb.left + nx * lane_w;
	float py = lb.bottom - pad - mid_gain * (lane_h - 2.f * pad);
	float hit = 10.f * _dpi_scale;
	if (std::fabs (x - px) <= hit && std::fabs (y - py) <= hit) {
		if (out_lane) {
			*out_lane = lane;
		}
		return true;
	}
	return false;
}

void
ShellWindow::draw_region_envelope ()
{
	if (!_rt || !_brush_accent || !_engine.is_linked ()) {
		return;
	}
	if (!_engine_snap.region_envelope_ok || !_engine_snap.has_selection
	    || _engine_snap.selection_end_sec <= _engine_snap.selection_start_sec) {
		return;
	}
	int lane = _engine_snap.selection_track;
	if (lane < 0 || lane >= kTracks || lane >= n_audio_lanes ()) {
		return;
	}
	const D2D1_RECT_F& lb = _lanes[lane].bounds;
	float view0 = _lanes[0].strip.view_start_sec ();
	float span = _lanes[0].strip.view_span_sec ();
	if (span < 1e-6f) {
		return;
	}
	float t0 = _engine_snap.selection_start_sec;
	float t1 = _engine_snap.selection_end_sec;
	float n0 = (t0 - view0) / span;
	float n1 = (t1 - view0) / span;
	if (n1 < 0.f || n0 > 1.f) {
		return;
	}
	n0 = std::max (0.f, std::min (1.f, n0));
	n1 = std::max (0.f, std::min (1.f, n1));
	if (n1 - n0 < 1e-4f) {
		return;
	}

	static constexpr size_t kEnvPts = 48;
	float y_0_1[kEnvPts];
	if (!_engine.fill_selected_region_envelope_curve (y_0_1, kEnvPts)) {
		return;
	}

	float pad = 3.f * _dpi_scale;
	float lane_w = lb.right - lb.left;
	float lane_h = lb.bottom - lb.top;
	float usable_h = lane_h - 2.f * pad;
	if (usable_h < 1.f) {
		return;
	}
	float x0 = lb.left + n0 * lane_w;
	float x1 = lb.left + n1 * lane_w;
	float w = x1 - x0;
	if (w < 2.f) {
		return;
	}

	ID2D1PathGeometry* geo = nullptr;
	ID2D1Factory* fac = nullptr;
	_rt->GetFactory (&fac);
	if (!fac || FAILED (fac->CreatePathGeometry (&geo)) || !geo) {
		return;
	}
	ID2D1GeometrySink* sink = nullptr;
	if (FAILED (geo->Open (&sink)) || !sink) {
		geo->Release ();
		return;
	}
	for (size_t i = 0; i < kEnvPts; ++i) {
		float frac = ((float)i + 0.5f) / (float)kEnvPts;
		float px = x0 + frac * w;
		float gy = std::max (0.f, std::min (1.f, y_0_1[i]));
		float py = lb.bottom - pad - gy * usable_h;
		if (i == 0) {
			sink->BeginFigure (D2D1::Point2F (px, py), D2D1_FIGURE_BEGIN_HOLLOW);
		} else {
			sink->AddLine (D2D1::Point2F (px, py));
		}
	}
	sink->EndFigure (D2D1_FIGURE_END_OPEN);
	sink->Close ();
	sink->Release ();

	_brush_accent->SetOpacity (0.85f);
	_rt->DrawGeometry (geo, _brush_accent, std::max (1.25f, 1.5f * _dpi_scale));
	_brush_accent->SetOpacity (1.f);
	geo->Release ();

	/* Mid tip cap — same geometry as hit_region_envelope_mid. */
	float mid_gain = _engine.selected_region_envelope_mid ();
	if (mid_gain < 0.f) {
		return;
	}
	float mid_sec = 0.5f * (t0 + t1);
	float nx = (mid_sec - view0) / span;
	if (nx < 0.f || nx > 1.f) {
		return;
	}
	float px = lb.left + nx * lane_w;
	float py = lb.bottom - pad - mid_gain * usable_h;
	float tip_r = std::max (3.f, 4.5f * _dpi_scale);
	_brush_accent->SetOpacity (0.95f);
	_rt->FillEllipse (D2D1::Ellipse (D2D1::Point2F (px, py), tip_r, tip_r), _brush_accent);
	if (_brush_pane) {
		_rt->DrawEllipse (D2D1::Ellipse (D2D1::Point2F (px, py), tip_r, tip_r),
		                  _brush_pane, std::max (1.f, 1.25f * _dpi_scale));
	}
	_brush_accent->SetOpacity (1.f);
}

bool
ShellWindow::smoke_envelope_mid_handle ()
{
	if (!_engine.is_linked ()) {
		std::fprintf (stderr, "airdour_win_shell: smoke-envelope-mid-handle FAILED (no engine)\n");
		std::fflush (stderr);
		return false;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	if (!_engine_snap.session_ok) {
		std::fprintf (stderr, "airdour_win_shell: smoke-envelope-mid-handle FAILED (no session)\n");
		std::fflush (stderr);
		return false;
	}
	if (!_engine.smoke_region_envelope_drag ()) {
		std::fprintf (stderr, "airdour_win_shell: smoke-envelope-mid-handle FAILED (envelope drag)\n");
		std::fflush (stderr);
		return false;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	apply_region_selection_to_lanes ();

	static constexpr size_t kEnvPts = 32;
	float y_0_1[kEnvPts];
	if (!_engine.fill_selected_region_envelope_curve (y_0_1, kEnvPts)) {
		std::fprintf (stderr, "airdour_win_shell: smoke-envelope-mid-handle FAILED (curve)\n");
		std::fflush (stderr);
		return false;
	}
	float mid_y = y_0_1[kEnvPts / 2];
	float edge_y = 0.5f * (y_0_1[0] + y_0_1[kEnvPts - 1]);
	if (!(mid_y + 0.05f < edge_y)) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-envelope-mid-handle FAILED "
		              "(curve not dipped mid=%.3f edge=%.3f)\n",
		              mid_y, edge_y);
		std::fflush (stderr);
		return false;
	}

	/* Fit selection so mid handle is on-screen, then hit-test the drawn tip. */
	int lane = _engine_snap.selection_track;
	if (lane < 0 || lane >= kTracks) {
		std::fprintf (stderr, "airdour_win_shell: smoke-envelope-mid-handle FAILED (lane)\n");
		std::fflush (stderr);
		return false;
	}
	float t0 = _engine_snap.selection_start_sec;
	float t1 = _engine_snap.selection_end_sec;
	for (int i = 0; i < kTracks; ++i) {
		_lanes[i].strip.fit_range (t0, t1, 0.12f);
	}
	layout_panes ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();

	float mid_gain = _engine.selected_region_envelope_mid ();
	if (mid_gain < 0.f) {
		std::fprintf (stderr, "airdour_win_shell: smoke-envelope-mid-handle FAILED (mid gain)\n");
		std::fflush (stderr);
		return false;
	}
	const D2D1_RECT_F& lb = _lanes[lane].bounds;
	float view0 = _lanes[0].strip.view_start_sec ();
	float span = _lanes[0].strip.view_span_sec ();
	float mid_sec = 0.5f * (t0 + t1);
	float nx = (mid_sec - view0) / span;
	float pad = 3.f * _dpi_scale;
	float px = lb.left + nx * (lb.right - lb.left);
	float py = lb.bottom - pad - mid_gain * ((lb.bottom - lb.top) - 2.f * pad);
	int hit_lane = -1;
	if (!hit_region_envelope_mid (px, py, &hit_lane) || hit_lane != lane) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-envelope-mid-handle FAILED "
		              "(hit miss at %.1f,%.1f lane=%d)\n",
		              px, py, hit_lane);
		std::fflush (stderr);
		return false;
	}

	std::fprintf (stderr,
	              "airdour_win_shell: smoke-envelope-mid-handle ok "
	              "(curve dip mid=%.2f edge=%.2f tip hit T%d)\n",
	              mid_y, edge_y, lane + 1);
	std::fflush (stderr);
	return true;
}

void
ShellWindow::prompt_export_selection_wav ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (!_engine_snap.has_selection) {
		std::fprintf (stderr, "airdour_win_shell: export selection — select a region first\n");
		std::fflush (stderr);
		return;
	}
	if (!_engine.start ()) {
		_engine_snap = _engine.snapshot ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	}

	wchar_t path[MAX_PATH] = L"airdour_export_selection.wav";
	OPENFILENAMEW ofn = {};
	ofn.lStructSize = sizeof (ofn);
	ofn.hwndOwner = _hwnd;
	ofn.lpstrFilter = L"WAV audio (*.wav)\0*.wav\0All files (*.*)\0*.*\0";
	ofn.lpstrFile = path;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
	ofn.lpstrTitle = L"Airdour — export selection WAV";
	ofn.lpstrDefExt = L"wav";
	if (!GetSaveFileNameW (&ofn)) {
		return;
	}

	char utf8[MAX_PATH * 4];
	WideCharToMultiByte (CP_UTF8, 0, path, -1, utf8, sizeof (utf8), nullptr, nullptr);
	if (_engine.export_selection_wav (utf8)) {
		std::fprintf (stderr, "airdour_win_shell: export selection dialog ok\n");
	} else {
		std::fprintf (stderr, "airdour_win_shell: export selection dialog failed\n");
	}
	std::fflush (stderr);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::prompt_rename_selected_region ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok
	    || !_engine_snap.has_selection) {
		return;
	}
	wchar_t name[EngineSnapshot::kRegionNameLen];
	if (_engine_snap.selection_region_name[0]) {
		wcsncpy_s (name, _engine_snap.selection_region_name, _TRUNCATE);
	} else {
		wcsncpy_s (name, L"Region", _TRUNCATE);
	}
	if (!run_rename_dialog (_hwnd, name, EngineSnapshot::kRegionNameLen)) {
		return;
	}
	char utf8[128];
	WideCharToMultiByte (CP_UTF8, 0, name, -1, utf8, sizeof (utf8), nullptr, nullptr);
	if (!_engine.set_selected_region_name (utf8)) {
		MessageBoxW (_hwnd, L"Could not rename region (empty name or no selection).",
		             L"Airdour", MB_OK | MB_ICONWARNING);
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	apply_region_selection_to_lanes ();
	InvalidateRect (_hwnd, nullptr, FALSE);
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
			active ? _engine_snap.selection_fade_out_sec : 0.f,
			active ? _engine_snap.selection_muted : false);
		_lanes[i].strip.set_sync_marker (
			active && _engine_snap.selection_sync_marked,
			_engine_snap.selection_sync_sec);
	}
}

bool
ShellWindow::smoke_region_sync_draw ()
{
	if (!_engine.is_linked ()) {
		std::fprintf (stderr, "airdour_win_shell: smoke-region-sync-draw FAILED (no engine)\n");
		return false;
	}
	if (!_engine.snapshot ().session_ok) {
		std::fprintf (stderr, "airdour_win_shell: smoke-region-sync-draw FAILED (no session)\n");
		return false;
	}
	/* Reuse API smoke to place a mid sync, then leave it marked for draw check. */
	if (!_engine.import_test_tone (0, 440.f, 1.2f)) {
		std::fprintf (stderr, "airdour_win_shell: smoke-region-sync-draw FAILED (import)\n");
		return false;
	}
	if (!_engine.select_region_at (0, 0.3f)) {
		std::fprintf (stderr, "airdour_win_shell: smoke-region-sync-draw FAILED (select)\n");
		return false;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	float s0 = _engine_snap.selection_start_sec;
	float e0 = _engine_snap.selection_end_sec;
	float mid = s0 + 0.40f;
	if (mid >= e0 - 0.05f) {
		std::fprintf (stderr, "airdour_win_shell: smoke-region-sync-draw FAILED (len)\n");
		return false;
	}
	if (!_engine.set_selection_sync_at (mid)) {
		std::fprintf (stderr, "airdour_win_shell: smoke-region-sync-draw FAILED (set)\n");
		return false;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	if (!_engine_snap.selection_sync_marked
	    || std::fabs (_engine_snap.selection_sync_sec - mid) > 0.05f) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-region-sync-draw FAILED "
		              "(sync=%.3f want=%.3f marked=%d)\n",
		              _engine_snap.selection_sync_sec, mid,
		              _engine_snap.selection_sync_marked ? 1 : 0);
		return false;
	}
	apply_region_selection_to_lanes ();
	do_fit_selection ();
	layout_panes ();
	const int lane = _engine_snap.selection_track;
	if (lane < 0 || lane >= kTracks) {
		std::fprintf (stderr, "airdour_win_shell: smoke-region-sync-draw FAILED (lane)\n");
		return false;
	}
	const D2D1_RECT_F& b = _lanes[lane].bounds;
	float sx = _lanes[lane].strip.x_at_time (b, _engine_snap.selection_sync_sec);
	float x0 = _lanes[lane].strip.x_at_time (b, _engine_snap.selection_start_sec);
	float x1 = _lanes[lane].strip.x_at_time (b, _engine_snap.selection_end_sec);
	if (sx < x0 - 1.f || sx > x1 + 1.f || sx < b.left - 1.f || sx > b.right + 1.f) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-region-sync-draw FAILED "
		              "(sx=%.1f region=[%.1f..%.1f] bounds=[%.1f..%.1f])\n",
		              sx, x0, x1, b.left, b.right);
		return false;
	}
	std::fprintf (stderr,
	              "airdour_win_shell: smoke-region-sync-draw ok "
	              "(sync=%.3fs sx=%.1f in [%.1f..%.1f])\n",
	              _engine_snap.selection_sync_sec, sx, x0, x1);
	return true;
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
ShellWindow::do_cycle_click_gain ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.cycle_click_gain ();
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
	/* Shift+click +A → stereo 2in2out Equal-Power (pan width capable). */
	if (GetKeyState (VK_SHIFT) & 0x8000) {
		_engine.add_stereo_audio_track ();
	} else {
		_engine.add_audio_track ();
	}
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
	refresh_automation_lane ();
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
	refresh_automation_lane ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::sync_monitor_from_engine ()
{
	_monitor.set_available (_engine_snap.monitor_ok);
	_monitor.set_dim (_engine_snap.monitor_dim);
	_monitor.set_dim_db (_engine_snap.monitor_dim_db);
	_monitor.set_mono (_engine_snap.monitor_mono);
	_monitor.set_mute (_engine_snap.monitor_cut);
	_monitor.set_solo_boost_db (_engine_snap.monitor_solo_boost_db);
	_monitor.set_aux_ok (_engine_snap.aux_bus_ok);
	_monitor.set_aux_return (_engine_snap.aux_return);
}

void
ShellWindow::do_set_aux_return (float gain_0_1)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.set_aux_return (gain_0_1);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_monitor_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
	_monitor.set_master_lim_ok (_engine_snap.master_fx_ok);
	_monitor.set_master_lim_bypassed (_engine_snap.master_fx_bypassed);
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
ShellWindow::do_clear_all_solos ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (_engine.clear_all_solos ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		sync_mixer_mute_solo_from_engine ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_toggle_master_fx ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.toggle_master_fx_bypass ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_monitor_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_cycle_monitor_solo_boost ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.cycle_monitor_solo_boost ();
	_engine.toggle_master_fx_bypass ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_monitor_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_clear_all_mutes ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (_engine.clear_all_mutes ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		sync_mixer_mute_solo_from_engine ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_scroll_mixer (float delta_px)
{
	if (std::fabs (delta_px) < 0.01f) {
		return;
	}
	_mixer_scroll_x += delta_px;
	layout_panes ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

bool
ShellWindow::smoke_mixer_scroll_layout ()
{
	/* Force overflow with wide min strip; scroll to end; strip 0 must leave clip. */
	_mixer_force_min_w = 140.f;
	_mixer_scroll_x = 0.f;
	layout_panes ();
	if (_mixer_scroll_max < 8.f) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-mixer-scroll FAILED (max=%.1f need overflow)\n",
		              _mixer_scroll_max);
		_mixer_force_min_w = 0.f;
		layout_panes ();
		return false;
	}
	float left0 = _mixers[0].bounds ().left;
	_mixer_scroll_x = _mixer_scroll_max;
	layout_panes ();
	float left1 = _mixers[0].bounds ().left;
	if (left1 >= left0 - 4.f) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-mixer-scroll FAILED (scroll no move %.1f→%.1f)\n",
		              left0, left1);
		_mixer_force_min_w = 0.f;
		_mixer_scroll_x = 0.f;
		layout_panes ();
		return false;
	}
	if (left1 > _mixer_strips_clip.left - 2.f) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-mixer-scroll FAILED (strip0 still in clip %.1f)\n",
		              left1);
		_mixer_force_min_w = 0.f;
		_mixer_scroll_x = 0.f;
		layout_panes ();
		return false;
	}
	_mixer_scroll_x = 0.f;
	_mixer_force_min_w = 0.f;
	layout_panes ();
	std::fprintf (stderr,
	              "airdour_win_shell: smoke-mixer-scroll ok (max=%.0f px, shifted %.0f)\n",
	              _mixer_scroll_max, left0 - left1);
	return true;
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
ShellWindow::do_toggle_group_relative ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (_engine_snap.group_member_count < 2) {
		return;
	}
	_engine.toggle_route_group_relative ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_mixer_group_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_toggle_vca_assign ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.ensure_vca_master ()) {
		_engine_snap = _engine.snapshot ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	const bool on = _engine_snap.vca_slave_count < 1;
	_engine.assign_track_to_vca (0, on);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_toggle_focused_track_active ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	int track = _focus_audio_track;
	if (track < 0 || track >= n_audio_lanes ()) {
		track = 0;
	}
	_engine.toggle_track_active (track);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_mixer_mute_solo_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_cycle_group_color ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (_engine_snap.group_member_count < 2) {
		return;
	}
	_engine.cycle_route_group_color ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_mixer_group_from_engine ();
	if (!_engine.ensure_vca_master ()) {
		_engine_snap = _engine.snapshot ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	const bool on = _engine_snap.vca_slave_count < 1;
	_engine.assign_track_to_vca (0, on);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
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
ShellWindow::do_toggle_comp_sidechain ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.toggle_track_comp_sidechain (1, 0)) {
		MessageBoxW (_hwnd, L"Could not toggle Comp sidechain (need ACE Comp on Audio 2).",
		             L"Airdour", MB_OK | MB_ICONWARNING);
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_mixer_fx_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::format_marker_ruler_label (const wchar_t* name, wchar_t* out, size_t out_len,
                                        int max_chars)
{
	if (!out || out_len == 0) {
		return;
	}
	out[0] = L'\0';
	if (!name || !name[0] || max_chars < 2 || out_len < 2) {
		return;
	}
	const size_t n = wcslen (name);
	const size_t cap = out_len - 1;
	if ((int)n <= max_chars) {
		wcsncpy_s (out, out_len, name, _TRUNCATE);
		return;
	}
	/* Keep max_chars-1 glyphs + ellipsis (… U+2026). */
	const int keep = max_chars - 1;
	if ((size_t)keep + 1 > cap) {
		wcsncpy_s (out, out_len, name, _TRUNCATE);
		return;
	}
	for (int i = 0; i < keep; ++i) {
		out[i] = name[i];
	}
	out[keep] = L'\x2026';
	out[keep + 1] = L'\0';
}

bool
ShellWindow::smoke_marker_name_ellipsis ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-marker-name-ellipsis FAILED (no session)\n");
		std::fflush (stderr);
		return false;
	}
	wchar_t short_lab[32];
	wchar_t long_lab[32];
	format_marker_ruler_label (L"Intro", short_lab, 32, 8);
	format_marker_ruler_label (L"VeryLongMarkerNameXYZ", long_lab, 32, 8);
	if (wcscmp (short_lab, L"Intro") != 0) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-marker-name-ellipsis FAILED (short=%ls)\n",
		              short_lab);
		std::fflush (stderr);
		return false;
	}
	if (wcslen (long_lab) != 8 || long_lab[7] != L'\x2026'
	    || wcsncmp (long_lab, L"VeryLon", 7) != 0) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-marker-name-ellipsis FAILED (long=%ls)\n",
		              long_lab);
		std::fflush (stderr);
		return false;
	}

	if (!_engine.add_marker_at (1.0f, "Short")
	    || !_engine.add_marker_at (2.0f, "VeryLongMarkerNameXYZ")) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-marker-name-ellipsis FAILED (add)\n");
		std::fflush (stderr);
		return false;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	bool short_ok = false;
	bool long_ok = false;
	for (int i = 0; i < _engine_snap.marker_count; ++i) {
		wchar_t lab[32];
		format_marker_ruler_label (_engine_snap.markers[i].name, lab, 32, 8);
		if (std::fabs (_engine_snap.markers[i].sec - 1.0f) < 0.05f
		    && wcscmp (lab, L"Short") == 0) {
			short_ok = true;
		}
		if (std::fabs (_engine_snap.markers[i].sec - 2.0f) < 0.05f
		    && wcslen (lab) == 8 && lab[7] == L'\x2026') {
			long_ok = true;
		}
	}
	(void)_engine.remove_marker_near (1.0f, 0.35f);
	(void)_engine.remove_marker_near (2.0f, 0.35f);
	if (!short_ok || !long_ok) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-marker-name-ellipsis FAILED "
		              "(snap short=%d long=%d)\n",
		              short_ok ? 1 : 0, long_ok ? 1 : 0);
		std::fflush (stderr);
		return false;
	}
	std::fprintf (stderr,
	              "airdour_win_shell: smoke-marker-name-ellipsis ok "
	              "(Short · VeryLon…)\n");
	std::fflush (stderr);
	return true;
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
ShellWindow::sync_history_from_engine ()
{
	_history.set_available (_engine_snap.session_ok);
	_history.set_depths (_engine_snap.undo_depth, _engine_snap.redo_depth);
	_history.set_next_names (_engine_snap.next_undo_name, _engine_snap.next_redo_name);
	_history.set_undo_rows (_engine_snap.undo_list_count, _engine_snap.undo_list_name);
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
	sync_history_from_engine ();
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
	sync_history_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_undo_to_index (int index)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.undo_to_index (index)) {
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	apply_region_selection_to_lanes ();
	sync_mixer_gains_from_engine ();
	sync_history_from_engine ();
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
ShellWindow::do_stretch_selection (float time_ratio)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.stretch_selected (time_ratio)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_pitch_shift_selection (float semitones)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.pitch_shift_selected (semitones)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_reverse_selection ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.reverse_selected ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_normalize_selection (float target_dbfs)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.normalize_selected (target_dbfs)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_strip_silence_selection ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.strip_silence_selected ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_consolidate_selection ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.consolidate_selected ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::sync_transport_freeze_from_engine ()
{
	int track = _engine_snap.has_selection ? _engine_snap.selection_track
	                                       : _focus_audio_track;
	bool frozen = false;
	if (track >= 0 && track < EngineSnapshot::kMeters) {
		frozen = _engine_snap.track_frozen[track];
	}
	_transport.set_freeze_active (frozen);
}

void
ShellWindow::do_toggle_freeze_track ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	int track = _engine_snap.has_selection ? _engine_snap.selection_track
	                                       : _focus_audio_track;
	if (track < 0) {
		track = 0;
	}
	if (_engine.toggle_freeze_track (track)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		sync_transport_freeze_from_engine ();
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
	_transport.set_snap_density (_engine_snap.snap_density);
	_transport.set_midi_step_enabled (_engine_snap.midi_step_enabled);
	_transport.set_midi_chord_quality (_engine_snap.midi_chord_quality);
	if (_engine_snap.snap_enabled) {
		_engine.refresh_grid_lines (
			_lanes[0].strip.view_start_sec (),
			_lanes[0].strip.view_span_sec ());
		_engine_snap = _engine.snapshot ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

bool
ShellWindow::smoke_midi_vel_chord ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-vel-chord FAILED (no session)\n");
		return false;
	}
	if (!_engine.ensure_midi_track () || !_engine.ensure_midi_region (16.f)) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-vel-chord FAILED (track/region)\n");
		return false;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	/* Clear existing notes one-by-one so the chord is alone. */
	for (int guard = 0; guard < 64 && _engine_snap.midi_note_count > 0; ++guard) {
		const EngineMidiNoteSnap& n = _engine_snap.midi_notes[0];
		_engine.select_midi_note_at (n.start_sec + n.length_sec * 0.5f, n.pitch);
		if (!_engine.delete_selected_midi_note ()) {
			break;
		}
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	if (!_engine.add_midi_note (60, 0.0f, 0.5f, 40)
	    || !_engine.add_midi_note (64, 0.0f, 0.5f, 110)) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-vel-chord FAILED (add)\n");
		return false;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	if (_engine_snap.midi_note_count < 2) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-vel-chord FAILED (count=%d)\n",
		              _engine_snap.midi_note_count);
		return false;
	}
	layout_panes ();
	const D2D1_RECT_F& lane = _midi_lane;
	const D2D1_RECT_F vel = midi_velocity_rect ();
	float vh = vel.bottom - vel.top;
	if (vh < 8.f || lane.right <= lane.left) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-vel-chord FAILED (layout)\n");
		return false;
	}
	float cx = _lanes[0].strip.x_at_time (lane, 0.25f);
	float quiet_h = vh * (40.f / 127.f);
	float loud_h = vh * (110.f / 127.f);
	float y_loud_only = vel.bottom - (quiet_h + loud_h) * 0.5f;
	float y_inner = vel.bottom - quiet_h * 0.5f;

	int i_loud = pick_midi_velocity_note_at (cx, y_loud_only);
	int i_quiet = pick_midi_velocity_note_at (cx, y_inner);
	if (i_loud < 0 || i_quiet < 0) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-vel-chord FAILED (pick miss)\n");
		return false;
	}
	if (_engine_snap.midi_notes[i_loud].velocity < 100
	    || _engine_snap.midi_notes[i_loud].pitch != 64) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-vel-chord FAILED "
		              "(loud pick p=%u v=%u)\n",
		              (unsigned)_engine_snap.midi_notes[i_loud].pitch,
		              (unsigned)_engine_snap.midi_notes[i_loud].velocity);
		return false;
	}
	if (_engine_snap.midi_notes[i_quiet].velocity > 60
	    || _engine_snap.midi_notes[i_quiet].pitch != 60) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-vel-chord FAILED "
		              "(quiet pick p=%u v=%u)\n",
		              (unsigned)_engine_snap.midi_notes[i_quiet].pitch,
		              (unsigned)_engine_snap.midi_notes[i_quiet].velocity);
		return false;
	}
	/* Apply selection through the same path mouse-down uses. */
	const EngineMidiNoteSnap& loud = _engine_snap.midi_notes[i_loud];
	if (!_engine.select_midi_note_at (loud.start_sec + loud.length_sec * 0.5f, loud.pitch)) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-vel-chord FAILED (select)\n");
		return false;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	if (_engine_snap.midi_selected_note_id != loud.id) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-vel-chord FAILED (selected id=%d want=%d)\n",
		              (int)_engine_snap.midi_selected_note_id, (int)loud.id);
		return false;
	}
	std::fprintf (stderr,
	              "airdour_win_shell: smoke-midi-vel-chord ok "
	              "(loud p=64 v=110 / quiet p=60 v=40)\n");
	return true;
}

void
ShellWindow::do_quantize_midi_notes ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (_engine.quantize_midi_notes ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_legato_midi_notes ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (_engine.legato_midi_notes ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_transpose_midi_notes (int semitones)
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (_engine.transpose_midi_notes (semitones)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_nudge_midi_velocity (int delta)
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (_engine.nudge_midi_velocity (delta)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_nudge_midi_length (float delta_sec)
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (_engine.nudge_midi_length (delta_sec)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_copy_midi_notes ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (_engine.copy_selected_midi_notes ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_cut_midi_notes ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (_engine.cut_selected_midi_notes ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_paste_midi_notes ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (_engine.paste_midi_notes (-1.f)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_duplicate_midi_notes ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (_engine.duplicate_selected_midi_notes ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_midi_panic ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	_engine.midi_panic ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_nudge_midi_program (int delta)
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (_engine.nudge_midi_program (delta)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_nudge_midi_bank (int delta)
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (_engine.nudge_midi_bank (delta)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_export_stems ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	int count = 0;
	if (_engine.export_stems (std::string (), &count)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_mute_all_tracks ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (_engine.mute_all_tracks ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		for (int i = 0; i < n_mixer_strips (); ++i) {
			_mixers[i].set_muted (true);
		}
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_solo_all_tracks ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (_engine.solo_all_tracks ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		for (int i = 0; i < n_mixer_strips (); ++i) {
			_mixers[i].set_soloed (true);
		}
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}


void
ShellWindow::do_humanize_midi_notes (float strength)
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (_engine.humanize_midi_notes (strength, 0)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_toggle_midi_step ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	_engine.toggle_midi_step_enabled ();
	_engine_snap = _engine.snapshot ();
	_transport.set_midi_step_enabled (_engine_snap.midi_step_enabled);
	_transport.set_midi_chord_quality (_engine_snap.midi_chord_quality);
	if (!_engine_snap.midi_step_enabled) {
		clear_step_piano_held ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_cycle_midi_chord ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	_engine.cycle_midi_chord_quality ();
	_engine_snap = _engine.snapshot ();
	_transport.set_midi_chord_quality (_engine_snap.midi_chord_quality);
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_toggle_midi_arp ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	_engine.toggle_midi_arp_enabled ();
	_engine_snap = _engine.snapshot ();
	_transport.set_midi_arp_enabled (_engine_snap.midi_arp_enabled);
	_transport.set_midi_arp_mode (_engine_snap.midi_arp_mode);
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_cycle_midi_arp_mode ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	_engine.cycle_midi_arp_mode ();
	_engine_snap = _engine.snapshot ();
	_transport.set_midi_arp_enabled (_engine_snap.midi_arp_enabled);
	_transport.set_midi_arp_mode (_engine_snap.midi_arp_mode);
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::sync_scale_to_transport ()
{
	_transport.set_midi_scale_lock (_engine_snap.midi_scale_lock);
	_transport.set_midi_scale_kind (_engine_snap.midi_scale_kind);
	_transport.set_midi_scale_root (_engine_snap.midi_scale_root);
}

void
ShellWindow::do_toggle_midi_scale ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	_engine.toggle_midi_scale_lock ();
	_engine_snap = _engine.snapshot ();
	sync_scale_to_transport ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_cycle_midi_scale_kind ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	_engine.cycle_midi_scale_kind ();
	if (!_engine.midi_scale_lock ()) {
		_engine.set_midi_scale_lock (true);
	}
	_engine_snap = _engine.snapshot ();
	sync_scale_to_transport ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_cycle_midi_scale_root ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	_engine.cycle_midi_scale_root ();
	if (!_engine.midi_scale_lock ()) {
		_engine.set_midi_scale_lock (true);
	}
	_engine_snap = _engine.snapshot ();
	sync_scale_to_transport ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::sync_arp_held_from_step_keys ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (_step_piano_held_count <= 0) {
		_engine.clear_midi_arp_held_pitches ();
		return;
	}
	uint8_t buf[16];
	int n = 0;
	for (int p = 0; p < 128 && n < 16; ++p) {
		if (_step_piano_held[p]) {
			buf[n++] = (uint8_t)p;
		}
	}
	_engine.set_midi_arp_held_pitches (buf, n);
}

void
ShellWindow::clear_step_piano_held ()
{
	for (int i = 0; i < 128; ++i) {
		_step_piano_held[i] = false;
	}
	_step_piano_held_count = 0;
	if (_engine.is_linked ()) {
		_engine.clear_midi_arp_held_pitches ();
	}
}

void
ShellWindow::do_step_input_pitch (uint8_t pitch, bool advance)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (_engine.step_input_midi_note (pitch, 100, advance)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		_playhead_sec = _engine_snap.transport_sec;
		sync_transport_clock_from_engine ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_step_piano_key_down (uint8_t pitch)
{
	if (pitch > 127) {
		return;
	}
	if (_step_piano_held[pitch]) {
		return; /* ignore WM_KEYDOWN autorepeat */
	}
	_step_piano_held[pitch] = true;
	++_step_piano_held_count;
	sync_arp_held_from_step_keys ();
	/* Place without advancing; advance on last key-up (multi-key chord). */
	do_step_input_pitch (pitch, false);
}

void
ShellWindow::do_step_piano_key_up (uint8_t pitch)
{
	if (pitch > 127 || !_step_piano_held[pitch]) {
		return;
	}
	_step_piano_held[pitch] = false;
	if (_step_piano_held_count > 0) {
		--_step_piano_held_count;
	}
	sync_arp_held_from_step_keys ();
	if (_step_piano_held_count > 0) {
		return;
	}
	/* Last piano key released → advance one grid step. */
	if (_engine.is_linked () && _engine_snap.session_ok) {
		_engine.step_advance (1);
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		_playhead_sec = _engine_snap.transport_sec;
		sync_transport_clock_from_engine ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

int
ShellWindow::step_piano_pitch_for_key (WPARAM wparam) const
{
	/* Classic two-octave computer keyboard (Z-row = C4, Q-row = C5). */
	struct Map { WPARAM key; uint8_t pitch; };
	static const Map kMap[] = {
		{ 'Z', 60 }, { 'S', 61 }, { 'X', 62 }, { 'D', 63 }, { 'C', 64 },
		{ 'V', 65 }, { 'G', 66 }, { 'B', 67 }, { 'H', 68 }, { 'N', 69 },
		{ 'J', 70 }, { 'M', 71 },
		{ 'Q', 72 }, { '2', 73 }, { 'W', 74 }, { '3', 75 }, { 'E', 76 },
		{ 'R', 77 }, { '5', 78 }, { 'T', 79 }, { '6', 80 }, { 'Y', 81 },
		{ '7', 82 }, { 'U', 83 },
	};
	WPARAM k = wparam;
	if (k >= 'a' && k <= 'z') {
		k = (WPARAM)(k - 'a' + 'A');
	}
	for (size_t i = 0; i < sizeof (kMap) / sizeof (kMap[0]); ++i) {
		if (kMap[i].key == k) {
			return (int)kMap[i].pitch;
		}
	}
	return -1;
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
	_transport.set_snap_density (_engine_snap.snap_density);
	_transport.set_midi_step_enabled (_engine_snap.midi_step_enabled);
	_transport.set_midi_chord_quality (_engine_snap.midi_chord_quality);
	InvalidateRect (_hwnd, nullptr, FALSE);
}

const wchar_t*
ShellWindow::snap_caption_suffix () const
{
	if (!_engine_snap.snap_enabled) {
		return L" OFF";
	}
	static const wchar_t* suf[] = {
		L" ON·bar", L" ON·beat", L" ON·1/2", L" ON·1/4"
	};
	int d = _engine_snap.snap_density;
	if (d < 0 || d > 3) {
		d = 1;
	}
	return suf[d];
}

bool
ShellWindow::smoke_snap_density_label ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-snap-density-label FAILED (no session)\n");
		std::fflush (stderr);
		return false;
	}
	static const wchar_t* want_on[] = { L"Bar", L"Beat", L"1/2", L"1/4" };
	static const wchar_t* want_cap[] = {
		L" ON·bar", L" ON·beat", L" ON·1/2", L" ON·1/4"
	};
	_engine.set_snap_enabled (true);
	for (int d = 0; d < 4; ++d) {
		_engine.set_snap_density (d);
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		_transport.set_snap_enabled (_engine_snap.snap_enabled);
		_transport.set_snap_density (_engine_snap.snap_density);
		if (wcscmp (_transport.snap_btn_label (), want_on[d]) != 0) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-snap-density-label FAILED "
			              "(btn dens=%d got=%ls want=%ls)\n",
			              d, _transport.snap_btn_label (), want_on[d]);
			std::fflush (stderr);
			return false;
		}
		if (wcscmp (snap_caption_suffix (), want_cap[d]) != 0) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-snap-density-label FAILED "
			              "(cap dens=%d got=%ls)\n",
			              d, snap_caption_suffix ());
			std::fflush (stderr);
			return false;
		}
	}
	_engine.set_snap_enabled (false);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	_transport.set_snap_enabled (false);
	_transport.set_snap_density (_engine_snap.snap_density);
	if (wcscmp (_transport.snap_btn_label (), L"Snap") != 0) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-snap-density-label FAILED (off label)\n");
		std::fflush (stderr);
		return false;
	}
	if (wcscmp (snap_caption_suffix (), L" OFF") != 0) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-snap-density-label FAILED (off cap)\n");
		std::fflush (stderr);
		return false;
	}
	float sx = 0.f;
	float sy = 0.f;
	if (!_transport.snap_btn_center (&sx, &sy)) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-snap-density-label FAILED (center)\n");
		std::fflush (stderr);
		return false;
	}
	std::fprintf (stderr,
	              "airdour_win_shell: smoke-snap-density-label ok "
	              "(Bar/Beat/1/2/1/4 · Snap off)\n");
	std::fflush (stderr);
	return true;
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

	/* Brand hero: display face; body/pane: text face; meta: mono for meters/DSP. */
	if (!make_fmt (L"Segoe UI Variable Display", L"Segoe UI", DWRITE_FONT_WEIGHT_BOLD, 44.f, &_fmt_brand)) {
		return false;
	}
	_fmt_brand->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_LEADING);
	_fmt_brand->SetParagraphAlignment (DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
	if (!make_fmt (L"Segoe UI Variable Text", L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, 15.f, &_fmt_body)) {
		return false;
	}
	if (!make_fmt (L"Cascadia Mono", L"Consolas", DWRITE_FONT_WEIGHT_MEDIUM, 12.f, &_fmt_meta)) {
		return false;
	}
	if (!make_fmt (L"Segoe UI Variable Text", L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 12.5f, &_fmt_pane)) {
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
	const float brand_h = 60.f * _dpi_scale;
	const float transport_h = 48.f * _dpi_scale;
	const float status_h = 36.f * _dpi_scale;
	const float gap = 14.f * _dpi_scale;

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
	float midi_h = 118.f * _dpi_scale; /* piano-roll + velocity lane */
	float midi_cc_h = 40.f * _dpi_scale; /* CC7 automation under MIDI */
	const bool show_spec = _engine_snap.spectrum_enabled;
	const bool show_phase = _engine_snap.correlation_enabled;
	const bool show_lufs = _engine_snap.loudness_enabled;
	const int meter_n = (show_spec ? 1 : 0) + (show_phase ? 1 : 0) + (show_lufs ? 1 : 0);
	float meter_h = (meter_n > 0) ? (72.f * _dpi_scale) : 0.f;
	float lanes_top = _editor_ruler.bottom + 4.f * _dpi_scale;
	float lanes_bot = _editor_pane.bottom - inset - midi_h - lane_gap
	                  - midi_cc_h - lane_gap
	                  - (meter_h > 0.f ? (meter_h + lane_gap) : 0.f);
	const int n_lanes = std::max (1, n_audio_lanes ());
	float lane_h = (lanes_bot - lanes_top - lane_gap * (float)(n_lanes - 1)) / (float)n_lanes;

	for (int i = 0; i < kTracksMax; ++i) {
		_lanes[i].bounds = D2D1::RectF ();
	}
	_auto_lane_bounds = D2D1::RectF ();
	_midi_cc_lane_bounds = D2D1::RectF ();
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
	_midi_cc_lane_bounds = D2D1::RectF (
		wave_left, _midi_lane.bottom + 3.f * _dpi_scale, wave_right,
		_midi_lane.bottom + 3.f * _dpi_scale + midi_cc_h);
	_spectrum_bounds = D2D1::RectF ();
	_correlation_bounds = D2D1::RectF ();
	_loudness_bounds = D2D1::RectF ();
	_spectrum.set_visible (false);
	_correlation.set_visible (false);
	_loudness.set_visible (false);
	if (meter_h > 0.f && meter_n > 0) {
		float meter_top = _midi_cc_lane_bounds.bottom + lane_gap;
		float meter_bot = meter_top + meter_h;
		float gap_m = 6.f * _dpi_scale;
		float span = wave_right - wave_left - gap_m * (float)(meter_n - 1);
		float cell = span / (float)meter_n;
		float x = wave_left;
		if (show_spec) {
			_spectrum_bounds = D2D1::RectF (x, meter_top, x + cell, meter_bot);
			_spectrum.layout (_spectrum_bounds);
			_spectrum.set_visible (true);
			x += cell + gap_m;
		}
		if (show_phase) {
			_correlation_bounds = D2D1::RectF (x, meter_top, x + cell, meter_bot);
			_correlation.layout (_correlation_bounds);
			_correlation.set_visible (true);
			x += cell + gap_m;
		}
		if (show_lufs) {
			_loudness_bounds = D2D1::RectF (x, meter_top, x + cell, meter_bot);
			_loudness.layout (_loudness_bounds);
			_loudness.set_visible (true);
		}
	}

	float mix_inset = 10.f * _dpi_scale;
	float mix_top = _mixer_pane.top + 36.f * _dpi_scale;
	float mix_bot = _mixer_pane.bottom - mix_inset;
	float mix_inner = _mixer_pane.right - _mixer_pane.left - mix_inset * 2.f;
	float strip_gap = 6.f * _dpi_scale;
	const int n_mix = std::max (1, n_mixer_strips ());
	/* Reserve History + Monitor strips on the right. */
	float mon_w = 56.f * _dpi_scale;
	float hist_w = 128.f * _dpi_scale;
	float strips_inner = mix_inner - mon_w - hist_w - strip_gap * 2.f;
	if (strips_inner < 40.f * _dpi_scale) {
		hist_w = 110.f * _dpi_scale;
		mon_w = 48.f * _dpi_scale;
		strips_inner = mix_inner - mon_w - hist_w - strip_gap * 2.f;
	}
	float equal_w = (strips_inner - strip_gap * (float)(n_mix - 1)) / (float)n_mix;
	float narrow_w = 52.f * _dpi_scale;
	float strip_w = (_engine_snap.mixer_narrow && equal_w > narrow_w)
		? narrow_w : equal_w;
	float min_w = _engine_snap.mixer_narrow ? narrow_w : 64.f * _dpi_scale;
	if (_mixer_force_min_w > 1.f) {
		min_w = _mixer_force_min_w * _dpi_scale;
	}
	if (strip_w < min_w) {
		strip_w = min_w;
	}
	float content_w = strip_w * (float)n_mix + strip_gap * (float)(n_mix - 1);
	_mixer_scroll_max = std::max (0.f, content_w - strips_inner);
	if (_mixer_scroll_x > _mixer_scroll_max) {
		_mixer_scroll_x = _mixer_scroll_max;
	}
	if (_mixer_scroll_x < 0.f) {
		_mixer_scroll_x = 0.f;
	}
	float strips_left = _mixer_pane.left + mix_inset;
	_mixer_strips_clip = D2D1::RectF (strips_left, mix_top,
	                                  strips_left + strips_inner, mix_bot);
	for (int i = 0; i < kMixerStripsMax; ++i) {
		_mixers[i].layout (D2D1::RectF (0, 0, 0, 0), _dpi_scale);
		_mixers[i].set_narrow (false);
	}
	for (int i = 0; i < n_mix; ++i) {
		_mixers[i].set_narrow (_engine_snap.mixer_narrow && !_mixers[i].is_midi_strip ());
		float x0 = strips_left + (float)i * (strip_w + strip_gap) - _mixer_scroll_x;
		_mixers[i].layout (D2D1::RectF (x0, mix_top, x0 + strip_w, mix_bot), _dpi_scale);
	}
	float mon_x = _mixer_pane.right - mix_inset - mon_w;
	float hist_x = mon_x - strip_gap - hist_w;
	_history.layout (D2D1::RectF (hist_x, mix_top, hist_x + hist_w, mix_bot), _dpi_scale);
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
	case WM_KEYUP:
		if (_engine_snap.midi_step_enabled
		    && !(GetKeyState (VK_CONTROL) & 0x8000)
		    && !(GetKeyState (VK_MENU) & 0x8000)
		    && step_piano_pitch_for_key (wparam) >= 0) {
			do_step_piano_key_up ((uint8_t)step_piano_pitch_for_key (wparam));
			return 0;
		}
		break;
	case WM_KEYDOWN:
		if (_about.splash_visible ()) {
			/* Any key dismisses splash (smokes use --no-splash / --timeout). */
			dismiss_splash ();
			return 0;
		}
		if (_recovery.visible ()) {
			if (wparam == VK_ESCAPE || wparam == 'N' || wparam == 'n') {
				apply_recovery_hit (RecoveryHit::Discard);
			} else if (wparam == VK_RETURN || wparam == 'Y' || wparam == 'y') {
				apply_recovery_hit (RecoveryHit::Restore);
			}
			return 0;
		}
		if (wparam == VK_ESCAPE) {
			if (_engine_snap.has_selection) {
				/* Esc = clear region selection (quit via second Esc). */
				do_clear_selection ();
			} else if (_recent.visible ()) {
				_recent.set_visible (false);
				InvalidateRect (_hwnd, nullptr, FALSE);
			} else if (_prefs_visible) {
				_prefs_visible = false;
				_prefs.set_visible (false);
				InvalidateRect (_hwnd, nullptr, FALSE);
			} else if (_about.about_visible ()) {
				_about.set_about_visible (false);
				InvalidateRect (_hwnd, nullptr, FALSE);
			} else if (_help_visible) {
				_help_visible = false;
				InvalidateRect (_hwnd, nullptr, FALSE);
			} else {
				PostQuitMessage (0);
			}
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'O' || wparam == 'o')
		           && (GetKeyState (VK_SHIFT) & 0x8000)) {
			/* Ctrl+Shift+O — recent sessions menu. */
			toggle_recent_menu ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == VK_OEM_COMMA || wparam == VK_OEM_PERIOD)) {
			/* Ctrl+Alt+,/. = prev/next cue marker. */
			if (wparam == VK_OEM_PERIOD) {
				do_locate_next_cue ();
			} else {
				do_locate_prev_cue ();
			}
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == VK_OEM_COMMA || wparam == VK_OEM_PERIOD)) {
			/* Alt+, /. = nudge playhead by snap unit. */
			do_nudge_playhead (wparam == VK_OEM_PERIOD ? 1 : -1);
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && wparam == VK_OEM_COMMA) {
			/* Ctrl+, — Preferences. */
			toggle_prefs ();
		} else if (wparam == VK_F3) {
			/* F3 — About Airdour. */
			toggle_about ();
		} else if (wparam == VK_F1
		           || ((wparam == VK_OEM_2 || wparam == VK_DIVIDE)
		               && (GetKeyState (VK_SHIFT) & 0x8000))) {
			/* F1 or Shift+/ (?) — toggle shortcuts help. */
			toggle_help ();
		} else if (wparam == VK_TAB) {
			/* Tab / Shift+Tab = cycle region selection forward/back. */
			do_cycle_region_selection ((GetKeyState (VK_SHIFT) & 0x8000) ? -1 : 1);
		} else if (wparam == VK_SPACE) {
			if (_transport_playing) {
				_engine.midi_panic ();
			}
			transport_toggle ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == 'D' || wparam == 'd')) {
			if (_engine_snap.has_selection) {
				do_duplicate_selection ();
			} else {
				do_duplicate_midi_notes ();
			}
		} else if (wparam == VK_DELETE || wparam == VK_BACK) {
			do_delete_selection ();
		} else if ((GetKeyState (VK_SHIFT) & 0x8000) && wparam == VK_HOME
		           && !(GetKeyState (VK_CONTROL) & 0x8000)) {
			/* Shift+Home = locate to selection start. */
			do_locate_selection_start ();
		} else if ((GetKeyState (VK_SHIFT) & 0x8000) && wparam == VK_END
		           && !(GetKeyState (VK_CONTROL) & 0x8000)) {
			/* Shift+End = locate to selection end. */
			do_locate_selection_end ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == '0' || wparam == VK_NUMPAD0)) {
			/* Ctrl+Alt+0 = center editor view on playhead (keep zoom). */
			do_center_on_playhead ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && (wparam == '0' || wparam == VK_NUMPAD0)) {
			/* Ctrl+Shift+0 = scroll view to keep playhead visible (keep zoom). */
			do_scroll_playhead_into_view ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_MENU) & 0x8000)
		           && wparam == VK_HOME) {
			/* Ctrl+Alt+Home = zoom editor to session extent. */
			do_zoom_to_session ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && wparam == VK_HOME) {
			/* Ctrl+Home = locate session start. */
			do_locate_session_start ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && wparam == VK_END) {
			/* Ctrl+End = locate session end. */
			do_locate_session_end ();
		} else if (wparam == VK_HOME || wparam == '0') {
			reset_shared_view ();
			refresh_editor_view_after_nav ();
			InvalidateRect (_hwnd, nullptr, FALSE);
		} else if (wparam == VK_F4) {
			/* F4 — toggle MIDI step-record. */
			do_toggle_midi_step ();
		} else if (wparam == VK_F5) {
			/* F5 — cycle MIDI chord quality Off → Maj → Min. */
			do_cycle_midi_chord ();
		} else if (wparam == VK_F6) {
			/* F6 — toggle arp; Shift+F6 cycle Up/Down/UpDown. */
			if (GetKeyState (VK_SHIFT) & 0x8000) {
				do_cycle_midi_arp_mode ();
			} else {
				do_toggle_midi_arp ();
			}
		} else if (wparam == VK_F7) {
			/* F7 — scale lock; Shift=kind; Ctrl=root. */
			if (GetKeyState (VK_CONTROL) & 0x8000) {
				do_cycle_midi_scale_root ();
			} else if (GetKeyState (VK_SHIFT) & 0x8000) {
				do_cycle_midi_scale_kind ();
			} else {
				do_toggle_midi_scale ();
			}
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'A' || wparam == 'a')) {
			/* Alt+Shift+A — cycle editor auto lane Gain → Pan → Send. */
			cycle_auto_lane_kind ();
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'Y' || wparam == 'y'
		               || wparam == 'P' || wparam == 'p'
		               || wparam == 'Q' || wparam == 'q')) {
			/* Alt+Shift+Y — cycle AutoState for current lane (P/Q redirect). */
			do_cycle_auto_state ();
		} else if (wparam == VK_F8) {
			/* F8 — cycle MIDI CC lane controller Volume→Mod→Expr. */
			cycle_midi_cc_controller ();
		} else if (wparam == VK_F9) {
			/* F9 — MIDI panic. */
			do_midi_panic ();
		} else if (_engine_snap.midi_step_enabled
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == VK_LEFT || wparam == VK_RIGHT)) {
			/* Step mode: ←/→ = rest back/forward one grid. */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				clear_step_piano_held ();
				_engine.step_advance (wparam == VK_RIGHT ? 1 : -1);
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				_playhead_sec = _engine_snap.transport_sec;
				sync_transport_clock_from_engine ();
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
		} else if (_engine_snap.midi_step_enabled
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && step_piano_pitch_for_key (wparam) >= 0) {
			do_step_piano_key_down ((uint8_t)step_piano_pitch_for_key (wparam));
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == VK_LEFT || wparam == VK_RIGHT)
		           && _engine_snap.has_selection) {
			/* Ctrl+←/→ = nudge selected region by snap unit (Shift = 4×). */
			float unit = _engine.snap_unit_sec ();
			if (!_engine_snap.snap_enabled) {
				unit = 0.05f;
			}
			if (GetKeyState (VK_SHIFT) & 0x8000) {
				unit *= 4.f;
			}
			do_nudge_selection (wparam == VK_RIGHT ? unit : -unit);
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == VK_LEFT || wparam == VK_RIGHT)) {
			/* Ctrl+Alt+←/→ = previous / next marker. */
			if (wparam == VK_RIGHT) {
				do_locate_next_marker ();
			} else {
				do_locate_prev_marker ();
			}
		} else if (wparam == VK_LEFT) {
			float step = (GetKeyState (VK_SHIFT) & 0x8000) ? 0.5f : 0.15f;
			do_scroll_editor (-step);
		} else if (wparam == VK_RIGHT) {
			float step = (GetKeyState (VK_SHIFT) & 0x8000) ? 0.5f : 0.15f;
			do_scroll_editor (step);
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == 'A' || wparam == 'a')) {
			/* Ctrl+Alt+A = zoom editor to all regions. */
			do_zoom_to_regions ();
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'A' || wparam == 'a')) {
			if (GetKeyState (VK_SHIFT) & 0x8000) {
				do_fit_session ();
			} else {
				do_fit_all_regions ();
			}
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == 'E' || wparam == 'e')) {
			/* Ctrl+Alt+E = zoom editor to selection. */
			do_zoom_to_selection ();
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'E' || wparam == 'e')) {
			if (GetKeyState (VK_SHIFT) & 0x8000) {
				/* Alt+Shift+E = export per-track stems. */
				do_export_stems ();
			} else {
				/* Alt+E = export selection range WAV. */
				prompt_export_selection_wav ();
			}
		} else if ((GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == 'E' || wparam == 'e')) {
			/* Shift+E = activate region gain envelope. */
			do_ensure_region_envelope ();
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'E' || wparam == 'e')
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && !(GetKeyState (VK_SHIFT) & 0x8000)) {
			/* E = fit selection (Ctrl+E remains export). */
			do_fit_selection ();
		} else if (wparam == 'L' || wparam == 'l') {
			if ((GetKeyState (VK_CONTROL) & 0x8000)
			    && (GetKeyState (VK_SHIFT) & 0x8000)
			    && !(GetKeyState (VK_MENU) & 0x8000)) {
				/* Ctrl+Shift+L = loop range ← selection. */
				do_loop_from_selection ();
			} else if ((GetKeyState (VK_CONTROL) & 0x8000)
			    && (GetKeyState (VK_MENU) & 0x8000)
			    && _engine_snap.has_selection) {
				/* Ctrl+Alt+L = position-lock selected region. */
				do_toggle_region_position_lock ();
			} else if ((GetKeyState (VK_CONTROL) & 0x8000)
			    && (GetKeyState (VK_MENU) & 0x8000)) {
				/* Ctrl+Alt+L = zoom editor to loop range. */
				do_zoom_to_loop ();
			} else if ((GetKeyState (VK_CONTROL) & 0x8000)
			           && !(GetKeyState (VK_SHIFT) & 0x8000)
			           && !(GetKeyState (VK_MENU) & 0x8000)) {
				/* Ctrl+L = lock / unlock selected region (blocks move/trim). */
				do_toggle_selection_locked ();
			} else {
				do_toggle_loop ();
			}
		} else if (wparam == 'O' || wparam == 'o') {
			if ((GetKeyState (VK_MENU) & 0x8000)
			    && !(GetKeyState (VK_CONTROL) & 0x8000)) {
				/* Alt+O = toggle region opaque / transparent. */
				do_toggle_selection_opaque ();
			}
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'L' || wparam == 'l')) {
			/* Alt+Shift+L — cycle Solo → PFL → AFL → Solo. */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				_engine.cycle_listen_mode ();
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_listen_from_engine ();
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
		} else if (!(GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == 'L' || wparam == 'l')) {
			do_toggle_loop ();
		} else if (wparam == 'P' || wparam == 'p') {
			if ((GetKeyState (VK_CONTROL) & 0x8000)
			    && (GetKeyState (VK_SHIFT) & 0x8000)
			    && !(GetKeyState (VK_MENU) & 0x8000)) {
				/* Ctrl+Shift+P = punch range ← selection. */
				do_punch_from_selection ();
			} else if ((GetKeyState (VK_CONTROL) & 0x8000)
			    && (GetKeyState (VK_MENU) & 0x8000)) {
				/* Ctrl+Alt+P = zoom editor to punch range. */
				do_zoom_to_punch ();
			} else if (!(GetKeyState (VK_CONTROL) & 0x8000)) {
				do_toggle_punch ();
			}
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_MENU) & 0x8000)
		           && wparam == VK_OEM_4) {
			/* Ctrl+Alt+[ = locate punch in. */
			do_locate_punch_in ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_MENU) & 0x8000)
		           && wparam == VK_OEM_6) {
			/* Ctrl+Alt+] = locate punch out. */
			do_locate_punch_out ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && wparam == VK_OEM_6) {
			/* Ctrl+Alt+Shift+] = cycle region layer top↔bottom. */
			do_cycle_region_layer ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && _engine_snap.has_selection
		           && (wparam == VK_OEM_4 || wparam == VK_OEM_6)) {
			/* Ctrl+[ / ] = lower / raise selected region (Shift = bottom / top). */
			bool extreme = (GetKeyState (VK_SHIFT) & 0x8000) != 0;
			if (wparam == VK_OEM_6) {
				do_raise_selection (extreme);
			} else {
				do_lower_selection (extreme);
			}
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && _engine_snap.has_selection
		           && (wparam == VK_OEM_4 || wparam == VK_OEM_6)) {
			/* Alt+[ / ] = nudge fade-in ±10ms; Alt+Shift = fade-out. */
			float delta = (wparam == VK_OEM_6 ? 0.010f : -0.010f);
			if (GetKeyState (VK_SHIFT) & 0x8000) {
				_engine.nudge_selected_fade_out_sec (delta);
			} else {
				_engine.nudge_selected_fade_in_sec (delta);
			}
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			apply_region_selection_to_lanes ();
			InvalidateRect (_hwnd, nullptr, FALSE);
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && wparam == VK_OEM_4) {
			/* Ctrl+[ = locate loop start. */
			do_locate_loop_start ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && wparam == VK_OEM_6) {
			/* Ctrl+] = locate loop end. */
			do_locate_loop_end ();
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
			if ((GetKeyState (VK_MENU) & 0x8000) && (GetKeyState (VK_SHIFT) & 0x8000)) {
				/* Alt+Shift+M = mute all audio tracks + MIDI. */
				do_mute_all_tracks ();
			} else if ((GetKeyState (VK_MENU) & 0x8000)
			           && !(GetKeyState (VK_SHIFT) & 0x8000)
			           && !(GetKeyState (VK_CONTROL) & 0x8000)) {
				/* Alt+M = clear all mutes. */
				do_clear_all_mutes ();
			} else if ((GetKeyState (VK_CONTROL) & 0x8000)
			    && !(GetKeyState (VK_SHIFT) & 0x8000)
			    && !(GetKeyState (VK_MENU) & 0x8000)) {
				/* Ctrl+M = mute / unmute selected region. */
				do_toggle_selection_muted ();
			} else if ((GetKeyState (VK_CONTROL) & 0x8000)
			    && (GetKeyState (VK_SHIFT) & 0x8000)) {
				/* Ctrl+Shift+M = lock/unlock marker at playhead. */
				do_toggle_marker_lock_at_playhead ();
			} else if ((GetKeyState (VK_CONTROL) & 0x8000)
			           && (GetKeyState (VK_MENU) & 0x8000)) {
				/* Ctrl+Alt+M = toggle CD flag on marker at playhead. */
				do_toggle_marker_cd_at_playhead ();
			} else if (GetKeyState (VK_SHIFT) & 0x8000) {
				do_remove_marker_at_playhead ();
			} else {
				do_add_marker_at_playhead ();
			}
		
		} else if ((GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'Z' || wparam == 'z')) {
			do_undo ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == 'Y' || wparam == 'y')) {
			/* Ctrl+Alt+Y = sync at playhead; Shift clears. */
			do_region_sync_at_playhead ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'Y' || wparam == 'y')) {
			do_redo ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && (wparam == 'C' || wparam == 'c')) {
			/* Ctrl+Alt+Shift+C = cycle scene MIDI channel. */
			do_cycle_scene_channel ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'C' || wparam == 'c')) {
			do_copy_midi_notes ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'X' || wparam == 'x')) {
			do_cut_midi_notes ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'V' || wparam == 'v')) {
			do_paste_midi_notes ();
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && (wparam == 'S' || wparam == 's')) {
			/* Alt+Shift+S = rename nearest section marker. */
			do_rename_section_at_playhead ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'S' || wparam == 's')) {
			do_save_session ();
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'S' || wparam == 's')) {
			do_split_at_playhead ();
		} else if (wparam == 'F' || wparam == 'f') {
			if ((GetKeyState (VK_CONTROL) & 0x8000)
			    && !(GetKeyState (VK_MENU) & 0x8000)) {
				/* Ctrl+F = toggle fade in/out active. */
				do_toggle_fade_active ();
			} else if (GetKeyState (VK_SHIFT) & 0x8000) {
				/* Shift+F = cycle fade shape (Linear→EqualPower→Fast→Slow→Symmetric). */
				do_cycle_fade_shape ();
			} else if (!(GetKeyState (VK_CONTROL) & 0x8000)) {
				do_apply_crossfade ();
			}
			} else if ((GetKeyState (VK_MENU) & 0x8000)
			           && (GetKeyState (VK_SHIFT) & 0x8000)
			           && !(GetKeyState (VK_CONTROL) & 0x8000)
			           && (wparam == 'T' || wparam == 't')) {
				/* Alt+Shift+T — reset focused strip trim to unity. */
				if (_engine.is_linked () && _engine_snap.session_ok) {
					int ti = _focus_audio_track;
					if (ti < 0) {
						ti = 0;
					}
					if (ti < n_audio_lanes ()) {
						_engine.set_track_trim (ti, 1.f);
						_engine.poll ();
						_engine_snap = _engine.snapshot ();
						sync_mixer_trim_from_engine ();
						InvalidateRect (_hwnd, nullptr, FALSE);
					}
				}
			} else if (!(GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == 'T' || wparam == 't')) {
			/* T = stretch 1.5×; Shift+T = compress 0.75× (pitch unchanged). */
			do_stretch_selection ((GetKeyState (VK_SHIFT) & 0x8000) ? 0.75f : 1.5f);
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == 'R' || wparam == 'r')) {
			/* Ctrl+Shift+R = range marker ← selection. */
			do_range_marker_from_selection ();
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'R' || wparam == 'r')) {
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'R' || wparam == 'r')) {
			/* Alt+Shift+R — reset focused strip fader/pan/send. */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				int ti = _focus_audio_track;
				if (ti < 0) {
					ti = 0;
				}
				if (ti < n_audio_lanes ()) {
					_engine.reset_strip_mixer (ti);
					_engine.poll ();
					_engine_snap = _engine.snapshot ();
					sync_mixer_gains_from_engine ();
					sync_mixer_pan_from_engine ();
					sync_mixer_send_from_engine ();
					InvalidateRect (_hwnd, nullptr, FALSE);
				}
			}
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == 'R' || wparam == 'r')) {
			/* R = pitch +1 st; Shift+R = −1 st (duration held). */
			do_pitch_shift_selection ((GetKeyState (VK_SHIFT) & 0x8000) ? -1.f : 1.f);
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'V' || wparam == 'v')) {
			/* Alt+Shift+V — ensure VCA + toggle Audio 1 assign. */
			do_toggle_vca_assign ();
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'V' || wparam == 'v')
		           && !(GetKeyState (VK_MENU) & 0x8000)) {
			/* V = reverse selected audio region. */
			do_reverse_selection ();
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'N' || wparam == 'n')) {
			/* Alt+Shift+N — toggle Route comment ("note") on focused strip. */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				int ti = _focus_audio_track;
				if (ti < 0) {
					ti = 0;
				}
				if (ti < n_audio_lanes ()) {
					_engine.toggle_track_comment (ti);
					_engine.poll ();
					_engine_snap = _engine.snapshot ();
					sync_mixer_comment_from_engine ();
					InvalidateRect (_hwnd, nullptr, FALSE);
				}
			}
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'W' || wparam == 'w')) {
			/* Alt+Shift+W — toggle narrow mixer strips. */
			_engine.toggle_mixer_narrow ();
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			layout_panes ();
			InvalidateRect (_hwnd, nullptr, FALSE);
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'D' || wparam == 'd')) {
			/* Alt+Shift+D — cycle monitor dim depth −20/−12/−6. */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				_engine.cycle_monitor_dim_level ();
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_monitor_from_engine ();
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'N' || wparam == 'n')) {
			/* N = normalize selection to −1 dBFS. */
			do_normalize_selection (-1.f);
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'I' || wparam == 'i')) {
			/* Alt+Shift+I — toggle polarity invert on focused strip. */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				int ti = _focus_audio_track;
				if (ti < 0) {
					ti = 0;
				}
				if (ti < n_audio_lanes ()) {
					_engine.toggle_track_phase_invert (ti);
					_engine.poll ();
					_engine_snap = _engine.snapshot ();
					sync_mixer_phase_from_engine ();
					InvalidateRect (_hwnd, nullptr, FALSE);
				}
			}
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == 'I' || wparam == 'i')) {
			/* I = strip silence on selection (−40 dBFS threshold). */
			do_strip_silence_selection ();
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'B' || wparam == 'b')) {
			/* Alt+Shift+B — activate every shell ACE insert (Reverb…Gate). */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				_engine.set_all_fx_bypassed (false);
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_fx_from_engine ();
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && !(GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'B' || wparam == 'b')) {
			/* Alt+B — bypass every shell ACE insert (Reverb…Gate). */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				_engine.set_all_fx_bypassed (true);
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_fx_from_engine ();
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == 'B' || wparam == 'b')) {
			/* B = consolidate / bounce selection in place. */
			do_consolidate_selection ();
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'Z' || wparam == 'z')) {
			/* Z = freeze / unfreeze focused (or selected) audio track. */
			do_toggle_freeze_track ();
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'G' || wparam == 'g')) {
			if (GetKeyState (VK_SHIFT) & 0x8000) {
				do_cycle_snap_density ();
			} else {
				do_toggle_snap ();
			}
		} else if ((wparam == 'Q' || wparam == 'q')) {
			if ((GetKeyState (VK_CONTROL) & 0x8000) && (GetKeyState (VK_MENU) & 0x8000)) {
				/* Ctrl+Alt+Q = legato selected (or all) MIDI notes. */
				do_legato_midi_notes ();
			} else if (!(GetKeyState (VK_CONTROL) & 0x8000)) {
				/* Q = quantize selected (or all) MIDI notes to snap grid. */
				do_quantize_midi_notes ();
			}
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == 'H' || wparam == 'h')) {
			/* Ctrl+Alt+H = toggle Comp SC Audio1→Audio2. */
			do_toggle_comp_sidechain ();
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'H' || wparam == 'h')) {
			/* H = humanize; Shift+H = 2× strength. */
			do_humanize_midi_notes ((GetKeyState (VK_SHIFT) & 0x8000) ? 2.f : 1.f);
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000)

		           && (wparam == VK_OEM_PERIOD || wparam == VK_OEM_COMMA)) {
			/* . / , = MIDI transpose +1 / −1; Shift = ±12 (octave). */
			int st = (wparam == VK_OEM_PERIOD) ? 1 : -1;
			if (GetKeyState (VK_SHIFT) & 0x8000) {
				st *= 12;
			}
			do_transpose_midi_notes (st);
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'C' || wparam == 'c')) {
			/* Alt+C — cycle PresentationInfo color on focused audio track. */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				int idx = _focus_audio_track;
				if (_engine_snap.has_selection && _engine_snap.selection_track >= 0) {
					idx = _engine_snap.selection_track;
				}
				if (idx < 0 || idx >= n_audio_lanes ()) {
					idx = 0;
				}
				if (_engine.cycle_track_color (idx)) {
					_engine.poll ();
					_engine_snap = _engine.snapshot ();
					sync_track_labels_from_engine ();
					InvalidateRect (_hwnd, nullptr, FALSE);
				}
			}
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == 'C' || wparam == 'c')) {
			do_toggle_count_in ();
		} else if (wparam == 'K' || wparam == 'k') {
			if ((GetKeyState (VK_CONTROL) & 0x8000)
			    && (GetKeyState (VK_SHIFT) & 0x8000)
			    && !(GetKeyState (VK_MENU) & 0x8000)) {
				/* Ctrl+Shift+K = skip range ← selection. */
				do_skip_range_from_selection ();
			} else if (!(GetKeyState (VK_CONTROL) & 0x8000)) {
				do_toggle_click ();
			}
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'K' || wparam == 'k')) {
			/* Alt+Shift+K — cycle monitor solo boost 0/+3/+6. */
			do_cycle_monitor_solo_boost ();
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == 'K' || wparam == 'k')) {
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'H' || wparam == 'h')) {
			/* Alt+Shift+H — cycle metronome click gain. */
			do_cycle_click_gain ();
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'K' || wparam == 'k')) {
			do_toggle_click ();
		} else if (wparam == VK_OEM_MINUS || wparam == VK_SUBTRACT) {
			do_nudge_tempo (-1.f);
		} else if (wparam == VK_OEM_PLUS || wparam == VK_ADD) {
			do_nudge_tempo (1.f);
		} else if ((GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'O' || wparam == 'o')
		           && !(GetKeyState (VK_SHIFT) & 0x8000)) {
			prompt_open_session (); /* Ctrl+O browse; Ctrl+Shift+O = recent menu */
		} else if ((GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'E' || wparam == 'e')) {
			prompt_export_wav ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == VK_OEM_PLUS || wparam == VK_ADD)) {
			/* Ctrl+Alt+= = zoom in around playhead. */
			do_zoom_around_playhead (1.15f);
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == VK_OEM_MINUS || wparam == VK_SUBTRACT)) {
			/* Ctrl+Alt+− = zoom out around playhead. */
			do_zoom_around_playhead (1.f / 1.15f);
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
			if ((GetKeyState (VK_MENU) & 0x8000)
			    && (GetKeyState (VK_SHIFT) & 0x8000)) {
				/* Alt+Shift+F2 = rename marker at playhead. */
				do_rename_marker_at_playhead ();
			} else if (GetKeyState (VK_SHIFT) & 0x8000) {
				/* Shift+F2 = rename selected region. */
				prompt_rename_selected_region ();
			} else {
				do_rename_focused_track ();
			}
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == VK_UP || wparam == VK_DOWN)
		           && _engine_snap.has_selection) {
			/* Ctrl+Shift+↑/↓ = nudge selected region gain ±1 dB. */
			do_nudge_selection_gain_db (wparam == VK_UP ? 1.0f : -1.0f);
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == VK_UP || wparam == VK_DOWN)
		           && _engine_snap.has_selection) {
			/* Ctrl+Alt+↑/↓ = nudge region envelope mid gain. */
			do_nudge_region_envelope_mid (wparam == VK_UP ? 0.08f : -0.08f);
		} else if ((GetKeyState (VK_MENU) & 0x8000) && wparam == VK_UP) {
			do_move_audio_track (-1);
		} else if ((GetKeyState (VK_MENU) & 0x8000) && wparam == VK_DOWN) {
			do_move_audio_track (+1);
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == VK_LEFT || wparam == VK_RIGHT)) {
			/* Alt+←/→ = MIDI note length ± (snap unit when snap on; else 50ms). */
			if (_engine.is_linked () && _engine_snap.midi_note_count > 0) {
				float unit = _engine.snap_unit_sec ();
				if (!_engine_snap.snap_enabled) {
					unit = 0.05f;
				}
				if (GetKeyState (VK_SHIFT) & 0x8000) {
					unit *= 4.f;
				}
				do_nudge_midi_length (wparam == VK_RIGHT ? unit : -unit);
			}
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == VK_UP || wparam == VK_DOWN)) {
			/* ↑/↓ = MIDI velocity ±1 (Shift=±8) when notes exist. */
			if (_engine.is_linked () && _engine_snap.midi_note_count > 0) {
				int d = (wparam == VK_UP) ? 1 : -1;
				if (GetKeyState (VK_SHIFT) & 0x8000) {
					d *= 8;
				}
				do_nudge_midi_velocity (d);
			}
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'Y' || wparam == 'y')) {
			if (_engine.is_linked ()) {
				if (GetKeyState (VK_SHIFT) & 0x8000) {
					_engine.toggle_spectrum_master ();
				} else {
					_engine.toggle_spectrum_enabled ();
				}
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				layout_panes ();
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'X' || wparam == 'x')) {
			if (_engine.is_linked ()) {
				if (GetKeyState (VK_SHIFT) & 0x8000) {
					_engine.toggle_correlation_master ();
				} else {
					_engine.toggle_correlation_enabled ();
				}
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				layout_panes ();
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'J' || wparam == 'j')) {
			if (_engine.is_linked ()) {
				if (GetKeyState (VK_SHIFT) & 0x8000) {
					_engine.toggle_loudness_master ();
				} else {
					_engine.toggle_loudness_enabled ();
				}
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				layout_panes ();
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
		} else if (wparam == 'U' || wparam == 'u') {
			if ((GetKeyState (VK_MENU) & 0x8000) && (GetKeyState (VK_SHIFT) & 0x8000)) {
				/* Alt+Shift+U = solo all audio tracks + MIDI. */
				do_solo_all_tracks ();
			} else if ((GetKeyState (VK_MENU) & 0x8000)
			           && !(GetKeyState (VK_SHIFT) & 0x8000)
			           && !(GetKeyState (VK_CONTROL) & 0x8000)) {
				/* Alt+U = clear all solos. */
				do_clear_all_solos ();
			} else if (!(GetKeyState (VK_CONTROL) & 0x8000)) {
				do_apply_or_clear_group ();
			}
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'G' || wparam == 'g')) {
			/* Alt+Shift+G — toggle RouteGroup relative gain. */
			do_toggle_group_relative ();
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == VK_LEFT || wparam == VK_RIGHT)) {
			/* Alt+Shift+←/→ — scroll mixer strips when overflowing. */
			float step = 64.f * _dpi_scale;
			do_scroll_mixer (wparam == VK_RIGHT ? step : -step);
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'C' || wparam == 'c')) {
			/* Alt+Shift+C — cycle RouteGroup color palette. */
			do_cycle_group_color ();
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'O' || wparam == 'o')) {
			/* Alt+Shift+O — cycle focused strip monitor Auto→In→Disk. */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				int ti = _focus_audio_track;
				if (ti < 0) {
					ti = 0;
				}
				if (ti < n_audio_lanes ()) {
					_engine.cycle_track_monitor_choice (ti);
					_engine.poll ();
					_engine_snap = _engine.snapshot ();
					sync_mixer_monitor_from_engine ();
					InvalidateRect (_hwnd, nullptr, FALSE);
				}
			}
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'U' || wparam == 'u')) {
			do_apply_or_clear_group ();
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'E' || wparam == 'e')) {
			/* Alt+Shift+E — toggle focused track Route::active (group shares). */
			do_toggle_focused_track_active ();
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
		/* Alt+dbl-click fader/pan/send → reset that strip (unity/center/0). */
		if ((GetKeyState (VK_MENU) & 0x8000) && _engine.is_linked ()
		    && _engine_snap.session_ok) {
			int si = hit_mixer_fader (x, y);
			if (si < 0) {
				si = hit_mixer_pan (x, y);
			}
			if (si < 0) {
				si = hit_mixer_send (x, y);
			}
			if (si >= 0 && si < n_audio_lanes ()) {
				_focus_audio_track = si;
				_engine.reset_strip_mixer (si);
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_gains_from_engine ();
				sync_mixer_pan_from_engine ();
				sync_mixer_send_from_engine ();
				InvalidateRect (_hwnd, nullptr, FALSE);
				return 0;
			}
		}
		/* Double-click FX param → insert default (before rename). */
		if (_engine.is_linked () && _engine_snap.session_ok) {
			int which = 0;
			int fxi = -1;
			if ((fxi = hit_mixer_fx_param3 (x, y)) >= 0
			    && _mixers[fxi].has_fx () && _mixers[fxi].fx_param_count () >= 3) {
				which = 3;
			} else if ((fxi = hit_mixer_fx_param2 (x, y)) >= 0
			           && _mixers[fxi].has_fx () && _mixers[fxi].fx_param_count () >= 2) {
				which = 2;
			} else if ((fxi = hit_mixer_fx_param (x, y)) >= 0 && _mixers[fxi].has_fx ()) {
				which = 1;
			}
			if (which > 0 && fxi >= 0 && fxi < n_audio_lanes ()) {
				if (_engine.reset_track_fx_param (fxi, which)) {
					_engine.poll ();
					_engine_snap = _engine.snapshot ();
					sync_mixer_fx_from_engine ();
					InvalidateRect (_hwnd, nullptr, FALSE);
				}
				return 0;
			}
		}
		int trim_i = hit_mixer_trim (x, y);
		if (trim_i >= 0 && _engine.is_linked () && _engine_snap.session_ok) {
			/* Double-click trim → unity (0 dB). */
			_engine.set_track_trim (trim_i, 1.f);
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_trim_from_engine ();
			InvalidateRect (_hwnd, nullptr, FALSE);
			return 0;
		}
		if (_monitor.hit_aux_return (x, y)) {
			do_set_aux_return (1.f); /* unity */
			return 0;
		}
		/* Double-click fader → unity; pan → center (before rename hit). */
		int fi = hit_mixer_fader (x, y);
		if (fi >= 0 && _engine.is_linked () && _engine_snap.session_ok) {
			_mixers[fi].set_gain (1.f);
			if (fi == midi_mixer_index ()) {
				_engine.set_midi_gain (1.f);
			} else {
				_engine.set_track_gain (fi, 1.f);
				if (fi == 0) {
					_engine_snap.gain = 1.f;
				}
			}
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_gains_from_engine ();
			InvalidateRect (_hwnd, nullptr, FALSE);
			return 0;
		}
		int pi = hit_mixer_pan (x, y);
		if (pi >= 0 && _engine.is_linked () && _engine_snap.session_ok) {
			_mixers[pi].set_pan (0.5f);
			_engine.set_track_pan (pi, 0.5f);
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_pan_from_engine ();
			InvalidateRect (_hwnd, nullptr, FALSE);
			return 0;
		}
		/* Double-click aux send → 0 (before rename hit). */
		int si = hit_mixer_send (x, y);
		if (si >= 0 && _engine.is_linked () && _engine_snap.session_ok) {
			_mixers[si].set_send (0.f);
			_engine.set_track_send (si, 0.f);
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_send_from_engine ();
			InvalidateRect (_hwnd, nullptr, FALSE);
			return 0;
		}
		int lab = hit_mixer_label (x, y);
		if (lab >= 0) {
			const int mi = midi_mixer_index ();
			if (lab == mi) {
				prompt_rename_track (0, true);
			} else if (lab < n_audio_lanes ()) {
				_focus_audio_track = lab;
				refresh_automation_lane ();
				prompt_rename_track (lab, false);
			}
			return 0;
		}
		int ed = hit_editor_lane_label (x, y);
		if (ed >= 0) {
			_focus_audio_track = ed;
			refresh_automation_lane ();
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

D2D1_RECT_F
ShellWindow::midi_piano_rect () const
{
	const D2D1_RECT_F& r = _midi_lane;
	float vel_h = 28.f * _dpi_scale;
	float h = r.bottom - r.top;
	if (h < vel_h + 24.f * _dpi_scale) {
		vel_h = std::max (12.f * _dpi_scale, h * 0.28f);
	}
	return D2D1::RectF (r.left, r.top, r.right, r.bottom - vel_h);
}

D2D1_RECT_F
ShellWindow::midi_velocity_rect () const
{
	const D2D1_RECT_F& r = _midi_lane;
	D2D1_RECT_F piano = midi_piano_rect ();
	return D2D1::RectF (r.left, piano.bottom, r.right, r.bottom);
}

bool
ShellWindow::hit_midi_lane (float x, float y) const
{
	const D2D1_RECT_F& r = _midi_lane;
	return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

bool
ShellWindow::hit_midi_velocity_lane (float x, float y) const
{
	const D2D1_RECT_F vr = midi_velocity_rect ();
	return x >= vr.left && x <= vr.right && y >= vr.top && y <= vr.bottom;
}

int
ShellWindow::pick_midi_velocity_note_at (float x, float y) const
{
	const D2D1_RECT_F vr = midi_velocity_rect ();
	float vh = vr.bottom - vr.top;
	if (vh <= 1.f) {
		return -1;
	}
	const float time_sec = midi_time_at_x (x);
	int best_contain = -1;
	float best_contain_h = 1.e9f;
	int best_tall = -1;
	int best_tall_v = -1;
	for (int i = 0; i < _engine_snap.midi_note_count; ++i) {
		const EngineMidiNoteSnap& n = _engine_snap.midi_notes[i];
		float t0 = n.start_sec;
		float t1 = n.start_sec + n.length_sec;
		if (time_sec < t0 - 0.02f || time_sec > t1 + 0.02f) {
			continue;
		}
		float bar_h = vh * ((float)n.velocity / 127.f);
		if (bar_h < 1.f * _dpi_scale && n.velocity > 0) {
			bar_h = 1.f * _dpi_scale;
		}
		float bar_top = vr.bottom - bar_h;
		if (y >= bar_top - 0.5f && y <= vr.bottom + 0.5f) {
			/* Innermost (shortest) containing bar — chord stack tip. */
			if (best_contain < 0 || bar_h < best_contain_h) {
				best_contain = i;
				best_contain_h = bar_h;
			}
		}
		if ((int)n.velocity > best_tall_v) {
			best_tall = i;
			best_tall_v = (int)n.velocity;
		}
	}
	if (best_contain >= 0) {
		return best_contain;
	}
	return best_tall;
}

uint8_t
ShellWindow::midi_pitch_at_y (float y) const
{
	const D2D1_RECT_F r = midi_piano_rect ();
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

int
ShellWindow::midi_velocity_at_y (float y) const
{
	const D2D1_RECT_F vr = midi_velocity_rect ();
	float h = vr.bottom - vr.top;
	if (h <= 1.f) {
		return 100;
	}
	/* Bottom of strip = 1, top = 127. */
	float t = (vr.bottom - y) / h;
	if (t < 0.f) {
		t = 0.f;
	}
	if (t > 1.f) {
		t = 1.f;
	}
	int v = 1 + (int)std::lround (t * 126.f);
	if (v < 1) {
		v = 1;
	}
	if (v > 127) {
		v = 127;
	}
	return v;
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
	const int n = n_mixer_strips ();
	for (int i = 0; i < n; ++i) {
		if (_mixers[i].hit_arm (x, y)) {
			return i;
		}
	}
	return -1;
}

int
ShellWindow::hit_mixer_mon (float x, float y) const
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < na; ++i) {
		if (_mixers[i].hit_mon (x, y)) {
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
ShellWindow::hit_mixer_trim (float x, float y) const
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < na; ++i) {
		if (_mixers[i].hit_trim (x, y)) {
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
ShellWindow::hit_mixer_fx_param2 (float x, float y) const
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < na; ++i) {
		if (_mixers[i].hit_fx_param2 (x, y)) {
			return i;
		}
	}
	return -1;
}

int
ShellWindow::hit_mixer_fx_param3 (float x, float y) const
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < na; ++i) {
		if (_mixers[i].hit_fx_param3 (x, y)) {
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
	/* Wheel over mixer strip area → horizontal strip scroll when overflowing. */
	if (x >= _mixer_strips_clip.left && x <= _mixer_strips_clip.right
	    && y >= _mixer_strips_clip.top && y <= _mixer_strips_clip.bottom
	    && _mixer_scroll_max > 1.f) {
		int delta = GET_WHEEL_DELTA_WPARAM (wparam);
		float step = 48.f * _dpi_scale;
		if (GetKeyState (VK_SHIFT) & 0x8000) {
			step *= 2.f;
		}
		do_scroll_mixer ((delta > 0) ? -step : step);
		return true;
	}
	int lane = hit_editor_lane (x, y);
	bool over_editor = (lane >= 0) || hit_editor_ruler (x, y) || hit_midi_lane (x, y)
	                   || _midi_cc_lane.hit (_midi_cc_lane_bounds, x, y);
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

	if (_about.splash_visible ()) {
		dismiss_splash ();
		return true;
	}

	if (_recovery.visible ()) {
		apply_recovery_hit (_recovery.hit_test (x, y));
		return true;
	}

	if (_recent.visible ()) {
		apply_recent_hit (_recent.hit_test (x, y));
		return true;
	}

	if (_prefs_visible) {
		PrefsHit ph = _prefs.hit_test (x, y);
		apply_prefs_hit (ph);
		return true;
	}

	if (_about.about_visible ()) {
		if (_about.hit_dismiss_about (x, y)) {
			_about.set_about_visible (false);
			InvalidateRect (_hwnd, nullptr, FALSE);
		}
		return true;
	}

	if (_help_visible) {
		/* Any click dismisses the shortcuts overlay. */
		_help_visible = false;
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	TransportHit th = _transport.hit_test (x, y);
	if (th == TransportHit::Help) {
		toggle_help ();
		return true;
	}
	if (th == TransportHit::Prefs) {
		toggle_prefs ();
		return true;
	}
	if (th == TransportHit::Spectrum) {
		if (_engine.is_linked ()) {
			if (GetKeyState (VK_SHIFT) & 0x8000) {
				_engine.toggle_spectrum_master ();
			} else {
				_engine.toggle_spectrum_enabled ();
			}
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			layout_panes ();
		}
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}
	if (th == TransportHit::Phase) {
		if (_engine.is_linked ()) {
			if (GetKeyState (VK_SHIFT) & 0x8000) {
				_engine.toggle_correlation_master ();
			} else {
				_engine.toggle_correlation_enabled ();
			}
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			layout_panes ();
		}
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}
	if (th == TransportHit::Loudness) {
		if (_engine.is_linked ()) {
			if (GetKeyState (VK_SHIFT) & 0x8000) {
				_engine.toggle_loudness_master ();
			} else {
				_engine.toggle_loudness_enabled ();
			}
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			layout_panes ();
		}
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}
	if (th == TransportHit::About) {
		toggle_about ();
		return true;
	}
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
		toggle_recent_menu ();
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
	if (th == TransportHit::Stretch) {
		do_stretch_selection ((GetKeyState (VK_SHIFT) & 0x8000) ? 0.75f : 1.5f);
		return true;
	}
	if (th == TransportHit::PitchUp) {
		do_pitch_shift_selection (1.f);
		return true;
	}
	if (th == TransportHit::PitchDown) {
		do_pitch_shift_selection (-1.f);
		return true;
	}
	if (th == TransportHit::Reverse) {
		do_reverse_selection ();
		return true;
	}
	if (th == TransportHit::Normalize) {
		do_normalize_selection (-1.f);
		return true;
	}
	if (th == TransportHit::StripSilence) {
		do_strip_silence_selection ();
		return true;
	}
	if (th == TransportHit::Consolidate) {
		do_consolidate_selection ();
		return true;
	}
	if (th == TransportHit::Freeze) {
		do_toggle_freeze_track ();
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
	if (th == TransportHit::Quantize) {
		do_quantize_midi_notes ();
		return true;
	}
	if (th == TransportHit::MidiTransUp) {
		do_transpose_midi_notes ((GetKeyState (VK_SHIFT) & 0x8000) ? 12 : 1);
		return true;
	}
	if (th == TransportHit::MidiTransDown) {
		do_transpose_midi_notes ((GetKeyState (VK_SHIFT) & 0x8000) ? -12 : -1);
		return true;
	}
	if (th == TransportHit::MidiVelUp) {
		do_nudge_midi_velocity ((GetKeyState (VK_SHIFT) & 0x8000) ? 8 : 1);
		return true;
	}
	if (th == TransportHit::MidiVelDown) {
		do_nudge_midi_velocity ((GetKeyState (VK_SHIFT) & 0x8000) ? -8 : -1);
		return true;
	}
	if (th == TransportHit::MidiLenUp || th == TransportHit::MidiLenDown) {
		float unit = _engine.is_linked () ? _engine.snap_unit_sec () : 0.5f;
		if (!_engine_snap.snap_enabled) {
			unit = 0.05f;
		}
		if (GetKeyState (VK_SHIFT) & 0x8000) {
			unit *= 4.f;
		}
		do_nudge_midi_length (th == TransportHit::MidiLenUp ? unit : -unit);
		return true;
	}
	if (th == TransportHit::MidiHumanize) {
		do_humanize_midi_notes ((GetKeyState (VK_SHIFT) & 0x8000) ? 2.f : 1.f);
		return true;
	}
	if (th == TransportHit::MidiStep) {
		do_toggle_midi_step ();
		return true;
	}
	if (th == TransportHit::MidiChord) {
		do_cycle_midi_chord ();
		return true;
	}
	if (th == TransportHit::MidiArp) {
		if (GetKeyState (VK_SHIFT) & 0x8000) {
			do_cycle_midi_arp_mode ();
		} else {
			do_toggle_midi_arp ();
		}
		return true;
	}
	if (th == TransportHit::MidiScale) {
		if (GetKeyState (VK_CONTROL) & 0x8000) {
			do_cycle_midi_scale_root ();
		} else if (GetKeyState (VK_SHIFT) & 0x8000) {
			do_cycle_midi_scale_kind ();
		} else {
			do_toggle_midi_scale ();
		}
		return true;
	}
	if (th == TransportHit::MidiPanic) {
		do_midi_panic ();
		return true;
	}
	if (th == TransportHit::MidiPgmUp) {
		do_nudge_midi_program (1);
		return true;
	}
	if (th == TransportHit::MidiPgmDown) {
		do_nudge_midi_program (-1);
		return true;
	}
	if (th == TransportHit::MidiBankUp) {
		do_nudge_midi_bank (1);
		return true;
	}
	if (th == TransportHit::MidiBankDown) {
		do_nudge_midi_bank (-1);
		return true;
	}
	if (th == TransportHit::CountIn) {
		do_toggle_count_in ();
		return true;
	}
	if (th == TransportHit::Click) {
		if (GetKeyState (VK_SHIFT) & 0x8000) {
			/* Shift+click Click → cycle metronome level −12/−6/0/+6. */
			do_cycle_click_gain ();
		} else {
			do_toggle_click ();
		}
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
		if ((GetKeyState (VK_MENU) & 0x8000)
		    && !(GetKeyState (VK_SHIFT) & 0x8000)) {
			/* Alt+click Grp → toggle relative gain linking. */
			do_toggle_group_relative ();
		} else {
			do_apply_or_clear_group ();
		}
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

	int hist_row = -1;
	HistoryHit hh = _history.hit_test (x, y, &hist_row);
	if (hh == HistoryHit::UndoBtn) {
		do_undo ();
		return true;
	}
	if (hh == HistoryHit::RedoBtn || hh == HistoryHit::RedoNext) {
		do_redo ();
		return true;
	}
	if (hh == HistoryHit::UndoRow && hist_row >= 0) {
		do_undo_to_index (hist_row);
		return true;
	}

	MonitorHit mh = _monitor.hit_test (x, y);
	if (mh == MonitorHit::Dim) {
		if ((GetKeyState (VK_SHIFT) & 0x8000)
		    && _engine.is_linked () && _engine_snap.session_ok) {
			/* Shift+click Dim → cycle dim depth −20/−12/−6. */
			_engine.cycle_monitor_dim_level ();
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_monitor_from_engine ();
			InvalidateRect (_hwnd, nullptr, FALSE);
		} else {
			do_toggle_monitor_dim ();
		}
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
	if (mh == MonitorHit::Boost) {
		do_cycle_monitor_solo_boost ();
		return true;
	}
	if (mh == MonitorHit::AuxReturn) {
		_drag = DragMode::AuxReturn;
		_drag_index = -1;
		if (_monitor.drag_aux_return_to (y)) {
			do_set_aux_return (_monitor.aux_return ());
		}
		SetCapture (_hwnd);
		SetCursor (LoadCursorW (nullptr, IDC_SIZENS));
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	if (mh == MonitorHit::MasterLim) {
		do_toggle_master_fx ();
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
		if (hit_midi_velocity_lane (x, y)) {
			/* Velocity lane: chord-aware pick by bar height under (x,y). */
			int pi = pick_midi_velocity_note_at (x, y);
			if (pi >= 0 && pi < _engine_snap.midi_note_count) {
				const EngineMidiNoteSnap& n = _engine_snap.midi_notes[pi];
				_engine.select_midi_note_at (n.start_sec + n.length_sec * 0.5f, n.pitch);
			} else if (_engine_snap.midi_selected_note_id < 0
			           && _engine_snap.midi_note_count > 0) {
				/* No selection → edit all on drag apply. */
			}
			(void)time_sec;
			_drag = DragMode::MidiVelocity;
			_drag_index = -1;
			_midi_vel_drag = midi_velocity_at_y (y);
			_pan_down_x = x;
			_pan_down_y = y;
			_pan_last_x = x;
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			SetCapture (_hwnd);
			InvalidateRect (_hwnd, nullptr, FALSE);
			return true;
		}
		uint8_t pitch = midi_pitch_at_y (y);
		/* Prefer trim handles on note edges (same pad as audio region trim). */
		{
			const float pad = std::max (7.f, 9.f * _dpi_scale);
			int best_edge = -1; /* 0=start 1=end */
			int best_i = -1;
			float best_d = pad + 1.f;
			for (int i = 0; i < _engine_snap.midi_note_count; ++i) {
				const EngineMidiNoteSnap& n = _engine_snap.midi_notes[i];
				if (n.pitch != pitch) {
					continue;
				}
				float x0 = _lanes[0].strip.x_at_time (_midi_lane, n.start_sec);
				float x1 = _lanes[0].strip.x_at_time (_midi_lane, n.start_sec + n.length_sec);
				if (x1 - x0 < 2.f) {
					x1 = x0 + 2.f;
				}
				float ds = std::fabs (x - x0);
				float de = std::fabs (x - x1);
				if (ds <= pad && ds <= best_d) {
					best_d = ds;
					best_edge = 0;
					best_i = i;
				}
				if (de <= pad && de < best_d) {
					best_d = de;
					best_edge = 1;
					best_i = i;
				}
			}
			if (best_i >= 0 && best_edge >= 0) {
				const EngineMidiNoteSnap& n = _engine_snap.midi_notes[best_i];
				_engine.select_midi_note_at (n.start_sec + n.length_sec * 0.5f, n.pitch);
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				_drag = (best_edge == 0) ? DragMode::MidiTrimStart : DragMode::MidiTrimEnd;
				_drag_index = -1;
				_midi_trim_start_sec = n.start_sec;
				_midi_trim_end_sec = n.start_sec + n.length_sec;
				_pan_down_x = x;
				_pan_down_y = y;
				_pan_last_x = x;
				SetCapture (_hwnd);
				SetCursor (LoadCursorW (nullptr, IDC_SIZEWE));
				InvalidateRect (_hwnd, nullptr, FALSE);
				return true;
			}
		}
		if (_engine.select_midi_note_at (time_sec, pitch)) {
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			InvalidateRect (_hwnd, nullptr, FALSE);
			return true;
		}
		if (_engine_snap.midi_step_enabled) {
			/* Step mode: empty cell places pitch at playhead and advances. */
			do_step_input_pitch (pitch);
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
			if ((GetKeyState (VK_MENU) & 0x8000)
			    && (GetKeyState (VK_SHIFT) & 0x8000)
			    && !(GetKeyState (VK_CONTROL) & 0x8000)
			    && _engine.is_linked () && _engine_snap.session_ok) {
				/* Alt+Shift+click label → toggle Route comment. */
				_focus_audio_track = lab;
				_engine.toggle_track_comment (lab);
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_comment_from_engine ();
				InvalidateRect (_hwnd, nullptr, FALSE);
				return true;
			}
			if ((GetKeyState (VK_MENU) & 0x8000)
			    && (GetKeyState (VK_SHIFT) & 0x8000)
			    && (GetKeyState (VK_CONTROL) & 0x8000)
			    && _engine.is_linked () && _engine_snap.session_ok) {
				/* Ctrl+Alt+Shift+click label — toggle Route::active (group shares). */
				_focus_audio_track = lab;
				do_toggle_focused_track_active ();
				return true;
			}
			if ((GetKeyState (VK_MENU) & 0x8000)
			    && _engine.is_linked () && _engine_snap.session_ok) {
				/* Alt+click label → toggle polarity invert. */
				_focus_audio_track = lab;
				_engine.toggle_track_phase_invert (lab);
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_phase_from_engine ();
				InvalidateRect (_hwnd, nullptr, FALSE);
				return true;
			}
			if (GetKeyState (VK_SHIFT) & 0x8000) {
				do_toggle_group_pick (lab);
				return true;
			}
			_focus_audio_track = lab;
			refresh_automation_lane ();
			InvalidateRect (_hwnd, nullptr, FALSE);
			if (_engine.is_linked () && _engine_snap.session_ok) {
				_engine.select_audio_track (lab);
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_group_from_engine ();
			}
			_drag = DragMode::TrackReorderPending;
			_drag_index = lab;
			_pan_down_x = x;
			_pan_down_y = y;
			_pan_last_x = x;
			SetCapture (_hwnd);
			InvalidateRect (_hwnd, nullptr, FALSE);
			return true;
		}
	}

	int ed_lab = hit_editor_lane_label (x, y);
	if (ed_lab >= 0) {
		_focus_audio_track = ed_lab;
		refresh_automation_lane ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	int arm = hit_mixer_arm (x, y);
	if (arm >= 0) {
		if (_engine.is_linked ()) {
			const bool alt = (GetKeyState (VK_MENU) & 0x8000) != 0;
			const bool shift = (GetKeyState (VK_SHIFT) & 0x8000) != 0;
			if (!_engine.start ()) {
				_engine_snap = _engine.snapshot ();
			} else if (alt && shift) {
				/* Alt+Shift+click Arm → arm every audio track. */
				_engine.arm_all_tracks ();
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_arm_from_engine ();
			} else if (alt) {
				/* Alt+click Arm → disarm every audio track. */
				_engine.disarm_all_tracks ();
			} else if ((GetKeyState (VK_CONTROL) & 0x8000) != 0
			           && arm < EngineSnapshot::kMeters) {
				/* Ctrl+click Arm → record-safe latch. */
				_engine.toggle_track_record_safe (arm);
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_arm_from_engine ();
			} else {
				if (arm == midi_mixer_index ()) {
					_engine.toggle_midi_armed ();
				} else {
					_engine.toggle_track_armed (arm);
				}
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_arm_from_engine ();
			}
		}
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	
	int mon = hit_mixer_mon (x, y);
	if (mon >= 0) {
		if (_engine.is_linked () && _engine.snapshot ().session_ok) {
			_engine.cycle_track_monitor_choice (mon);
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_monitor_from_engine ();
		}
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	int mute_i = hit_mixer_mute (x, y);
	if (mute_i >= 0) {
		if (_engine.is_linked () && _engine.snapshot ().session_ok) {
			if ((GetKeyState (VK_CONTROL) & 0x8000) != 0
			    && mute_i != midi_mixer_index ()
			    && mute_i < EngineSnapshot::kMeters) {
				/* Ctrl+click Mute → cycle mute points Main↔AllPoints. */
				_engine.cycle_track_mute_points (mute_i);
			} else if (mute_i == midi_mixer_index ()) {
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
			const bool shift = (GetKeyState (VK_SHIFT) & 0x8000) != 0;
			const bool ctrl = (GetKeyState (VK_CONTROL) & 0x8000) != 0;
			const bool alt = (GetKeyState (VK_MENU) & 0x8000) != 0;
			if (shift && solo_i != midi_mixer_index ()) {
				/* Shift+click Solo → toggle solo-isolate. */
				_engine.toggle_track_solo_isolated (solo_i);
			} else if (ctrl && solo_i != midi_mixer_index ()) {
				/* Ctrl+click Solo → toggle solo-safe. */
				_engine.toggle_track_solo_safe (solo_i);
			} else if (solo_i == midi_mixer_index ()) {
				if (alt) {
					_engine.exclusive_solo_midi ();
				} else {
					_engine.toggle_midi_soloed ();
				}
			} else if (alt) {
				/* Alt+click Solo → exclusive solo. */
				_engine.exclusive_solo_track (solo_i);
			} else {
				_engine.toggle_track_soloed (solo_i);
			}
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			if (shift) {
				sync_mixer_solo_isolate_from_engine ();
			} else if (ctrl) {
				sync_mixer_solo_safe_from_engine ();
			} else {
				sync_mixer_mute_solo_from_engine ();
			}
		}
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	
	int trim_i = hit_mixer_trim (x, y);
	if (trim_i >= 0) {
		_drag = DragMode::Trim;
		_drag_index = trim_i;
		if (_mixers[trim_i].drag_trim_to (x)) {
			_engine.set_track_trim_norm (trim_i, _mixers[trim_i].trim ());
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_trim_from_engine ();
		}
		SetCapture (_hwnd);
		SetCursor (LoadCursorW (nullptr, IDC_SIZEWE));
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	int pan_i = hit_mixer_pan (x, y);
	if (pan_i >= 0) {
		const bool alt = (GetKeyState (VK_MENU) & 0x8000) != 0;
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
			}
		}
		SetCapture (_hwnd);
		SetCursor (LoadCursorW (nullptr, IDC_SIZEWE));
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	int send_i = hit_mixer_send (x, y);
	if (send_i >= 0) {
		const bool alt = (GetKeyState (VK_MENU) & 0x8000) != 0;
		if (alt && _engine.is_linked () && _engine.snapshot ().session_ok) {
			/* Alt+click send → zero every aux send. */
			_engine.reset_all_sends ();
		}
		/* Ctrl+click send → mute/unmute (Processor deactivate).
		 * Alt+click remains send-reset-all on tip #38. */
		if ((GetKeyState (VK_CONTROL) & 0x8000) != 0
		    && _engine.is_linked () && _engine_snap.session_ok) {
			_engine.toggle_track_send_muted (send_i);
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_send_from_engine ();
			InvalidateRect (_hwnd, nullptr, FALSE);
			return true;
		}
		const bool shift = (GetKeyState (VK_SHIFT) & 0x8000) != 0;
		if (shift && _mixers[send_i].send_pan_ok ()) {
			/* Shift+drag send → aux send pan azimuth. */
			_drag = DragMode::SendPan;
			_drag_index = send_i;
			if (_mixers[send_i].drag_send_pan_to (x)) {
				_engine.set_track_send_pan (send_i, _mixers[send_i].send_pan ());
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_send_from_engine ();
			}
		} else {
			_drag = DragMode::Send;
			_drag_index = send_i;
			if (_mixers[send_i].drag_send_to (x)) {
				_engine.set_track_send (send_i, _mixers[send_i].send ());
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_send_from_engine ();
			}
		}
		SetCapture (_hwnd);
		SetCursor (LoadCursorW (nullptr, IDC_SIZEWE));
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	/* Ctrl+click meter → Peak↔K14. Shift+click meter → cycle meter point.
	 * Alt+click → clear every strip's clip latch.
	 * Plain click (when clipped) → clear peak-hold clip latch. */
	int meter_i = hit_mixer_meter (x, y);
	if (meter_i >= 0 && (GetKeyState (VK_CONTROL) & 0x8000) != 0
	    && meter_i < EngineSnapshot::kMeters
	    && _engine.is_linked () && _engine_snap.session_ok
	    && !_mixers[meter_i].is_midi_strip ()) {
		_engine.cycle_track_meter_type (meter_i);
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		_mixers[meter_i].set_meter_type (_engine_snap.track_meter_type[meter_i]);
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}
	if (meter_i >= 0 && (GetKeyState (VK_SHIFT) & 0x8000) != 0
	    && meter_i < EngineSnapshot::kMeters
	    && _engine.is_linked () && _engine_snap.session_ok
	    && !_mixers[meter_i].is_midi_strip ()) {
		_engine.cycle_track_meter_point (meter_i);
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		_mixers[meter_i].set_meter_point (_engine_snap.track_meter_point[meter_i]);
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}
	if (meter_i >= 0 && (GetKeyState (VK_MENU) & 0x8000) != 0) {
		for (int i = 0; i < kMixerStripsMax; ++i) {
			_mixers[i].clear_meter_clip ();
		}
		if (_engine.is_linked () && _engine.snapshot ().session_ok) {
			_engine.clear_all_meter_clips ();
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
		}
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}
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

	int fxpi3 = hit_mixer_fx_param3 (x, y);
	if (fxpi3 >= 0 && _mixers[fxpi3].has_fx () && _mixers[fxpi3].fx_param_count () >= 3) {
		_drag = DragMode::FxParam3;
		_drag_index = fxpi3;
		if (_mixers[fxpi3].drag_fx_param3_to (x)) {
			_engine.set_track_eq_gain_h_norm (fxpi3, _mixers[fxpi3].fx_param3 ());
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_fx_from_engine ();
		}
		SetCapture (_hwnd);
		SetCursor (LoadCursorW (nullptr, IDC_SIZEWE));
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	int fxpi2 = hit_mixer_fx_param2 (x, y);
	if (fxpi2 >= 0 && _mixers[fxpi2].has_fx () && _mixers[fxpi2].fx_param_count () >= 2) {
		_drag = DragMode::FxParam2;
		_drag_index = fxpi2;
		if (_mixers[fxpi2].drag_fx_param2_to (x)) {
			if (fxpi2 == 5 && _engine_snap.track_gate_ok) {
				_engine.set_track_gate_hold_norm (fxpi2, _mixers[fxpi2].fx_param2 ());
			} else if (fxpi2 == 4 && _engine_snap.track_limiter_ok) {
				_engine.set_track_limiter_release_norm (fxpi2, _mixers[fxpi2].fx_param2 ());
			} else if (fxpi2 == 3 && _engine_snap.track_delay_ok) {
				_engine.set_track_delay_feedback_norm (fxpi2, _mixers[fxpi2].fx_param2 ());
			} else if (fxpi2 == 2 && _engine_snap.track_eq_ok) {
				_engine.set_track_eq_gain_m_norm (fxpi2, _mixers[fxpi2].fx_param2 ());
			} else {
				_engine.set_track_comp_ratio_norm (fxpi2, _mixers[fxpi2].fx_param2 ());
			}
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_fx_from_engine ();
		}
		SetCapture (_hwnd);
		SetCursor (LoadCursorW (nullptr, IDC_SIZEWE));
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	int fxpi = hit_mixer_fx_param (x, y);
	if (fxpi >= 0 && _mixers[fxpi].has_fx ()) {
		_drag = DragMode::FxParam;
		_drag_index = fxpi;
		if (_mixers[fxpi].drag_fx_param_to (x)) {
			if (fxpi == 5 && _engine_snap.track_gate_ok) {
				_engine.set_track_gate_threshold_norm (fxpi, _mixers[fxpi].fx_param ());
			} else if (fxpi == 4 && _engine_snap.track_limiter_ok) {
				_engine.set_track_limiter_ceiling_norm (fxpi, _mixers[fxpi].fx_param ());
			} else if (fxpi == 3 && _engine_snap.track_delay_ok) {
				_engine.set_track_delay_time_norm (fxpi, _mixers[fxpi].fx_param ());
			} else if (fxpi == 2 && _engine_snap.track_eq_ok) {
				_engine.set_track_eq_gain_l_norm (fxpi, _mixers[fxpi].fx_param ());
			} else if (fxpi == 1 && _engine_snap.track_comp_ok) {
				_engine.set_track_comp_threshold_norm (fxpi, _mixers[fxpi].fx_param ());
			} else {
				_engine.set_track_fx_param (fxpi, _mixers[fxpi].fx_param ());
			}
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
			const bool alt = (GetKeyState (VK_MENU) & 0x8000) != 0;
			const bool shift = (GetKeyState (VK_SHIFT) & 0x8000) != 0;
			if (alt && shift) {
				/* Alt+Shift+click FX → activate every shell ACE insert. */
				_engine.set_all_fx_bypassed (false);
			} else if (alt) {
				/* Alt+click FX → bypass every shell ACE insert. */
				_engine.set_all_fx_bypassed (true);
			} else if (fxi == 5) {
				if (_mixers[fxi].has_fx ()) {
					_engine.toggle_track_gate_bypass (fxi);
				} else {
					_engine.insert_track_gate (fxi);
				}
			} else if (fxi == 4) {
				if (_mixers[fxi].has_fx ()) {
					_engine.toggle_track_limiter_bypass (fxi);
				} else {
					_engine.insert_track_limiter (fxi);
				}
			} else if (fxi == 3) {
				if (_mixers[fxi].has_fx ()) {
					_engine.toggle_track_delay_bypass (fxi);
				} else {
					_engine.insert_track_delay (fxi);
				}
			} else if (fxi == 2) {
				if (_mixers[fxi].has_fx ()) {
					_engine.toggle_track_eq_bypass (fxi);
				} else {
					_engine.insert_track_eq (fxi);
				}
			} else if (fxi == 1) {
				if (_mixers[fxi].has_fx ()) {
					if (GetKeyState (VK_SHIFT) & 0x8000) {
						bool on = !_engine_snap.track_comp_sidechain;
						_engine.set_track_comp_sidechain (1, 0, on);
					} else {
						_engine.toggle_track_comp_bypass (fxi);
					}
				} else {
					_engine.insert_track_comp (fxi);
				}
			} else if (_mixers[fxi].has_fx ()) {
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
		const bool alt = (GetKeyState (VK_MENU) & 0x8000) != 0;
		if (alt && _engine.is_linked () && _engine.snapshot ().session_ok) {
			/* Alt+click fader → all strips to unity. */
			_engine.reset_all_faders ();
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_gains_from_engine ();
			InvalidateRect (_hwnd, nullptr, FALSE);
			return true;
		}
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
		if (GetKeyState (VK_SHIFT) & 0x8000) {
			/* Shift+click auto lane → cycle Gain → Pan → Send. */
			cycle_auto_lane_kind ();
			return true;
		}
		_drag = DragMode::AutoWrite;
		_drag_index = 0;
		_auto_last_write_sec = -1.f;
		if (_auto_lane_kind == 1) {
			_engine.ensure_track_pan_automation (0);
		} else if (_auto_lane_kind == 2) {
			_engine.ensure_track_send_automation (0);
		} else {
			_engine.ensure_track_gain_automation (0);
		}
		write_automation_at (x, y);
		SetCapture (_hwnd);
		SetCursor (LoadCursorW (nullptr, IDC_CROSS));
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	if (_midi_cc_lane.hit (_midi_cc_lane_bounds, x, y)
	    && _engine.is_linked () && _engine_snap.session_ok) {
		_drag = DragMode::MidiCcWrite;
		_drag_index = 0;
		_midi_cc_last_write_sec = -1.f;
		if (_midi_cc_number == -1) {
			_engine.ensure_midi_bend_automation ();
		} else if (_midi_cc_number == -2) {
			_engine.ensure_midi_pressure_automation ();
		} else {
			_engine.ensure_midi_cc_automation (_midi_cc_number);
		}
		write_midi_cc_at (x, y);
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
			int env_lane = -1;
			if (_engine_snap.region_envelope_ok
			    && hit_region_envelope_mid (x, y, &env_lane) && env_lane == lane) {
				_drag = DragMode::EnvelopeMid;
				_drag_index = lane;
				float coeff = 0.35f;
				if (envelope_mid_from_y (lane, y, &coeff)) {
					do_set_region_envelope_mid (coeff);
				}
				SetCapture (_hwnd);
				SetCursor (LoadCursorW (nullptr, IDC_SIZENS));
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
			if (_drag_index == 5 && _engine_snap.track_gate_ok) {
				_engine.set_track_gate_threshold_norm (
					_drag_index, _mixers[_drag_index].fx_param ());
			} else if (_drag_index == 4 && _engine_snap.track_limiter_ok) {
				_engine.set_track_limiter_ceiling_norm (
					_drag_index, _mixers[_drag_index].fx_param ());
			} else if (_drag_index == 3 && _engine_snap.track_delay_ok) {
				_engine.set_track_delay_time_norm (
					_drag_index, _mixers[_drag_index].fx_param ());
			} else if (_drag_index == 2 && _engine_snap.track_eq_ok) {
				_engine.set_track_eq_gain_l_norm (
					_drag_index, _mixers[_drag_index].fx_param ());
			} else if (_drag_index == 1 && _engine_snap.track_comp_ok) {
				_engine.set_track_comp_threshold_norm (
					_drag_index, _mixers[_drag_index].fx_param ());
			} else {
				_engine.set_track_fx_param (_drag_index, _mixers[_drag_index].fx_param ());
			}
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_fx_from_engine ();
			InvalidateRect (_hwnd, nullptr, FALSE);
		}
		return true;
	}

	if (_drag == DragMode::FxParam2 && _drag_index >= 0 && _drag_index < kTracks) {
		if (_mixers[_drag_index].drag_fx_param2_to (x)) {
			if (_drag_index == 5 && _engine_snap.track_gate_ok) {
				_engine.set_track_gate_hold_norm (
					_drag_index, _mixers[_drag_index].fx_param2 ());
			} else if (_drag_index == 4 && _engine_snap.track_limiter_ok) {
				_engine.set_track_limiter_release_norm (
					_drag_index, _mixers[_drag_index].fx_param2 ());
			} else if (_drag_index == 3 && _engine_snap.track_delay_ok) {
				_engine.set_track_delay_feedback_norm (
					_drag_index, _mixers[_drag_index].fx_param2 ());
			} else if (_drag_index == 2 && _engine_snap.track_eq_ok) {
				_engine.set_track_eq_gain_m_norm (
					_drag_index, _mixers[_drag_index].fx_param2 ());
			} else {
				_engine.set_track_comp_ratio_norm (
					_drag_index, _mixers[_drag_index].fx_param2 ());
			}
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_fx_from_engine ();
			InvalidateRect (_hwnd, nullptr, FALSE);
		}
		return true;
	}

	if (_drag == DragMode::FxParam3 && _drag_index >= 0 && _drag_index < kTracks) {
		if (_mixers[_drag_index].drag_fx_param3_to (x)) {
			_engine.set_track_eq_gain_h_norm (
				_drag_index, _mixers[_drag_index].fx_param3 ());
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_fx_from_engine ();
			InvalidateRect (_hwnd, nullptr, FALSE);
		}
		return true;
	}

	if (_drag == DragMode::Trim && _drag_index >= 0 && _drag_index < kTracks) {
		if (_mixers[_drag_index].drag_trim_to (x)) {
			_engine.set_track_trim_norm (_drag_index, _mixers[_drag_index].trim ());
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_trim_from_engine ();
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

	if (_drag == DragMode::PanWidth && _drag_index >= 0 && _drag_index < kTracks) {
		if (_mixers[_drag_index].drag_pan_width_to (x)) {
			_engine.set_track_pan_width (
				_drag_index, _mixers[_drag_index].pan_width ());
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

	if (_drag == DragMode::AuxReturn) {
		if (_monitor.drag_aux_return_to (y)) {
			do_set_aux_return (_monitor.aux_return ());
		}
		return true;
	}

	if (_drag == DragMode::SendPan && _drag_index >= 0 && _drag_index < kTracks) {
		if (_mixers[_drag_index].drag_send_pan_to (x)) {
			_engine.set_track_send_pan (
				_drag_index, _mixers[_drag_index].send_pan ());
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

	if (_drag == DragMode::MidiCcWrite) {
		write_midi_cc_at (x, y);
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	if (_drag == DragMode::EnvelopeMid && _drag_index >= 0 && _drag_index < kTracks) {
		float coeff = 0.35f;
		if (envelope_mid_from_y (_drag_index, y, &coeff)) {
			do_set_region_envelope_mid (coeff);
		}
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
	if (_drag == DragMode::MidiVelocity) {
		_pan_last_x = x;
		_midi_vel_drag = midi_velocity_at_y (y);
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}
	if (_drag == DragMode::MidiTrimStart || _drag == DragMode::MidiTrimEnd) {
		_pan_last_x = x;
		float t = midi_time_at_x (x);
		if (_engine_snap.snap_enabled) {
			t = _engine.snap_time (t);
		}
		if (_drag == DragMode::MidiTrimStart) {
			float end = _midi_trim_end_sec;
			if (t > end - 0.05f) {
				t = end - 0.05f;
			}
			if (t < 0.f) {
				t = 0.f;
			}
			_midi_trim_start_sec = t;
		} else {
			float start = _midi_trim_start_sec;
			if (t < start + 0.05f) {
				t = start + 0.05f;
			}
			_midi_trim_end_sec = t;
		}
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
	if (_drag == DragMode::MidiVelocity && _engine.is_linked () && _engine_snap.session_ok) {
		_engine.set_midi_velocity (_midi_vel_drag);
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
	if ((_drag == DragMode::MidiTrimStart || _drag == DragMode::MidiTrimEnd)
	    && _engine.is_linked () && _engine_snap.session_ok) {
		if (_drag == DragMode::MidiTrimStart) {
			_engine.set_midi_note_start (_midi_trim_start_sec);
		} else {
			_engine.set_midi_note_end (_midi_trim_end_sec);
		}
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
			refresh_automation_lane ();
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

	/* Brief splash auto-dismiss (~1.6s); click/Esc/any key also clears. */
	if (_about.splash_visible () && _splash_shown_ms != 0
	    && (now - _splash_shown_ms) >= 1600u) {
		dismiss_splash ();
	}

	if (!_recovery.visible () && _timeout_secs == 0) {
		_engine.autosave_tick (now);
	}

	/* Poll engine often enough for meters while playing. */
	static DWORD last_poll = 0;
	static DWORD last_wave = 0;
	const bool rolling = _transport_playing || _engine_snap.transport_rolling;
	DWORD poll_ms = (rolling || _engine_snap.spectrum_enabled
	                 || _engine_snap.correlation_enabled
	                 || _engine_snap.loudness_enabled) ? 50u : 250u;
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
		sync_transport_freeze_from_engine ();
		sync_transport_clock_from_engine ();
		sync_history_from_engine ();
		apply_region_selection_to_lanes ();
		apply_loop_to_lanes ();
		refresh_automation_lane ();
		refresh_midi_cc_lane ();
		_transport.set_snap_enabled (_engine_snap.snap_enabled);
		_transport.set_snap_density (_engine_snap.snap_density);
		_transport.set_midi_step_enabled (_engine_snap.midi_step_enabled);
		_transport.set_midi_chord_quality (_engine_snap.midi_chord_quality);
		_transport.set_midi_arp_enabled (_engine_snap.midi_arp_enabled);
		_transport.set_midi_arp_mode (_engine_snap.midi_arp_mode);
		_transport.set_dsp_load_pct (_engine_snap.dsp_load_pct);
		sync_scale_to_transport ();
		_transport.set_spectrum_enabled (_engine_snap.spectrum_enabled);
		_transport.set_correlation_enabled (_engine_snap.correlation_enabled);
		_transport.set_loudness_enabled (_engine_snap.loudness_enabled);
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
		if (i < EngineSnapshot::kMeters) {
			_mixers[i].set_meter_point (_engine_snap.track_meter_point[i]);
			_mixers[i].set_meter_type (_engine_snap.track_meter_type[i]);
		}
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
	sync_mixer_solo_isolate_from_engine ();
	sync_mixer_solo_safe_from_engine ();
	sync_mixer_comment_from_engine ();
	sync_mixer_phase_from_engine ();
	sync_mixer_trim_from_engine ();
	sync_mixer_listen_from_engine ();
	sync_mixer_monitor_from_engine ();
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
			wchar_t label[32];
			format_marker_ruler_label (name, label, 32, 8);
			_rt->DrawTextW (label, (UINT32)wcslen (label), _fmt_meta,
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
			              snap_caption_suffix ());
		} else if (with_audio > 0) {
			_snwprintf_s (caption, _TRUNCATE,
			              L"%d tracks  ·  %d with audio  ·  G=snap%s  ·  marks %d",
			              kTracks, with_audio,
			              snap_caption_suffix (),
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

	draw_region_envelope ();
	draw_automation_lane ();
	draw_midi_lane ();
	draw_midi_cc_lane ();
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
	const D2D1_RECT_F piano = midi_piano_rect ();
	const D2D1_RECT_F vel = midi_velocity_rect ();

	float inset = 12.f * _dpi_scale;
	D2D1_RECT_F label_rc = D2D1::RectF (
		_editor_pane.left + inset, r.top, r.left - 4.f * _dpi_scale, r.bottom);
	const wchar_t* midi_lab = (_engine_snap.midi_track_name[0] != L'\0')
		? _engine_snap.midi_track_name : L"MIDI 1";
	_rt->DrawTextW (midi_lab, (UINT32)wcslen (midi_lab), _fmt_meta, label_rc, _brush_midi);

	_rt->FillRectangle (r, _brush_pane_edge);
	_rt->DrawRectangle (r, _brush_midi, 1.f);

	const int keys = kMidiPitchRows;
	float h = piano.bottom - piano.top;
	float row_h = h / (float)keys;
	for (int k = 0; k < keys; ++k) {
		float y0 = piano.top + (float)k * row_h;
		uint8_t pitch = (uint8_t)(kMidiPitchHi - k);
		int pc = pitch % 12;
		bool black = (pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10);
		_brush_body->SetOpacity (black ? 0.18f : 0.08f);
		_rt->FillRectangle (D2D1::RectF (piano.left, y0, piano.right, y0 + row_h - 0.5f), _brush_body);
		_brush_body->SetOpacity (1.f);
	}

	/* Session MIDI notes — height scales with velocity (bottom-aligned in row). */
	const bool trimming = (_drag == DragMode::MidiTrimStart || _drag == DragMode::MidiTrimEnd);
	for (int i = 0; i < _engine_snap.midi_note_count; ++i) {
		const EngineMidiNoteSnap& n = _engine_snap.midi_notes[i];
		if (n.pitch < kMidiPitchLo || n.pitch > kMidiPitchHi) {
			continue;
		}
		float t0 = n.start_sec;
		float t1 = n.start_sec + n.length_sec;
		if (trimming && n.selected) {
			t0 = _midi_trim_start_sec;
			t1 = _midi_trim_end_sec;
		}
		float x0 = _lanes[0].strip.x_at_time (r, t0);
		float x1 = _lanes[0].strip.x_at_time (r, t1);
		if (x1 < r.left || x0 > r.right) {
			continue;
		}
		if (x1 - x0 < 2.f) {
			x1 = x0 + 2.f;
		}
		int row = (int)(kMidiPitchHi - n.pitch);
		float row_top = piano.top + (float)row * row_h + 1.f;
		float row_bot = row_top + row_h - 2.f;
		float vmin = 3.f * _dpi_scale;
		float full = std::max (vmin, row_bot - row_top);
		float vh = vmin + (full - vmin) * ((float)n.velocity / 127.f);
		float y1 = row_bot;
		float y0 = y1 - vh;
		ID2D1SolidColorBrush* brush = n.selected ? _brush_accent : _brush_midi;
		if (!brush) {
			continue;
		}
		brush->SetOpacity (n.selected ? 0.95f : 0.7f);
		_rt->FillRectangle (D2D1::RectF (x0, y0, x1, y1), brush);
		brush->SetOpacity (1.f);
		if (n.selected && _brush_playhead) {
			_rt->DrawRectangle (D2D1::RectF (x0, y0, x1, y1), _brush_playhead, 1.2f);
		}
	}

	/* Velocity lane under piano-roll. */
	if (vel.bottom > vel.top + 2.f) {
		_rt->FillRectangle (vel, _brush_pane);
		_rt->DrawRectangle (vel, _brush_pane_edge, 1.f);
		if (_fmt_meta && _brush_body) {
			_rt->DrawTextW (L"Vel", 3, _fmt_meta,
			                D2D1::RectF (vel.left + 4.f * _dpi_scale, vel.top,
			                             vel.left + 36.f * _dpi_scale, vel.bottom),
			                _brush_body);
		}
		float vh = vel.bottom - vel.top;
		for (int i = 0; i < _engine_snap.midi_note_count; ++i) {
			const EngineMidiNoteSnap& n = _engine_snap.midi_notes[i];
			float x0 = _lanes[0].strip.x_at_time (r, n.start_sec);
			float x1 = _lanes[0].strip.x_at_time (r, n.start_sec + n.length_sec);
			if (x1 < vel.left || x0 > vel.right) {
				continue;
			}
			if (x1 - x0 < 2.f) {
				x1 = x0 + 2.f;
			}
			int show_v = n.velocity;
			if (_drag == DragMode::MidiVelocity
			    && (n.selected || _engine_snap.midi_selected_note_id < 0)) {
				/* Preview drag value for selected (or all when none selected). */
				bool have_sel = false;
				for (int j = 0; j < _engine_snap.midi_note_count; ++j) {
					if (_engine_snap.midi_notes[j].selected) {
						have_sel = true;
						break;
					}
				}
				if (!have_sel || n.selected) {
					show_v = _midi_vel_drag;
				}
			}
			float bar_h = vh * ((float)show_v / 127.f);
			if (bar_h < 1.f * _dpi_scale && show_v > 0) {
				bar_h = 1.f * _dpi_scale;
			}
			ID2D1SolidColorBrush* brush = n.selected ? _brush_accent : _brush_midi;
			if (!brush) {
				continue;
			}
			brush->SetOpacity (n.selected ? 0.9f : 0.55f);
			_rt->FillRectangle (D2D1::RectF (x0, vel.bottom - bar_h, x1, vel.bottom), brush);
			brush->SetOpacity (1.f);
		}
		if (_drag == DragMode::MidiVelocity && _fmt_meta && _brush_brand) {
			wchar_t vlab[24];
			_snwprintf_s (vlab, _TRUNCATE, L"v=%d", _midi_vel_drag);
			_rt->DrawTextW (vlab, (UINT32)wcslen (vlab), _fmt_meta,
			                D2D1::RectF (vel.right - 48.f * _dpi_scale, vel.top,
			                             vel.right - 4.f * _dpi_scale, vel.bottom),
			                _brush_brand);
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
		float y0 = piano.top + (float)row * row_h + 1.f;
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
	                D2D1::RectF (piano.left + 6.f * _dpi_scale, piano.top,
	                             piano.right - 4.f * _dpi_scale, piano.top + 14.f * _dpi_scale),
	                _brush_brand);
}

void
ShellWindow::show_splash ()
{
	_about.set_splash_visible (true);
	_about.set_about_visible (false);
	_prefs_visible = false;
	_prefs.set_visible (false);
	_help_visible = false;
	_splash_shown_ms = GetTickCount ();
	if (_splash_shown_ms == 0) {
		_splash_shown_ms = 1; /* reserve 0 = inactive */
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::dismiss_splash ()
{
	if (!_about.splash_visible () && _splash_shown_ms == 0) {
		return;
	}
	_about.set_splash_visible (false);
	_splash_shown_ms = 0;
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::toggle_about ()
{
	dismiss_splash ();
	if (_prefs_visible) {
		_prefs_visible = false;
		_prefs.set_visible (false);
	}
	if (_help_visible) {
		_help_visible = false;
	}
	bool on = !_about.about_visible ();
	_about.set_about_visible (on);
	if (on) {
		_about.layout ((float)_width, (float)_height, _dpi_scale);
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::toggle_help ()
{
	dismiss_splash ();
	if (_prefs_visible) {
		_prefs_visible = false;
		_prefs.set_visible (false);
	}
	if (_about.about_visible ()) {
		_about.set_about_visible (false);
	}
	_help_visible = !_help_visible;
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::sync_prefs_from_engine ()
{
	_prefs.set_current_backend (_engine_snap.backend.c_str ());
	_prefs.set_pref_backend (_engine_snap.pref_backend.c_str ());
	_prefs.set_current_period (_engine_snap.period_frames, _engine_snap.sample_rate);
	_prefs.set_pref_period (_engine_snap.pref_period_frames);
	_prefs.set_click_default (_engine_snap.pref_click_default);
	_prefs.set_snap_default (_engine_snap.pref_snap_default);
	_prefs.set_vst3_hint (_engine_snap.vst3_path_hint);
}

void
ShellWindow::toggle_prefs ()
{
	dismiss_splash ();
	if (_help_visible) {
		_help_visible = false;
	}
	if (_about.about_visible ()) {
		_about.set_about_visible (false);
	}
	_prefs_visible = !_prefs_visible;
	_prefs.set_visible (_prefs_visible);
	if (_prefs_visible) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		sync_prefs_from_engine ();
		_prefs.set_status (L"");
		_prefs.layout ((float)_width, (float)_height, _dpi_scale);
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::apply_prefs_hit (PrefsHit hit)
{
	switch (hit) {
	case PrefsHit::Close:
		_prefs_visible = false;
		_prefs.set_visible (false);
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	case PrefsHit::BackendDummy:
		_engine.set_pref_backend ("Dummy");
		break;
	case PrefsHit::BackendWasapi:
		_engine.set_pref_backend ("WASAPI");
		break;
	case PrefsHit::PeriodAuto:
		_engine.set_pref_period_frames (0);
		break;
	case PrefsHit::Period480:
		_engine.set_pref_period_frames (480);
		break;
	case PrefsHit::Period512:
		_engine.set_pref_period_frames (512);
		break;
	case PrefsHit::ClickToggle:
		_engine.set_pref_click_default (!_engine.snapshot ().pref_click_default);
		break;
	case PrefsHit::SnapToggle:
		_engine.set_pref_snap_default (!_engine.snapshot ().pref_snap_default);
		break;
	case PrefsHit::Save:
		if (_engine.save_shell_prefs ()) {
			_prefs.set_status (L"Saved");
		} else {
			_prefs.set_status (L"Save failed");
		}
		break;
	case PrefsHit::None:
	default:
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_prefs_from_engine ();
	sync_transport_tempo_from_engine ();
	_transport.set_snap_enabled (_engine_snap.snap_enabled);
	_transport.set_snap_density (_engine_snap.snap_density);
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::draw_help_overlay ()
{
	if (!_rt) {
		return;
	}

	/* Dim the shell; card lists bindings the shell actually handles. */
	_brush_pane->SetOpacity (0.78f);
	_rt->FillRectangle (D2D1::RectF (0, 0, (float)_width, (float)_height), _brush_pane);
	_brush_pane->SetOpacity (1.f);

	float pad = 28.f * _dpi_scale;
	float card_w = std::min ((float)_width - pad * 2.f, 720.f * _dpi_scale);
	float card_h = std::min ((float)_height - pad * 2.f, 520.f * _dpi_scale);
	float cx = ((float)_width - card_w) * 0.5f;
	float cy = ((float)_height - card_h) * 0.5f;
	D2D1_RECT_F card = D2D1::RectF (cx, cy, cx + card_w, cy + card_h);
	_rt->FillRectangle (card, _brush_pane);
	_rt->DrawRectangle (card, _brush_accent, 1.5f * _dpi_scale);

	DWRITE_TEXT_ALIGNMENT ta = _fmt_pane->GetTextAlignment ();
	_fmt_pane->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_LEADING);
	_rt->DrawTextW (L"Airdour — shortcuts", 20, _fmt_pane,
	                D2D1::RectF (card.left + 16.f * _dpi_scale, card.top + 10.f * _dpi_scale,
	                             card.right - 16.f * _dpi_scale, card.top + 34.f * _dpi_scale),
	                _brush_brand);
	_fmt_pane->SetTextAlignment (ta);

	struct Row { const wchar_t* key; const wchar_t* action; };
	static const Row kRows[] = {
		{ L"Space", L"Play / Stop" },
		{ L"Esc", L"Close help · quit app" },
		{ L"F1  /  ?", L"Toggle this help" },
		{ L"F3", L"About Airdour" },
		{ L"Ctrl+,", L"Preferences" },
		{ L"Ctrl+Z / Y", L"Undo / Redo" },
		{ L"Ctrl+S / O / E", L"Save / Open browse / Export" },
		{ L"Ctrl+Shift+O", L"Recent sessions menu" },
		{ L"MLim (Monitor)", L"Master ACE Limiter bypass toggle" },
		{ L"S / F / Del", L"Split · Fade · Delete selection" },
		{ L"T / Shift+T", L"Stretch 1.5× · compress 0.75× (RubberBand)" },
		{ L"Alt+Shift+T / dbl-click trim", L"Reset strip preamp trim to 0 dB" },
		{ L"R / Shift+R", L"Pitch +1 / −1 semitone (duration held)" },
		{ L"Alt+Shift+R / Alt+dbl-click strip", L"Reset strip fader·pan·send" },
		{ L"V", L"Reverse selected audio region" },
		{ L"N", L"Normalize selection to −1 dBFS" },
		{ L"Alt+Shift+N / Alt+Shift+click label", L"Toggle track comment (Cm)" },
		{ L"Alt+Shift+W", L"Toggle narrow mixer strips" },
		{ L"Alt+Shift+D / Shift+click Dim", L"Cycle monitor dim −20/−12/−6 dB" },
		{ L"I", L"Strip silence (−40 dBFS threshold)" },
		{ L"Alt+Shift+I / Alt+click label", L"Toggle strip polarity invert (Ø)" },
		{ L"B", L"Consolidate / bounce selection in place" },
		{ L"Alt+B / Alt+click FX", L"Bypass all ACE inserts" },
		{ L"Alt+Shift+B / Alt+Shift+click FX", L"Activate all ACE inserts" },
		{ L"Z", L"Freeze / unfreeze focused audio track" },
		{ L"G / Shift+G", L"Snap on/off · cycle density" },
		{ L"Q / Qnt", L"Quantize MIDI notes to snap grid" },
		{ L"Ctrl+Alt+Q", L"Legato MIDI notes (abut ends)" },
		{ L". / ,  Shift", L"MIDI transpose ±1 · ±12 (T+/T−)" },
		{ L"↑ / ↓  Shift", L"MIDI velocity ±1 · ±8 (V+/V−)" },
		{ L"Alt+←/→  Shift", L"MIDI note length ± (L+/L−; snap unit)" },
		{ L"Ctrl+C / X / V", L"MIDI note copy · cut · paste (at playhead)" },
		{ L"H / Shift+H", L"MIDI humanize · 2× strength (Hum)" },
		{ L"F4 / Step", L"MIDI step-record · Z/Q piano · ←→ rest" },
		{ L"F5 / Chd", L"MIDI chord Off→Maj→Min · multi-key latch" },
		{ L"F6 / Arp", L"MIDI arp on/off · Shift=Up/Dn/UD · grid rate" },
		{ L"F7 / Scl", L"Scale lock · Shift=maj/min · Ctrl=root" },
		{ L"F8", L"MIDI auto lane · CC7→1→11→64→Bend→Pressure→PolyAT" },
		{ L"F9 / Panic", L"MIDI panic · kill hanging notes + reset controllers" },
		{ L"P+ / P−  B+ / B−", L"MIDI Program Change ± · Bank Select ±" },
		{ L"Alt+Shift+M / U", L"Mute / Solo all audio tracks + MIDI" },
		{ L"Alt+Shift+E", L"Export per-track stem WAV files" },
		{ L"Alt+Shift+A / Shift+click auto", L"Cycle Gain→Pan→Send automation lane" },
		{ L"Alt+Shift+Y", L"Cycle auto Off→Play→Write→Touch (current lane)" },
		{ L"L / P", L"Loop · Punch" },
		{ L"Alt+Shift+L", L"Cycle Solo → PFL → AFL (listen mode)" },
		{ L"[ ]  Shift+[ ]", L"Loop start/end · Punch start/end" },
		{ L"M / Shift+M", L"Add / remove marker" },
		{ L"Ctrl+Alt+←/→", L"Previous / next marker" },
		{ L"Ctrl+Shift+M", L"Lock / unlock marker at playhead" },
		{ L"Ctrl+Alt+M", L"Toggle CD flag on marker at playhead" },
		{ L"Ctrl+Shift+R", L"Range marker ← selection" },
		{ L"Ctrl+Alt+Y", L"Region sync at playhead (Shift=clear)" },
		{ L"Ctrl+Alt+Shift+C", L"Cycle scene marker MIDI channel 0..15" },
		{ L"Ctrl+Alt+,/.", L"Previous / next cue marker" },
		{ L"Alt+Shift+S", L"Rename nearest section marker" },
		{ L"Ctrl+Alt+[ / ]", L"Locate punch in / out" },
		{ L"Ctrl+[ / ]", L"Locate loop start / end" },
		{ L"Ctrl+Home / End", L"Locate session start / end" },
		{ L"Alt+, / .", L"Nudge playhead by snap unit" },
		{ L"Ctrl+Alt+P", L"Zoom editor to punch range" },
		{ L"Ctrl+Alt+L", L"Zoom editor to loop range" },
		{ L"Ctrl+Alt+E", L"Zoom editor to selection" },
		{ L"Ctrl+Alt+Home", L"Zoom editor to session extent" },
		{ L"Ctrl+Alt+A", L"Zoom editor to all regions" },
		{ L"Ctrl+Alt+0", L"Center view on playhead (keep zoom)" },
		{ L"Ctrl+Shift+0", L"Scroll playhead into view (keep zoom)" },
		{ L"Ctrl+M", L"Mute / unmute selected region" },
		{ L"Ctrl+Shift+↑/↓", L"Region gain ±1 dB" },
		{ L"Ctrl+L", L"Lock / unlock selected region (blocks move/trim)" },
		{ L"Ctrl+Alt+L", L"Position-lock selected region (blocks move)" },
		{ L"Alt+[ / ]", L"Fade-in ±10ms · Alt+Shift = fade-out" },
		{ L"Ctrl+D", L"Duplicate selected region after itself" },
		{ L"Ctrl+←/→", L"Nudge selected region (snap unit; Shift=4×)" },
		{ L"Ctrl+] / [", L"Raise / lower region · Shift = top/bottom" },
		{ L"Ctrl+Alt+Shift+]", L"Cycle region layer top↔bottom" },
		{ L"Alt+O", L"Toggle region opaque / transparent" },
		{ L"Alt+E", L"Export selection time range WAV" },
		{ L"Ctrl+Shift+P", L"Punch range ← selection" },
		{ L"Ctrl+Shift+L", L"Loop range ← selection" },
		{ L"Ctrl+Shift+K", L"Skip range ← selection" },
		{ L"Shift+F2", L"Rename selected region" },
		{ L"Shift+F", L"Cycle fade shapes (Linear/Power/Fast/Slow/Sym)" },
		{ L"Ctrl+F", L"Toggle fade in/out active" },
		{ L"Shift+E", L"Region gain envelope (unity→dip→unity)" },
		{ L"Ctrl+Alt+↑/↓", L"Nudge region envelope mid gain" },
		{ L"Tab / Shift+Tab", L"Cycle region selection" },
		{ L"Ctrl+Alt+±", L"Zoom around playhead" },
		{ L"Alt+M", L"Clear all mutes (audio + MIDI)" },
		{ L"Ctrl+Alt+H", L"Toggle Comp sidechain (Audio 1→2)" },
		{ L"C / K", L"Count-in · Click (metronome)" },
		{ L"Alt+Shift+K / Boost", L"Cycle monitor solo boost 0/+3/+6 dB" },
		{ L"Alt+Shift+H / Shift+click Click", L"Cycle click gain −12/−6/0/+6 dB" },
		{ L"Alt+C", L"Cycle focused track color" },
		{ L"− / +", L"Tempo −1 / +1 BPM" },
		{ L"+A / Shift+click +A", L"Add mono audio · stereo Equal-Power" },
		{ L"A / Shift+A / E", L"Fit all regions · Fit session · Fit selection" },
		{ L"Home / 0", L"Follow transport (reset view)" },
		{ L"Shift+Home / End", L"Locate to selection start / end" },
		{ L"← →  Shift+←→", L"Scroll editor · larger step" },
		{ L"Wheel / Shift+Wheel", L"Zoom · horizontal scroll" },
		{ L"Ctrl+±", L"Zoom in / out (center)" },
		{ L"F2", L"Rename focused track" },
		{ L"Dbl-click FX param", L"Reset FX knob to insert default" },
		{ L"Dbl-click fader/pan", L"Reset fader → unity · pan → center" },
		{ L"Dbl-click send", L"Reset aux send → 0 (off)" },
		{ L"Alt+↑ / Alt+↓", L"Reorder audio track" },
		{ L"Alt+Shift+click Arm", L"Arm all audio tracks" },
		{ L"U", L"Apply / clear track group" },
		{ L"Shift+click Solo", L"Toggle solo-isolate (Si)" },
		{ L"Ctrl+click Solo", L"Toggle solo-safe (Ss)" },
		{ L"Alt+click pan", L"Center all track pans" },
		{ L"Alt+click send", L"Zero all aux sends" },
		{ L"Alt+click fader", L"Reset all faders to unity" },
		{ L"Alt+click Arm", L"Disarm all audio tracks" },
		{ L"Alt+U", L"Clear all solos (audio + MIDI)" },
		{ L"Alt+click meter", L"Clear all PeakMeter clip latches" },
		{ L"Alt+click Solo", L"Exclusive solo (clear other solos)" },
		{ L"U / Shift+U", L"Apply / clear track group (mute·solo·gain·arm)" },
		{ L"Click track label", L"Focus track · gain auto lane follows" },
		{ L"U / Shift+U", L"Apply / clear track group" },
		{ L"Alt+Shift+G / Alt+Grp", L"Toggle group relative gain" },
		{ L"Alt+Shift+←/→ · wheel", L"Scroll mixer strips when overflowing" },
		{ L"Alt+Shift+C", L"Cycle track group color (G bar)" },
		{ L"U / Shift+U", L"Apply / clear track group (mute·solo·gain·active)" },
		{ L"Alt+Shift+E", L"Toggle track active (group shares)" },
		{ L"U / Shift+U", L"Apply / clear track group (mute·solo·gain·select)" },
		{ L"Mon / Alt+Shift+O", L"Cycle monitor Auto → In → Disk" },
		{ L"Alt+Shift+V", L"VCA assign Audio 1 (status line)" },
		{ L"Y / Shift+Y", L"Spectrum on/off · Master mix" },
		{ L"X / Shift+X", L"Phase corr / gonio · Master L/R" },
		{ L"J / Shift+J", L"LUFS ≈ on/off · Master mix" },
		{ L"Ctrl+click Send", L"Mute / unmute Aux send (Sm)" },
		{ L"Shift+click meter", L"Cycle meter tap In/Pre/Post/Out" },
		{ L"Ctrl+click meter", L"Cycle meter type Peak↔K14" },
	};

	float y = card.top + 40.f * _dpi_scale;
	float row_h = 20.f * _dpi_scale;
	float col0 = card.left + 18.f * _dpi_scale;
	float col1 = card.left + 190.f * _dpi_scale;
	float bot = card.bottom - 28.f * _dpi_scale;
	DWRITE_TEXT_ALIGNMENT ta_m = _fmt_meta->GetTextAlignment ();
	_fmt_meta->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_LEADING);
	for (size_t i = 0; i < sizeof (kRows) / sizeof (kRows[0]); ++i) {
		if (y + row_h > bot) {
			break;
		}
		_rt->DrawTextW (kRows[i].key, (UINT32)wcslen (kRows[i].key), _fmt_meta,
		                D2D1::RectF (col0, y, col1 - 8.f * _dpi_scale, y + row_h),
		                _brush_accent);
		_rt->DrawTextW (kRows[i].action, (UINT32)wcslen (kRows[i].action), _fmt_meta,
		                D2D1::RectF (col1, y, card.right - 16.f * _dpi_scale, y + row_h),
		                _brush_body);
		y += row_h;
	}
	_fmt_meta->SetTextAlignment (ta_m);

	_rt->DrawTextW (L"Click anywhere or Esc to close", 30, _fmt_meta,
	                D2D1::RectF (card.left + 16.f * _dpi_scale, card.bottom - 24.f * _dpi_scale,
	                             card.right - 16.f * _dpi_scale, card.bottom - 6.f * _dpi_scale),
	                _brush_body);
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
	if (_engine_snap.track_eq_ok && !_engine_snap.track_eq_name.empty ()) {
		_snwprintf_s (hint, _TRUNCATE,
		              L"PeakMeter  ·  %s%s  ·  L/M/H gain (click FX=bypass)",
		              _engine_snap.track_eq_name.c_str (),
		              _engine_snap.track_eq_bypassed ? L" BYP" : L"");
	} else if (_engine_snap.track_fx_ok && !_engine_snap.track_fx_name.empty ()) {
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
	_rt->PushAxisAlignedClip (_mixer_strips_clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
	for (int i = 0; i < n_mix; ++i) {
		_mixers[i].draw (_rt, _brush_brand, _brush_body, _brush_accent, _brush_pane,
		                 _brush_pane_edge, _brush_meter_hot, _fmt_pane, _fmt_meta, _dpi_scale);
	}
	_rt->PopAxisAlignedClip ();
	/* Scroll hint when strips overflow. */
	if (_mixer_scroll_max > 1.f) {
		wchar_t scr[32];
		_snwprintf_s (scr, _TRUNCATE, L"◀ ▶ %.0f", _mixer_scroll_x);
		_rt->DrawTextW (scr, (UINT32)wcslen (scr), _fmt_meta,
		                D2D1::RectF (_mixer_strips_clip.right - 72.f * _dpi_scale,
		                             r.top + 10.f * _dpi_scale,
		                             _mixer_strips_clip.right - 4.f * _dpi_scale,
		                             r.top + 30.f * _dpi_scale),
		                _brush_body);
	}
	_history.draw (_rt, _brush_brand, _brush_body, _brush_accent, _brush_pane,
	               _brush_pane_edge, _fmt_pane, _fmt_meta, _dpi_scale);
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

	_brush_brand->SetOpacity (0.96f + 0.04f * _pulse);
	_rt->DrawTextW (L"Airdour", 7, _fmt_brand,
	                D2D1::RectF (_brand_bar.left + 28.f * _dpi_scale, _brand_bar.top,
	                             _brand_bar.right - 28.f * _dpi_scale, _brand_bar.bottom),
	                _brush_brand);
	_brush_brand->SetOpacity (1.f);

	/* Accent rule under brand — presence, not noise. */
	float rule_y = _brand_bar.bottom - 6.f * _dpi_scale;
	float rule_w = (120.f + 28.f * _pulse) * _dpi_scale;
	_rt->FillRectangle (D2D1::RectF (28.f * _dpi_scale, rule_y,
	                                 28.f * _dpi_scale + rule_w, rule_y + 2.5f * _dpi_scale),
	                    _brush_accent);

	sync_transport_clock_from_engine ();
	_transport.draw (_rt, _brush_brand, _brush_body, _brush_accent, _brush_pane,
	                 _brush_pane_edge, _fmt_pane, _playhead_sec, _dpi_scale);

	draw_editor_pane ();
	draw_mixer_pane ();
	if (_engine_snap.spectrum_enabled) {
		_spectrum.set_bands (_engine_snap.spectrum_band, EngineSnapshot::kSpectrumBands);
		_spectrum.set_label (_engine_snap.spectrum_label);
		_spectrum.set_visible (true);
		_spectrum.draw (_rt, _brush_brand, _brush_body, _brush_accent,
		               _brush_pane, _brush_pane_edge, _fmt_meta, _dpi_scale);
	}
	if (_engine_snap.correlation_enabled) {
		_correlation.set_correlation (_engine_snap.correlation);
		_correlation.set_gonio (_engine_snap.gonio_x, _engine_snap.gonio_y,
		                        EngineSnapshot::kGonioPts);
		_correlation.set_label (_engine_snap.correlation_label);
		_correlation.set_visible (true);
		_correlation.draw (_rt, _brush_brand, _brush_body, _brush_accent,
		                   _brush_pane, _brush_pane_edge, _fmt_meta, _dpi_scale);
	}
	if (_engine_snap.loudness_enabled) {
		_loudness.set_values (_engine_snap.loudness_momentary,
		                      _engine_snap.loudness_integrated);
		_loudness.set_label (_engine_snap.loudness_label);
		_loudness.set_visible (true);
		_loudness.draw (_rt, _brush_brand, _brush_body, _brush_accent,
		                _brush_pane, _brush_pane_edge, _fmt_meta, _dpi_scale);
	}
	if (_help_visible) {
		draw_help_overlay ();
	}
	if (_about.about_visible ()) {
		_about.layout ((float)_width, (float)_height, _dpi_scale);
		_about.draw_about (_rt, _brush_brand, _brush_body, _brush_accent, _brush_pane,
		                   _brush_pane_edge, _fmt_pane, _fmt_meta, _dpi_scale);
	}
	if (_prefs_visible) {
		_prefs.layout ((float)_width, (float)_height, _dpi_scale);
		_prefs.draw (_rt, _brush_brand, _brush_body, _brush_accent, _brush_pane,
		             _brush_pane_edge, _fmt_pane, _fmt_meta, _dpi_scale);
	}
	if (_recent.visible ()) {
		_recent.layout_under (_transport.open_btn (), (float)_width, (float)_height, _dpi_scale);
		_recent.draw (_rt, _brush_brand, _brush_body, _brush_accent, _brush_pane,
		              _brush_pane_edge, _fmt_meta, _dpi_scale);
	}
	if (_recovery.visible ()) {
		_recovery.layout ((float)_width, (float)_height, _dpi_scale);
		_recovery.draw (_rt, _brush_brand, _brush_body, _brush_accent, _brush_pane,
		                _brush_pane_edge, _fmt_pane, _fmt_meta, _dpi_scale);
	}
	if (_about.splash_visible ()) {
		_about.draw_splash (_rt, _brush_brand, _brush_body, _brush_accent, _brush_pane,
		                    _fmt_brand, _fmt_meta, _dpi_scale, _pulse);
	}

	wchar_t status[512];
	float dsp = _engine_snap.dsp_load_pct;
	if (dsp < 0.f) {
		dsp = 0.f;
	}
	if (dsp > 100.f) {
		dsp = 100.f;
	}
	wchar_t vca_frag[96] = L"";
	if (_engine_snap.vca_ok) {
		_snwprintf_s (vca_frag, _TRUNCATE, L"  ·  VCA 1 · slaves=%d · g=%.2f",
		              _engine_snap.vca_slave_count, _engine_snap.vca_gain);
	}
	wchar_t vst_frag[160] = L"";
	if (_engine_snap.vst3_ok) {
		if (_engine_snap.vst3_param_count > 0) {
			_snwprintf_s (vst_frag, _TRUNCATE,
			              L"  ·  VST3 %s%s · %s=%.2f",
			              _engine_snap.vst3_name,
			              _engine_snap.vst3_bypassed ? L" BYP" : L"",
			              _engine_snap.vst3_param_name[0],
			              _engine_snap.vst3_param_norm[0]);
		} else {
			_snwprintf_s (vst_frag, _TRUNCATE, L"  ·  VST3 %s%s",
			              _engine_snap.vst3_name,
			              _engine_snap.vst3_bypassed ? L" BYP" : L"");
		}
	}
	if (_engine_snap.running) {
		_snwprintf_s (status, _TRUNCATE,
		              L"Engine  %s  ·  %u Hz  ·  period %u  ·  DSP %.1f%%  ·  cb %lluµs avg  ·  xruns %llu%s%s  ·  %s",
		              _engine_snap.backend.c_str (),
		              _engine_snap.sample_rate,
		              _engine_snap.period_frames,
		              dsp,
		              (unsigned long long)_engine_snap.callback_avg_us,
		              (unsigned long long)_engine_snap.xrun_count,
		              vca_frag,
		              vst_frag,
		              _engine_snap.detail.c_str ());
	} else if (_engine_snap.available) {
		_snwprintf_s (status, _TRUNCATE,
		              L"Engine idle  ·  DSP %.1f%%  ·  %d tracks%s%s  ·  %s",
		              dsp, kTracks, vca_frag, vst_frag, _engine_snap.detail.c_str ());
	} else {
		_snwprintf_s (status, _TRUNCATE,
		              L"UI shell  ·  %d tracks%s%s  ·  %s",
		              kTracks, vca_frag, vst_frag, _engine_snap.detail.c_str ());
	}

	const float meter_w = 96.f * _dpi_scale;
	const float text_right = _status_bar.right - 20.f * _dpi_scale - meter_w - 12.f * _dpi_scale;
	_rt->DrawTextW (status, (UINT32)wcslen (status), _fmt_meta,
	                D2D1::RectF (_status_bar.left + 20.f * _dpi_scale, _status_bar.top,
	                             text_right, _status_bar.bottom),
	                _brush_body);

	/* Status-bar DSP meter (same load % as transport; UI-rate from poll). */
	if (_engine_snap.available) {
		float mid_y = (_status_bar.top + _status_bar.bottom) * 0.5f;
		float bh = 12.f * _dpi_scale;
		D2D1_RECT_F track = D2D1::RectF (_status_bar.right - 20.f * _dpi_scale - meter_w,
		                                 mid_y - bh * 0.5f,
		                                 _status_bar.right - 20.f * _dpi_scale,
		                                 mid_y + bh * 0.5f);
		_rt->FillRectangle (track, _brush_pane_edge);
		float fill_w = meter_w * (dsp / 100.f);
		if (fill_w > 1.f) {
			D2D1_RECT_F fill = D2D1::RectF (track.left, track.top,
			                                track.left + fill_w, track.bottom);
			_rt->FillRectangle (fill, dsp >= 85.f ? _brush_accent : _brush_brand);
		}
		wchar_t dsp_lab[24];
		_snwprintf_s (dsp_lab, _TRUNCATE, L"DSP %.0f%%", dsp);
		DWRITE_TEXT_ALIGNMENT ta = _fmt_meta->GetTextAlignment ();
		DWRITE_PARAGRAPH_ALIGNMENT pa = _fmt_meta->GetParagraphAlignment ();
		_fmt_meta->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_CENTER);
		_fmt_meta->SetParagraphAlignment (DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
		_rt->DrawTextW (dsp_lab, (UINT32)wcslen (dsp_lab), _fmt_meta, track, _brush_body);
		_fmt_meta->SetTextAlignment (ta);
		_fmt_meta->SetParagraphAlignment (pa);
	}

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

