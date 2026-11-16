/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Immediate-mode File/Edit/Track/View/Plugins menu bar with dropdowns and a
 * shared context-menu popup. No Win32 resources: everything is Direct2D.
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
#include <d2d1helper.h>

#include <string>
#include <vector>

#include "ui_draw.h"

enum class MenuCommand : int {
	None = 0,

	FileNew,
	FileOpen,
	FileSave,
	FileSaveAs,
	FileImport,
	FileExportMix,
	FileExportStems,
	FileRecent,
	FileDemo,

	EditUndo,
	EditRedo,
	EditDelete,
	EditSplit,
	EditCrossfade,
	EditConsolidate,
	EditSnap,

	TrackAddAudio,
	TrackAddMidi,
	TrackRemove,
	TrackLeft,
	TrackRight,
	TrackRename,
	TrackMute,
	TrackSolo,
	TrackArm,
	TrackFx,
	TrackVst3Scan,
	TrackVst3Remove,
	PluginVst3Pick,

	ViewZoomSession,
	ViewFitRegions,
	ViewHelp,
	ViewPrefs,
	ViewAbout,

	PluginReverb,
	PluginComp,
	PluginEq,
	PluginDelay,
	PluginLimiter,
	PluginGate,
};

class MenuBar
{
public:
	MenuBar ();

	/** Bar bounds are owned by the shell; height is the bar's logical height. */
	void layout (const D2D1_RECT_F& bounds, float dpi_scale);

	void draw (UiDraw& ui) const;
	/** Dropdown/context layer; call after the rest of the window. */
	void draw_overlay (UiDraw& ui) const;

	/** Index of the menu title under the pointer, or -1. */
	int menu_at (float x, float y) const;
	/** Command under the pointer while a popup is open (None otherwise). */
	MenuCommand item_at (float x, float y) const;
	/**
	 * Row payload under the pointer, or -1.
	 *
	 * Read it before close(): closing drops the rows it is looked up from.
	 */
	int item_payload (float x, float y) const;

	void toggle_menu (int index);
	void close ();
	bool open () const { return _open != -1; }
	bool context_open () const { return _open == kContextMenu; }

	bool overlay_contains (float x, float y) const;

	void open_context (float x, float y, const MenuCommand* cmds, int count);

	/** One row of a caller-built context menu. */
	struct ContextItem {
		MenuCommand   cmd;
		std::wstring  label;   /* empty draws a separator */
		int           payload; /* handed back through item_payload (); -1 for none */
	};

	/** Context menu whose labels come from the caller — the scanned plugin names. */
	void open_context (float x, float y, const std::vector<ContextItem>& items);

	void set_hover (float x, float y);

	/** Bottom of the client area; a context popup is pushed up to stay inside. */
	void set_client_bottom (float y) { _client_bottom = y; }

	const D2D1_RECT_F& bounds () const { return _bounds; }
	bool menu_title_rect (int index, D2D1_RECT_F* out) const;

	static const wchar_t* command_label (MenuCommand cmd);

private:
	static constexpr int kContextMenu = -2;

	struct TitleSlot {
		D2D1_RECT_F rect;
		int        index;
	};

	struct PopupRow {
		D2D1_RECT_F   rect;
		MenuCommand   cmd;
		const wchar_t* label;
		bool          separator;
		int           payload;
	};

	void rebuild_popup ();
	float drop_width () const;
	float row_h () const;
	void clamp_popup ();

	D2D1_RECT_F  _bounds;
	float        _dpi_scale;
	int          _open;
	std::vector<TitleSlot> _titles;
	std::vector<PopupRow>  _rows;
	D2D1_RECT_F  _popup;
	float        _hover_x;
	float        _hover_y;
	float        _ctx_x;
	float        _ctx_y;
	float        _client_bottom;
	/**
	 * Rows of the open context menu. The dropdown's PopupRow labels point into
	 * these strings, so nothing may resize this vector while a popup is open.
	 */
	std::vector<ContextItem> _ctx_items;
};
