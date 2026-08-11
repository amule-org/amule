//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2002-2011 Merkur ( devs@emule-project.net / http://www.emule-project.net )
//
// Any parts of this program derived from the xMule, lMule or eMule project,
// or contributed by third-party developers are copyrighted by their
// respective authors.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA
//

#ifndef OSCOPECTRL_H
#define OSCOPECTRL_H

#include <vector>
#include <wx/control.h> // Needed for wxControl
#include <wx/colour.h>

#include "Constants.h" // Needed for StatsGraphType

class wxDC;
// Only ever held as a pointer here, so this stays valid in a wxWidgets
// built without wxUSE_GRAPHICS_CONTEXT, where the class does not exist.
class wxGraphicsContext;

/////////////////////////////////////////////////////////////////////////////
// COScopeCtrl window
//
// A scrolling line graph for the Statistics and Networks->Kad panels. All
// four graphs in the GUI are instances of this control, so anything changed
// here applies to every one of them.
//
// The control keeps no copy of the plotted samples: every paint asks
// CStatistics::GetHistory for as many points as the plot area is wide and
// redraws the whole curve. That is what lets the curves be drawn through a
// wxGraphicsContext (anti-aliased, resolution independent) -- the previous
// incremental design scrolled a cached bitmap sideways and appended one
// segment per sample, which cannot carry anti-aliased content without
// smearing it, and pinned the graph to the logical, non-HiDPI resolution.

class COScopeCtrl : public wxControl
{
public:
	COScopeCtrl(int NTrends, int nDecimals, StatsGraphType type, wxWindow *parent = nullptr);

	void SetRange(float dLower, float dUpper, unsigned iTrend = 0);
	void SetRanges(float dLower, float dUpper);
	void SetYUnits(const wxString &string, const wxString &YMin = "", const wxString &YMax = "");
	void SetBackgroundColor(const wxColour &color);
	void SetGridColor(const wxColour &color);
	void SetPlotColor(const wxColour &color, unsigned iTrend = 0);
	// Name shown for this trend in the hover tooltip, i.e. the same text
	// the legend beside the graph carries.
	void SetTrendLabel(const wxString &label, unsigned iTrend = 0);
	float GetUpperLimit() const { return m_trends[0].fUpperLimit; }

	// Freeze the graph at the last sample (statistics update delay set to 0).
	void Stop();
	// Resume, and adopt a new seconds-per-sample period.
	void Reset(double sNewPeriod);
	// A new sample has been recorded: it is already in the history, so this
	// only has to ask for a repaint.
	void AppendPoints(double sTimestamp);
	// Repaint after something that changes the shape of the curve without
	// adding a sample, e.g. a new running-average window.
	void InvalidateGraph() { Refresh(false); }

	StatsGraphType graph_type;

private:
	struct PlotData_t
	{
		wxColour crPlot;
		wxString strLabel;
		float fLowerLimit;
		float fUpperLimit;
	};

	void OnPaint(wxPaintEvent &evt);
	void OnSize(wxSizeEvent &evt);
	void OnMouseMove(wxMouseEvent &evt);
	void OnMouseLeave(wxMouseEvent &evt);

	// Splits the client area into the plot rectangle and the label gutters,
	// measuring the axis labels rather than assuming a character width.
	wxRect ComputePlotRect(wxDC &dc) const;
	void DrawGrid(wxDC &dc, const wxRect &rectPlot);
	// gc is created by the caller, which is where the concrete paint DC
	// type is still known; it is null if this wxWidgets has no graphics
	// context, and the curves then fall back to plain wxDC polylines.
	void DrawCurves(wxDC &dc, wxGraphicsContext *gc, const wxRect &rectPlot);
	// Crosshair, per-trend markers and the value readout at the hovered
	// sample. samples is what DrawCurves has just plotted.
	void DrawHover(wxGraphicsContext *gc,
		const wxRect &rectPlot,
		const std::vector<std::vector<float>> &samples,
		unsigned cntFilled);
	// Formats the y axis labels; empty strings fall back to the trend range.
	wxString GetYMaxLabel() const;
	wxString GetYMinLabel() const;
	wxString GetXUnitsLabel(const wxRect &rectPlot) const;
	// y for one sample, clamped into the plot area with room for the pen.
	double GetPlotY(float value, const PlotData_t &trend, const wxRect &rectPlot) const;
	// Whether this graph shades the area under its instantaneous trend.
	bool IsShaded() const;

	std::vector<PlotData_t> m_trends;
	unsigned m_nYGrids;
	unsigned m_nShiftPixels; // horizontal distance between two samples
	unsigned m_nYDecimals;

	wxString m_strYUnits, m_strYMin, m_strYMax;
	wxColour m_bgColour;
	wxColour m_gridColour;

	bool m_bStopped;
	double m_sLastTimestamp;
	double m_sLastPeriod;

	// Cursor position while it is over the control, wxDefaultPosition
	// otherwise. Drives the hover readout.
	wxPoint m_ptHover;

	wxDECLARE_EVENT_TABLE();
};

#endif // OSCOPECTRL_H
// File_checked_for_headers
