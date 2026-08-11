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

#include <algorithm>
#include <cmath>
#include <memory>
#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/geometry.h> // Needed for wxPoint2DDouble
#if wxUSE_GRAPHICS_CONTEXT
#include <wx/graphics.h>
#endif

#include <common/Format.h>

#include "amule.h"          // Needed for theApp
#include "OScopeCtrl.h"     // Interface declarations
#include "OtherFunctions.h" // Needed for CastSecondsToHM
#include "Statistics.h"

wxBEGIN_EVENT_TABLE(COScopeCtrl, wxControl)
	EVT_PAINT(COScopeCtrl::OnPaint)
	EVT_SIZE(COScopeCtrl::OnSize)
	EVT_MOTION(COScopeCtrl::OnMouseMove)
	EVT_LEAVE_WINDOW(COScopeCtrl::OnMouseLeave)
wxEND_EVENT_TABLE()

namespace
{
// CStatistics::GetHistory always writes three arrays, whatever the caller
// asked to plot, so the buffers handed to it are never shorter than this.
const unsigned kHistoryTrends = 3;

// Trend whose area is shaded. GetHistory puts the instantaneous value in
// the third array, which is the spiky curve the shading reads well under;
// the other two are averages that would shade as near-solid blocks.
const unsigned kShadedTrend = 2;

// Alpha of the shading where it meets its curve. It fades to nothing at
// the bottom of the plot.
const unsigned char kShadeAlpha = 0x48;

// Ink for text drawn over the graph background, which the user chooses.
wxColour ContrastInk(const wxColour &bg)
{
	const double luma = 0.299 * bg.Red() + 0.587 * bg.Green() + 0.114 * bg.Blue();
	return luma < 128.0 ? wxColour(0xF0, 0xF0, 0xF0) : wxColour(0x20, 0x20, 0x20);
}

// G.Hayduk: the first 15 trends have predefined colours, further ones are
// drawn in white unless SetPlotColor is called. In practice every graph in
// the GUI overrides all of its colours from the preferences during Init().
const wxColour crPreset[16] = { wxColour(0xFF, 0x00, 0x00),
	wxColour(0xFF, 0xC0, 0xC0),
	wxColour(0xFF, 0xFF, 0x00),
	wxColour(0xFF, 0xA0, 0x00),
	wxColour(0xA0, 0x60, 0x00),
	wxColour(0x00, 0xFF, 0x00),
	wxColour(0x00, 0xA0, 0x00),
	wxColour(0x00, 0x00, 0xFF),
	wxColour(0x00, 0xA0, 0xFF),
	wxColour(0x00, 0xFF, 0xFF),
	wxColour(0x00, 0xA0, 0xA0),
	wxColour(0xC0, 0xC0, 0xFF),
	wxColour(0xFF, 0x00, 0xFF),
	wxColour(0xA0, 0x00, 0xA0),
	wxColour(0xFF, 0xFF, 0xFF),
	wxColour(0x80, 0x80, 0x80) };
} // namespace

COScopeCtrl::COScopeCtrl(int cntTrends, int nDecimals, StatsGraphType type, wxWindow *parent)
: wxControl(parent, -1, wxDefaultPosition, wxDefaultSize)
, graph_type(type)
, m_trends(std::max<unsigned>((unsigned)cntTrends, 1))
, m_nYGrids(5)
, m_nShiftPixels(1)
, m_nYDecimals((unsigned)nDecimals)
, m_bgColour(0, 0, 0)       // see also SetBackgroundColor
, m_gridColour(0, 255, 255) // see also SetGridColor
, m_bStopped(false)
, m_sLastTimestamp(0.0)
, m_sLastPeriod(1.0)
, m_ptHover(wxDefaultPosition)
{
	for (unsigned i = 0; i < m_trends.size(); ++i) {
		m_trends[i].crPlot = (i < 15 ? crPreset[i] : *wxWHITE);
		m_trends[i].fLowerLimit = m_trends[i].fUpperLimit = 0.0;
	}

	SetCursor(wxCursor(wxCURSOR_CROSS));

	m_strYUnits = "Y"; // can also be set with SetYUnits

	// Everything inside the client area is painted by OnPaint.
	SetBackgroundStyle(wxBG_STYLE_PAINT);
}

void COScopeCtrl::SetRange(float fLower, float fUpper, unsigned iTrend)
{
	if (iTrend >= m_trends.size()) {
		return;
	}
	PlotData_t &trend = m_trends[iTrend];
	if (trend.fLowerLimit == fLower && trend.fUpperLimit == fUpper) {
		return;
	}
	trend.fLowerLimit = fLower;
	trend.fUpperLimit = fUpper;
	Refresh(false);
}

void COScopeCtrl::SetRanges(float fLower, float fUpper)
{
	for (unsigned iTrend = 0; iTrend < m_trends.size(); ++iTrend) {
		SetRange(fLower, fUpper, iTrend);
	}
}

void COScopeCtrl::SetYUnits(const wxString &strUnits, const wxString &strMin, const wxString &strMax)
{
	m_strYUnits = strUnits;
	m_strYMin = strMin;
	m_strYMax = strMax;
	Refresh(false);
}

void COScopeCtrl::SetGridColor(const wxColour &cr)
{
	if (cr == m_gridColour) {
		return;
	}
	m_gridColour = cr;
	Refresh(false);
}

void COScopeCtrl::SetPlotColor(const wxColour &cr, unsigned iTrend)
{
	if (iTrend >= m_trends.size() || m_trends[iTrend].crPlot == cr) {
		return;
	}
	m_trends[iTrend].crPlot = cr;
	Refresh(false);
}

void COScopeCtrl::SetTrendLabel(const wxString &label, unsigned iTrend)
{
	if (iTrend >= m_trends.size()) {
		return;
	}
	m_trends[iTrend].strLabel = label;
}

void COScopeCtrl::SetBackgroundColor(const wxColour &cr)
{
	if (cr == m_bgColour) {
		return;
	}
	m_bgColour = cr;
	Refresh(false);
}

void COScopeCtrl::AppendPoints(double sTimestamp)
{
	// The sample itself is already in CStatistics' history by the time we
	// get here, and the next paint reads it back from there, so there is
	// nothing to accumulate -- just ask for that paint.
	m_sLastTimestamp = sTimestamp;
	Refresh(false);
}

void COScopeCtrl::Reset(double sNewPeriod)
{
	const bool bWasStopped = m_bStopped;
	m_bStopped = false;
	if (m_sLastPeriod != sNewPeriod || bWasStopped) {
		m_sLastPeriod = sNewPeriod;
		Refresh(false);
	}
}

void COScopeCtrl::Stop()
{
	m_bStopped = true;
	Refresh(false);
}

wxString COScopeCtrl::GetYMaxLabel() const
{
	if (!m_strYMax.IsEmpty()) {
		return m_strYMax;
	}
	return wxString::Format("%.*f", (int)m_nYDecimals, m_trends[0].fUpperLimit);
}

wxString COScopeCtrl::GetYMinLabel() const
{
	if (!m_strYMin.IsEmpty()) {
		return m_strYMin;
	}
	return wxString::Format("%.*f", (int)m_nYDecimals, m_trends[0].fLowerLimit);
}

wxString COScopeCtrl::GetXUnitsLabel(const wxRect &rectPlot) const
{
	// floor(x + 0.5) rounds the period to whole seconds.
	const uint32 sSpan =
		(uint32)(rectPlot.GetWidth() / m_nShiftPixels) * (uint32)std::floor(m_sLastPeriod + 0.5);
	const wxString strSpan = CastSecondsToHM(sSpan);

	if (m_bStopped) {
		return CFormat(_("Disabled [%s]")) % strSpan;
	}
	return strSpan;
}

wxRect COScopeCtrl::ComputePlotRect(wxDC &dc) const
{
	const wxRect rectClient = GetClientRect();
	const int pad = FromDIP(4);
	const int lineHeight = dc.GetCharHeight();

	// The left gutter has to hold whichever of the three y axis labels is
	// widest. The old code assumed 6 pixels per character and reserved room
	// for seven of them, which was too much at small values and too little
	// once the axis went into five digits.
	int gutter = std::max(dc.GetTextExtent(GetYMaxLabel()).x, dc.GetTextExtent(GetYMinLabel()).x);
	if (!m_strYUnits.IsEmpty()) {
		gutter = std::max(gutter, dc.GetTextExtent(m_strYUnits).x);
	}

	wxRect rectPlot;
	rectPlot.x = rectClient.GetLeft() + gutter + 2 * pad;
	rectPlot.y = rectClient.GetTop() + lineHeight / 2 + pad;
	rectPlot.SetRight(rectClient.GetRight() - 2 * pad);
	rectPlot.SetBottom(rectClient.GetBottom() - lineHeight - 2 * pad);
	return rectPlot;
}

void COScopeCtrl::DrawGrid(wxDC &dc, const wxRect &rectPlot)
{
	const int pad = FromDIP(4);

	// Frame, just outside the plot area so it never overlaps a curve.
	wxRect rectFrame = rectPlot;
	rectFrame.Inflate(1, 1);
	dc.SetPen(wxPen(m_gridColour, 1));
	dc.SetBrush(*wxTRANSPARENT_BRUSH);
	dc.DrawRectangle(rectFrame);

	// Horizontal rules. Kept on the plain wxDC rather than the graphics
	// context on purpose: these are axis-aligned hairlines and stay crisp
	// only if they are not anti-aliased.
	dc.SetPen(wxPen(m_gridColour, 1, wxPENSTYLE_LONG_DASH));
	for (unsigned j = 1; j <= m_nYGrids; ++j) {
		const int y = rectPlot.GetTop() + (int)((unsigned)rectPlot.GetHeight() * j / (m_nYGrids + 1));
		dc.DrawLine(rectPlot.GetLeft(), y, rectPlot.GetRight(), y);
	}

	dc.SetTextForeground(m_gridColour);

	wxString label = GetYMaxLabel();
	wxSize extent = dc.GetTextExtent(label);
	dc.DrawText(label, rectPlot.GetLeft() - pad - extent.x, rectPlot.GetTop() - extent.y / 2);

	label = GetYMinLabel();
	extent = dc.GetTextExtent(label);
	dc.DrawText(label, rectPlot.GetLeft() - pad - extent.x, rectPlot.GetBottom() - extent.y / 2);

	if (!m_strYUnits.IsEmpty()) {
		extent = dc.GetTextExtent(m_strYUnits);
		dc.DrawText(m_strYUnits,
			rectPlot.GetLeft() - pad - extent.x,
			(rectPlot.GetTop() + rectPlot.GetBottom() - extent.y) / 2);
	}

	label = GetXUnitsLabel(rectPlot);
	extent = dc.GetTextExtent(label);
	dc.DrawText(
		label, (rectPlot.GetLeft() + rectPlot.GetRight() - extent.x) / 2, rectPlot.GetBottom() + pad);
}

void COScopeCtrl::DrawCurves(wxDC &dc, wxGraphicsContext *gc, const wxRect &rectPlot)
{
	// A paint can reach us before the application has finished starting up.
	if (theApp == nullptr || theApp->m_statistics == nullptr) {
		return;
	}

	const unsigned cntPoints = (unsigned)rectPlot.GetWidth() / m_nShiftPixels + 1;
	const unsigned cntTrends = std::max<unsigned>((unsigned)m_trends.size(), kHistoryTrends);

	std::vector<std::vector<float>> samples(cntTrends, std::vector<float>(cntPoints, 0.0f));
	std::vector<float *> apf(cntTrends);
	for (unsigned i = 0; i < cntTrends; ++i) {
		apf[i] = samples[i].data();
	}

	// A stopped graph keeps showing the window that ended with the last
	// sample it saw instead of sliding towards the present.
	const double sFinal = m_bStopped ? m_sLastTimestamp : -1.0;
	const unsigned cntFilled =
		theApp->m_statistics->GetHistory(cntPoints, m_sLastPeriod, sFinal, apf, graph_type);
	if (cntFilled < 2) {
		return;
	}

	// GetHistory returns the samples newest first, so index 0 is the point
	// at the right hand edge and the curve is walked leftwards from there.
	const double lineWidth = FromDIP(3) / 2.0;
	auto xAt = [&](unsigned i) { return rectPlot.GetRight() - (double)i * m_nShiftPixels; };

#if wxUSE_GRAPHICS_CONTEXT
	if (gc) {
		gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
		gc->Clip(rectPlot.x, rectPlot.y, rectPlot.GetWidth(), rectPlot.GetHeight());

		// Shading first, so every curve stays legible on top of it.
		if (IsShaded()) {
			wxGraphicsPath area = gc->CreatePath();
			area.MoveToPoint(xAt(0), rectPlot.GetBottom());
			for (unsigned i = 0; i < cntFilled; ++i) {
				area.AddLineToPoint(xAt(i),
					GetPlotY(samples[kShadedTrend][i], m_trends[kShadedTrend], rectPlot));
			}
			area.AddLineToPoint(xAt(cntFilled - 1), rectPlot.GetBottom());
			area.CloseSubpath();

			// Anchor the gradient to the curve's own peak, not to the top
			// of the plot. Anchoring it to the plot would tie the shading's
			// opacity to how much of the y range the graph happens to be
			// using: the Kad graph grows its range to fit, so its curve
			// rides near the top and shades fine, but a transfer rate well
			// under the configured maximum sits low in the plot, where a
			// plot-anchored gradient has already faded to nothing.
			double yPeak = rectPlot.GetBottom();
			for (unsigned i = 0; i < cntFilled; ++i) {
				yPeak = std::min(yPeak,
					GetPlotY(samples[kShadedTrend][i], m_trends[kShadedTrend], rectPlot));
			}

			const wxColour &cr = m_trends[kShadedTrend].crPlot;
			if (yPeak < rectPlot.GetBottom() - 1.0) {
				gc->SetBrush(gc->CreateLinearGradientBrush(0.0,
					yPeak,
					0.0,
					rectPlot.GetBottom(),
					wxColour(cr.Red(), cr.Green(), cr.Blue(), kShadeAlpha),
					wxColour(cr.Red(), cr.Green(), cr.Blue(), wxALPHA_TRANSPARENT)));
			} else {
				// Curve flat on the baseline: nothing to grade over.
				gc->SetBrush(wxBrush(wxColour(cr.Red(), cr.Green(), cr.Blue(), kShadeAlpha)));
			}
			gc->SetPen(*wxTRANSPARENT_PEN);
			gc->FillPath(area);
		}

		for (unsigned iTrend = 0; iTrend < m_trends.size(); ++iTrend) {
			wxGraphicsPath path = gc->CreatePath();
			path.MoveToPoint(xAt(0), GetPlotY(samples[iTrend][0], m_trends[iTrend], rectPlot));
			for (unsigned i = 1; i < cntFilled; ++i) {
				path.AddLineToPoint(
					xAt(i), GetPlotY(samples[iTrend][i], m_trends[iTrend], rectPlot));
			}

			gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(m_trends[iTrend].crPlot, lineWidth)
							 .Join(wxJOIN_ROUND)
							 .Cap(wxCAP_ROUND)));
			gc->StrokePath(path);
		}

		gc->ResetClip();
		DrawHover(gc, rectPlot, samples, cntFilled);
		return;
	}
#else
	(void)gc;
#endif

	// Fallback for a wxWidgets built without a graphics context: aliased
	// hairlines, i.e. what the graphs looked like before this renderer.
	dc.SetClippingRegion(rectPlot);
	std::vector<wxPoint> points(cntFilled);
	for (unsigned iTrend = 0; iTrend < m_trends.size(); ++iTrend) {
		for (unsigned i = 0; i < cntFilled; ++i) {
			points[i] = wxPoint(
				(int)xAt(i), (int)GetPlotY(samples[iTrend][i], m_trends[iTrend], rectPlot));
		}
		dc.SetPen(wxPen(m_trends[iTrend].crPlot, 1));
		dc.DrawLines((int)cntFilled, points.data());
	}
	dc.DestroyClippingRegion();
}

bool COScopeCtrl::IsShaded() const
{
	if (kShadedTrend >= m_trends.size()) {
		return false;
	}

	// On the rate graphs and the Kad graph the three trends are three
	// views of one quantity -- its current value, its running average and
	// its session average -- so shading the current one states that
	// quantity's magnitude. The connections graph instead plots three
	// unrelated counts (active uploads, connections, active downloads);
	// there is no primary among them to shade, and singling one out would
	// only be invisible today because the counts sit low against the
	// axis range.
	return graph_type != GRAPH_CONN;
}

double COScopeCtrl::GetPlotY(float value, const PlotData_t &trend, const wxRect &rectPlot) const
{
	// Half a pen width of headroom keeps a curve pinned to the top or the
	// bottom of the range from painting over the frame.
	const double halfPen = FromDIP(3) / 4.0;
	const double yTop = rectPlot.GetTop() + halfPen;
	const double yBottom = rectPlot.GetBottom() - halfPen;

	const float fSpan = trend.fUpperLimit - trend.fLowerLimit;
	if (fSpan <= 0.0f) {
		return yBottom;
	}

	const double fraction = (value - trend.fLowerLimit) / fSpan;
	return std::min(std::max(rectPlot.GetBottom() - fraction * rectPlot.GetHeight(), yTop), yBottom);
}

void COScopeCtrl::OnPaint(wxPaintEvent &WXUNUSED(evt))
{
	wxAutoBufferedPaintDC dc(this);

	dc.SetBackground(wxBrush(m_bgColour));
	dc.Clear();

	// Axis labels are conventionally a step below the UI font.
	dc.SetFont(GetFont().Smaller());

	const wxRect rectPlot = ComputePlotRect(dc);
	if (rectPlot.GetWidth() < 2 || rectPlot.GetHeight() < 2) {
		return;
	}

	// Everything drawn straight onto the wxDC has to be done before the
	// graphics context is created from it: the two share one surface and
	// only the most recently created of them may write to it.
	DrawGrid(dc, rectPlot);

#if wxUSE_GRAPHICS_CONTEXT
	// Created here rather than inside DrawCurves because wxGraphicsContext
	// has no overload taking the wxDC base class, and this is the last
	// place that still knows which kind of paint DC we are holding.
	std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
	DrawCurves(dc, gc.get(), rectPlot);
#else
	DrawCurves(dc, nullptr, rectPlot);
#endif
}

void COScopeCtrl::DrawHover(wxGraphicsContext *gc,
	const wxRect &rectPlot,
	const std::vector<std::vector<float>> &samples,
	unsigned cntFilled)
{
#if wxUSE_GRAPHICS_CONTEXT
	if (m_ptHover == wxDefaultPosition || !rectPlot.Contains(m_ptHover)) {
		return;
	}

	// Snap to the sample under the cursor. One sample is m_nShiftPixels
	// wide, so this is the pixel the cursor is on in all current graphs.
	unsigned iSample = (unsigned)((rectPlot.GetRight() - m_ptHover.x + (int)m_nShiftPixels / 2) /
				      (int)m_nShiftPixels);
	if (iSample >= cntFilled) {
		return;
	}

	const wxColour ink = ContrastInk(m_bgColour);
	const double x = rectPlot.GetRight() - (double)iSample * m_nShiftPixels;
	const double markerRadius = FromDIP(4) / 2.0 + 1.0;

	// Crosshair.
	gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(wxColour(ink.Red(), ink.Green(), ink.Blue(), 0x60), 1.0)));
	wxGraphicsPath crosshair = gc->CreatePath();
	crosshair.MoveToPoint(x, rectPlot.GetTop());
	crosshair.AddLineToPoint(x, rectPlot.GetBottom());
	gc->StrokePath(crosshair);

	// One marker per trend, ringed in the background colour so markers
	// that land on top of each other stay separable.
	for (unsigned iTrend = 0; iTrend < m_trends.size(); ++iTrend) {
		const double y = GetPlotY(samples[iTrend][iSample], m_trends[iTrend], rectPlot);
		gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(m_bgColour, 2.0)));
		gc->SetBrush(wxBrush(m_trends[iTrend].crPlot));
		gc->DrawEllipse(x - markerRadius, y - markerRadius, markerRadius * 2, markerRadius * 2);
	}

	// Readout. The header is how far back in time this sample is, in the
	// same units the x axis label under the graph uses. Deliberately not
	// the word "Current" for the newest sample: one of the trends is
	// already called that.
	const uint32 sAgo = (uint32)iSample * (uint32)std::floor(m_sLastPeriod + 0.5);
	const wxString strHeader = CFormat("-%s") % CastSecondsToHM(sAgo);

	std::vector<wxString> aLabel, aValue;
	for (unsigned iTrend = 0; iTrend < m_trends.size(); ++iTrend) {
		aLabel.push_back(m_trends[iTrend].strLabel);
		wxString strValue = wxString::Format("%.*f", (int)m_nYDecimals, samples[iTrend][iSample]);
		if (!m_strYUnits.IsEmpty()) {
			strValue << " " << m_strYUnits;
		}
		aValue.push_back(strValue);
	}

	const wxFont font = GetFont();
	gc->SetFont(font, ink);

	double wLabel = 0.0, wValue = 0.0, lineHeight = 0.0, w = 0.0, h = 0.0, d = 0.0, e = 0.0;
	gc->GetTextExtent(strHeader, &w, &h, &d, &e);
	lineHeight = h;
	double wHeader = w;
	for (unsigned i = 0; i < aLabel.size(); ++i) {
		gc->GetTextExtent(aLabel[i], &w, &h, &d, &e);
		wLabel = std::max(wLabel, w);
		lineHeight = std::max(lineHeight, h);
		gc->GetTextExtent(aValue[i], &w, &h, &d, &e);
		wValue = std::max(wValue, w);
	}

	const double pad = FromDIP(6);
	const double gap = FromDIP(12);
	const double dot = FromDIP(6);
	const double boxW = std::max(wHeader, dot + pad / 2 + wLabel + gap + wValue) + 2 * pad;
	const double cntLines = (double)(aLabel.size() + 1);
	const double boxH = cntLines * lineHeight + cntLines * 2.0 + 2 * pad;

	// Prefer the right of the crosshair, flip when that would overflow.
	double boxX = x + gap;
	if (boxX + boxW > rectPlot.GetRight()) {
		boxX = x - gap - boxW;
	}
	boxX = std::max(boxX, (double)rectPlot.GetLeft());
	double boxY = std::min(
		std::max(m_ptHover.y - boxH / 2, (double)rectPlot.GetTop()), rectPlot.GetBottom() - boxH);

	gc->SetBrush(wxBrush(wxColour(m_bgColour.Red(), m_bgColour.Green(), m_bgColour.Blue(), 0xE6)));
	gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(wxColour(ink.Red(), ink.Green(), ink.Blue(), 0x50), 1.0)));
	gc->DrawRoundedRectangle(boxX, boxY, boxW, boxH, FromDIP(4));

	double textY = boxY + pad;
	gc->DrawText(strHeader, boxX + pad, textY);
	textY += lineHeight + 2.0;

	for (unsigned i = 0; i < aLabel.size(); ++i) {
		gc->SetBrush(wxBrush(m_trends[i].crPlot));
		gc->SetPen(*wxTRANSPARENT_PEN);
		gc->DrawEllipse(boxX + pad, textY + (lineHeight - dot) / 2, dot, dot);

		gc->DrawText(aLabel[i], boxX + pad + dot + pad / 2, textY);
		gc->GetTextExtent(aValue[i], &w, &h, &d, &e);
		gc->DrawText(aValue[i], boxX + boxW - pad - w, textY);
		textY += lineHeight + 2.0;
	}
#else
	(void)gc;
	(void)rectPlot;
	(void)samples;
	(void)cntFilled;
#endif
}

void COScopeCtrl::OnMouseMove(wxMouseEvent &evt)
{
	const wxPoint pt = evt.GetPosition();
	if (pt != m_ptHover) {
		m_ptHover = pt;
		Refresh(false);
	}
	evt.Skip();
}

void COScopeCtrl::OnMouseLeave(wxMouseEvent &evt)
{
	if (m_ptHover != wxDefaultPosition) {
		m_ptHover = wxDefaultPosition;
		Refresh(false);
	}
	evt.Skip();
}

void COScopeCtrl::OnSize(wxSizeEvent &WXUNUSED(evt))
{
	// The plot is laid out from the client size on every paint, so a resize
	// only has to invalidate the whole control -- a partial repaint would
	// leave the old axis labels behind.
	Refresh(false);
}

// File_checked_for_headers
