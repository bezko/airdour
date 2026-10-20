/*
 * Copyright (C) 2026 Airdour contributors
 *
 * D2D crash-recovery prompt: Restore / Discard. Brand Airdour — no GTK.
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

enum class RecoveryHit {
	None,
	Restore,
	Discard
};

class RecoveryPanel
{
public:
	static constexpr int kPathLen = 96;

	RecoveryPanel ();

	void layout (float view_w, float view_h, float dpi_scale);

	void set_visible (bool on) { _visible = on; }
	bool visible () const { return _visible; }

	void set_session_hint (const wchar_t* label, const wchar_t* path);

	RecoveryHit hit_test (float x, float y) const;

	void draw (ID2D1RenderTarget* rt,
	           ID2D1SolidColorBrush* brand,
	           ID2D1SolidColorBrush* body,
	           ID2D1SolidColorBrush* accent,
	           ID2D1SolidColorBrush* pane,
	           ID2D1SolidColorBrush* edge,
	           IDWriteTextFormat* fmt_title,
	           IDWriteTextFormat* fmt_meta,
	           float dpi_scale) const;

private:
	D2D1_RECT_F _card;
	D2D1_RECT_F _restore_btn;
	D2D1_RECT_F _discard_btn;
	bool        _visible;
	wchar_t     _label[48];
	wchar_t     _path[kPathLen];
};
