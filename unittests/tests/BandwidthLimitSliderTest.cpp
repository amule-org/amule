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

#include <wx/wx.h>

#include "BandwidthLimitSlider.h"

#include <iostream>
#include <stdexcept>
#include <tuple>

class BandwidthTestApp : public wxApp
{
public:
	bool OnInit() override { return true; }
};

wxIMPLEMENT_APP_NO_MAIN(BandwidthTestApp);

namespace
{
void TestLimitControl()
{
	wxDialog dialog(nullptr, wxID_ANY, "Bandwidth limit test");
	const auto makeControl = [&dialog](int capacityId,
					 int limitId,
					 int enabledId,
					 int sliderId,
					 int capacityValue,
					 int limitValue) {
		auto *capacity = new wxSpinCtrl(&dialog,
			capacityId,
			wxEmptyString,
			wxDefaultPosition,
			wxDefaultSize,
			0,
			3,
			1000000,
			capacityValue);
		auto *limit = new wxSpinCtrl(&dialog,
			limitId,
			wxEmptyString,
			wxDefaultPosition,
			wxDefaultSize,
			0,
			0,
			1000000,
			limitValue);
		auto *enabled = new wxCheckBox(&dialog, enabledId, "Enable limit");
		auto *equivalent = new wxStaticText(&dialog, wxID_ANY, wxEmptyString);
		auto *capacityEquivalent = new wxStaticText(&dialog, wxID_ANY, wxEmptyString);
		auto *slider = new BandwidthLimitSlider(
			&dialog, sliderId, limit, capacity, enabled, equivalent, capacityEquivalent);
		return std::make_tuple(capacity, limit, enabled, equivalent, capacityEquivalent, slider);
	};

	auto [downloadCapacity,
		downloadLimit,
		downloadEnabled,
		downloadEquivalent,
		downloadCapacityEquivalent,
		downloadSlider] = makeControl(IDC_DOWNLOAD_CAP,
		IDC_MAXDOWN,
		IDC_DOWNLOAD_LIMIT_ENABLED,
		IDC_DOWNLOAD_LIMIT_SLIDER,
		51200,
		51200);
	Check(downloadSlider->GetMin() == 1 && downloadSlider->GetMax() == 51200,
		"Slider must cover 1 KiB/s through the full line capacity");
	Check(downloadSlider->GetValue() == 51200 && downloadEnabled->GetValue(),
		"Loaded limit must enable and position the slider");
	Check(downloadEquivalent->GetLabel().Contains("50.00 MiB/s"), "MiB/s conversion is wrong");
	Check(downloadEquivalent->GetLabel().StartsWith("KiB/s ("),
		"Limit equivalent must not repeat the numeric field");
	Check(downloadCapacityEquivalent->GetLabel().Contains("50.00 MiB/s"),
		"Loaded capacity equivalent is wrong");
	Check(downloadCapacityEquivalent->GetLabel().StartsWith("KiB/s ("),
		"Capacity equivalent must not repeat the numeric field");
	int notifiedValue = -1;
	dialog.Bind(
		wxEVT_SPINCTRL,
		[&notifiedValue](wxSpinEvent &event) { notifiedValue = event.GetPosition(); },
		IDC_MAXDOWN);
	wxCommandEvent previewLimit(wxEVT_TEXT, downloadLimit->GetId());
	previewLimit.SetEventObject(downloadLimit);
	previewLimit.SetString("2048");
	downloadLimit->GetEventHandler()->ProcessEvent(previewLimit);
	Check(downloadLimit->GetValue() == 51200 && downloadSlider->GetValue() == 2048 &&
			downloadEquivalent->GetLabel().Contains("2.00 MiB/s") && notifiedValue == -1,
		"Typing a limit must preview it without committing or notifying the ratio handler");
	downloadSlider->Sync();
	wxCommandEvent previewCapacity(wxEVT_TEXT, downloadCapacity->GetId());
	previewCapacity.SetEventObject(downloadCapacity);
	previewCapacity.SetString("102400");
	downloadCapacity->GetEventHandler()->ProcessEvent(previewCapacity);
	Check(downloadCapacity->GetValue() == 51200 && downloadSlider->GetMax() == 102400 &&
			downloadCapacityEquivalent->GetLabel().Contains("100.00 MiB/s"),
		"Typing capacity must preview its slider range and equivalent without committing");
	previewCapacity.SetString("2000000");
	downloadCapacity->GetEventHandler()->ProcessEvent(previewCapacity);
	Check(downloadCapacity->GetValue() == 51200 && downloadSlider->GetMax() == 1000000 &&
			downloadCapacityEquivalent->GetLabel().Contains("976.56 MiB/s"),
		"Typed capacity preview must clamp to the control maximum");
	downloadSlider->Sync();

	downloadSlider->SetValue(40000);
	wxCommandEvent slide(wxEVT_SLIDER, downloadSlider->GetId());
	slide.SetEventObject(downloadSlider);
	downloadSlider->GetEventHandler()->ProcessEvent(slide);
	Check(downloadLimit->GetValue() == 40000 && downloadSlider->GetValue() == 40000,
		"Slider change did not reach the saved spin control");
	Check(notifiedValue == 40000, "Slider change did not reach the ratio event handler");

	downloadEnabled->SetValue(false);
	wxCommandEvent disable(wxEVT_CHECKBOX, downloadEnabled->GetId());
	disable.SetInt(0);
	disable.SetEventObject(downloadEnabled);
	downloadEnabled->GetEventHandler()->ProcessEvent(disable);
	Check(downloadLimit->GetValue() == 0 && !downloadSlider->IsEnabled() &&
			downloadEquivalent->GetLabel() == _("Unlimited"),
		"Disabled limit must save zero and show unlimited");
	Check(downloadSlider->GetMin() == 1, "Unlimited must not become the slider minimum");

	downloadEnabled->SetValue(true);
	wxCommandEvent enable(wxEVT_CHECKBOX, downloadEnabled->GetId());
	enable.SetInt(1);
	enable.SetEventObject(downloadEnabled);
	downloadEnabled->GetEventHandler()->ProcessEvent(enable);
	Check(downloadLimit->GetValue() == 40000 && downloadSlider->IsEnabled(),
		"Re-enabling must restore the previous limit");

	downloadCapacity->SetValue(81920);
	wxCommandEvent capacityChanged(wxEVT_TEXT, downloadCapacity->GetId());
	capacityChanged.SetEventObject(downloadCapacity);
	capacityChanged.SetString("81920");
	downloadCapacity->GetEventHandler()->ProcessEvent(capacityChanged);
	Check(downloadSlider->GetMax() == 81920 && downloadSlider->GetValue() == 40000,
		"Changing capacity must update the range without moving the selected limit");
	Check(downloadCapacityEquivalent->GetLabel().Contains("80.00 MiB/s"),
		"Changing capacity must update its MiB/s equivalent");

	downloadLimit->SetValue(1000000);
	wxCommandEvent limitTyped(wxEVT_TEXT, downloadLimit->GetId());
	limitTyped.SetEventObject(downloadLimit);
	limitTyped.SetString("1000000");
	downloadLimit->GetEventHandler()->ProcessEvent(limitTyped);
	Check(downloadSlider->GetMax() == 81920 && downloadSlider->GetValue() == 81920,
		"A saved limit above capacity must not expand the slider range");
	Check(downloadEquivalent->GetLabel().Contains("976.56 MiB/s"),
		"The label must retain the exact saved limit above capacity");
	Check(downloadEquivalent->GetMinSize().x >=
			downloadEquivalent->GetTextExtent(downloadEquivalent->GetLabel()).x,
		"The maximum value must fit the reserved label width");

	downloadEnabled->SetValue(false);
	downloadEnabled->GetEventHandler()->ProcessEvent(disable);
	downloadEnabled->SetValue(true);
	downloadEnabled->GetEventHandler()->ProcessEvent(enable);
	Check(downloadLimit->GetValue() == 1000000 && downloadSlider->GetValue() == 81920,
		"Toggling a limit above capacity must restore the exact saved value");
	downloadLimit->SetValue(0);
	InitializeBandwidthLimitSliders(&dialog);
	downloadEnabled->SetValue(true);
	downloadEnabled->GetEventHandler()->ProcessEvent(enable);
	Check(downloadLimit->GetValue() == 81920 && downloadSlider->GetValue() == 81920,
		"First download enable after loading unlimited must start at line capacity");

	auto [uploadCapacity,
		uploadLimit,
		uploadEnabled,
		uploadEquivalent,
		uploadCapacityEquivalent,
		uploadSlider] = makeControl(IDC_UPLOAD_CAP,
		IDC_MAXUP,
		IDC_UPLOAD_LIMIT_ENABLED,
		IDC_UPLOAD_LIMIT_SLIDER,
		2500,
		10);
	// The widget starts with its built-in default, then preferences load unlimited.
	uploadLimit->SetValue(0);
	InitializeBandwidthLimitSliders(&dialog);
	Check(uploadLimit->GetValue() == 0 && !uploadEnabled->GetValue() && !uploadSlider->IsEnabled() &&
			uploadSlider->GetMin() == 1,
		"Loaded zero must keep the upload limit disabled");
	Check(uploadCapacityEquivalent->GetLabel().Contains("2.44 MiB/s"),
		"Loaded upload capacity equivalent is wrong");
	Check(uploadEquivalent->GetMinSize().x >= uploadEquivalent->GetTextExtent(_("Unlimited")).x,
		"The unlimited label must fit the reserved width");

	uploadEnabled->SetValue(true);
	wxCommandEvent enableUpload(wxEVT_CHECKBOX, uploadEnabled->GetId());
	enableUpload.SetInt(1);
	enableUpload.SetEventObject(uploadEnabled);
	uploadEnabled->GetEventHandler()->ProcessEvent(enableUpload);
	Check(uploadLimit->GetValue() == 2500 && uploadSlider->GetValue() == 2500,
		"First enable after loading unlimited must start at line capacity");
	wxCommandEvent previewUpload(wxEVT_TEXT, uploadLimit->GetId());
	previewUpload.SetEventObject(uploadLimit);
	previewUpload.SetString("1");
	int uploadNotifications = 0;
	dialog.Bind(
		wxEVT_SPINCTRL, [&uploadNotifications](wxSpinEvent &) { ++uploadNotifications; }, IDC_MAXUP);
	uploadLimit->GetEventHandler()->ProcessEvent(previewUpload);
	Check(uploadLimit->GetValue() == 2500 && uploadSlider->GetValue() == 1 && uploadNotifications == 0,
		"Typing an upload limit must preview it without committing or running the ratio check");
	uploadEnabled->SetValue(false);
	wxCommandEvent disableUpload(wxEVT_CHECKBOX, uploadEnabled->GetId());
	disableUpload.SetInt(0);
	disableUpload.SetEventObject(uploadEnabled);
	uploadEnabled->GetEventHandler()->ProcessEvent(disableUpload);
	uploadEnabled->SetValue(true);
	uploadEnabled->GetEventHandler()->ProcessEvent(enableUpload);
	Check(uploadLimit->GetValue() == 2500,
		"An uncommitted text preview must not replace the last enabled limit");
}
} // namespace

int main(int argc, char **argv)
{
	return RunGuiTest(argc, argv, "No GUI display available; skipping bandwidth limit tests", [] {
		TestLimitControl();
		std::cout << "Bandwidth limit tests passed\n";
	});
}
