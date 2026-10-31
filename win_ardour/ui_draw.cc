/*
 * Copyright (C) 2026 Airdour contributors
 */

#include "ui_draw.h"

#include <algorithm>
#include <cmath>
#include <cwchar>

namespace {

inline float
clamp01 (float v)
{
	return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
}

inline D2D1_RECT_F
inset (const D2D1_RECT_F& r, float d)
{
	return D2D1::RectF (r.left + d, r.top + d, r.right - d, r.bottom - d);
}

} // namespace

UiDraw::UiDraw ()
	: _rt (nullptr)
	, _dw (nullptr)
	, _dpi_scale (1.f)
{
	for (int i = 0; i < kBrushCount; ++i) {
		_brushes[i] = nullptr;
	}
	for (int i = 0; i < kFormatCount; ++i) {
		_formats[i] = nullptr;
	}
}

UiDraw::~UiDraw ()
{
	detach ();
}

void
UiDraw::release_brushes ()
{
	for (int i = 0; i < kBrushCount; ++i) {
		if (_brushes[i]) {
			_brushes[i]->Release ();
			_brushes[i] = nullptr;
		}
	}
}

void
UiDraw::release_formats ()
{
	for (int i = 0; i < kFormatCount; ++i) {
		if (_formats[i]) {
			_formats[i]->Release ();
			_formats[i] = nullptr;
		}
	}
}

void
UiDraw::detach ()
{
	release_formats ();
	release_brushes ();
	_rt = nullptr;
	_dw = nullptr;
	_dpi_scale = 1.f;
}

bool
UiDraw::make_format (const wchar_t* preferred, const wchar_t* fallback,
                     DWRITE_FONT_WEIGHT weight, float size_dip,
                     IDWriteTextFormat** out)
{
	*out = nullptr;
	if (!_dw) {
		return false;
	}
	const float size = std::max (1.f, size_dip * _dpi_scale);
	HRESULT hr = _dw->CreateTextFormat (preferred, nullptr, weight,
	                                    DWRITE_FONT_STYLE_NORMAL,
	                                    DWRITE_FONT_STRETCH_NORMAL, size,
	                                    L"en-us", out);
	if (FAILED (hr)) {
		hr = _dw->CreateTextFormat (fallback, nullptr, weight,
		                            DWRITE_FONT_STYLE_NORMAL,
		                            DWRITE_FONT_STRETCH_NORMAL, size,
		                            L"en-us", out);
	}
	if (FAILED (hr)) {
		*out = nullptr;
		return false;
	}
	(*out)->SetWordWrapping (DWRITE_WORD_WRAPPING_NO_WRAP);
	(*out)->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_LEADING);
	(*out)->SetParagraphAlignment (DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
	return true;
}

void
UiDraw::rebuild_text_formats (float dpi_scale)
{
	if (dpi_scale > 0.f) {
		_dpi_scale = dpi_scale;
	}
	release_formats ();
	if (!_dw) {
		return;
	}
	const ui::Theme& th = ui::theme ();
	bool ok = true;
	ok = make_format (th.font_display, th.font_display_fallback,
	                  DWRITE_FONT_WEIGHT_BOLD, th.type.brand,
	                  &_formats[static_cast<int> (ui::TextRole::Brand)]) && ok;
	ok = make_format (th.font_ui, th.font_ui_fallback,
	                  DWRITE_FONT_WEIGHT_SEMI_BOLD, th.type.title,
	                  &_formats[static_cast<int> (ui::TextRole::Title)]) && ok;
	ok = make_format (th.font_ui, th.font_ui_fallback,
	                  DWRITE_FONT_WEIGHT_SEMI_BOLD, th.type.section,
	                  &_formats[static_cast<int> (ui::TextRole::Section)]) && ok;
	ok = make_format (th.font_ui, th.font_ui_fallback,
	                  DWRITE_FONT_WEIGHT_NORMAL, th.type.body,
	                  &_formats[static_cast<int> (ui::TextRole::Body)]) && ok;
	ok = make_format (th.font_ui, th.font_ui_fallback,
	                  DWRITE_FONT_WEIGHT_NORMAL, th.type.caption,
	                  &_formats[static_cast<int> (ui::TextRole::Caption)]) && ok;
	ok = make_format (th.font_mono, th.font_mono_fallback,
	                  DWRITE_FONT_WEIGHT_MEDIUM, th.type.mono,
	                  &_formats[static_cast<int> (ui::TextRole::Mono)]) && ok;
	ok = make_format (th.font_mono, th.font_mono_fallback,
	                  DWRITE_FONT_WEIGHT_MEDIUM, th.type.clock,
	                  &_formats[static_cast<int> (ui::TextRole::Clock)]) && ok;
	ok = make_format (th.font_glyph, th.font_glyph_fallback,
	                  DWRITE_FONT_WEIGHT_NORMAL, th.type.glyph,
	                  &_formats[static_cast<int> (ui::TextRole::Glyph)]) && ok;
	if (!ok) {
		release_formats ();
	}
}

bool
UiDraw::attach (ID2D1RenderTarget* rt, IDWriteFactory* dw, float dpi_scale)
{
	if (!rt || !dw) {
		return false;
	}
	if (_rt == rt && _dw == dw && _brushes[0] && _formats[0] &&
	    std::fabs (_dpi_scale - dpi_scale) < 0.001f) {
		return true;
	}
	detach ();
	_rt = rt;
	_dw = dw;
	_dpi_scale = (dpi_scale > 0.f) ? dpi_scale : 1.f;

	const ui::Theme& th = ui::theme ();
	for (int i = 0; i < kBrushCount; ++i) {
		const HRESULT hr = _rt->CreateSolidColorBrush (th.colors.v[i],
		                                               &_brushes[i]);
		if (FAILED (hr)) {
			release_brushes ();
			_rt = nullptr;
			_dw = nullptr;
			return false;
		}
	}
	rebuild_text_formats (_dpi_scale);
	return _brushes[0] && _formats[0];
}

ID2D1SolidColorBrush*
UiDraw::brush (ui::ColorToken token) const
{
	const int i = static_cast<int> (token);
	if (i < 0 || i >= kBrushCount) {
		return nullptr;
	}
	return _brushes[i];
}

IDWriteTextFormat*
UiDraw::fmt (ui::TextRole role) const
{
	const int i = static_cast<int> (role);
	if (i < 0 || i >= kFormatCount) {
		return nullptr;
	}
	return _formats[i];
}

void
UiDraw::fill_round (const D2D1_RECT_F& r, float radius_dip, ui::ColorToken c,
                    float opacity)
{
	if (!_rt) {
		return;
	}
	ID2D1SolidColorBrush* br = brush (c);
	if (!br) {
		return;
	}
	br->SetOpacity (clamp01 (opacity));
	if (radius_dip <= 0.01f || r.right <= r.left || r.bottom <= r.top) {
		_rt->FillRectangle (r, br);
	} else {
		const float rad = dp (radius_dip);
		_rt->FillRoundedRectangle (D2D1::RoundedRect (r, rad, rad), br);
	}
	br->SetOpacity (1.f);
}

void
UiDraw::stroke_round (const D2D1_RECT_F& r, float radius_dip, ui::ColorToken c,
                      float width_dip, float opacity)
{
	if (!_rt) {
		return;
	}
	ID2D1SolidColorBrush* br = brush (c);
	if (!br) {
		return;
	}
	br->SetOpacity (clamp01 (opacity));
	if (radius_dip <= 0.01f) {
		_rt->DrawRectangle (r, br, dp (width_dip));
	} else {
		const float rad = dp (radius_dip);
		_rt->DrawRoundedRectangle (D2D1::RoundedRect (r, rad, rad), br,
		                           dp (width_dip));
	}
	br->SetOpacity (1.f);
}

void
UiDraw::shadow (const D2D1_RECT_F& r, float radius_dip)
{
	if (!_rt) {
		return;
	}
	struct Layer { float expand; float offset_y; float alpha; };
	static const Layer layers[] = {
		{ 2.f, 2.f, 0.10f },
		{ 5.f, 5.f, 0.06f },
		{ 9.f, 9.f, 0.035f },
	};
	for (const Layer& l : layers) {
		D2D1_RECT_F s = r;
		s.left -= dp (l.expand);
		s.right += dp (l.expand);
		s.top += dp (l.offset_y * 0.5f);
		s.bottom += dp (l.offset_y);
		fill_round (s, radius_dip + l.expand * 0.5f, ui::ColorToken::Shadow,
		            l.alpha);
	}
}

void
UiDraw::panel (const D2D1_RECT_F& r, float radius_dip, bool raised)
{
	if (raised) {
		shadow (r, radius_dip);
	}
	fill_round (r, radius_dip,
	            raised ? ui::ColorToken::SurfaceRaised : ui::ColorToken::Surface);
	stroke_round (r, radius_dip, ui::ColorToken::Border, 1.f);
}

void
UiDraw::card (const D2D1_RECT_F& r)
{
	shadow (r, ui::theme ().metrics.radius_lg);
	fill_round (r, ui::theme ().metrics.radius_lg, ui::ColorToken::Card);
	stroke_round (r, ui::theme ().metrics.radius_lg,
	              ui::ColorToken::BorderStrong, 1.f);
}

void
UiDraw::divider (const D2D1_RECT_F& r)
{
	fill_round (r, 0.f, ui::ColorToken::Divider, 0.9f);
}

void
UiDraw::text_n (const wchar_t* s, UINT32 len, ui::TextRole role,
                ui::ColorToken c, const D2D1_RECT_F& r,
                DWRITE_TEXT_ALIGNMENT align)
{
	if (!_rt || !s) {
		return;
	}
	IDWriteTextFormat* f = fmt (role);
	ID2D1SolidColorBrush* br = brush (c);
	if (!f || !br) {
		return;
	}
	const DWRITE_TEXT_ALIGNMENT old_align = f->GetTextAlignment ();
	const DWRITE_PARAGRAPH_ALIGNMENT old_para = f->GetParagraphAlignment ();
	f->SetTextAlignment (align);
	f->SetParagraphAlignment (DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
	_rt->DrawTextW (s, len, f, r, br);
	f->SetTextAlignment (old_align);
	f->SetParagraphAlignment (old_para);
}

void
UiDraw::text (const wchar_t* s, ui::TextRole role, ui::ColorToken c,
              const D2D1_RECT_F& r, DWRITE_TEXT_ALIGNMENT align)
{
	if (!s) {
		return;
	}
	text_n (s, static_cast<UINT32> (std::wcslen (s)), role, c, r, align);
}

void
UiDraw::text_vcenter_n (const wchar_t* s, UINT32 len, ui::TextRole role,
                        ui::ColorToken c, const D2D1_RECT_F& r,
                        DWRITE_TEXT_ALIGNMENT align)
{
	if (!_rt || !s) {
		return;
	}
	IDWriteTextFormat* f = fmt (role);
	ID2D1SolidColorBrush* br = brush (c);
	if (!f || !br) {
		return;
	}
	const DWRITE_TEXT_ALIGNMENT old_align = f->GetTextAlignment ();
	const DWRITE_PARAGRAPH_ALIGNMENT old_para = f->GetParagraphAlignment ();
	f->SetTextAlignment (align);
	f->SetParagraphAlignment (DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
	_rt->DrawTextW (s, len, f, r, br);
	f->SetTextAlignment (old_align);
	f->SetParagraphAlignment (old_para);
}

void
UiDraw::text_vcenter (const wchar_t* s, ui::TextRole role, ui::ColorToken c,
                      const D2D1_RECT_F& r, DWRITE_TEXT_ALIGNMENT align)
{
	if (!s) {
		return;
	}
	text_vcenter_n (s, static_cast<UINT32> (std::wcslen (s)), role, c, r, align);
}

void
UiDraw::button (const D2D1_RECT_F& r, const ButtonStyle& st)
{
	if (!_rt || r.right <= r.left || r.bottom <= r.top) {
		return;
	}
	const float radius = 5.f;
	ui::ColorToken bg = ui::ColorToken::Control;
	if (st.latched) {
		bg = st.latch;
		if (st.latch == ui::ColorToken::Accent) {
			if (st.pressed) {
				bg = ui::ColorToken::AccentPressed;
			} else if (st.hovered) {
				bg = ui::ColorToken::AccentHover;
			}
		}
	} else if (st.pressed) {
		bg = ui::ColorToken::ControlPressed;
	} else if (st.hovered) {
		bg = ui::ColorToken::ControlHover;
	}
	const ui::ColorToken border =
	    (st.latched || st.hovered || st.pressed) ? ui::ColorToken::BorderStrong
	                                             : ui::ColorToken::Border;
	fill_round (r, radius, bg);
	stroke_round (r, radius, border, 1.f);

	const ui::ColorToken ink =
	    st.latched ? ui::ColorToken::AccentInk : ui::ColorToken::TextPrimary;
	const ui::ColorToken sub_ink =
	    st.latched ? ui::ColorToken::AccentInk : ui::ColorToken::TextSecondary;

	if (st.glyph && st.label && st.show_label) {
		const float split = r.top + (r.bottom - r.top) * 0.58f;
		const D2D1_RECT_F gr = D2D1::RectF (r.left, r.top + dp (2.f),
		                                    r.right, split);
		const D2D1_RECT_F lr = D2D1::RectF (r.left, split - dp (1.f),
		                                    r.right, r.bottom - dp (1.f));
		text_vcenter (st.glyph, ui::TextRole::Glyph, ink, gr,
		              DWRITE_TEXT_ALIGNMENT_CENTER);
		text_vcenter (st.label, ui::TextRole::Caption, sub_ink, lr,
		              DWRITE_TEXT_ALIGNMENT_CENTER);
	} else if (st.glyph) {
		text_vcenter (st.glyph, ui::TextRole::Glyph, ink, r,
		              DWRITE_TEXT_ALIGNMENT_CENTER);
	} else if (st.label) {
		text_vcenter (st.label, ui::TextRole::Section, ink, r,
		              DWRITE_TEXT_ALIGNMENT_CENTER);
	}
}

void
UiDraw::slider (const D2D1_RECT_F& r, float t01, ui::ColorToken fill,
                bool hovered)
{
	if (!_rt || r.right <= r.left || r.bottom <= r.top) {
		return;
	}
	const float h = r.bottom - r.top;
	const float radius = h * 0.5f / std::max (0.001f, _dpi_scale);
	fill_round (r, radius, ui::ColorToken::Control);
	stroke_round (r, radius, ui::ColorToken::Border, 1.f);
	const float t = clamp01 (t01);
	const float fill_right = r.left + (r.right - r.left) * t;
	if (fill_right > r.left + 0.5f) {
		fill_round (D2D1::RectF (r.left, r.top, fill_right, r.bottom),
		            radius, fill, 0.9f);
	}
	const float knob_w = dp (10.f);
	float knob_x = r.left + (r.right - r.left) * t;
	knob_x = std::max (r.left + knob_w * 0.5f,
	                   std::min (r.right - knob_w * 0.5f, knob_x));
	const D2D1_RECT_F knob = D2D1::RectF (knob_x - knob_w * 0.5f,
	                                      r.top - dp (2.f),
	                                      knob_x + knob_w * 0.5f,
	                                      r.bottom + dp (2.f));
	fill_round (knob, 3.f, hovered ? ui::ColorToken::AccentHover : fill);
	stroke_round (knob, 3.f, ui::ColorToken::AccentInk, 1.f, 0.55f);
}

void
UiDraw::meter (const D2D1_RECT_F& r, float peak01, float rms01, float hold01,
               bool clipped, bool draw_ticks)
{
	if (!_rt || r.right <= r.left || r.bottom <= r.top) {
		return;
	}
	fill_round (r, 2.f, ui::ColorToken::Control);
	stroke_round (r, 2.f, ui::ColorToken::Border, 1.f, 0.8f);
	const D2D1_RECT_F in = inset (r, dp (1.f));
	const float h = in.bottom - in.top;
	if (h <= 1.f) {
		return;
	}
	const float level = clamp01 (peak01);
	auto band = [&] (float lo, float hi, ui::ColorToken c) {
		const float a = std::max (lo, 0.f);
		const float b = std::min (hi, level);
		if (b <= a) {
			return;
		}
		D2D1_RECT_F q = in;
		q.bottom = in.bottom - h * a;
		q.top = in.bottom - h * b;
		fill_round (q, 1.5f, c, 0.96f);
	};
	band (0.f, 0.75f, ui::ColorToken::Peak);
	band (0.75f, 0.93f, ui::ColorToken::Solo);
	band (0.93f, 1.f, ui::ColorToken::Record);

	const float rms = clamp01 (rms01);
	if (rms > 0.01f) {
		const float pw = (in.right - in.left) * 0.42f;
		const float cx = (in.left + in.right) * 0.5f;
		const D2D1_RECT_F q = D2D1::RectF (cx - pw * 0.5f,
		                                  in.bottom - h * rms,
		                                  cx + pw * 0.5f, in.bottom);
		fill_round (q, 1.f, ui::ColorToken::Rms, 0.9f);
	}
	if (draw_ticks) {
		for (float t : { 0.25f, 0.5f, 0.75f, 0.9f }) {
			const float y = in.bottom - h * t;
			_rt->DrawLine (D2D1::Point2F (in.left, y),
			               D2D1::Point2F (in.right, y),
			               brush (ui::ColorToken::GridBeat), 1.f);
		}
	}
	const float hold = clamp01 (hold01);
	if (hold > 0.001f) {
		const float y = in.bottom - h * hold;
		_rt->DrawLine (D2D1::Point2F (in.left, y),
		               D2D1::Point2F (in.right, y),
		               brush (ui::ColorToken::MeterHold), dp (1.5f));
	}
	if (clipped) {
		fill_round (D2D1::RectF (in.left, in.top, in.right,
		                         in.top + dp (3.f)),
		            1.f, ui::ColorToken::Clip);
	}
}

void
UiDraw::modal_scrim (const D2D1_RECT_F& view, float opacity)
{
	fill_round (view, 0.f, ui::ColorToken::Shadow, opacity);
}

void
UiDraw::tooltip (const wchar_t* s, D2D1_POINT_2F cursor,
                 const D2D1_RECT_F& window)
{
	if (!_rt || !s || !*s) {
		return;
	}
	const size_t n = std::wcslen (s);
	const float w = dp (14.f) + dp (6.6f) * static_cast<float> (n);
	const float h = dp (26.f);
	float x = cursor.x + dp (12.f);
	float y = cursor.y + dp (18.f);
	if (x + w > window.right - dp (6.f)) {
		x = window.right - dp (6.f) - w;
	}
	if (y + h > window.bottom - dp (6.f)) {
		y = cursor.y - dp (18.f) - h;
	}
	x = std::max (window.left + dp (6.f), x);
	y = std::max (window.top + dp (6.f), y);
	const D2D1_RECT_F r = D2D1::RectF (x, y, x + w, y + h);
	shadow (r, 4.f);
	fill_round (r, 4.f, ui::ColorToken::Control, 0.98f);
	stroke_round (r, 4.f, ui::ColorToken::BorderStrong, 1.f);
	text_vcenter_n (s, static_cast<UINT32> (n), ui::TextRole::Caption,
	                ui::ColorToken::TextPrimary, inset (r, dp (7.f)),
	                DWRITE_TEXT_ALIGNMENT_LEADING);
}
