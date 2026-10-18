/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Minimal Win32 + Direct2D + DirectWrite shell window.
 * No GTK, no Cairo — Windows-native GUI path for Airdour.
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

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

class ShellWindow
{
public:
	ShellWindow ();
	~ShellWindow ();

	bool create (HINSTANCE instance, int show_cmd);
	int  run_message_loop (unsigned timeout_secs);

	HWND hwnd () const { return _hwnd; }

private:
	static LRESULT CALLBACK wnd_proc (HWND, UINT, WPARAM, LPARAM);
	LRESULT handle_message (UINT msg, WPARAM wparam, LPARAM lparam);

	bool init_d2d ();
	void discard_device_resources ();
	bool ensure_device_resources ();
	void on_resize (UINT width, UINT height);
	void on_paint ();
	void on_tick ();

	HWND _hwnd;
	UINT _width;
	UINT _height;
	bool _running;
	DWORD _start_tick;
	unsigned _timeout_secs;

	ID2D1Factory*          _d2d_factory;
	IDWriteFactory*        _dwrite_factory;
	ID2D1HwndRenderTarget* _rt;
	ID2D1SolidColorBrush*  _brush_brand;
	ID2D1SolidColorBrush*  _brush_body;
	ID2D1SolidColorBrush*  _brush_accent;
	ID2D1LinearGradientBrush* _brush_bg;
	IDWriteTextFormat*     _fmt_brand;
	IDWriteTextFormat*     _fmt_body;
	IDWriteTextFormat*     _fmt_meta;

	float _pulse; /* 0..1 brand presence motion */
};
