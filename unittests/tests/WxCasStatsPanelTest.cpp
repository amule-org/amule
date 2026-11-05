//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
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

#include "GuiTestDiagnostics.h"

#include "wxcas.h"
#include "wxcascte.h"

#include <wx/file.h>
#include <wx/fileconf.h>
#include <wx/mstream.h>
#include <wx/statusbr.h>

#include <iostream>
#include <memory>
#include <vector>

class WxCasTestApp : public WxCas
{
public:
	bool OnInit() override
	{
		wxSizerFlags::DisableConsistencyChecks();
		return true;
	}
};

namespace
{
class StatsFrame : public WxCasFrame
{
public:
	StatsFrame()
	: WxCasFrame("Statistics test")
	{
	}
	using WxCasFrame::OnRefreshTimer;
	using WxCasFrame::UpdateAll;
	using WxCasFrame::UpdateStatsPanel;
};

void CollectLabels(wxWindow &window, std::vector<wxString> &labels)
{
	for (auto *child : window.GetChildren()) {
		if (auto *text = dynamic_cast<wxStaticText *>(child)) {
			labels.push_back(text->GetLabel());
		}
		CollectLabels(*child, labels);
	}
}

wxString SharingLine(StatsFrame &frame)
{
	std::vector<wxString> labels;
	CollectLabels(frame, labels);
	for (const auto &label : labels) {
		if (label.StartsWith("Sharing: ")) {
			return label;
		}
	}
	throw std::runtime_error("Missing sharing label");
}

void WriteSignature(const wxString &path,
	int ed2k,
	int kad,
	int runtime,
	int rate,
	int queue,
	const wxString &user = "user")
{
	wxFile file(path, wxFile::write);
	Check(file.IsOpened(), "Cannot create signature fixture");
	Check(file.Write(wxString::Format("%d\nserver\n127.0.0.1\n4661\nH\n%d\n%d\n1\n%"
					  "d\n42\n%s\n1024\n2048\ntest\n512\n256\n%d\n",
		      ed2k,
		      kad,
		      rate,
		      queue,
		      user,
		      runtime)),
		"Cannot write signature fixture");
}

void TestResetCallers(const wxString &path, wxFileConfig &prefs)
{
	const wxString imagePath =
		wxFileName(wxFileName(path).GetPath(), WxCasCte::AMULESIG_IMG_NAME, "png").GetFullPath();
	prefs.Write(WxCasCte::ENABLE_AUTOSTATIMG_KEY, true);
	prefs.Write(WxCasCte::AUTOSTATIMG_DIR_KEY, wxFileName(path).GetPath());
	prefs.Write(WxCasCte::AUTOSTATIMG_TYPE_KEY, "PNG");
	prefs.Write(WxCasCte::ENABLE_FTP_UPDATE_KEY, false);
	for (int resetIndex : { 0, 1 }) {
		prefs.Write(WxCasCte::ABSOLUTE_MAX_DL_KEY, 0L);
		WriteSignature(path, 1, 2, 3600, 100, 101);
		StatsFrame frame;
		frame.UpdateAll(true);
		const wxSize before = frame.GetClientSize();
		wxWindow *panel = nullptr;
		std::vector<wxButton *> buttons;
		for (auto *child : frame.GetChildren()) {
			if (dynamic_cast<wxPanel *>(child)) {
				panel = child;
				for (auto *control : child->GetChildren()) {
					if (auto *button = dynamic_cast<wxButton *>(control)) {
						buttons.push_back(button);
					}
				}
			}
		}
		Check(panel && buttons.size() == 2, "Missing statistics panel or reset buttons");
		// Reset also reloads the signature. Its new labels must be fitted immediately,
		// since the refresh consumes the width change before the next timer tick.
		// Grow just past the existing width: a fixed long name can exceed the
		// Windows CI desktop, where native frame sizing clamps to the work area.
		std::vector<wxString> initialLabels;
		CollectLabels(frame, initialLabels);
		wxString suffix;
		for (const auto &label : initialLabels) {
			if (label.StartsWith("user is on ")) {
				suffix = label.Mid(4);
			}
		}
		Check(!suffix.empty(), "Missing connection label");
		wxString longUser = "user";
		while (frame.GetTextExtent(longUser + suffix).x < before.x) {
			longUser += 'W';
		}
		WriteSignature(path, 1, 2, 3600, 5, 987, longUser);
		wxCommandEvent reset(wxEVT_BUTTON, buttons[resetIndex]->GetId());
		reset.SetEventObject(buttons[resetIndex]);
		Check(buttons[resetIndex]->GetEventHandler()->ProcessEvent(reset),
			"Reset event was not handled");
		Check(frame.GetClientSize().x > before.x, "Reset caller discarded the required frame resize");
		std::cout << "Reset " << resetIndex << ": initial width=" << before.x
			  << " frame width=" << frame.GetClientSize().x
			  << " panel width=" << panel->GetSize().x
			  << " required width=" << panel->GetSizer()->GetMinSize().x << '\n';
		Check(panel->GetSize().x >= panel->GetSizer()->GetMinSize().x,
			"Refreshed statistics are clipped after reset");
		Check(prefs.Read(WxCasCte::ABSOLUTE_MAX_DL_KEY, 0L) ==
				(resetIndex == 0 ? 100L * 1024 : 5L * 1024),
			"Reset changed the wrong persisted record");
		std::vector<wxString> labels;
		CollectLabels(frame, labels);
		int resetRecords = 0;
		int retainedRecords = 0;
		for (const auto &label : labels) {
			resetRecords += label.StartsWith("5.00 KiB/s on ");
			retainedRecords += label.StartsWith("100.00 KiB/s on ");
		}
		Check(resetRecords == 1 && retainedRecords == 1,
			"Reset must preserve the other download record");

		WriteSignature(path, 1, 2, 3600, 2, 101);
		wxTimer timerSource(&frame);
		wxTimerEvent timer(timerSource);
		frame.OnRefreshTimer(timer);
		Check(frame.GetClientSize().x == before.x, "Timer caller did not restore compact layout");
		const wxSize unchanged = frame.GetClientSize();
		frame.OnRefreshTimer(timer);
		Check(frame.GetClientSize() == unchanged, "Unchanged timer refresh resized the frame");
		std::unique_ptr<wxImage> image(frame.GetStatImage());
		Check(image->IsOk(), "Refreshed statistics cannot be exported");
		wxImage exported(imagePath);
		Check(exported.IsOk() && exported.GetSize() == image->GetSize(),
			"Timer did not export a valid statistics image");
		wxRemoveFile(imagePath);
	}
	prefs.Write(WxCasCte::ENABLE_AUTOSTATIMG_KEY, false);
}

void TestRefreshStates()
{
	// Stream-backed preferences never read or write the user's wxCas settings.
	wxMemoryInputStream empty("", 0);
	wxFileConfig prefs(empty);
	const wxString directory = wxFileName::CreateTempFileName("wxcas-stats-");
	wxRemoveFile(directory);
	Check(wxMkdir(directory), "Cannot create fixture directory");
	const wxString path = wxFileName(directory, WxCasCte::AMULESIG_FILENAME).GetFullPath();
	auto *previous = wxConfigBase::Set(&prefs);
	try {
		prefs.Write(WxCasCte::REFRESH_RATE_KEY, 3600L);
		prefs.Write(WxCasCte::FTP_UPDATE_RATE_KEY, 1440L);
		prefs.Write(WxCasCte::AMULESIG_PATH_KEY, directory);
		TestResetCallers(path, prefs);
		for (int runtime : { 0, 3600 }) {
			for (int ed2k : { -1, 0, 1, 2, 3 }) {
				for (int kad : { -1, 0, 1, 2, 3 }) {
					std::cout << "runtime=" << runtime << " ed2k=" << ed2k
						  << " kad=" << kad << '\n';
					prefs.Write(WxCasCte::ABSOLUTE_MAX_DL_KEY, 0L);
					WriteSignature(path, 1, 2, 3600, 1, 101);
					// Constructor needs its default signature path before
					// SetAmuleSigFile.
					wxFileName fixture(path);
					prefs.Write(WxCasCte::AMULESIG_PATH_KEY, fixture.GetPath());
					StatsFrame frame;
					frame.SetAmuleSigFile(fixture);
					frame.UpdateStatsPanel();
					const wxString oldSharing = SharingLine(frame);
					WriteSignature(path, ed2k, kad, runtime, 123, 987);
					frame.UpdateStatsPanel();

					const bool known = ed2k == 2 ||
							   ((ed2k == 0 || ed2k == 1) && kad >= 0 && kad <= 2);
					const bool refresh = runtime != 0 && known;
					const wxString expected =
						runtime == 0            ? "Oh Oh, aMule is not running..."
						: !known                ? "Oh Oh, aMule status is unknown..."
						: ed2k == 2             ? "aMule is connecting..."
						: ed2k == 0 && kad == 0 ? "aMule is running, but disconnected"
									: "aMule is running";
					Check(frame.GetStatusBar()->GetStatusText() == expected,
						"Wrong status message");
					Check(SharingLine(frame) == (refresh ? "Sharing: 42 file(s), Clients "
									       "on queue: 987"
									     : oldSharing),
						"Wrong running/stopped/unknown refresh behavior");
					Check(prefs.Read(WxCasCte::ABSOLUTE_MAX_DL_KEY, 0L) ==
							(refresh ? 123L * 1024 : 1024L),
						"Wrong persisted download record");
					std::vector<wxString> labels;
					CollectLabels(frame, labels);
					int records = 0;
					for (const auto &label : labels) {
						if (label.StartsWith(refresh ? "123.00 KiB/s on "
									     : "1.00 KiB/s on ")) {
							++records;
						}
					}
					Check(records == 2,
						"Both session and absolute record labels must be preserved");
					Check(!frame.UpdateStatsPanel(),
						"Unchanged refresh must not request a resize");
					Check(!frame.IsFrozen(), "Refresh left frame frozen");
					WriteSignature(path, 1, 2, 3600, 1, 202);
					wxTimer timerSource(&frame);
					wxTimerEvent timer(timerSource);
					frame.OnRefreshTimer(timer);
					Check(frame.GetStatusBar()->GetStatusText() == "aMule is running" &&
							SharingLine(frame) ==
								"Sharing: 42 file(s), Clients on queue: 202",
						"Timer failed to recover from the previous connection state");
				}
			}
		}
	} catch (...) {
		const wxString imagePath =
			wxFileName(directory, WxCasCte::AMULESIG_IMG_NAME, "png").GetFullPath();
		if (wxFileExists(imagePath)) {
			wxRemoveFile(imagePath);
		}
		wxConfigBase::Set(previous);
		wxRemoveFile(path);
		wxRmdir(directory);
		throw;
	}
	wxConfigBase::Set(previous);
	wxRemoveFile(path);
	wxRmdir(directory);
}
} // namespace

int main(int argc, char **argv)
{
	wxApp::SetInstance(new WxCasTestApp);
	return RunGuiTest(argc, argv, "No GUI display available; skipping wxCas tests", [] {
		wxInitAllImageHandlers();
		TestRefreshStates();
	});
}
