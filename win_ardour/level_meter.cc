/*
 * Copyright (C) 2026 Airdour contributors
 */

#include "level_meter.h"

#include <algorithm>

LevelMeter::LevelMeter ()
	: _bounds ()
	, _level (0.f)
	, _hold (0.f)
	, _hold_age (0.f)
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
}

void
LevelMeter::tick (float dt_sec)
{
	dt_sec = std::max (0.f, dt_sec);
	/* ~12 dB/s-ish visual fall. */
	float fall = 1.8f * dt_sec;
	_level = std::max (0.f, _level - fall);
	_hold_age += dt_sec;
	if (_hold_age > 0.8f) {
		_hold = std::max (_level, _hold - 0.6f * dt_sec);
	}
}

void
LevelMeter::draw (ID2D1RenderTarget* rt,
                  ID2D1SolidColorBrush* fill,
                  ID2D1SolidColorBrush* edge,
                  ID2D1SolidColorBrush* hot,
                  float dpi_scale) const
{
	if (!rt) {
		return;
	}

	rt->FillRectangle (_bounds, edge);
	rt->DrawRectangle (_bounds, edge, 1.f);

	float h = _bounds.bottom - _bounds.top;
	if (h < 2.f) {
		return;
	}

	float y_level = _bounds.bottom - _level * h;
	if (_level > 0.001f) {
		ID2D1SolidColorBrush* brush = (_level > 0.85f) ? hot : fill;
		float old_op = 1.f;
		(void)old_op;
		rt->FillRectangle (D2D1::RectF (_bounds.left + 1.f, y_level, _bounds.right - 1.f, _bounds.bottom), brush);
	}

	if (_hold > 0.01f) {
		float y_hold = _bounds.bottom - _hold * h;
		float thick = std::max (1.5f, 2.f * dpi_scale);
		rt->FillRectangle (D2D1::RectF (_bounds.left, y_hold - thick * 0.5f,
		                                _bounds.right, y_hold + thick * 0.5f),
		                   hot);
	}
}
