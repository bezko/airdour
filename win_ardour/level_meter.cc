/*
 * Copyright (C) 2026 Airdour contributors
 */

#include "level_meter.h"

#include <algorithm>

namespace {
constexpr float kClipLinear = 0.999f; /* ≈ 0 dBFS after clamp */
constexpr float kHoldSecs   = 1.0f;
constexpr float kClipSecs   = 2.5f;
}

LevelMeter::LevelMeter ()
	: _bounds ()
	, _level (0.f)
	, _hold (0.f)
	, _hold_age (0.f)
	, _clipped (false)
	, _clip_age (0.f)
	, _led_h (0.f)
{
}

void
LevelMeter::layout (const D2D1_RECT_F& bounds)
{
	_bounds = bounds;
}

void
LevelMeter::set_peak (float peak)
{
	peak = std::max (0.f, std::min (1.f, peak));
	if (peak > _level) {
		_level = peak;
	}
	if (peak >= _hold) {
		_hold = peak;
		_hold_age = 0.f;
	}
	if (peak >= kClipLinear) {
		_clipped = true;
		_clip_age = 0.f;
	}
}

void
LevelMeter::set_clipped (bool clipped)
{
	if (clipped) {
		_clipped = true;
		_clip_age = 0.f;
	}
}

void
LevelMeter::clear_clip ()
{
	_clipped = false;
	_clip_age = 0.f;
}

void
LevelMeter::tick (float dt_sec)
{
	dt_sec = std::max (0.f, dt_sec);
	/* ~12 dB/s-ish visual fall. */
	float fall = 1.8f * dt_sec;
	_level = std::max (0.f, _level - fall);
	_hold_age += dt_sec;
	if (_hold_age > kHoldSecs) {
		_hold = std::max (_level, _hold - 0.55f * dt_sec);
	}
	if (_clipped) {
		_clip_age += dt_sec;
		if (_clip_age > kClipSecs) {
			_clipped = false;
			_clip_age = 0.f;
		}
	}
}

bool
LevelMeter::hit (float x, float y) const
{
	return x >= _bounds.left && x <= _bounds.right
	    && y >= _bounds.top && y <= _bounds.bottom;
}

void
LevelMeter::set_clip_led_height (float h_px)
{
	_led_h = std::max (0.f, h_px);
}

bool
LevelMeter::hit_clip_led (float x, float y) const
{
	float h = _bounds.bottom - _bounds.top;
	float led = (_led_h > 0.f) ? _led_h : std::max (3.f, h * 0.08f);
	return x >= _bounds.left && x <= _bounds.right
	    && y >= _bounds.top && y <= _bounds.top + led;
}

void
LevelMeter::draw (UiDraw& ui) const
{
	const D2D1_RECT_F& b = _bounds;
	ui.fill_round (b, 2.f, ui::ColorToken::Control);
	ui.stroke_round (b, 2.f, ui::ColorToken::Border, 1.f, 0.8f);

	float h = b.bottom - b.top - ui.dp (2.f);
	if (h < 2.f) {
		return;
	}
	const float x0 = b.left + ui.dp (1.f);
	const float x1 = b.right - ui.dp (1.f);
	const float bot = b.bottom - ui.dp (1.f);

	float y_level = bot - _level * h;
	if (_level > 0.001f) {
		ui.fill_round (D2D1::RectF (x0, y_level, x1, bot), 1.5f,
		               _level > 0.85f ? ui::ColorToken::MeterHot
		                              : ui::ColorToken::Peak,
		               0.95f);
	}

	/* Peak hold tick — brief latch, distinct from clip LED. */
	if (_hold > 0.01f) {
		float y_hold = bot - _hold * h;
		float thick = std::max (1.5f, 2.f * ui.dpi_scale ());
		ui.fill_round (D2D1::RectF (x0, y_hold - thick * 0.5f,
		                             x1, y_hold + thick * 0.5f),
		               1.f,
		               _hold >= kClipLinear ? ui::ColorToken::MeterHot
		                                    : ui::ColorToken::MeterHold,
		               0.9f);
	}

	/* Clip LED at meter top when ≥0 dBFS (or PeakMeter max-peak). */
	if (_clipped) {
		float led_h = (_led_h > 0.f) ? _led_h : std::max (3.f, 4.f * ui.dpi_scale ());
		ui.fill_round (D2D1::RectF (b.left, b.top, b.right, b.top + led_h),
		               2.f, ui::ColorToken::Clip);
	}
}
