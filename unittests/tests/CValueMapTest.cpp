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

// CValueMap is the incremental-update filter behind every EC tag that carries
// a value map: it decides whether a field has changed since the last response
// to this client, and only then does the tag get built and emitted.
//
// Its failure mode is silence. A field that is wrongly judged unchanged simply
// stops updating in the GUI -- no crash, no log line, nothing a build or a
// clean daemon run would catch. These tests pin the contract instead.

#include <muleunit/test.h>

#include <ec/cpp/ECSpecialTags.h>

using namespace muleunit;

namespace
{
// Children of a throwaway parent: CreateTag appends to whatever it is given,
// so the child count is the observable "did this get emitted".
size_t EmittedCount(const CECTag &parent)
{
	return parent.GetTagCount();
}
} // namespace

DECLARE_SIMPLE(CValueMapTest)

TEST(CValueMapTest, FirstValueIsAlwaysEmitted)
{
	CValueMap vm;
	CECEmptyTag parent(1);
	vm.CreateTag(100, (uint32)42, &parent);
	ASSERT_EQUALS((size_t)1, EmittedCount(parent));
}

TEST(CValueMapTest, UnchangedValueIsSuppressed)
{
	CValueMap vm;
	CECEmptyTag first(1);
	vm.CreateTag(100, (uint32)42, &first);
	ASSERT_EQUALS((size_t)1, EmittedCount(first));

	// Same value again: nothing should reach the second parent.
	CECEmptyTag second(1);
	vm.CreateTag(100, (uint32)42, &second);
	ASSERT_EQUALS((size_t)0, EmittedCount(second));
}

TEST(CValueMapTest, ChangedValueIsEmittedAgain)
{
	CValueMap vm;
	CECEmptyTag first(1);
	vm.CreateTag(100, (uint32)42, &first);

	CECEmptyTag second(1);
	vm.CreateTag(100, (uint32)43, &second);
	ASSERT_EQUALS((size_t)1, EmittedCount(second));

	// And the new value is what is now remembered, not the original.
	CECEmptyTag third(1);
	vm.CreateTag(100, (uint32)43, &third);
	ASSERT_EQUALS((size_t)0, EmittedCount(third));
}

TEST(CValueMapTest, StringsRoundTripThroughTheirOwnCache)
{
	CValueMap vm;
	CECEmptyTag first(1);
	vm.CreateTag(100, wxString(wxT("alpha")), &first);
	ASSERT_EQUALS((size_t)1, EmittedCount(first));

	CECEmptyTag second(1);
	vm.CreateTag(100, wxString(wxT("alpha")), &second);
	ASSERT_EQUALS((size_t)0, EmittedCount(second));

	CECEmptyTag third(1);
	vm.CreateTag(100, wxString(wxT("beta")), &third);
	ASSERT_EQUALS((size_t)1, EmittedCount(third));
}

// bool needs its own overload to resolve ambiguity -- without it a bool
// argument is ambiguous across uint8/16/32/64 and does not compile. It is NOT
// a wire-format concern: CECTag(name, bool) calls InitInt() exactly as
// CECTag(name, uint8) does, so folding it into the integer overload would
// produce an identical tag. Only the caching behaviour is observable here, so
// that is all this asserts.
TEST(CValueMapTest, BoolCachesSeparatelyFromItsValue)
{
	CValueMap vm;
	CECEmptyTag first(1);
	vm.CreateTag(100, true, &first);
	ASSERT_EQUALS((size_t)1, EmittedCount(first));
	ASSERT_EQUALS((uint64_t)1, first.GetFirstTagSafe()->GetInt());

	CECEmptyTag second(1);
	vm.CreateTag(100, true, &second);
	ASSERT_EQUALS((size_t)0, EmittedCount(second));

	CECEmptyTag third(1);
	vm.CreateTag(100, false, &third);
	ASSERT_EQUALS((size_t)1, EmittedCount(third));
	ASSERT_EQUALS((uint64_t)0, third.GetFirstTagSafe()->GetInt());
}

// double is a genuine value concern, unlike bool: routed through an integer
// overload the fractional part is lost, so the round-trip is what this asserts
// rather than the emit counts alone.
TEST(CValueMapTest, DoubleKeepsItsFractionalPart)
{
	CValueMap vm;
	CECEmptyTag first(1);
	vm.CreateTag(100, 1.5, &first);
	ASSERT_EQUALS((size_t)1, EmittedCount(first));
	ASSERT_EQUALS(1.5, first.GetFirstTagSafe()->GetDoubleData());

	CECEmptyTag second(1);
	vm.CreateTag(100, 1.5, &second);
	ASSERT_EQUALS((size_t)0, EmittedCount(second));

	// 1.5 and 2.5 would both truncate to different ints, so also check a pair
	// that collides under truncation: 1.5 -> 1 and 1.25 -> 1.
	CECEmptyTag third(1);
	vm.CreateTag(100, 1.25, &third);
	ASSERT_EQUALS((size_t)1, EmittedCount(third));
	ASSERT_EQUALS(1.25, third.GetFirstTagSafe()->GetDoubleData());
}

// A string literal must not reach the bool overload. `const char*` -> bool is a
// standard conversion and beats the user-defined one to wxString, so without an
// explicit pointer overload this would emit a boolean tag here while the plain
// CECTag path -- which has its own pointer constructors -- emitted a string one.
TEST(CValueMapTest, StringLiteralDoesNotBecomeABool)
{
	CValueMap vm;
	CECEmptyTag parent(1);
	vm.CreateTag(100, "n/a", &parent);
	ASSERT_EQUALS((size_t)1, EmittedCount(parent));
	ASSERT_EQUALS(wxString(wxT("n/a")), parent.GetFirstTagSafe()->GetStringData());
}

// Distinct tag names never share a slot, even at the same value.
TEST(CValueMapTest, DifferentTagsAreIndependent)
{
	CValueMap vm;
	CECEmptyTag first(1);
	vm.CreateTag(100, (uint32)42, &first);
	vm.CreateTag(101, (uint32)42, &first);
	ASSERT_EQUALS((size_t)2, EmittedCount(first));

	CECEmptyTag second(1);
	vm.CreateTag(100, (uint32)42, &second);
	vm.CreateTag(101, (uint32)99, &second);
	ASSERT_EQUALS((size_t)1, EmittedCount(second));
}

// AddDiffTag has two branches -- value map present, and absent for callers not
// doing an incremental update -- and the design rests on them producing the
// same tag. The bug this file was written for lived exactly there: a string
// literal took the bool overload through the map and the const char*
// constructor without it, so one call site emitted different wire types on an
// incremental update than on a full request.
TEST(CValueMapTest, AddDiffTagBranchesAgreeOnLiterals)
{
	CValueMap vm;
	CECEmptyTag viaMap(1);
	CECEmptyTag viaDirect(1);
	AddDiffTag(&viaMap, 100, "n/a", &vm);
	AddDiffTag(&viaDirect, 100, "n/a", nullptr);

	ASSERT_EQUALS((size_t)1, EmittedCount(viaMap));
	ASSERT_EQUALS((size_t)1, EmittedCount(viaDirect));
	ASSERT_EQUALS(viaDirect.GetFirstTagSafe()->IsString(), viaMap.GetFirstTagSafe()->IsString());
	ASSERT_TRUE(viaMap.GetFirstTagSafe()->IsString());
	ASSERT_EQUALS(
		viaDirect.GetFirstTagSafe()->GetStringData(), viaMap.GetFirstTagSafe()->GetStringData());
}

TEST(CValueMapTest, AddDiffTagBranchesAgreeOnStrings)
{
	CValueMap vm;
	CECEmptyTag viaMap(1);
	CECEmptyTag viaDirect(1);
	AddDiffTag(&viaMap, 100, wxString(wxT("alpha")), &vm);
	AddDiffTag(&viaDirect, 100, wxString(wxT("alpha")), nullptr);

	ASSERT_EQUALS(viaDirect.GetFirstTagSafe()->IsString(), viaMap.GetFirstTagSafe()->IsString());
	ASSERT_EQUALS(
		viaDirect.GetFirstTagSafe()->GetStringData(), viaMap.GetFirstTagSafe()->GetStringData());
}

// Without a value map every call emits, incremental filtering being the map's
// whole job -- so a NULL-map caller must never be silently suppressed.
TEST(CValueMapTest, AddDiffTagWithoutMapAlwaysEmits)
{
	CECEmptyTag parent(1);
	AddDiffTag(&parent, 100, (uint32)42, nullptr);
	AddDiffTag(&parent, 100, (uint32)42, nullptr);
	ASSERT_EQUALS((size_t)2, EmittedCount(parent));
}

// --- HasTag: which cache it reads --------------------------------------
// HasTag gates the media clear emission: a field that is now absent gets an
// explicit zero / empty frame only when a value was previously SENT for it,
// because a tag simply not offered reads as UNCHANGED on the remote side.
//
// The property worth pinning is not "true after a write" but WHICH cache it
// reads. The two write forms keep separate caches, and this header's own
// comment above AddDiffTag warns that mixing them for one tagname means
// neither sees the other's last value. HasTag reads m_map_tag, the cache
// AddTag(const CECTag &, CECTag *) writes -- so if a media field were ever
// routed through AddDiffTag for efficiency, HasTag would silently report
// false for it and that field's clear would stop being emitted, with nothing
// failing anywhere.

TEST(CValueMapTest, HasTagIsFalseBeforeAnythingIsSent)
{
	CValueMap vm;
	ASSERT_TRUE(!vm.HasTag(100));
}

TEST(CValueMapTest, HasTagIsTrueAfterTheCECTagFormWrites)
{
	CValueMap vm;
	CECEmptyTag parent(1);
	parent.AddTag(CECTag(static_cast<ec_tagname_t>(100), wxT("value")), &vm);
	ASSERT_TRUE(vm.HasTag(100));
}

TEST(CValueMapTest, HasTagDoesNotSeeTheTypedCacheWrites)
{
	// AddDiffTag writes the TYPED cache, not m_map_tag. HasTag must report
	// false for it -- not because that is desirable, but because it is the
	// truth about which cache holds the value, and a caller mixing the two
	// forms for one tagname is the documented bug this exposes rather than
	// hides.
	CValueMap vm;
	CECEmptyTag parent(1);
	AddDiffTag(&parent, static_cast<ec_tagname_t>(101), wxString(wxT("value")), &vm);
	ASSERT_TRUE(!vm.HasTag(101));
}

// The hazard that keeps EC_TAG_CLIENT_UPLOAD_FILE on the old path: one tag name
// written through two different types keeps two independent caches, so neither
// sees the other's last value and a transition between them is not suppressed.
// Documented in the PR; executable here so converting that site later fails.
TEST(CValueMapTest, OneTagNameAcrossTwoTypesKeepsSeparateCaches)
{
	CValueMap vm;
	CECEmptyTag first(1);
	vm.CreateTag(100, (uint32)1, &first);
	ASSERT_EQUALS((size_t)1, EmittedCount(first));

	// Same name, same numeric value, different type: a shared cache would
	// suppress this. Separate caches must emit it.
	CECEmptyTag second(1);
	vm.CreateTag(100, true, &second);
	ASSERT_EQUALS((size_t)1, EmittedCount(second));
}
