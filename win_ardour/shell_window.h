/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Win32 + Direct2D + DirectWrite shell: transport, multi-track editor,
 * mixer faders + meters, record, import, undo, loop, markers.
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdint>
#include <string>

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include "automation_lane.h"
#include "engine_host.h"
#include "mixer_strip.h"
#include "transport_bar.h"
#include "wave_strip.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "comdlg32.lib")

class ShellWindow
{
public:
	static constexpr int kTracks = 3;
	static constexpr int kMixerStrips = 4; /* Audio 1–3 + MIDI */

	ShellWindow ();
	~ShellWindow ();

	bool create (HINSTANCE instance, int show_cmd);
	/**
	 * When smoke_timeout_secs > 0, create Session/PeakMeter but do not auto-roll
	 * (avoids intermittent AVs under Dummy/WASAPI during unattended --timeout).
	 * smoke_* flags exercise paths without changing the plain --timeout
	 * exit-0 contract when those flags are off.
	 */
	bool attach_engine (const std::string& backend_name,
	                    unsigned smoke_timeout_secs = 0,
	                    bool smoke_import = false,
	                    bool smoke_record = false,
	                    bool smoke_save_load = false,
	                    bool smoke_export = false,
	                    bool smoke_undo = false,
	                    bool smoke_loop = false,
	                    bool smoke_midi = false,
	                    bool smoke_midi_edit = false,
	                    bool smoke_plugin = false,
	                    bool smoke_perf = false,
	                    const std::string& open_session_path = std::string (),
	                    unsigned period_frames = 0,
	                    bool smoke_vst3 = false,
	                    bool smoke_mute_solo = false,
	                    bool smoke_pan = false,
	                    bool smoke_send = false,
	                    bool smoke_automation = false,
	                    bool smoke_edit = false);
	int  run_message_loop (unsigned timeout_secs);

	HWND hwnd () const { return _hwnd; }

private:
	enum class DragMode {
		None,
		WavePending, /* click may become select or pan */
		WavePan,
		Fader,
		FxParam, /* drag Mixer Blend slider */
		Pan, /* drag Mixer pan azimuth */
		Send, /* drag Mixer aux send level */
		AutoWrite, /* click/drag Editor automation lane */
		FadeIn, /* drag selection fade-in handle */
		FadeOut, /* drag selection fade-out handle */
		MidiDraw /* click/drag on MIDI lane to create a note */
	};

	static constexpr uint8_t kMidiPitchLo = 48; /* C3 */
	static constexpr uint8_t kMidiPitchHi = 71; /* B4 */
	static constexpr int     kMidiPitchRows = (int)(kMidiPitchHi - kMidiPitchLo + 1);

	enum class SmokeRecordPhase {
		Off,
		NeedArm,
		ArmWait,
		Rolling,
		FinalizeWait,
		Done
	};

	struct EditorLane {
		WaveStrip   strip;
		D2D1_RECT_F bounds;
		wchar_t     label[24];
	};

	static LRESULT CALLBACK wnd_proc (HWND, UINT, WPARAM, LPARAM);
	LRESULT handle_message (UINT msg, WPARAM wparam, LPARAM lparam);

	bool init_d2d ();
	void discard_device_resources ();
	bool ensure_device_resources ();
	void on_resize (UINT width, UINT height);
	void on_dpi_changed (UINT dpi, const RECT* suggested);
	void on_paint ();
	void on_tick ();
	void layout_panes ();
	void draw_editor_pane ();
	void draw_automation_lane ();
	void refresh_automation_lane ();
	void write_automation_at (float x, float y);
	void draw_mixer_pane ();
	void transport_play ();
	void transport_stop ();
	void transport_toggle ();
	void transport_toggle_record ();
	void prompt_import_audio ();
	void do_save_session ();
	void prompt_open_session ();
	void prompt_export_wav ();
	void do_undo ();
	void do_redo ();
	void do_delete_selection ();
	void do_split_at_playhead ();
	void do_apply_crossfade ();
	void do_toggle_loop ();
	void do_add_marker_at_playhead ();
	void do_remove_marker_at_playhead ();
	void sync_mixer_arm_from_engine ();
	void sync_mixer_mute_solo_from_engine ();
	void sync_mixer_pan_from_engine ();
	void sync_mixer_send_from_engine ();
	void sync_mixer_fx_from_engine ();
	void sync_transport_loop_from_engine ();
	void apply_shared_view (int source_lane);
	void reset_shared_view ();
	void apply_region_selection_to_lanes ();
	void apply_loop_to_lanes ();
	void draw_editor_ruler ();
	void draw_midi_lane ();
	bool hit_editor_ruler (float x, float y) const;
	bool hit_midi_lane (float x, float y) const;
	uint8_t midi_pitch_at_y (float y) const;
	float midi_time_at_x (float x) const;
	int  hit_editor_lane (float x, float y) const;
	int  hit_mixer_fader (float x, float y) const;
	int  hit_mixer_arm (float x, float y) const;
	int  hit_mixer_mute (float x, float y) const;
	int  hit_mixer_solo (float x, float y) const;
	int  hit_mixer_pan (float x, float y) const;
	int  hit_mixer_send (float x, float y) const;
	int  hit_mixer_meter (float x, float y) const;
	int  hit_mixer_fx (float x, float y) const;
	int  hit_mixer_fx_param (float x, float y) const;
	void client_from_lparam (LPARAM lparam, float* out_x, float* out_y) const;
	bool on_mouse_wheel (WPARAM wparam, LPARAM lparam);
	bool on_mouse_down (LPARAM lparam);
	bool on_mouse_move (LPARAM lparam);
	void on_mouse_up ();
	float stub_meter_peak (int track, float dt) const;
	void tick_smoke_record (DWORD now);

	HWND _hwnd;
	UINT _width;
	UINT _height;
	UINT _dpi;
	float _dpi_scale;
	bool _running;
	DWORD _start_tick;
	unsigned _timeout_secs;
	DragMode _drag;
	int _drag_index;
	float _pan_last_x;
	float _pan_down_x;
	float _pan_down_y;
	float _midi_draw_start_sec;
	uint8_t _midi_draw_pitch;

	bool  _transport_playing;
	float _playhead_sec;
	DWORD _last_tick_ms;

	SmokeRecordPhase _smoke_record;
	DWORD            _smoke_record_start_ms;
	bool             _smoke_failed; /* any --smoke-* reported FAILED */

	D2D1_RECT_F _brand_bar;
	D2D1_RECT_F _transport_bounds;
	D2D1_RECT_F _editor_pane;
	D2D1_RECT_F _editor_ruler;
	D2D1_RECT_F _midi_lane;
	D2D1_RECT_F _auto_lane_bounds;
	D2D1_RECT_F _mixer_pane;
	D2D1_RECT_F _status_bar;

	ID2D1Factory*             _d2d_factory;
	IDWriteFactory*           _dwrite_factory;
	ID2D1HwndRenderTarget*    _rt;
	ID2D1SolidColorBrush*     _brush_brand;
	ID2D1SolidColorBrush*     _brush_body;
	ID2D1SolidColorBrush*     _brush_accent;
	ID2D1SolidColorBrush*     _brush_pane;
	ID2D1SolidColorBrush*     _brush_pane_edge;
	ID2D1SolidColorBrush*     _brush_peak;
	ID2D1SolidColorBrush*     _brush_rms;
	ID2D1SolidColorBrush*     _brush_playhead;
	ID2D1SolidColorBrush*     _brush_selection;
	ID2D1SolidColorBrush*     _brush_loop;
	ID2D1SolidColorBrush*     _brush_marker;
	ID2D1SolidColorBrush*     _brush_midi;
	ID2D1SolidColorBrush*     _brush_meter_hot;
	ID2D1LinearGradientBrush* _brush_bg;
	IDWriteTextFormat*        _fmt_brand;
	IDWriteTextFormat*        _fmt_body;
	IDWriteTextFormat*        _fmt_meta;
	IDWriteTextFormat*        _fmt_pane;

	float _pulse;
	EngineHost     _engine;
	EngineSnapshot _engine_snap;
	TransportBar   _transport;
	EditorLane     _lanes[kTracks];
	AutomationLane _auto_lane;
	MixerStrip     _mixers[kMixerStrips];
	float          _auto_last_write_sec;
};
