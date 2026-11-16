/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Immediate-mode menubar: top-level File/Edit/Track/View/Plugins titles with
 * dropdown rows, plus a reusable context-menu popup used by the editor lanes
 * and mixer strips.
 */

#include "menu_bar.h"

#include <algorithm>

namespace {

struct TitleDef {
	const wchar_t* title;
	float          width_dip;
};

const TitleDef kTitles[] = {
	{ L"File",    44.f },
	{ L"Edit",    42.f },
	{ L"Track",   50.f },
	{ L"View",    46.f },
	{ L"Plugins", 64.f },
	{ L"Help",    44.f },
};
constexpr int kTitleCount = (int)(sizeof (kTitles) / sizeof (kTitles[0]));

struct ItemDef {
	const wchar_t* label; /* nullptr → separator */
	MenuCommand    cmd;
};

const ItemDef kFileItems[] = {
	{ L"New Session",       MenuCommand::FileNew },
	{ L"Open Session…",     MenuCommand::FileOpen },
	{ L"Save Session",      MenuCommand::FileSave },
	{ L"Save Session As…",  MenuCommand::FileSaveAs },
	{ nullptr,              MenuCommand::None },
	{ L"Import Audio…",     MenuCommand::FileImport },
	{ L"Export Mixdown…",   MenuCommand::FileExportMix },
	{ L"Export Stems…",     MenuCommand::FileExportStems },
	{ nullptr,              MenuCommand::None },
	{ L"Recent Sessions",   MenuCommand::FileRecent },
};

const ItemDef kEditItems[] = {
	{ L"Undo",              MenuCommand::EditUndo },
	{ L"Redo",              MenuCommand::EditRedo },
	{ nullptr,              MenuCommand::None },
	{ L"Delete Selection",  MenuCommand::EditDelete },
	{ L"Split at Playhead", MenuCommand::EditSplit },
	{ L"Apply Crossfade",   MenuCommand::EditCrossfade },
	{ L"Consolidate",       MenuCommand::EditConsolidate },
	{ nullptr,              MenuCommand::None },
	{ L"Toggle Snap",       MenuCommand::EditSnap },
};

const ItemDef kTrackItems[] = {
	{ L"Add Audio Track",   MenuCommand::TrackAddAudio },
	{ L"Add MIDI Track",    MenuCommand::TrackAddMidi },
	{ nullptr,              MenuCommand::None },
	{ L"Remove Track",      MenuCommand::TrackRemove },
	{ L"Move Track Left",   MenuCommand::TrackLeft },
	{ L"Move Track Right",  MenuCommand::TrackRight },
	{ L"Rename Track…",     MenuCommand::TrackRename },
};

const ItemDef kViewItems[] = {
	{ L"Zoom to Session",   MenuCommand::ViewZoomSession },
	{ L"Fit All Regions",   MenuCommand::ViewFitRegions },
	{ nullptr,              MenuCommand::None },
	{ L"Keyboard Help",     MenuCommand::ViewHelp },
	{ L"Preferences",       MenuCommand::ViewPrefs },
	{ L"About Airdour",     MenuCommand::ViewAbout },
};

const ItemDef kPluginItems[] = {
	{ L"Reverb",            MenuCommand::PluginReverb },
	{ L"Compressor",        MenuCommand::PluginComp },
	{ L"EQ",                MenuCommand::PluginEq },
	{ L"Delay",             MenuCommand::PluginDelay },
	{ L"Limiter",           MenuCommand::PluginLimiter },
	{ L"Gate",              MenuCommand::PluginGate },
};

struct MenuDef {
	const ItemDef* items;
	int            count;
};

const MenuDef kMenus[] = {
	{ kFileItems,   (int)(sizeof (kFileItems) / sizeof (kFileItems[0])) },
	{ kEditItems,   (int)(sizeof (kEditItems) / sizeof (kEditItems[0])) },
	{ kTrackItems,  (int)(sizeof (kTrackItems) / sizeof (kTrackItems[0])) },
	{ kViewItems,   (int)(sizeof (kViewItems) / sizeof (kViewItems[0])) },
	{ kPluginItems, (int)(sizeof (kPluginItems) / sizeof (kPluginItems[0])) },
	{ nullptr, 0 }, /* Help: opens the keyboard overlay directly, no dropdown */
};

bool point_in (const D2D1_RECT_F& r, float x, float y)
{
	return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

} // namespace

MenuBar::MenuBar ()
	: _bounds (D2D1::RectF ())
	, _dpi_scale (1.f)
	, _open (-1)
	, _popup (D2D1::RectF ())
	, _hover_x (-1.f)
	, _hover_y (-1.f)
	, _ctx_x (0.f)
	, _ctx_y (0.f)
{
}

void
MenuBar::layout (const D2D1_RECT_F& bounds, float dpi_scale)
{
	_bounds = bounds;
	_dpi_scale = (dpi_scale > 0.f) ? dpi_scale : 1.f;
	_titles.clear ();

	float x = bounds.left + 14.f * _dpi_scale;
	for (int i = 0; i < kTitleCount; ++i) {
		const float w = kTitles[i].width_dip * _dpi_scale;
		_titles.push_back (TitleSlot { D2D1::RectF (x, bounds.top, x + w, bounds.bottom), i });
		x += w + 4.f * _dpi_scale;
	}

	if (_open == kContextMenu) {
		rebuild_popup ();
	} else if (_open >= 0) {
		rebuild_popup ();
	}
}

bool
MenuBar::menu_title_rect (int index, D2D1_RECT_F* out) const
{
	if (index < 0 || index >= (int)_titles.size ()) {
		return false;
	}
	if (out) {
		*out = _titles[(size_t)index].rect;
	}
	return true;
}

int
MenuBar::menu_at (float x, float y) const
{
	for (size_t i = 0; i < _titles.size (); ++i) {
		if (point_in (_titles[i].rect, x, y)) {
			return _titles[i].index;
		}
	}
	return -1;
}

float
MenuBar::drop_width () const
{
	return 234.f * _dpi_scale;
}

float
MenuBar::row_h () const
{
	return 26.f * _dpi_scale;
}

void
MenuBar::rebuild_popup ()
{
	_rows.clear ();
	const float sep_h = 7.f * _dpi_scale;
	const float w = drop_width ();

	float x0 = _bounds.left;
	float y0 = _bounds.bottom;
	if (_open == kContextMenu) {
		x0 = _ctx_x;
		y0 = _ctx_y;
	} else if (_open >= 0 && _open < (int)_titles.size ()) {
		x0 = _titles[(size_t)_open].rect.left;
	} else {
		return;
	}

	const int count = (_open == kContextMenu)
	                  ? (int)_ctx_cmds.size ()
	                  : ((_open >= 0 && _open < (int)(sizeof (kMenus) / sizeof (kMenus[0])))
	                     ? kMenus[_open].count : 0);

	float y = y0;
	for (int i = 0; i < count; ++i) {
		MenuCommand cmd;
		const wchar_t* label;
		if (_open == kContextMenu) {
			cmd = _ctx_cmds[(size_t)i];
			label = (cmd == MenuCommand::None) ? nullptr : command_label (cmd);
		} else {
			const ItemDef& it = kMenus[_open].items[i];
			cmd = it.cmd;
			label = it.label;
		}
		const bool sep = (label == nullptr);
		const float h = sep ? sep_h : row_h ();
		_rows.push_back (PopupRow { D2D1::RectF (x0, y, x0 + w, y + h), cmd, label, sep });
		y += h;
	}

	_popup = D2D1::RectF (x0, y0, x0 + w, y);
	clamp_popup ();
}

void
MenuBar::clamp_popup ()
{
	if (_titles.empty () && !context_open ()) {
		return;
	}
	const float win_r = _bounds.right;
	const float dx = (_popup.right > win_r) ? (win_r - _popup.right) : 0.f;
	if (dx < 0.f) {
		_popup.left += dx;
		_popup.right += dx;
		for (size_t i = 0; i < _rows.size (); ++i) {
			_rows[i].rect.left += dx;
			_rows[i].rect.right += dx;
		}
	}
}

void
MenuBar::toggle_menu (int index)
{
	if (index == 5) {
		/* Help title opens the overlay; ShellWindow handles the command. */
		_open = -1;
		_rows.clear ();
		return;
	}
	if (_open == index) {
		close ();
		return;
	}
	_open = index;
	rebuild_popup ();
}

void
MenuBar::close ()
{
	_open = -1;
	_rows.clear ();
	_ctx_cmds.clear ();
	_popup = D2D1::RectF ();
}

void
MenuBar::open_context (float x, float y, const MenuCommand* cmds, int count)
{
	_ctx_cmds.assign (cmds, cmds + (count > 0 ? count : 0));
	_ctx_x = x;
	_ctx_y = y;
	_open = kContextMenu;
	rebuild_popup ();
}

MenuCommand
MenuBar::item_at (float x, float y) const
{
	if (!open ()) {
		return MenuCommand::None;
	}
	for (size_t i = 0; i < _rows.size (); ++i) {
		if (!_rows[i].separator && point_in (_rows[i].rect, x, y)) {
			return _rows[i].cmd;
		}
	}
	return MenuCommand::None;
}

bool
MenuBar::overlay_contains (float x, float y) const
{
	if (!open ()) {
		return false;
	}
	return point_in (_popup, x, y);
}

void
MenuBar::set_hover (float x, float y)
{
	_hover_x = x;
	_hover_y = y;
}

void
MenuBar::draw (UiDraw& ui) const
{
	if (_bounds.right - _bounds.left < 2.f) {
		return;
	}
	ui.fill_round (_bounds, 0.f, ui::ColorToken::Surface, 0.5f);
	ui.divider (D2D1::RectF (_bounds.left, _bounds.bottom - ui.dp (1.f),
	                         _bounds.right, _bounds.bottom));

	for (size_t i = 0; i < _titles.size (); ++i) {
		const int idx = _titles[i].index;
		const D2D1_RECT_F& r = _titles[i].rect;
		const bool hot = (_open == idx)
		                 || (!context_open () && point_in (r, _hover_x, _hover_y));
		if (hot) {
			ui.fill_round (D2D1::RectF (r.left + ui.dp (2.f), r.top + ui.dp (3.f),
			                            r.right - ui.dp (2.f), r.bottom - ui.dp (3.f)),
			               ui.dp (4.f), ui::ColorToken::Control, _open == idx ? 0.95f : 0.7f);
		}
		ui.text_vcenter (kTitles[i].title, ui::TextRole::Caption,
		                 hot ? ui::ColorToken::TextPrimary : ui::ColorToken::TextSecondary,
		                 r, DWRITE_TEXT_ALIGNMENT_CENTER);
	}
}

void
MenuBar::draw_overlay (UiDraw& ui) const
{
	if (!open () || _rows.empty ()) {
		return;
	}

	ui.shadow (_popup, ui.dp (8.f));
	ui.panel (_popup, ui.dp (6.f), true);
	ui.stroke_round (_popup, ui.dp (6.f), ui::ColorToken::BorderStrong, 1.f, 0.9f);

	for (size_t i = 0; i < _rows.size (); ++i) {
		const PopupRow& row = _rows[i];
		if (row.separator) {
			const float y = (row.rect.top + row.rect.bottom) * 0.5f;
			ui.divider (D2D1::RectF (row.rect.left + ui.dp (10.f), y,
			                         row.rect.right - ui.dp (10.f), y));
			continue;
		}
		const bool hot = point_in (row.rect, _hover_x, _hover_y);
		if (hot) {
			ui.fill_round (D2D1::RectF (row.rect.left + ui.dp (4.f), row.rect.top + ui.dp (1.f),
			                            row.rect.right - ui.dp (4.f), row.rect.bottom - ui.dp (1.f)),
			               ui.dp (4.f), ui::ColorToken::Accent, 0.20f);
		}
		ui.text_vcenter (row.label, ui::TextRole::Body,
		                 hot ? ui::ColorToken::TextPrimary : ui::ColorToken::TextSecondary,
		                 D2D1::RectF (row.rect.left + ui.dp (14.f), row.rect.top,
		                              row.rect.right - ui.dp (12.f), row.rect.bottom));
	}
}

const wchar_t*
MenuBar::command_label (MenuCommand cmd)
{
	for (int m = 0; m < (int)(sizeof (kMenus) / sizeof (kMenus[0])); ++m) {
		for (int i = 0; i < kMenus[m].count; ++i) {
			if (kMenus[m].items[i].cmd == cmd && kMenus[m].items[i].label) {
				return kMenus[m].items[i].label;
			}
		}
	}
	/* Context-menu-only commands. */
	switch (cmd) {
	case MenuCommand::TrackMute: return L"Mute";
	case MenuCommand::TrackSolo: return L"Solo";
	case MenuCommand::TrackArm:  return L"Arm";
	case MenuCommand::TrackFx:   return L"Cycle FX bypass";
	default: break;
	}
	return L"?";
}
