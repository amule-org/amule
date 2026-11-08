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
#include <wx/textfile.h>

#include <algorithm>
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
};

void CollectLabels(wxWindow &window, std::vector<wxString> &labels)
{
	for (auto *child : window.GetChildren()) {
		if (!child) {
			throw std::runtime_error("Invalid child in GUI fixture");
		}
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

class CountingFileConfig : public wxFileConfig
{
public:
	using wxFileConfig::wxFileConfig;
	unsigned int flushCount = 0;

	bool Flush(bool currentOnly = false) override
	{
		++flushCount;
		return wxFileConfig::Flush(currentOnly);
	}
};

class SignatureFixture
{
private:
	wxMemoryInputStream m_empty;
	wxConfigBase *m_previous = nullptr;

public:
	CountingFileConfig prefs;
	const wxString directory;
	const wxString path;

	SignatureFixture()
	: m_empty("", 0)
	, prefs(m_empty)
	, directory(wxFileName::CreateTempFileName("wxcas-stats-"))
	, path(wxFileName(directory, WxCasCte::AMULESIG_FILENAME).GetFullPath())
	{
		wxRemoveFile(directory);
		Check(wxMkdir(directory), "Cannot create fixture directory");
		prefs.Write(WxCasCte::REFRESH_RATE_KEY, 3600L);
		prefs.Write(WxCasCte::FTP_UPDATE_RATE_KEY, 1440L);
		prefs.Write(WxCasCte::AMULESIG_PATH_KEY, directory);
		m_previous = wxConfigBase::Set(&prefs);
	}

	~SignatureFixture()
	{
		wxConfigBase::Set(m_previous);
		// Cleanup must not replace an assertion failure with a logging exit.
		wxLogNull quiet;
		for (const auto &file :
			{ path, wxFileName(directory, WxCasCte::AMULESIG_IMG_NAME, "png").GetFullPath() }) {
			if (wxFileExists(file) && !wxRemoveFile(file)) {
				std::cerr << "Could not remove fixture file\n";
			}
		}
		if (!wxRmdir(directory)) {
			std::cerr << "Could not remove fixture directory\n";
		}
	}
};

void WriteSignature(const wxString &path,
	int ed2k,
	int kad,
	int runtime,
	int rate,
	int queue,
	const wxString &user = "user",
	bool changedTotals = false)
{
	wxFile file(path, wxFile::write);
	Check(file.IsOpened(), "Cannot create signature fixture");
	const wxString contents = wxString::Format(
		"%d\nserver\n127.0.0.1\n4661\nH\n%d\n%d\n%d\n%d\n%d\n%s\n%d\n%d\ntest\n%d\n%d\n%d\n",
		ed2k,
		kad,
		rate,
		changedTotals ? 7 : 1,
		queue,
		changedTotals ? 84 : 42,
		user,
		changedTotals ? 4096 : 1024,
		changedTotals ? 8192 : 2048,
		changedTotals ? 3072 : 512,
		changedTotals ? 6144 : 256,
		runtime);
	Check(file.Write(contents), "Cannot write signature fixture");
}

void RequireLabel(StatsFrame &frame, const wxString &expected)
{
	std::vector<wxString> labels;
	CollectLabels(frame, labels);
	if (std::find(labels.begin(), labels.end(), expected) == labels.end()) {
		throw std::runtime_error("Missing label: " + expected.ToStdString());
	}
}

std::vector<wxButton *> ResetButtons(StatsFrame &frame)
{
	std::vector<wxButton *> result;
	for (auto *child : frame.GetChildren()) {
		for (auto *control : child->GetChildren()) {
			if (auto *button = dynamic_cast<wxButton *>(control)) {
				result.push_back(button);
			}
		}
	}
	Check(result.size() == 2, "Missing reset buttons");
	return result;
}

void PressReset(StatsFrame &frame, int index)
{
	auto *button = ResetButtons(frame)[index];
	wxCommandEvent event(wxEVT_BUTTON, button->GetId());
	event.SetEventObject(button);
	Check(button->GetEventHandler()->ProcessEvent(event), "Reset event was not handled");
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
		auto *panel = ResetButtons(frame).front()->GetParent();
		Check(panel && panel->GetSizer(), "Missing statistics panel");
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
		PressReset(frame, resetIndex);
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
	}
	prefs.Write(WxCasCte::ENABLE_AUTOSTATIMG_KEY, false);
}

wxString ExpectedConnection(int ed2k, int kad, const wxString &user)
{
	if (ed2k == 0 && kad == 0) {
		return user + " is not connected !";
	}
	const wxString kadText = kad == 2 ? "ok" : kad == 1 ? "firewalled" : "off";
	return user +
	       (ed2k == 0 ? " is connected to  Kad: "
			  : " is on server [127.0.0.1:4661] with HighID | Kad: ") +
	       kadText;
}

void TestRefreshStates(SignatureFixture &fixture)
{
	auto &prefs = fixture.prefs;
	const auto &path = fixture.path;
	for (int runtime : { 0, 3600 }) {
		for (int ed2k : { -1, 0, 1, 2, 3 }) {
			for (int kad : { -1, 0, 1, 2, 3 }) {
				std::cout << "runtime=" << runtime << " ed2k=" << ed2k << " kad=" << kad
					  << '\n';
				prefs.Write(WxCasCte::ABSOLUTE_MAX_DL_KEY, 0L);
				WriteSignature(path, 1, 2, 3600, 1, 101);
				StatsFrame frame;
				frame.UpdateAll();
				WriteSignature(path, ed2k, kad, runtime, 123, 987, "updated-user", true);
				frame.UpdateAll();

				const bool known =
					ed2k == 2 || ((ed2k == 0 || ed2k == 1) && kad >= 0 && kad <= 2);
				const bool refresh = runtime != 0 && known;
				const wxString expected = runtime == 0 ? "Oh Oh, aMule is not running..."
							  : !known     ? "Oh Oh, aMule status is unknown..."
							  : ed2k == 2  ? "aMule is connecting..."
							  : ed2k == 0 && kad == 0
								  ? "aMule is running, but disconnected"
								  : "aMule is running";
				Check(frame.GetStatusBar()->GetStatusText() == expected,
					"Wrong status message");
				RequireLabel(frame,
					refresh || runtime == 0
						? ExpectedConnection(ed2k, kad, "updated-user")
						: ExpectedConnection(1, 2, "user"));
				RequireLabel(frame,
					refresh ? "Total Download: 4.00 KiB, Upload: 8.00 KiB"
						: "Total Download: 1024 B, Upload: 2.00 KiB");
				RequireLabel(frame,
					refresh ? "Session Download: 3.00 KiB, Upload: 6.00 KiB"
						: "Session Download: 512 B, Upload: 256 B");
				RequireLabel(frame,
					refresh ? "Download: 123 KiB/s, Upload: 7 KiB/s"
						: "Download: 1 KiB/s, Upload: 1 KiB/s");
				RequireLabel(frame,
					refresh ? "Sharing: 84 file(s), Clients on queue: 987"
						: "Sharing: 42 file(s), Clients on queue: 101");
				Check(prefs.Read(WxCasCte::ABSOLUTE_MAX_DL_KEY, 0L) == 123L * 1024,
					"Record must persist in stopped and unknown states too");
				std::vector<wxString> labels;
				CollectLabels(frame, labels);
				Check(std::count_if(labels.begin(),
					      labels.end(),
					      [](const wxString &label) {
						      return label.StartsWith("123.00 KiB/s on ");
					      }) == 2,
					"Both record labels must update in every state");
				frame.UpdateAll();
				const wxSize unchanged = frame.GetClientSize();
				frame.UpdateAll();
				Check(frame.GetClientSize() == unchanged,
					"Unchanged refresh resized the frame");
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
}

void TestPixelSizing(SignatureFixture &fixture)
{
	WriteSignature(fixture.path, 1, 2, 3600, 1, 101);
	StatsFrame frame;
	frame.UpdateAll(true);
	const int originalWidth = frame.GetClientSize().x;
	const wxString suffix = ExpectedConnection(1, 2, "");
	wxString wide = "W";
	while (frame.GetTextExtent(wide + suffix).x < originalWidth) {
		wide += 'W';
	}
	const wxString narrow('i', wide.length());
	WriteSignature(fixture.path, 1, 2, 3600, 1, 101, narrow);
	frame.UpdateAll();
	WriteSignature(fixture.path, 1, 2, 3600, 1, 101, wide);
	frame.UpdateAll();
	// Check for clipping, not growth: a monospace UI font draws both names at
	// the same width, so the frame has no reason to grow.
	auto *panel = ResetButtons(frame).front()->GetParent();
	Check(panel->GetSize().x >= panel->GetSizer()->GetMinSize().x, "Equal-length wider text was clipped");

	// New records may be the longest labels in locales with verbose dates.
	WriteSignature(fixture.path, 0, 0, 3600, 999, 101, wide);
	frame.UpdateAll();
	const wxSize enlarged = frame.GetClientSize() + wxSize(40, 20);
	frame.SetClientSize(enlarged);
	frame.UpdateAll();
	frame.UpdateAll();
	Check(frame.GetClientSize() == enlarged, "Unchanged content undid a manual enlargement");
}

void TestStoppedResets(SignatureFixture &fixture)
{
	for (int index : { 0, 1 }) {
		fixture.prefs.Write(WxCasCte::ABSOLUTE_MAX_DL_KEY, 0L);
		WriteSignature(fixture.path, 1, 2, 3600, 100, 101);
		StatsFrame frame;
		WriteSignature(fixture.path, 0, 0, 0, 0, 101);
		PressReset(frame, index);
		std::vector<wxString> labels;
		CollectLabels(frame, labels);
		Check(std::count_if(labels.begin(),
			      labels.end(),
			      [](const wxString &label) { return label.StartsWith("0.00 KiB/s on "); }) == 1,
			"Stopped reset left a stale record label");
		Check(fixture.prefs.Read(WxCasCte::ABSOLUTE_MAX_DL_KEY, 0L) ==
				(index == 1 ? 0L : 100L * 1024),
			"Stopped reset persisted the wrong record");
		WriteSignature(fixture.path, 0, 0, 3600, 0, 101);
		frame.UpdateAll();
		labels.clear();
		CollectLabels(frame, labels);
		Check(std::count_if(labels.begin(),
			      labels.end(),
			      [](const wxString &label) { return label.StartsWith("0.00 KiB/s on "); }) == 1,
			"Restart at zero restored the stale record label");
	}
}

void TestTimerLifetime(SignatureFixture &fixture)
{
	WriteSignature(fixture.path, 1, 2, 3600, 1, 101);
	{
		StatsFrame frame;
		Check(frame.ChangeRefreshPeriod(1) && frame.ChangeFtpUpdatePeriod(1),
			"Could not start lifetime-test timers");
	}
	// Let the real GUI loop observe expired timers after their frame is gone.
	wxMilliSleep(5);
	wxYield();
}

void TestMissingSignature(SignatureFixture &fixture)
{
	wxRemoveFile(fixture.path);
	// Missing input is deliberately exercised; ignore wxFile's expected open error.
	wxLogNull quiet;
	OnLineSig signature(wxFileName(fixture.path));
	Check(signature.GetAmuleState() == 0 && signature.GetKadState() == 0 &&
			signature.GetUpStatus() == 0 && !signature.IsSessionMaxDlChanged() &&
			!signature.IsAbsoluteMaxDlChanged(),
		"Missing signature has undefined state");
	Check(signature.GetConvertedTotalDL() == "0 B" && signature.GetConvertedTotalUL() == "0 B",
		"Missing totals must convert to zero");
	signature.ResetSessionMaxDL();
	signature.ResetAbsoluteMaxDL();
	signature.Refresh();
	Check(signature.IsSessionMaxDlChanged() && signature.IsAbsoluteMaxDlChanged(),
		"Missing-file refresh lost the pending resets");
	signature.Refresh();
	Check(!signature.IsSessionMaxDlChanged() && !signature.IsAbsoluteMaxDlChanged(),
		"Missing-file refresh repeated the reset notification");
	fixture.prefs.Write(WxCasCte::ABSOLUTE_MAX_DL_KEY, 100L * 1024);
	StatsFrame frame;
	frame.UpdateAll();
	Check(frame.GetStatusBar()->GetStatusText() == "Oh Oh, aMule is not running...",
		"Missing signature must report not running");
	const auto beforeReset = fixture.prefs.flushCount;
	PressReset(frame, 1);
	Check(fixture.prefs.flushCount == beforeReset + 1,
		"Absolute reset must flush the record exactly once");
	frame.UpdateAll();
	frame.UpdateAll();
	Check(fixture.prefs.flushCount == beforeReset + 1,
		"Missing-file ticks flushed an unchanged record again");
	std::vector<wxString> labels;
	CollectLabels(frame, labels);
	Check(std::count_if(labels.begin(),
		      labels.end(),
		      [](const wxString &label) { return label.StartsWith("0.00 KiB/s on "); }) == 2,
		"Missing-file reset left a stale record label");
	WriteSignature(fixture.path, 1, 2, 3600, 7, 101);
	frame.UpdateAll();
	Check(fixture.prefs.Read(WxCasCte::ABSOLUTE_MAX_DL_KEY, 0L) == 7L * 1024 &&
			fixture.prefs.flushCount == beforeReset + 2,
		"New record was not saved once after the file reappeared");
	frame.UpdateAll();
	Check(fixture.prefs.flushCount == beforeReset + 2, "Unchanged recovered record was saved again");
}

void TestInvalidRates(SignatureFixture &fixture)
{
	for (const wxString &rate : { wxString(), wxString("not-a-number") }) {
		WriteSignature(fixture.path, 1, 2, 3600, 100, 101);
		OnLineSig signature(wxFileName(fixture.path));
		{
			wxTextFile file(fixture.path);
			Check(file.Open(), "Cannot open the invalid-rate fixture");
			if (rate.empty()) {
				// The stream skips blank lines. End at the rate field to
				// exercise an empty conversion rather than the next field.
				while (file.GetLineCount() > 6) {
					file.RemoveLine(6);
				}
				file.AddLine(rate);
			} else {
				file[6] = rate;
			}
			Check(file.Write(), "Cannot write the invalid-rate fixture");
		}
		signature.ResetSessionMaxDL();
		signature.ResetAbsoluteMaxDL();
		signature.Refresh();
		Check(signature.GetDLRate() == rate, "Fixture did not reach the invalid rate parser");
		Check(signature.GetSessionMaxDL() == 0.0 && signature.GetAbsoluteMaxDL() == 0.0,
			"Invalid rate became a download record");
		Check(signature.IsSessionMaxDlChanged() && signature.IsAbsoluteMaxDlChanged(),
			"Zero-rate parsing must preserve reset notifications");
		signature.Refresh();
		Check(!signature.IsSessionMaxDlChanged() && !signature.IsAbsoluteMaxDlChanged(),
			"Unchanged invalid rate repeated reset notifications");
		WriteSignature(fixture.path, 1, 2, 3600, 7, 101);
		signature.Refresh();
		Check(signature.GetSessionMaxDL() == 7.0 && signature.GetAbsoluteMaxDL() == 7.0 &&
				signature.IsSessionMaxDlChanged() && signature.IsAbsoluteMaxDlChanged(),
			"Valid rate did not recover after invalid input");
	}
}

} // namespace

int main(int argc, char **argv)
{
	wxApp::SetInstance(new WxCasTestApp);
	return RunGuiTest(
		argc,
		argv,
		"No GUI display available; skipping wxCas tests",
		[] {
			wxInitAllImageHandlers();
			SignatureFixture fixture;
			TestResetCallers(fixture.path, fixture.prefs);
			TestPixelSizing(fixture);
			TestStoppedResets(fixture);
			TestRefreshStates(fixture);
			TestTimerLifetime(fixture);
			TestMissingSignature(fixture);
			TestInvalidRates(fixture);
		},
		GuiTestEventLoop::Activate);
}
