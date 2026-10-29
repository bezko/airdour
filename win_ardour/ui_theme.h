/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Semantic UI theme: colors, metrics, type roles and icon glyphs.
 * All sizes are DIPs at 96 DPI; UiDraw applies the window DPI scale.
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdint>

#include <d2d1.h>

namespace ui {

enum class ColorToken : int {
	WindowTop,
	WindowBottom,
	Surface,
	SurfaceRaised,
	Card,
	Control,
	ControlHover,
	ControlPressed,
	Border,
	BorderStrong,
	Divider,
	TextPrimary,
	TextSecondary,
	TextMuted,
	TextDisabled,
	Accent,
	AccentHover,
	AccentPressed,
	AccentInk,
	Record,
	Solo,
	Mute,
	Loop,
	Punch,
	Marker,
	Selection,
	Playhead,
	Midi,
	Clip,
	Peak,
	Rms,
	MeterHot,
	MeterHold,
	GridBar,
	GridBeat,
	Shadow,
	Count
};

inline constexpr int kColorTokenCount = static_cast<int> (ColorToken::Count);

enum class TextRole : int {
	Brand,
	Title,
	Section,
	Body,
	Caption,
	Mono,
	Clock,
	Glyph,
	Count
};

inline constexpr int kTextRoleCount = static_cast<int> (TextRole::Count);

/* Segoe MDL2 Assets / Segoe Fluent Icons private-use codepoints.
 * Every codepoint below is verified present in Segoe MDL2 Assets on
 * Windows 10 22H2; DirectWrite font fallback covers Windows 11. */
namespace glyph {
inline constexpr wchar_t Play[]    = L"\uE768";
inline constexpr wchar_t Pause[]   = L"\uE769";
inline constexpr wchar_t Stop[]    = L"\uE71A";
inline constexpr wchar_t Record[]  = L"\uE7C8";
inline constexpr wchar_t Repeat[]  = L"\uE8EE";
inline constexpr wchar_t Flag[]    = L"\uE7C1";
inline constexpr wchar_t Cut[]     = L"\uE8C6";
inline constexpr wchar_t Save[]    = L"\uE74E";
inline constexpr wchar_t Open[]    = L"\uE8E5";
inline constexpr wchar_t Import[]  = L"\uE8B5";
inline constexpr wchar_t Share[]   = L"\uE72A";
inline constexpr wchar_t Settings[] = L"\uE713";
inline constexpr wchar_t Help[]    = L"\uE897";
inline constexpr wchar_t Info[]    = L"\uE946";
inline constexpr wchar_t Add[]     = L"\uE710";
inline constexpr wchar_t Remove[]  = L"\uE738";
inline constexpr wchar_t Delete[]  = L"\uE74D";
inline constexpr wchar_t Undo[]    = L"\uE7A7";
inline constexpr wchar_t Redo[]    = L"\uE7A6";
inline constexpr wchar_t Up[]      = L"\uE74A";
inline constexpr wchar_t Down[]    = L"\uE74B";
inline constexpr wchar_t ChevronDown[] = L"\uE70D";
inline constexpr wchar_t ChevronUp[]   = L"\uE70E";
inline constexpr wchar_t More[]    = L"\uE712";
inline constexpr wchar_t Close[]   = L"\uE711";
inline constexpr wchar_t Check[]   = L"\uE73E";
inline constexpr wchar_t ZoomFit[] = L"\uE8A3";
inline constexpr wchar_t ZoomOut[] = L"\uE71F";
inline constexpr wchar_t Warning[] = L"\uE7BA";
inline constexpr wchar_t Volume[]  = L"\uE767";
inline constexpr wchar_t Clock[]   = L"\uE823";
inline constexpr wchar_t Previous[] = L"\uE892";
inline constexpr wchar_t Next[]    = L"\uE893";
} // namespace glyph

inline D2D1_COLOR_F
rgb (uint32_t v, float a = 1.f)
{
	return D2D1_COLOR_F {
		((v >> 16) & 0xffu) / 255.f,
		((v >> 8) & 0xffu) / 255.f,
		(v & 0xffu) / 255.f,
		a
	};
}

struct ThemeColors {
	D2D1_COLOR_F v[kColorTokenCount];
};

struct ThemeMetrics {
	float radius_sm;
	float radius_md;
	float radius_lg;
	float space_1;
	float space_2;
	float space_3;
	float space_4;
	float space_5;
	float space_6;
	float stroke_hair;
	float stroke;
	float stroke_focus;
	float brand_h;
	float transport_h;
	float status_h;
};

struct ThemeType {
	float brand;
	float title;
	float section;
	float body;
	float caption;
	float clock;
	float mono;
	float glyph;
};

struct Theme {
	ThemeColors colors;
	ThemeMetrics metrics;
	ThemeType type;
	const wchar_t* font_ui;
	const wchar_t* font_ui_fallback;
	const wchar_t* font_display;
	const wchar_t* font_display_fallback;
	const wchar_t* font_mono;
	const wchar_t* font_mono_fallback;
	const wchar_t* font_glyph;
	const wchar_t* font_glyph_fallback;
};

inline Theme
make_theme ()
{
	Theme t {};
	D2D1_COLOR_F* c = t.colors.v;

	c[static_cast<int> (ColorToken::WindowTop)]     = rgb (0x0a0d12);
	c[static_cast<int> (ColorToken::WindowBottom)]  = rgb (0x121821);
	c[static_cast<int> (ColorToken::Surface)]       = rgb (0x11161d);
	c[static_cast<int> (ColorToken::SurfaceRaised)] = rgb (0x18202a);
	c[static_cast<int> (ColorToken::Card)]          = rgb (0x151c25);
	c[static_cast<int> (ColorToken::Control)]       = rgb (0x1b2430);
	c[static_cast<int> (ColorToken::ControlHover)]  = rgb (0x243040);
	c[static_cast<int> (ColorToken::ControlPressed)]= rgb (0x2d3a4c);
	c[static_cast<int> (ColorToken::Border)]        = rgb (0x27323f);
	c[static_cast<int> (ColorToken::BorderStrong)]  = rgb (0x36445a);
	c[static_cast<int> (ColorToken::Divider)]       = rgb (0x1d2733);
	c[static_cast<int> (ColorToken::TextPrimary)]   = rgb (0xedf3f8);
	c[static_cast<int> (ColorToken::TextSecondary)] = rgb (0xa7b8c5);
	c[static_cast<int> (ColorToken::TextMuted)]     = rgb (0x6f808e);
	c[static_cast<int> (ColorToken::TextDisabled)]  = rgb (0x4b5966);
	c[static_cast<int> (ColorToken::Accent)]        = rgb (0x25d9bc);
	c[static_cast<int> (ColorToken::AccentHover)]   = rgb (0x43e7cf);
	c[static_cast<int> (ColorToken::AccentPressed)] = rgb (0x18b79f);
	c[static_cast<int> (ColorToken::AccentInk)]     = rgb (0x06120f);
	c[static_cast<int> (ColorToken::Record)]        = rgb (0xf2554b);
	c[static_cast<int> (ColorToken::Solo)]          = rgb (0xf5b44a);
	c[static_cast<int> (ColorToken::Mute)]          = rgb (0xe4574f);
	c[static_cast<int> (ColorToken::Loop)]          = rgb (0xf0a93b);
	c[static_cast<int> (ColorToken::Punch)]         = rgb (0xe8574a);
	c[static_cast<int> (ColorToken::Marker)]        = rgb (0xd6e36b);
	c[static_cast<int> (ColorToken::Selection)]     = rgb (0x3fc3e8);
	c[static_cast<int> (ColorToken::Playhead)]      = rgb (0xffd24a);
	c[static_cast<int> (ColorToken::Midi)]          = rgb (0x46c2e8);
	c[static_cast<int> (ColorToken::Clip)]          = rgb (0xff5d45);
	c[static_cast<int> (ColorToken::Peak)]          = rgb (0x35e6c8);
	c[static_cast<int> (ColorToken::Rms)]           = rgb (0x1c7a6e);
	c[static_cast<int> (ColorToken::MeterHot)]      = rgb (0xf26b3a);
	c[static_cast<int> (ColorToken::MeterHold)]     = rgb (0xf4f8fa);
	c[static_cast<int> (ColorToken::GridBar)]       = rgb (0x9fb3c4);
	c[static_cast<int> (ColorToken::GridBeat)]      = rgb (0x8095a8);
	c[static_cast<int> (ColorToken::Shadow)]        = rgb (0x000000);

	t.metrics.radius_sm = 3.f;
	t.metrics.radius_md = 6.f;
	t.metrics.radius_lg = 10.f;
	t.metrics.space_1 = 2.f;
	t.metrics.space_2 = 4.f;
	t.metrics.space_3 = 8.f;
	t.metrics.space_4 = 12.f;
	t.metrics.space_5 = 16.f;
	t.metrics.space_6 = 24.f;
	t.metrics.stroke_hair = 1.f;
	t.metrics.stroke = 1.25f;
	t.metrics.stroke_focus = 2.f;
	t.metrics.brand_h = 56.f;
	t.metrics.transport_h = 88.f;
	t.metrics.status_h = 32.f;

	t.type.brand = 28.f;
	t.type.title = 20.f;
	t.type.section = 13.f;
	t.type.body = 12.5f;
	t.type.caption = 10.5f;
	t.type.clock = 16.f;
	t.type.mono = 11.f;
	t.type.glyph = 17.f;

	t.font_ui = L"Segoe UI Variable Text";
	t.font_ui_fallback = L"Segoe UI";
	t.font_display = L"Segoe UI Variable Display";
	t.font_display_fallback = L"Segoe UI";
	t.font_mono = L"Cascadia Mono";
	t.font_mono_fallback = L"Consolas";
	t.font_glyph = L"Segoe Fluent Icons";
	t.font_glyph_fallback = L"Segoe MDL2 Assets";

	return t;
}

inline const Theme&
theme ()
{
	static const Theme t = make_theme ();
	return t;
}

} // namespace ui
