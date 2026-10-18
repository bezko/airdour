/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Win32 + Direct2D + DirectWrite shell: DPI-aware host surface,
 * editor/mixer panes, wave strip zoom/pan, live engine status.
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

#include "engine_host.h"
#include "wave_strip.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shcore.lib")

class ShellWindow
{
public:
	ShellWindow ();
	~ShellWindow ();

	bool create (HINSTANCE instance, int show_cmd);
	bool attach_engine (const std::string& backend_name);
	int  run_message_loop (unsigned timeout_secs);

	HWND hwnd () const { return _hwnd; }

private:
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
	bool hit_wave (float x, float y) const;
	void client_from_lparam (LPARAM lparam, float* out_x, float* out_y) const;
	bool on_mouse_wheel (WPARAM wparam, LPARAM lparam);
	bool on_mouse_down (LPARAM lparam);
	bool on_mouse_move (LPARAM lparam);
	void on_mouse_up ();
	float dip (float px) const { return px * _dpi_scale; }

	HWND _hwnd;
	UINT _width;
	UINT _height;
	UINT _dpi;
	float _dpi_scale;
	bool _running;
	DWORD _start_tick;
	unsigned _timeout_secs;
	bool _panning;
	float _pan_last_x;

	D2D1_RECT_F _brand_bar;
	D2D1_RECT_F _editor_pane;
	D2D1_RECT_F _mixer_pane;
	D2D1_RECT_F _status_bar;
	D2D1_RECT_F _wave_bounds;

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
	ID2D1LinearGradientBrush* _brush_bg;
	IDWriteTextFormat*        _fmt_brand;
	IDWriteTextFormat*        _fmt_body;
	IDWriteTextFormat*        _fmt_meta;
	IDWriteTextFormat*        _fmt_pane;

	float _pulse;
	EngineHost _engine;
	EngineSnapshot _engine_snap;
	WaveStrip _wave;
};
