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

#include "CaptchaGenerator.h"
#include "RandomFunctions.h"

#include <algorithm>
#include <cmath>

namespace
{
// Original 5 x 7 block glyphs. Omit digits 2, 5, 6 and 8, which resemble Z, S, G and B.
// Answers are checked locally; the alphabet is not part of the wire format.
// No system fonts, image handlers or wx GUI initialization are needed.
constexpr char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ13479";
constexpr unsigned char glyphs[][7] = {
	{ 14, 17, 17, 31, 17, 17, 17 }, // A
	{ 30, 17, 17, 30, 17, 17, 30 }, // B
	{ 14, 17, 16, 16, 16, 17, 14 }, // C
	{ 30, 17, 17, 17, 17, 17, 30 }, // D
	{ 31, 16, 16, 30, 16, 16, 31 }, // E
	{ 31, 16, 16, 30, 16, 16, 16 }, // F
	{ 14, 17, 16, 23, 17, 17, 15 }, // G
	{ 17, 17, 17, 31, 17, 17, 17 }, // H
	{ 7, 2, 2, 2, 18, 18, 12 },     // J
	{ 17, 18, 20, 24, 20, 18, 17 }, // K
	{ 16, 16, 16, 16, 16, 16, 31 }, // L
	{ 17, 27, 21, 21, 17, 17, 17 }, // M
	{ 17, 25, 25, 21, 19, 19, 17 }, // N
	{ 30, 17, 17, 30, 16, 16, 16 }, // P
	{ 14, 17, 17, 17, 21, 18, 13 }, // Q
	{ 30, 17, 17, 30, 20, 18, 17 }, // R
	{ 15, 16, 16, 14, 1, 1, 30 },   // S
	{ 31, 4, 4, 4, 4, 4, 4 },       // T
	{ 17, 17, 17, 17, 17, 17, 14 }, // U
	{ 17, 17, 17, 17, 17, 10, 4 },  // V
	{ 17, 17, 17, 21, 21, 27, 17 }, // W
	{ 17, 17, 10, 4, 10, 17, 17 },  // X
	{ 17, 17, 10, 4, 4, 4, 4 },     // Y
	{ 31, 1, 2, 4, 8, 16, 31 },     // Z
	{ 4, 12, 4, 4, 4, 4, 14 },      // 1
	{ 30, 1, 1, 14, 1, 1, 30 },     // 3
	{ 2, 6, 10, 18, 31, 2, 2 },     // 4
	{ 31, 1, 2, 4, 8, 8, 8 },       // 7
	{ 14, 17, 17, 15, 1, 1, 14 },   // 9
};
static_assert(
	sizeof(glyphs) / sizeof(glyphs[0]) == sizeof(alphabet) - 1, "Every CAPTCHA character needs a glyph");
constexpr unsigned height = 48;
constexpr unsigned letterSpacing = 22;
constexpr unsigned pixelOffset = 62; // BITMAPFILEHEADER, BITMAPINFOHEADER, two palette entries
} // namespace

bool MatchesChatCaptchaAnswer(const wxString &challenge, const wxString &answer)
{
	const wxString expected = challenge.Upper();
	wxString supplied = answer.Upper();
	supplied.Trim();
	if (expected.IsEmpty() || supplied.Length() < expected.Length()) {
		return false;
	}
	// Peers may prefix the answer with explanatory text; keep the existing suffix behavior.
	supplied = supplied.Right(expected.Length());
	const auto canonical = [](wxUniChar character) {
		switch (character.GetValue()) {
		case '2':
			return wxUniChar('Z');
		case '5':
			return wxUniChar('S');
		case '6':
			return wxUniChar('G');
		case '8':
			return wxUniChar('B');
		default:
			return character;
		}
	};
	for (size_t i = 0; i < expected.Length(); ++i) {
		if (canonical(supplied[i]) != canonical(expected[i])) {
			return false;
		}
	}
	return true;
}

CCaptchaGenerator::CCaptchaGenerator(uint32 nLetterCount)
{
	ReGenerateCaptcha(nLetterCount);
}

void CCaptchaGenerator::ReGenerateCaptcha(uint32 nLetterCount)
{
	// Keep every generated image within the peer validator's 10 < width < 150 bounds.
	const unsigned count = std::max(1U, std::min(nLetterCount, 6U));
	const unsigned width = 8 + count * letterSpacing;
	const unsigned stride = ((width + 31) / 32) * 4;
	m_image.assign(pixelOffset + stride * height, 0);
	const auto write32 = [this](unsigned offset, unsigned value) {
		for (unsigned byte = 0; byte < 4; ++byte) {
			m_image[offset + byte] = static_cast<unsigned char>(value >> (8 * byte));
		}
	};
	m_image[0] = 'B';
	m_image[1] = 'M';
	write32(2, static_cast<unsigned>(m_image.size()));
	write32(10, pixelOffset);
	write32(14, 40);
	write32(18, width);
	write32(22, height); // bottom-up rows
	m_image[26] = 1;     // one plane
	m_image[28] = 1;     // one bit per pixel, BI_RGB (uncompressed)
	write32(34, stride * height);
	write32(46, 2);
	m_image[54] = m_image[55] = m_image[56] = 255; // white background, black foreground

	m_strCaptchaText.Clear();
	for (unsigned letter = 0; letter < count; ++letter) {
		const unsigned index = GetRandomUint16() % (sizeof(alphabet) - 1);
		m_strCaptchaText += static_cast<wxChar>(alphabet[index]);
		const double scaleX = 2.6 + (GetRandomUint16() % 101) * 0.004;
		const double scaleY = 3.0 + (GetRandomUint16() % 101) * 0.006;
		const int centerX = 15 + static_cast<int>(letter * letterSpacing);
		const int centerY = 24 + static_cast<int>(GetRandomUint16() % 5) - 2;
		const double angle =
			(static_cast<int>(GetRandomUint16() % 21) - 10) * 3.14159265358979323846 / 180.0;
		const double shear = (static_cast<int>(GetRandomUint16() % 21) - 10) / 100.0;
		const double cosine = std::cos(angle);
		const double sine = std::sin(angle);
		// Each glyph stays in its own cell: neighboring letters may touch, never overlap.
		// Inverse-map destination pixels so enlarged and rotated strokes have no holes.
		for (unsigned y = 0; y < height; ++y) {
			for (unsigned x = 4 + letter * letterSpacing; x < 4 + (letter + 1) * letterSpacing;
				++x) {
				const double dx = static_cast<double>(x) - centerX;
				const double dy = static_cast<double>(y) - centerY;
				const int column = static_cast<int>(std::floor(
					(dx * cosine + dy * sine - shear * (-dx * sine + dy * cosine)) /
						scaleX +
					2.5));
				const int row = static_cast<int>(
					std::floor((-dx * sine + dy * cosine) / scaleY + 3.5));
				if (column >= 0 && column < 5 && row >= 0 && row < 7 &&
					(glyphs[index][row] & (1U << (4 - column)))) {
					m_image[pixelOffset + (height - 1 - y) * stride + x / 8] |=
						static_cast<unsigned char>(0x80U >> (x % 8));
				}
			}
		}
	}
	// Light background noise must not obscure the distinguishing strokes of a letter.
	for (unsigned dot = 0; dot < width * height / 128; ++dot) {
		const unsigned x = GetRandomUint16() % width;
		const unsigned y = GetRandomUint16() % height;
		m_image[pixelOffset + y * stride + x / 8] |= static_cast<unsigned char>(0x80U >> (x % 8));
	}
}

bool CCaptchaGenerator::WriteCaptchaImage(wxMemoryOutputStream &file)
{
	file.Write(m_image.data(), m_image.size());
	return file.IsOk();
}
