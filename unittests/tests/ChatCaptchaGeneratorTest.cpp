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

#include <muleunit/test.h>
#include "CaptchaGenerator.h"
#include "ChatCaptcha.h"

#include <array>
#include <limits>
#include <wx/mstream.h>

using namespace muleunit;
DECLARE_SIMPLE(ChatCaptchaGenerator)

namespace
{
std::array<uint16_t, 6> randomValues{};
unsigned randomCursor = 0;
} // namespace

// Deterministically exercise every glyph at the extreme supported transforms.
uint16_t GetRandomUint16()
{
	const unsigned position = randomCursor++;
	if (position < 4 * randomValues.size()) {
		return randomValues[position % randomValues.size()];
	}
	return 0;
}

TEST(ChatCaptchaGenerator, EveryGlyphAndExtremeTransformIsAValidHeadlessBitmap)
{
	const wxString alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ13479";
	for (unsigned glyph = 0; glyph < alphabet.length(); ++glyph) {
		for (uint16_t size : { 0, 100 }) {
			for (uint16_t angle : { 0, 10, 20 }) {
				for (uint16_t shear : { 0, 10, 20 }) {
					for (uint16_t offset : { 0, 4 }) {
						randomValues = { static_cast<uint16_t>(glyph),
							size,
							size,
							offset,
							angle,
							shear };
						randomCursor = 0;
						CCaptchaGenerator captcha;
						ASSERT_EQUALS(wxString(alphabet[glyph], 4),
							captcha.GetCaptchaText());
						wxMemoryOutputStream stream;
						ASSERT_TRUE(captcha.WriteCaptchaImage(stream));
						const auto *bytes = static_cast<const unsigned char *>(
							stream.GetOutputStreamBuffer()->GetBufferStart());
						ASSERT_TRUE(IsValidChatCaptcha(bytes, stream.GetLength()));
						ASSERT_EQUALS(static_cast<size_t>(638), stream.GetLength());
						ASSERT_EQUALS(static_cast<unsigned char>(1), bytes[28]);
						// Each character must have visible ink, including narrow
						// glyphs (1/J/T).
						for (unsigned letter = 0; letter < 4; ++letter) {
							unsigned ink = 0;
							for (unsigned y = 0; y < 48; ++y) {
								for (unsigned x = 4 + letter * 22;
									x < 26 + letter * 22;
									++x) {
									ink += (bytes[62 + y * 12 + x / 8] >>
										       (7 - x % 8)) &
									       1U;
								}
							}
							ASSERT_TRUE(ink > 40);
							ASSERT_TRUE(ink < 650);
							// No crossing line or glyph reaches the next cell,
							// even at opposite shear/rotation extremes.
							const unsigned boundary = 4 + letter * 22;
							for (unsigned y = 0; y < 48; ++y) {
								ASSERT_EQUALS(0U,
									(bytes[62 + y * 12 + boundary / 8] >>
										(7 - boundary % 8)) &
										1U);
							}
						}
					}
				}
			}
		}
	}
}

TEST(ChatCaptchaGenerator, AnswersAcceptOnlyTheSpecifiedVisualEquivalents)
{
	ASSERT_TRUE(MatchesChatCaptchaAnswer("SZGB", "5268"));
	ASSERT_TRUE(MatchesChatCaptchaAnswer("SZGB", "s2g8"));
	ASSERT_TRUE(MatchesChatCaptchaAnswer("SZGB", "  s2g8  "));
	ASSERT_TRUE(MatchesChatCaptchaAnswer("SZGB", "Captcha: 5268"));
	ASSERT_TRUE(MatchesChatCaptchaAnswer("AC79", "ac79"));
	ASSERT_FALSE(MatchesChatCaptchaAnswer("SZGB", "2568"));  // Transposed characters.
	ASSERT_FALSE(MatchesChatCaptchaAnswer("SZGB", "526"));   // Missing character.
	ASSERT_FALSE(MatchesChatCaptchaAnswer("SZGB", "5269"));  // Unrelated substitution.
	ASSERT_FALSE(MatchesChatCaptchaAnswer("SZGB", "5268X")); // Wrong suffix.
	ASSERT_FALSE(MatchesChatCaptchaAnswer("SZGB", "5 268")); // Internal whitespace.
	ASSERT_FALSE(MatchesChatCaptchaAnswer("HUMN", "NJWH"));  // No other look-alike pairs.
	ASSERT_FALSE(MatchesChatCaptchaAnswer("", ""));
}

TEST(ChatCaptchaGenerator, BoundsRegenerationAndAppendingToTheWireStream)
{
	CCaptchaGenerator captcha(0);
	ASSERT_EQUALS(static_cast<size_t>(1), captcha.GetCaptchaText().length());
	captcha.ReGenerateCaptcha(std::numeric_limits<uint32>::max());
	ASSERT_EQUALS(static_cast<size_t>(6), captcha.GetCaptchaText().length());
	wxMemoryOutputStream stream;
	stream.PutC(0); // OP_CHATCAPTCHAREQ's empty tag count precedes the BMP.
	ASSERT_TRUE(captcha.WriteCaptchaImage(stream));
	const auto *bytes =
		static_cast<const unsigned char *>(stream.GetOutputStreamBuffer()->GetBufferStart());
	ASSERT_EQUALS(static_cast<unsigned char>(0), bytes[0]);
	ASSERT_TRUE(IsValidChatCaptcha(bytes + 1, stream.GetLength() - 1));
	captcha.ReGenerateCaptcha(4);
	ASSERT_EQUALS(static_cast<size_t>(4), captcha.GetCaptchaText().length());
}
