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

// GUI rendering regression test. CI requires a display; local headless runs may skip.

#include "GuiTestDiagnostics.h"

#include <wx/app.h>
#include <wx/frame.h>
#include <wx/mstream.h>
#include <wx/image.h>
#include <wx/settings.h>
#include <wx/menu.h>
#include <wx/filename.h>
#include "CamuleArtProvider.h"
#include "CountryFlags.h"
#include <vector>
#include "MenuIcons.h"
#include "TrayIconBundle.h"
#include "icons/icon_data.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#ifdef _WIN32
#include <wx/msw/wrapwin.h>
#include <dbghelp.h>
#include <io.h>
#else
#include <unistd.h>
#endif

// The test has hung silently on Windows CI. Name each step on stderr, and let a
// watchdog report the last step, and write a minidump on Windows, before CTest's
// 10-second timeout kills the process with no output.
static std::atomic<const char *> g_phase{ "start" };
static std::atomic<const char *> g_icon{ "" };
static std::atomic<int> g_scalePercent{ 0 };

// Raw writes: a stuck main thread may hold the iostream or stdio lock.
static void WriteStderr(const std::string &text)
{
#ifdef _WIN32
	(void)_write(2, text.data(), static_cast<unsigned>(text.size()));
#else
	(void)!write(2, text.data(), text.size());
#endif
}

static void Phase(const char *name)
{
	g_phase = name;
	g_icon = "";
	g_scalePercent = 0;
	WriteStderr(std::string("phase: ") + name + "\n");
}

static void Rendering(const char *icon, double scale)
{
	g_icon = icon;
	g_scalePercent = static_cast<int>(std::lround(scale * 100));
}

#ifdef _WIN32
static void WriteMiniDump()
{
	const char *dir = std::getenv("AMULE_ICON_TEST_DUMP_DIR");
	if (!dir) {
		return;
	}
	const std::string path =
		std::string(dir) + "\\IconArtworkTest-" + std::to_string(GetCurrentProcessId()) + ".dmp";
	HANDLE file = CreateFileA(
		path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		WriteStderr("watchdog: cannot create " + path + "\n");
		return;
	}
	const auto type = static_cast<MINIDUMP_TYPE>(
		MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs);
	const BOOL ok = MiniDumpWriteDump(
		GetCurrentProcess(), GetCurrentProcessId(), file, type, nullptr, nullptr, nullptr);
	CloseHandle(file);
	WriteStderr(ok ? "watchdog: wrote " + path + "\n" : std::string("watchdog: minidump failed\n"));
}
#endif

class Watchdog
{
public:
	explicit Watchdog(int seconds)
	: m_thread([this, seconds]() { Run(seconds); })
	{
	}

	~Watchdog()
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_done = true;
		}
		m_wake.notify_one();
		m_thread.join();
	}

private:
	void Run(int seconds)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		if (seconds <= 0 ||
			m_wake.wait_for(lock, std::chrono::seconds(seconds), [this]() { return m_done; })) {
			return;
		}
		std::string report = "watchdog: no exit after " + std::to_string(seconds) +
				     " s; last phase: " + g_phase.load();
		if (*g_icon.load()) {
			report += std::string(", rendering ") + g_icon.load() + " at " +
				  std::to_string(g_scalePercent.load()) + "%";
		}
		WriteStderr(report + "\n");
#ifdef _WIN32
		WriteMiniDump();
#endif
		std::_Exit(EXIT_FAILURE);
	}

	std::mutex m_mutex;
	std::condition_variable m_wake;
	bool m_done = false;
	std::thread m_thread; // Last: it starts running in the constructor.
};

class CheckApp : public wxApp
{
public:
	bool OnInit() override { return true; }
};
wxIMPLEMENT_APP_NO_MAIN(CheckApp);
void require(bool ok, const char *message)
{
	if (!ok)
		throw std::runtime_error(message);
}
// wx versions bundle different NanoSVG rasterizers: the same SVG can have
// different antialiased coverage and shading. Validate each rendition's visible
// requirements independently instead of requiring SVG/PNG byte identity.
static void CheckTrayArtwork(const wxImage &image, const char *state)
{
	require(image.IsOk() && image.HasAlpha() && image.GetSize() == wxSize(22, 22),
		"tray artwork dimensions or alpha invalid");
	int opaque = 0, brown = 0, yellow = 0, gray = 0, white = 0;
	for (int y = 0; y < 22; ++y) {
		require(image.GetAlpha(20, y) == 0 && image.GetAlpha(21, y) == 0,
			"tray artwork intrudes into speed-bar columns");
		for (int x = 0; x < 20; ++x) {
			opaque += image.GetAlpha(x, y) > 0;
			if (image.GetAlpha(x, y) == 255) {
				const int r = image.GetRed(x, y);
				const int g = image.GetGreen(x, y);
				const int b = image.GetBlue(x, y);
				brown += r > g + 30 && g > b + 10;
				yellow += std::abs(r - g) < 40 && g > b + 40;
				const int low = std::min({ r, g, b });
				const int high = std::max({ r, g, b });
				gray += high - low < 5 && low > 60 && high < 220;
				// Limit the near-white check to the face: low-ID body
				// gradients can otherwise satisfy it with black eyes.
				white += low > 220 && y <= 10;
			}
		}
	}
	require(opaque > 80, "tray artwork rendered empty");
	require(white >= 2, "tray artwork lost white eyes or muzzle");
	require(gray >= 5, "tray artwork lost gray bench planks");
	// NanoSVG ignores SVG colour filters: check the actual rendered fills,
	// so a low-ID icon accidentally rendering brown cannot pass unnoticed.
	if (std::strcmp(state, "tray_low") == 0) {
		require(yellow > 25, "low-ID tray artwork is not yellow");
	} else if (std::strcmp(state, "tray_high") == 0) {
		require(brown > 30 && yellow == 0, "high-ID tray artwork is not brown");
	} else {
		require(gray > 80 && brown == 0 && yellow == 0, "disconnected tray artwork is not gray");
	}
}

static void CheckTrayComposition(
	const wxBitmapBundle &artwork, const char *state, const char *format, const char *output)
{
	Phase(std::strcmp(format, "svg") == 0 ? "tray SVG composition" : "tray PNG fallback composition");
	const wxColour colour(19, 137, 241);
	const auto half = MuleTray::MakeIconBundle(artwork, 50, colour);
	for (int pixels : { 16, 17, 22, 24, 33, 39, 44, 66, 88, 100, 22 }) {
		Rendering(state, pixels / 22.0);
		const wxSize size(pixels, pixels);
		// Normalize the reference through the same native bitmap codec as the
		// composed image: MSW/Cocoa premultiply RGB at antialiased edges.
		const auto base = wxBitmap(artwork.GetBitmap(size).ConvertToImage()).ConvertToImage();
		for (unsigned int percent : { 0u, 29u, 50u, 57u, 58u, 100u, 200u, 0u }) {
			const auto bundle =
				percent == 50 ? half : MuleTray::MakeIconBundle(artwork, percent, colour);
			const auto image = bundle.GetBitmap(size).ConvertToImage();
			require(image.IsOk() && image.GetSize() == size && image.HasAlpha(),
				"tray size or alpha lost");
			const int barWidth = std::max(1, pixels / 11);
			const int barHeight = pixels * std::min(percent, 100u) / 100;
			for (int y = 0; y < pixels; ++y) {
				for (int x = 0; x < pixels; ++x) {
					const bool bar = x >= pixels - barWidth && y >= pixels - barHeight;
					if (bar) {
						require(base.GetAlpha(x, y) == 0,
							"speed bar overwrites artwork");
					}
					require(image.GetAlpha(x, y) == (bar ? 255 : base.GetAlpha(x, y)),
						"tray transparency or bar height changed");
					require(image.GetRed(x, y) ==
								(bar ? colour.Red() : base.GetRed(x, y)) &&
							image.GetGreen(x, y) == (bar ? colour.Green()
										     : base.GetGreen(x, y)) &&
							image.GetBlue(x, y) ==
								(bar ? colour.Blue() : base.GetBlue(x, y)),
						"tray bar overlaps artwork or has wrong colour");
				}
			}
			if (output) {
				image.SaveFile(
					wxString::Format(
						"%s/%s-%s-%d-%u.png", output, state, format, pixels, percent),
					wxBITMAP_TYPE_PNG);
			}
		}
	}
}

int main(int argc, char **argv)
{
	// Below CTest's 10 s limit; 0 disables it, e.g. under a debugger.
	const char *watchdogSeconds = std::getenv("AMULE_ICON_TEST_WATCHDOG_SECONDS");
	Watchdog watchdog(watchdogSeconds ? std::atoi(watchdogSeconds) : 8);
	Phase("wx startup");
	// Cover startup without turning an optional headless skip into a failure.
	const bool requireGui = std::getenv("AMULE_ICON_TEST_REQUIRE_GUI") != nullptr;
	InstallGuiTestDiagnostics(requireGui);
	if (!wxEntryStart(argc, argv)) {
		std::cerr << "GUI initialization failed: no native rendering checks ran\n";
		return requireGui ? 1 : 77;
	}
	RestoreGuiTestLog();
	Phase("OnInit");
	if (!wxTheApp->CallOnInit()) {
		wxEntryCleanup();
		return 1;
	}
	Phase("image handlers and art provider");
	wxInitAllImageHandlers();
	wxArtProvider::Push(new CamuleArtProvider);
	int count = 0, flags = 0, menus = 0;
	const auto entries = amule_get_all_icons(&count);
	const char *output = std::getenv("AMULE_ICON_TEST_OUTPUT");
	if (output)
		wxFileName::Mkdir(wxString::FromUTF8(output), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
	int status = 0;
	try {
		Phase("theme-aware menu bundles");
		// Prime wx's neutral cache, then switch menu colours without resetting
		// it. Live theme changes must affect both SVG and PNG menu rendering.
		wxArtProvider::GetBitmapBundle("amule:menu_pause_fill", wxART_MENU, wxSize(16, 16));
		for (bool preferSvg : { true, false }) {
			for (const wxColour &colour :
				{ wxColour(0, 0, 0), wxColour(255, 255, 255), wxColour(0, 0, 0) }) {
				const auto bundle = CamuleArtProvider::GetMenuBitmapBundle(
					"menu_pause_fill", wxSize(16, 16), colour, preferSvg);
				require(bundle.IsOk(), "theme-aware bundle invalid");
				const auto image = bundle.GetBitmap(wxSize(16, 16)).ConvertToImage();
				require(image.GetRed(5, 7) == colour.Red() &&
						image.GetGreen(5, 7) == colour.Green() &&
						image.GetBlue(5, 7) == colour.Blue(),
					"live theme switch kept stale menu colour");
			}
		}

		Phase("tray SVGs and speed bars");
		for (const char *state : { "tray_high", "tray_low", "tray_disconnected" }) {
			const auto entry = amule_find_icon(state);
			require(entry && entry->svg_len, "tray SVG missing");
			const auto artwork =
				wxArtProvider::GetBitmapBundle(wxString("amule:") + state, wxART_OTHER);
			require(artwork.IsOk() && artwork.GetDefaultSize() == wxSize(22, 22),
				"tray bundle invalid");
			const auto natural = artwork.GetBitmap(wxSize(22, 22)).ConvertToImage();
			CheckTrayArtwork(natural, state);
			wxMemoryInputStream pngStream(entry->png_data, entry->png_len);
			const wxImage fallback(pngStream, wxBITMAP_TYPE_PNG);
			CheckTrayArtwork(fallback, state);
			const wxColour colour(19, 137, 241);
			// Revisit 1x after 4x using the same bundle, as on a monitor move.
			const auto half = MuleTray::MakeIconBundle(artwork, 50, colour);
#ifdef __WXMSW__
			// Compare with wx's independently queried native small-icon metrics at
			// the current display DPI, rather than the artwork's 22px canvas.
			wxFrame metricWindow(nullptr, wxID_ANY, "tray metrics");
			require(half.GetPreferredBitmapSizeAtScale(metricWindow.GetDPIScaleFactor()) ==
					wxSize(wxSystemSettings::GetMetric(wxSYS_SMALLICON_X, &metricWindow),
						wxSystemSettings::GetMetric(
							wxSYS_SMALLICON_Y, &metricWindow)),
				"tray bundle does not use native small-icon metrics");
#endif
#if defined(wxHAS_SVG) && !defined(__WXMSW__)
			require(half.GetPreferredBitmapSizeAtScale(1.5) == wxSize(33, 33) &&
					half.GetPreferredBitmapSizeAtScale(3) == wxSize(66, 66),
				"tray bundle lost vector scale selection");
#endif
			CheckTrayComposition(artwork, state, "svg", output);
			CheckTrayComposition(MuleTray::MakeArtworkFallback(fallback), state, "png", output);
		}

		Phase("macOS template tray state shapes");
		std::vector<std::vector<unsigned char>> masks;
		for (const char *state :
			{ "tray_high_template", "tray_low_template", "tray_disconnected_template" }) {
			const auto entry = amule_find_icon(state);
			require(entry && entry->svg_len && entry->png_len, "template artwork missing");
			const auto artwork =
				wxArtProvider::GetBitmapBundle(wxString("amule:") + state, wxART_OTHER);
			require(artwork.IsOk(), "template bundle invalid");
			wxMemoryInputStream stream(entry->png_data, entry->png_len);
			const wxImage fallback(stream, wxBITMAP_TYPE_PNG);
			for (const auto &bundle : { artwork, MuleTray::MakeArtworkFallback(fallback) }) {
				for (int size : { 16, 22, 32, 44, 64 }) {
					const wxImage image =
						bundle.GetBitmap(wxSize(size, size)).ConvertToImage();
					require(image.IsOk() && image.HasAlpha(), "template alpha missing");
					int opaque = 0;
					for (int y = 0; y < size; ++y) {
						for (int x = 0; x < size; ++x) {
							if (image.GetAlpha(x, y)) {
								require(image.GetRed(x, y) == 0 &&
										image.GetGreen(x, y) == 0 &&
										image.GetBlue(x, y) == 0,
									"template is not monochrome");
								++opaque;
							}
						}
					}
					require(opaque > size * size / 8 && opaque < size * size * 3 / 4,
						"template silhouette empty or solid");
				}
			}
			const wxImage image = artwork.GetBitmap(wxSize(22, 22)).ConvertToImage();
			masks.emplace_back(image.GetAlpha(), image.GetAlpha() + 22 * 22);
			CheckTrayComposition(artwork, state, "svg", output);
			CheckTrayComposition(MuleTray::MakeArtworkFallback(fallback), state, "png", output);
		}
		require(masks[0] != masks[1] && masks[1] != masks[2] && masks[0] != masks[2],
			"template states have identical shapes");

		Phase("legacy flag padding");
		for (const char *code : { "an", "unknown" }) {
			for (int scale : { 1, 2, 3 }) {
				const auto entry = amule_find_icon((std::string("flag_") + code).c_str());
				require(entry != nullptr, "legacy flag missing");
				const auto data = scale == 3   ? entry->png3x_data
						  : scale == 2 ? entry->png2x_data
							       : entry->png_data;
				const auto length = scale == 3   ? entry->png3x_len
						    : scale == 2 ? entry->png2x_len
								 : entry->png_len;
				wxMemoryInputStream stream(data, length);
				wxImage image(stream, wxBITMAP_TYPE_PNG);
				require(image.IsOk() && image.GetSize() == wxSize(16 * scale, 12 * scale),
					"legacy flag wrong dimensions");
				require(image.HasAlpha(), "legacy flag padding lacks transparency");
				// The 1x original is padded; filtering of enlarged raster-only
				// artwork can blend the boundary, so require its last row only.
				for (int x = 0; x < 16 * scale; ++x) {
					require(image.GetAlpha(x, 12 * scale - 1) == 0,
						"legacy bottom row not transparent");
				}
			}
		}

		Phase("icon inventory");
		CCountryFlags cache;
		for (int i = 0; i < count; ++i) {
			const auto &entry = entries[i];
			bool flag = std::strncmp(entry.name, "flag_", 5) == 0;
			bool menu = std::strncmp(entry.name, "menu_", 5) == 0;
			if (!flag && !menu)
				continue;
			if (flag)
				++flags;
			else
				++menus;
			for (double scale : { 1.0, 1.5, 2.0, 3.0, 4.0 }) {
				Rendering(entry.name, scale);
				wxBitmap b;
				// wxMSW scales DIP into drawing coordinates before GetFlag.
				// GTK2 also has no bitmap backing-scale support: its logical
				// dimensions are physical pixels. GTK3/macOS retain logical size.
#if defined(__WXMSW__) || (defined(__WXGTK__) && !defined(__WXGTK3__))
				const wxSize flagSize(wxRound(16 * scale), wxRound(12 * scale));
				const double contentScale = 1.0;
#else
				const wxSize flagSize(16, 12);
				const double contentScale = scale;
#endif
				if (flag)
					b = cache.GetFlag(
						wxString::FromUTF8(entry.name + 5), flagSize, contentScale);
				else {
					const auto bundle = wxArtProvider::GetBitmapBundle(
						wxString("amule:") + entry.name, wxART_MENU, wxSize(16, 16));
					require(bundle.IsOk(), "menu bundle invalid");
					b = bundle.GetBitmap(
						wxSize(wxRound(16 * scale), wxRound(16 * scale)));
				}
				require(b.IsOk(), "bitmap invalid");
				require(b.GetWidth() == wxRound(16 * scale), "wrong pixel width");
				if (flag)
					require(b.GetLogicalSize() == flagSize, "wrong logical size");
				auto image = b.ConvertToImage();
				if (output)
					require(image.SaveFile(wxFileName(wxString::FromUTF8(output),
								       wxString::Format("%s-%d.png",
									       entry.name,
									       wxRound(16 * scale)))
								       .GetFullPath(),
							wxBITMAP_TYPE_PNG),
						"PNG export failed");
			}
		}
		require(flags == 253, "embedded flag inventory incomplete");
		Phase("PNG density selection");
		// Check source selection independently at each density. Compare after
		// native bitmap conversion to allow platform alpha premultiplication.
		CCountryFlags pngCache;
		const auto raster = amule_find_icon("flag_us");
		require(raster && raster->png2x_data && raster->png3x_data && !raster->svg_data,
			"PNG density table incomplete");
		for (double scale : { 1.0, 1.5, 2.0, 3.0, 4.0 }) {
			const unsigned char *data = scale > 2   ? raster->png3x_data
						    : scale > 1 ? raster->png2x_data
								: raster->png_data;
			const unsigned int length = scale > 2   ? raster->png3x_len
						    : scale > 1 ? raster->png2x_len
								: raster->png_len;
			wxMemoryInputStream stream(data, length);
			wxImage source(stream, wxBITMAP_TYPE_PNG);
			require(source.IsOk(), "density PNG decode failed");
			if (!source.HasAlpha()) {
				source.InitAlpha();
			}
			const wxSize pixels(wxRound(16 * scale), wxRound(12 * scale));
			const auto scaled = source.GetSize() == pixels
						    ? source
						    : source.Scale(pixels.x, pixels.y, wxIMAGE_QUALITY_HIGH);
			wxBitmap expectedBitmap(scaled);
			expectedBitmap.SetScaleFactor(scale);
			const auto expected = expectedBitmap.ConvertToImage();
			const auto actual = pngCache.GetFlag("us", wxSize(16, 12), scale).ConvertToImage();
			require(actual.IsOk() && expected.IsOk() && actual.GetSize() == pixels,
				"density rendering failed");
			if (scale == 1.5) {
				const auto nearest =
					wxBitmap(source.Scale(pixels.x, pixels.y, wxIMAGE_QUALITY_NEAREST))
						.ConvertToImage();
				require(std::memcmp(nearest.GetData(),
						expected.GetData(),
						pixels.x * pixels.y * 3) != 0,
					"fixture cannot distinguish box filter from nearest neighbour");
			}
			require(std::memcmp(actual.GetData(), expected.GetData(), pixels.x * pixels.y * 3) ==
					0,
				"wrong source density or PNG resampling");
			require(actual.HasAlpha() == expected.HasAlpha() &&
					(!expected.HasAlpha() || std::memcmp(actual.GetAlpha(),
									 expected.GetAlpha(),
									 pixels.x * pixels.y) == 0),
				"resampling changed alpha");
		}
		Phase("flag cache and DPI transition");
		const auto cached = cache.GetFlag("us", wxSize(16, 12), 2);
		const auto cachedAgain = cache.GetFlag("us", wxSize(16, 12), 2);
		require(cached.GetRefData() == cachedAgain.GetRefData(),
			"flag pixels copied on repeated draw");
		require(!cache.GetFlag("us", wxSize(0, 12), 1).IsOk(), "invalid flag size accepted");
		const auto unknown = cache.GetFlag("unknown", wxSize(16, 12), 1);
		const auto missing = cache.GetFlag("zz", wxSize(16, 12), 1);
		require(unknown.GetRefData() == missing.GetRefData(), "unknown code missed bitmap cache");
		require(std::memcmp(unknown.ConvertToImage().GetData(),
				missing.ConvertToImage().GetData(),
				16 * 12 * 3) == 0,
			"unknown fallback changed");
		// Windows converts DIP to drawing coordinates; backing scale remains one.
		auto windows = cache.GetFlag("us", wxSize(32, 24), 1);
		require(windows.GetLogicalSize() == wxSize(32, 24), "Windows DIP sizing");
		// The same bundle must render correctly after a monitor transition back to 1x.
		auto back = cache.GetFlag("us", wxSize(16, 12), 1);
		require(back.GetSize() == wxSize(16, 12), "monitor transition sizing");
		Phase("menu semantics");
		wxMenu menu;
		auto item = AppendMenuIcon(&menu, 1234, "&Pause", MenuIcon::Pause);
		require(item->GetId() == 1234 && item->GetItemLabel() == "&Pause",
			"menu command or mnemonic changed");
		require(item->GetBitmapBundle().IsOk(), "menu bitmap missing");
#ifdef __WXOSX_COCOA__
		require(LastMenuIconIsTemplate(menu), "Cocoa menu image not marked as a template");
#endif
		item->Enable(false);
		require(!item->IsEnabled(), "disabled state");
		auto check = menu.AppendCheckItem(1235, "Auto");
		check->Check();
		require(check->IsChecked(), "check state");
#ifdef __WXOSX_COCOA__
		const wxColour textColour = *wxBLACK;
#else
		const auto textColour = wxSystemSettings::GetColour(wxSYS_COLOUR_MENUTEXT);
#endif
		const auto pause = item->GetBitmapBundle().GetBitmap(wxSize(16, 16)).ConvertToImage();
		require(pause.GetRed(5, 7) == textColour.Red() &&
				pause.GetGreen(5, 7) == textColour.Green() &&
				pause.GetBlue(5, 7) == textColour.Blue(),
			"menu theme colour");
	} catch (const std::exception &e) {
		std::cerr << e.what() << "\n";
		status = 1;
	}
	Phase("cleanup");
	wxArtProvider::Pop();
	wxTheApp->OnExit();
	wxEntryCleanup();
	Phase("done");
	if (!status)
		std::cout << "PASS: " << flags << " flags and " << menus
			  << " menu icons at 1x/1.5x/2x/3x/4x; three tray states at 16-100px; fallback, "
			     "bitmap cache, DPI transition, menu "
			     "semantics and live theme "
			     "colour\n";
	return status;
}
