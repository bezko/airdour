/*
 * Copyright (C) 2026 Airdour contributors
 */

#include "shell_window.h"

#include <cmath>
#include <cstring>

namespace {

constexpr wchar_t kClassName[] = L"AirdourWinShell";
constexpr wchar_t kWindowTitle[] = L"Airdour";

/* Visual direction: deep slate + teal accent (not purple / cream / broadsheet). */
constexpr D2D1_COLOR_F kBgTop    = { 0.055f, 0.075f, 0.095f, 1.f }; /* #0e1318 */
constexpr D2D1_COLOR_F kBgBottom = { 0.090f, 0.140f, 0.165f, 1.f }; /* #17242a */
constexpr D2D1_COLOR_F kBrand    = { 0.930f, 0.955f, 0.970f, 1.f }; /* cool near-white */
constexpr D2D1_COLOR_F kBody     = { 0.620f, 0.720f, 0.760f, 1.f };
constexpr D2D1_COLOR_F kAccent   = { 0.180f, 0.820f, 0.720f, 1.f }; /* teal */

} // namespace

ShellWindow::ShellWindow ()
	: _hwnd (nullptr)
	, _width (960)
	, _height (600)
	, _running (false)
	, _start_tick (0)
	, _timeout_secs (0)
	, _d2d_factory (nullptr)
	, _dwrite_factory (nullptr)
	, _rt (nullptr)
	, _brush_brand (nullptr)
	, _brush_body (nullptr)
	, _brush_accent (nullptr)
	, _brush_bg (nullptr)
	, _fmt_brand (nullptr)
	, _fmt_body (nullptr)
	, _fmt_meta (nullptr)
	, _pulse (0.f)
{
}

ShellWindow::~ShellWindow ()
{
	discard_device_resources ();
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

	/* Prefer a display face; fall back to Segoe UI if Variable Display is absent. */
	hr = _dwrite_factory->CreateTextFormat (
		L"Segoe UI Variable Display", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
		DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 72.0f, L"en-us", &_fmt_brand);
	if (FAILED (hr)) {
		hr = _dwrite_factory->CreateTextFormat (
			L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
			DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 72.0f, L"en-us", &_fmt_brand);
	}
	if (FAILED (hr)) {
		return false;
	}
	_fmt_brand->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_LEADING);
	_fmt_brand->SetParagraphAlignment (DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

	hr = _dwrite_factory->CreateTextFormat (
		L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
		DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 20.0f, L"en-us", &_fmt_body);
	if (FAILED (hr)) {
		hr = _dwrite_factory->CreateTextFormat (
			L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
			DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 20.0f, L"en-us", &_fmt_body);
	}
	if (FAILED (hr)) {
		return false;
	}
	_fmt_body->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_LEADING);

	hr = _dwrite_factory->CreateTextFormat (
		L"Cascadia Mono", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
		DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &_fmt_meta);
	if (FAILED (hr)) {
		hr = _dwrite_factory->CreateTextFormat (
			L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
			DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &_fmt_meta);
	}
	if (FAILED (hr)) {
		return false;
	}
	_fmt_meta->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_LEADING);

	return true;
}

void
ShellWindow::discard_device_resources ()
{
	if (_brush_bg) {
		_brush_bg->Release ();
		_brush_bg = nullptr;
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

	return _brush_brand && _brush_body && _brush_accent && _brush_bg;
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

	ShowWindow (_hwnd, show_cmd);
	UpdateWindow (_hwnd);
	SetTimer (_hwnd, 1, 16, nullptr); /* ~60 Hz for presence motion */
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
		discard_device_resources ();
		PostQuitMessage (0);
		return 0;
	default:
		return DefWindowProcW (_hwnd, msg, wparam, lparam);
	}
}

void
ShellWindow::on_resize (UINT width, UINT height)
{
	_width = width ? width : 1;
	_height = height ? height : 1;
	if (_rt) {
		_rt->Resize (D2D1::SizeU (_width, _height));
		/* Rebuild vertical gradient for new height. */
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

	if (_timeout_secs > 0 && (now - _start_tick) >= (_timeout_secs * 1000u)) {
		PostQuitMessage (0);
		return;
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
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

	/* Soft accent wash (motion 1: drifting opacity with pulse). */
	float wash = 0.04f + 0.03f * _pulse;
	_brush_accent->SetOpacity (wash);
	_rt->FillRectangle (D2D1::RectF (0, sz.height * 0.55f, sz.width, sz.height), _brush_accent);
	_brush_accent->SetOpacity (1.f);

	/* Accent rule under brand (motion 2: width breathes with pulse). */
	float rule_w = 120.f + 40.f * _pulse;
	float left = 64.f;
	float brand_y = sz.height * 0.28f;
	_rt->FillRectangle (D2D1::RectF (left, brand_y + 86.f, left + rule_w, brand_y + 89.f), _brush_accent);

	/* Brand — hero signal (must dominate the first viewport). */
	_brush_brand->SetOpacity (0.92f + 0.08f * _pulse); /* motion 3: presence */
	const wchar_t* brand = L"Airdour";
	_rt->DrawTextW (brand, (UINT32)wcslen (brand), _fmt_brand,
	                D2D1::RectF (left, brand_y, sz.width - left, brand_y + 96.f),
	                _brush_brand);
	_brush_brand->SetOpacity (1.f);

	const wchar_t* line = L"Windows-native shell. WASAPI shared. Direct2D canvas next.";
	_rt->DrawTextW (line, (UINT32)wcslen (line), _fmt_body,
	                D2D1::RectF (left, brand_y + 108.f, sz.width - left, brand_y + 160.f),
	                _brush_body);

	const wchar_t* meta = L"win_ardour  ·  Esc to quit  ·  no GTK2";
	_rt->DrawTextW (meta, (UINT32)wcslen (meta), _fmt_meta,
	                D2D1::RectF (left, sz.height - 48.f, sz.width - left, sz.height - 24.f),
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

	MSG msg = {};
	while (_running && GetMessageW (&msg, nullptr, 0, 0) > 0) {
		TranslateMessage (&msg);
		DispatchMessageW (&msg);
	}
	return (int)msg.wParam;
}
