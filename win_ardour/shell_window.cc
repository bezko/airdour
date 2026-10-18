/*
 * Copyright (C) 2026 Airdour contributors
 */

#include "shell_window.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr wchar_t kClassName[] = L"AirdourWinShell";
constexpr wchar_t kWindowTitle[] = L"Airdour";

/* Visual direction: deep slate + teal accent (not purple / cream / broadsheet). */
constexpr D2D1_COLOR_F kBgTop     = { 0.055f, 0.075f, 0.095f, 1.f };
constexpr D2D1_COLOR_F kBgBottom  = { 0.090f, 0.140f, 0.165f, 1.f };
constexpr D2D1_COLOR_F kBrand     = { 0.930f, 0.955f, 0.970f, 1.f };
constexpr D2D1_COLOR_F kBody      = { 0.620f, 0.720f, 0.760f, 1.f };
constexpr D2D1_COLOR_F kAccent    = { 0.180f, 0.820f, 0.720f, 1.f };
constexpr D2D1_COLOR_F kPaneFill  = { 0.070f, 0.100f, 0.120f, 0.92f };
constexpr D2D1_COLOR_F kPaneEdge  = { 0.160f, 0.220f, 0.250f, 1.f };
constexpr D2D1_COLOR_F kPeak      = { 0.220f, 0.920f, 0.820f, 1.f }; /* teal peak tips */
constexpr D2D1_COLOR_F kRms       = { 0.120f, 0.420f, 0.400f, 0.85f }; /* darker RMS body */

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
			/* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4 */
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
			set_aware (2); /* PROCESS_PER_MONITOR_DPI_AWARE */
		}
		FreeLibrary (shcore);
	}
}

} // namespace

ShellWindow::ShellWindow ()
	: _hwnd (nullptr)
	, _width (1100)
	, _height (700)
	, _dpi (96)
	, _dpi_scale (1.f)
	, _running (false)
	, _start_tick (0)
	, _timeout_secs (0)
	, _brand_bar ()
	, _editor_pane ()
	, _mixer_pane ()
	, _status_bar ()
	, _wave_bounds ()
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
	, _brush_bg (nullptr)
	, _fmt_brand (nullptr)
	, _fmt_body (nullptr)
	, _fmt_meta (nullptr)
	, _fmt_pane (nullptr)
	, _pulse (0.f)
	, _engine_snap ()
{
	enable_dpi_awareness ();
	_engine_snap = _engine.snapshot ();
}

ShellWindow::~ShellWindow ()
{
	_engine.stop ();
	discard_device_resources ();
	if (_fmt_pane) {
		_fmt_pane->Release ();
		_fmt_pane = nullptr;
	}
	if (_fmt_meta) {
		_fmt_meta->Release ();
		_fmt_meta = nullptr;
	}
	if (_fmt_body) {
		_fmt_body->Release ();
		_fmt_body = nullptr;
	}
	if (_fmt_brand) {
		_fmt_brand->Release ();
		_fmt_brand = nullptr;
	}
	if (_dwrite_factory) {
		_dwrite_factory->Release ();
		_dwrite_factory = nullptr;
	}
	if (_d2d_factory) {
		_d2d_factory->Release ();
		_d2d_factory = nullptr;
	}
	if (_hwnd) {
		DestroyWindow (_hwnd);
		_hwnd = nullptr;
	}
}

bool
ShellWindow::attach_engine (const std::string& backend_name)
{
	if (!_engine.is_linked ()) {
		_engine_snap = _engine.snapshot ();
		return false;
	}
	bool ok = _engine.start (backend_name);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	return ok;
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
	if (_brush_bg) {
		_brush_bg->Release ();
		_brush_bg = nullptr;
	}
	if (_brush_rms) {
		_brush_rms->Release ();
		_brush_rms = nullptr;
	}
	if (_brush_peak) {
		_brush_peak->Release ();
		_brush_peak = nullptr;
	}
	if (_brush_pane_edge) {
		_brush_pane_edge->Release ();
		_brush_pane_edge = nullptr;
	}
	if (_brush_pane) {
		_brush_pane->Release ();
		_brush_pane = nullptr;
	}
	if (_brush_accent) {
		_brush_accent->Release ();
		_brush_accent = nullptr;
	}
	if (_brush_body) {
		_brush_body->Release ();
		_brush_body = nullptr;
	}
	if (_brush_brand) {
		_brush_brand->Release ();
		_brush_brand = nullptr;
	}
	if (_rt) {
		_rt->Release ();
		_rt = nullptr;
	}
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

	/* Physical pixels; DPI scale applied in layout DIPs via font sizes already in DIPs
	 * when render target uses default DPI from HWND. */
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

	ID2D1GradientStopCollection* stops = nullptr;
	D2D1_GRADIENT_STOP gs[2];
	gs[0].position = 0.f;
	gs[0].color = kBgTop;
	gs[1].position = 1.f;
	gs[1].color = kBgBottom;
	hr = _rt->CreateGradientStopCollection (gs, 2, &stops);
	if (SUCCEEDED (hr)) {
		_rt->CreateLinearGradientBrush (
			D2D1::LinearGradientBrushProperties (D2D1::Point2F (0, 0), D2D1::Point2F (0, (float)size.height)),
			stops, &_brush_bg);
		stops->Release ();
	}

	return _brush_brand && _brush_body && _brush_accent && _brush_pane && _brush_peak && _brush_rms && _brush_bg;
}

void
ShellWindow::layout_panes ()
{
	const float w = (float)_width;
	const float h = (float)_height;
	const float pad = 16.f * _dpi_scale;
	const float brand_h = 88.f * _dpi_scale;
	const float status_h = 40.f * _dpi_scale;
	const float gap = 12.f * _dpi_scale;

	_brand_bar = D2D1::RectF (0, 0, w, brand_h);
	_status_bar = D2D1::RectF (0, h - status_h, w, h);

	float body_top = brand_h + pad;
	float body_bot = h - status_h - pad;
	float body_h = body_bot - body_top;
	float left = pad;
	float right = w - pad;
	float split = left + (right - left) * 0.62f;

	_editor_pane = D2D1::RectF (left, body_top, split - gap * 0.5f, body_bot);
	_mixer_pane = D2D1::RectF (split + gap * 0.5f, body_top, right, body_bot);

	/* Wave strip fills most of the editor under the title row. */
	float inset = 14.f * _dpi_scale;
	float title_h = 48.f * _dpi_scale;
	_wave_bounds = D2D1::RectF (
		_editor_pane.left + inset,
		_editor_pane.top + title_h,
		_editor_pane.right - inset,
		_editor_pane.bottom - inset);
	_wave.resize_for_width (_wave_bounds.right - _wave_bounds.left);

	(void)body_h;
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
	_dpi = 96;
	_dpi_scale = 1.f;

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
	SetTimer (_hwnd, 1, 16, nullptr);
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
		}
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
		gs[0].position = 0.f;
		gs[0].color = kBgTop;
		gs[1].position = 1.f;
		gs[1].color = kBgBottom;
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
	float t = (now - _start_tick) * 0.001f;
	_pulse = 0.5f + 0.5f * std::sin (t * 1.6f);

	/* Poll engine on UI thread only (~4 Hz). */
	static DWORD last_poll = 0;
	if (now - last_poll > 250) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		last_poll = now;
	}

	WaveStripParams wp;
	wp.engine_running = _engine_snap.running;
	wp.sample_rate = _engine_snap.sample_rate;
	wp.period_frames = _engine_snap.period_frames;
	wp.time_sec = t;
	_wave.update (wp);

	if (_timeout_secs > 0 && (now - _start_tick) >= (_timeout_secs * 1000u)) {
		PostQuitMessage (0);
		return;
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::draw_editor_pane ()
{
	const D2D1_RECT_F& r = _editor_pane;
	_rt->FillRectangle (r, _brush_pane);
	_rt->DrawRectangle (r, _brush_pane_edge, 1.f);
	_rt->FillRectangle (D2D1::RectF (r.left, r.top, r.left + 3.f * _dpi_scale, r.bottom), _brush_accent);

	float inset = 14.f * _dpi_scale;
	const wchar_t* title = L"Editor";
	_rt->DrawTextW (title, (UINT32)wcslen (title), _fmt_pane,
	                D2D1::RectF (r.left + inset, r.top + inset, r.right - inset, r.top + inset + 22.f * _dpi_scale),
	                _brush_brand);

	wchar_t caption[128];
	if (_engine_snap.running) {
		_snwprintf_s (caption, _TRUNCATE, L"Wave strip v0 (demo peaks/RMS)  ·  live %s",
		              _engine_snap.backend.c_str ());
	} else {
		_snwprintf_s (caption, _TRUNCATE, L"Wave strip v0 (demo peaks/RMS)  ·  Direct2D, not Cairo");
	}
	_rt->DrawTextW (caption, (UINT32)wcslen (caption), _fmt_meta,
	                D2D1::RectF (r.left + inset + 90.f * _dpi_scale, r.top + inset + 2.f * _dpi_scale,
	                             r.right - inset, r.top + inset + 22.f * _dpi_scale),
	                _brush_body);

	/* Inner well behind the strip. */
	_rt->FillRectangle (_wave_bounds, _brush_pane_edge);
	_wave.draw (_rt, _wave_bounds, _brush_peak, _brush_rms, _brush_body, _dpi_scale);
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

	/* Brand bar — hero product name. */
	_brush_brand->SetOpacity (0.94f + 0.06f * _pulse);
	const wchar_t* brand = L"Airdour";
	_rt->DrawTextW (brand, (UINT32)wcslen (brand), _fmt_brand,
	                D2D1::RectF (_brand_bar.left + 24.f * _dpi_scale, _brand_bar.top,
	                             _brand_bar.right - 24.f * _dpi_scale, _brand_bar.bottom),
	                _brush_brand);
	_brush_brand->SetOpacity (1.f);

	float rule_y = _brand_bar.bottom - 10.f * _dpi_scale;
	float rule_w = (100.f + 36.f * _pulse) * _dpi_scale;
	_rt->FillRectangle (D2D1::RectF (24.f * _dpi_scale, rule_y,
	                                 24.f * _dpi_scale + rule_w, rule_y + 2.f * _dpi_scale),
	                    _brush_accent);

	draw_editor_pane ();

	/* Mixer placeholder pane. */
	{
		const D2D1_RECT_F& r = _mixer_pane;
		_rt->FillRectangle (r, _brush_pane);
		_rt->DrawRectangle (r, _brush_pane_edge, 1.f);
		_rt->FillRectangle (D2D1::RectF (r.left, r.top, r.left + 3.f * _dpi_scale, r.bottom), _brush_accent);
		float inset = 14.f * _dpi_scale;
		const wchar_t* mt = L"Mixer";
		const wchar_t* mb = L"Strip placeholders. Win32/D2D controls - no GTK widgets.";
		_rt->DrawTextW (mt, (UINT32)wcslen (mt), _fmt_pane,
		                D2D1::RectF (r.left + inset, r.top + inset, r.right - inset, r.top + inset + 22.f * _dpi_scale),
		                _brush_brand);
		_rt->DrawTextW (mb, (UINT32)wcslen (mb), _fmt_body,
		                D2D1::RectF (r.left + inset, r.top + inset + 28.f * _dpi_scale,
		                             r.right - inset, r.bottom - inset),
		                _brush_body);
	}

	/* Status bar: live engine snapshot (UI thread). */
	std::wstring status;
	if (_engine_snap.running) {
		wchar_t buf[256];
		_snwprintf_s (buf, _TRUNCATE,
		              L"Engine  %s  ·  %u Hz  ·  %u frames (%.1f ms)  ·  dpi %u",
		              _engine_snap.backend.c_str (),
		              _engine_snap.sample_rate,
		              _engine_snap.period_frames,
		              _engine_snap.sample_rate
		                  ? (1000.0 * (double)_engine_snap.period_frames / (double)_engine_snap.sample_rate)
		                  : 0.0,
		              _dpi);
		status = buf;
	} else if (_engine_snap.available) {
		status = L"Engine idle — ";
		status += _engine_snap.detail;
	} else {
		status = L"Engine not linked — UI shell only. ";
		status += _engine_snap.detail;
	}

	_rt->DrawTextW (status.c_str (), (UINT32)status.size (), _fmt_meta,
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
	_running = true;
	_engine_snap = _engine.snapshot ();

	MSG msg = {};
	while (_running && GetMessageW (&msg, nullptr, 0, 0) > 0) {
		TranslateMessage (&msg);
		DispatchMessageW (&msg);
	}

	_engine.stop ();
	return (int)msg.wParam;
}
