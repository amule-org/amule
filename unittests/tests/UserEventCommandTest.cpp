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

#include "ExternalCommand.h"
#include "TestScratchDir.h"

#include <wx/ffile.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/utils.h>

#ifdef __WINDOWS__
#include <wx/msw/wrapwin.h>
#include <shellapi.h>
#endif

using namespace muleunit;

DECLARE_SIMPLE(UserEventCommand)

TEST(UserEventCommand, HostileValueRemainsOneArgument)
{
#ifdef __WINDOWS__
	const wxString value = "file ' $HOME `id`; & --extra=1";
#else
	const wxString value = "file \" ' $HOME `id`; & --extra=1 %HASH";
#endif
	const auto args = ExternalCommand::Build(
		"notify --file=%FILE %HASH", { { "%FILE", value }, { "%HASH", "0123" } });
	ASSERT_EQUALS(size_t(3), args.size());
	ASSERT_EQUALS(wxString("--file=") + value, args[1]);
	ASSERT_EQUALS(wxString("0123"), args[2]);
}

TEST(UserEventCommand, QuotesBelongToTemplate)
{
#ifdef __WINDOWS__
	const wxString command = "\"C:\\Program Files\\notify.exe\" \"fixed argument\" \"%NAME\"";
	const wxString executable = "C:\\Program Files\\notify.exe";
#else
	const wxString command = "'/opt/my tools/notify' 'fixed argument' '%NAME'";
	const wxString executable = "/opt/my tools/notify";
#endif
#ifdef __WINDOWS__
	const wxString value = "punctuation & and spaces";
#else
	const wxString value = "quotes \" and spaces";
#endif
	const auto args = ExternalCommand::Build(command, { { "%NAME", value } });
	ASSERT_EQUALS(size_t(3), args.size());
	ASSERT_EQUALS(executable, args[0]);
	ASSERT_EQUALS(wxString("fixed argument"), args[1]);
	ASSERT_EQUALS(value, args[2]);
}

TEST(UserEventCommand, EmptyAndRepeatedValues)
{
	const auto args = ExternalCommand::Build("notify %NAME %NAME:%NAME %UNKNOWN", { { "%NAME", "" } });
	ASSERT_EQUALS(size_t(4), args.size());
	ASSERT_EQUALS(wxString(""), args[1]);
	ASSERT_EQUALS(wxString(":"), args[2]);
	ASSERT_EQUALS(wxString("%UNKNOWN"), args[3]);
}

TEST(UserEventCommand, UnicodeAndBackslashesStayLiteral)
{
#ifdef __WINDOWS__
	const wxString value = wxString::FromUTF8("文件\\folder\\a b.txt");
#else
	const wxString value = wxString::FromUTF8("文件\\folder\\a \"b\".txt");
#endif
	const auto args = ExternalCommand::Build("notify %FILE", { { "%FILE", value } });
	ASSERT_EQUALS(size_t(2), args.size());
	ASSERT_EQUALS(value, args[1]);
}

TEST(UserEventCommand, EmptyTemplateDoesNotSpawn)
{
	ASSERT_TRUE(ExternalCommand::Build("", {}).IsEmpty());
	ASSERT_TRUE(ExternalCommand::Build("   ", {}).IsEmpty());
}

#ifndef __WINDOWS__
TEST(UserEventCommand, ShellPositionalArgumentIsData)
{
	const wxString output = wxFileName::CreateTempFileName("amule-event-command-");
	const wxString value = "\"; exit 99; # $(exit 99) `exit 99` & %HASH\nsecond line";
	const auto args = ExternalCommand::Build("sh -c 'printf \"%s\" \"$1\" > \"$2\"' _ %FILE %OUTPUT",
		{ { "%FILE", value }, { "%OUTPUT", output } });
	ASSERT_EQUALS(size_t(6), args.size());
	std::vector<wxWCharBuffer> buffers;
	std::vector<const wchar_t *> argv;
	buffers.reserve(args.size());
	for (const auto &arg : args) {
		buffers.emplace_back(arg.wc_str());
		argv.push_back(buffers.back().data());
	}
	argv.push_back(nullptr);
	const long status = wxExecute(argv.data(), wxEXEC_SYNC);
	wxString received;
	{
		wxFFile file(output, "rb");
		ASSERT_TRUE(file.IsOpened());
		ASSERT_TRUE(file.ReadAll(&received));
	}
	wxRemoveFile(output);
	ASSERT_EQUALS(0L, status);
	ASSERT_EQUALS(value, received);
}

TEST(UserEventCommand, DetachedShellReceivesMultipleHostileValues)
{
	const wxString output = wxFileName::CreateTempFileName("amule-event-detached-");
	const wxString file = "a\"; exit 99; # $(exit 99) `exit 99` & %HASH";
	const wxString name = "name with spaces ' \\ $HOME ; | %FILE";
	const wxString hash = "0123456789";
	const auto args = ExternalCommand::Build(
		"sh -c 'printf \"%s|%s|%s\" \"$1\" \"$2\" \"$3\" > \"$4\"' _ %FILE %HASH %NAME %OUTPUT",
		{ { "%FILE", file }, { "%HASH", hash }, { "%NAME", name }, { "%OUTPUT", output } });
	ASSERT_TRUE(ExternalCommand::RunDetached("hostile positional values", args));
	wxString received;
	for (unsigned attempt = 0; attempt < 100; ++attempt) {
		wxFFile result(output, "rb");
		if (result.IsOpened()) {
			result.ReadAll(&received);
		}
		if (received == file + "|" + hash + "|" + name) {
			break;
		}
		wxMilliSleep(10);
	}
	wxRemoveFile(output);
	ASSERT_EQUALS(file + "|" + hash + "|" + name, received);
}
#endif

TEST(UserEventCommand, FixedWindowsTemplatesPreserveShellSyntax)
{
	for (const wxString &command : { wxString("cmd /c echo done>>C:\\log.txt"),
		     wxString("cmd /c copy a b && del a"),
		     wxString("cmd /c \"echo done\""),
		     wxString("  notify.exe   \"fixed text\"  ") }) {
		bool substituted = true;
		const auto args = ExternalCommand::Build(command,
			{ { "%SENDER", "unused" } },
			&substituted,
			ExternalCommand::Platform::Windows);
		ASSERT_FALSE(args.IsEmpty());
		ASSERT_FALSE(substituted);
		ASSERT_EQUALS(command,
			ExternalCommand::BuildWindowsCommandLine(args, substituted ? nullptr : &command));
	}
	const wxString command = "cmd /c echo %SENDER";
	bool substituted = false;
	const auto args = ExternalCommand::Build(command,
		{ { "%SENDER", "x&echo INJECTED" } },
		&substituted,
		ExternalCommand::Platform::Windows);
	ASSERT_TRUE(substituted);
	ASSERT_EQUALS(ExternalCommand::BuildWindowsCommandLine(args),
		ExternalCommand::BuildWindowsCommandLine(args, substituted ? nullptr : &command));
	const wxString filename = "100% Hits!.mp3";
	const auto preview = ExternalCommand::Build(
		"player.exe", {}, nullptr, ExternalCommand::Platform::Windows, &filename);
	ASSERT_EQUALS("\"player.exe\" \"100% Hits!.mp3\"", ExternalCommand::BuildWindowsCommandLine(preview));
}

TEST(UserEventCommand, WindowsEscapesTrailingBackslashes)
{
	wxArrayString args;
	args.Add("notify.exe");
	args.Add("C:\\folder with spaces\\");
	args.Add("next");
	ASSERT_EQUALS(wxString("\"notify.exe\" \"C:\\folder with spaces\\\\\" \"next\""),
		ExternalCommand::BuildWindowsCommandLine(args));
}

TEST(UserEventCommand, WindowsEscapesBackslashesBeforeQuotes)
{
	wxArrayString args;
	args.Add("notify.exe");
	args.Add("sender\\\" --extra");
	args.Add("");
	ASSERT_EQUALS(wxString("\"notify.exe\" \"sender\\\\\\\" --extra\" \"\""),
		ExternalCommand::BuildWindowsCommandLine(args));
}

TEST(UserEventCommand, WindowsShellQuotingKeepsInnerQuotesAndLiteralBackslashes)
{
	const auto args = ExternalCommand::Build("cmd /c echo %SENDER",
		{ { "%SENDER", "x&echo INJECTED\\" } },
		nullptr,
		ExternalCommand::Platform::Windows);
	ASSERT_EQUALS(wxString("\"cmd\" /d /s /c \"echo \"x&echo INJECTED\\\"\""),
		ExternalCommand::BuildWindowsCommandLine(args));
	const auto batch = ExternalCommand::Build("fixed.cmd %SENDER",
		{ { "%SENDER", "x|echo INJECTED\\" } },
		nullptr,
		ExternalCommand::Platform::Windows);
	ASSERT_TRUE(ExternalCommand::BuildWindowsCommandLine(batch).EndsWith(
		" /d /s /c \"\"fixed.cmd\" \"x|echo INJECTED\\\"\""));
}

TEST(UserEventCommand, CmdAllowsQuotedFixedPathsWithSpacesAndParentheses)
{
	for (const wxString &program : { wxString("C:\\Program Files (x86)\\hooks\\on-chat.cmd"),
		     wxString("C:\\Program Files\\hooks\\on-chat.bat"),
		     wxString("C:\\Program Files\\hooks\\notify.exe"),
		     wxString("C:\\Program Files\\hooks\\notify.com") }) {
		const wxString command = "cmd /c \"" + program + "\" %SENDER";
		const auto args = ExternalCommand::Build(command,
			{ { "%SENDER", "value with spaces & trailing\\" } },
			nullptr,
			ExternalCommand::Platform::Windows);
		ASSERT_EQUALS(size_t(4), args.size());
		ASSERT_EQUALS(program, args[2]);
		const auto serialized = ExternalCommand::BuildWindowsCommandLine(args);
		ASSERT_TRUE(serialized.Contains("\"" + program + "\""));
		const bool batch = program.EndsWith(".cmd") || program.EndsWith(".bat");
		ASSERT_TRUE(serialized.EndsWith(batch ? "trailing\\\"\"" : "trailing\\\\\"\""));
	}
}

TEST(UserEventCommand, CmdRefusesAmbiguousExtensionlessTargetsWhenInsertingData)
{
	for (const wxString &command : { wxString("cmd /c on-chat %SENDER"),
		     wxString("cmd /c C:\\hooks\\on-chat %SENDER"),
		     wxString("cmd /k \"C:\\Program Files\\hooks\\on-chat\" %SENDER"),
		     wxString("cmd /c \"echo fixed\" %SENDER") }) {
		ExternalCommand::RejectionReason reason;
		ASSERT_TRUE(ExternalCommand::Build(command,
			{ { "%SENDER", "trailing\\" } },
			nullptr,
			ExternalCommand::Platform::Windows,
			nullptr,
			&reason)
				    .IsEmpty());
		ASSERT_TRUE(reason == ExternalCommand::RejectionReason::UnsupportedWindowsCommand);
	}
	const wxString filename = "trailing\\";
	ASSERT_TRUE(ExternalCommand::Build(
		"cmd /c on-chat", {}, nullptr, ExternalCommand::Platform::Windows, &filename)
			    .IsEmpty());
	const wxString fixed = "cmd /c on-chat fixed";
	bool substituted = true;
	const auto args = ExternalCommand::Build(fixed, {}, &substituted, ExternalCommand::Platform::Windows);
	ASSERT_FALSE(args.IsEmpty());
	ASSERT_FALSE(substituted);
	ASSERT_EQUALS(fixed, ExternalCommand::BuildWindowsCommandLine(args, &fixed));
}

#ifdef __WINDOWS__
TEST(UserEventCommand, WindowsNativeArgumentRoundTrip)
{
	wxArrayString args;
	args.Add("C:\\Program Files\\notify.exe");
	args.Add("C:\\folder with spaces\\");
	args.Add("sender\\\" --extra");
	args.Add("two\\\\\"quotes\"");
	args.Add("C:\\music\\100% Hits.mp3");
	args.Add("C:\\videos\\Help!.avi");
	args.Add("");
	int count = 0;
	wchar_t **parsed =
		CommandLineToArgvW(ExternalCommand::BuildWindowsCommandLine(args).wc_str(), &count);
	ASSERT_TRUE(parsed != nullptr);
	wxArrayString received;
	for (int i = 0; i < count; ++i) {
		received.Add(parsed[i]);
	}
	LocalFree(parsed);
	ASSERT_EQUALS(args.size(), received.size());
	for (size_t i = 0; i < args.size(); ++i) {
		ASSERT_EQUALS(args[i], received[i]);
	}
}
#endif

TEST(UserEventCommand, PlayerAliasesExpandOnce)
{
	bool substituted = false;
	const auto args = ExternalCommand::Build("player %PARTFILE %PARTNAME $file",
		{ { "%PARTFILE", "x%PARTNAME.avi" },
			{ "%PARTNAME", "name with spaces" },
			{ "$file", "x%PARTNAME.avi" } },
		&substituted,
		ExternalCommand::Platform::Posix);
	ASSERT_TRUE(substituted);
	ASSERT_EQUALS(size_t(4), args.size());
	ASSERT_EQUALS(wxString("x%PARTNAME.avi"), args[1]);
	ASSERT_EQUALS(wxString("name with spaces"), args[2]);
	ASSERT_EQUALS(wxString("x%PARTNAME.avi"), args[3]);
	ExternalCommand::Build("player --fullscreen", {}, &substituted, ExternalCommand::Platform::Posix);
	ASSERT_FALSE(substituted);
}

TEST(UserEventCommand, RejectQuotesBeforeCmdOrBatchCanInterpretThem)
{
	for (const wxString &value : { wxString("x\"&echo INJECTED"),
		     wxString("x\\\"&echo INJECTED"),
		     wxString("\""),
		     wxString("x\"\r\necho INJECTED") }) {
		for (const wxString &command : { wxString("cmd /c echo %SENDER"),
			     wxString("on-chat.bat %SENDER"),
			     wxString("on-chat.cmd fixed%SENDER"),
			     wxString("on-chat.cmd %SENDER %SENDER") }) {
			ASSERT_TRUE(ExternalCommand::Build(command,
				{ { "%SENDER", value } },
				nullptr,
				ExternalCommand::Platform::Windows)
					    .IsEmpty());
		}
	}
	// Unused event values do not affect a command containing only fixed arguments.
	ASSERT_EQUALS(size_t(2),
		ExternalCommand::Build("notify.exe fixed",
			{ { "%SENDER", "x\"&echo INJECTED" } },
			nullptr,
			ExternalCommand::Platform::Windows)
			.size());
}

TEST(UserEventCommand, EmptyArgumentsDoNotSpawn)
{
	ASSERT_FALSE(ExternalCommand::RunDetached("empty", {}));
	wxArrayString args;
	args.Add("");
	args.Add("ignored");
	ASSERT_FALSE(ExternalCommand::RunDetached("empty executable", args));
}

TEST(UserEventCommand, ExecutableCannotComeFromEventData)
{
	for (const auto platform : { ExternalCommand::Platform::Posix, ExternalCommand::Platform::Windows }) {
		bool substituted = true;
		ASSERT_TRUE(ExternalCommand::Build(
			"%SENDER --fixed", { { "%SENDER", "hostile-program" } }, &substituted, platform)
				    .IsEmpty());
		ASSERT_FALSE(substituted);
		ASSERT_TRUE(ExternalCommand::Build(
			"/fixed/%SENDER", { { "%SENDER", "program" } }, nullptr, platform)
				    .IsEmpty());
	}
}

TEST(UserEventCommand, RejectEmbeddedNulBeforeAnyArgumentCanBeTruncated)
{
	wxString value = "before";
	value += wxChar(0);
	value += "after";
	ASSERT_EQUALS(size_t(12), value.length());
	for (const auto platform : { ExternalCommand::Platform::Posix, ExternalCommand::Platform::Windows }) {
		ASSERT_TRUE(
			ExternalCommand::Build("notify %SENDER", { { "%SENDER", value } }, nullptr, platform)
				.IsEmpty());
		ASSERT_TRUE(ExternalCommand::Build("notify " + value, {}, nullptr, platform).IsEmpty());
	}
	wxArrayString args;
	args.Add("notify");
	args.Add(value);
	ASSERT_FALSE(ExternalCommand::RunDetached("NUL value", args));
}

TEST(UserEventCommand, WindowsShellsAndBatchFilesAllowOnlyLiteralData)
{
	for (const wxString &command : { wxString("cmd /c echo %SENDER"),
		     wxString("\"C:\\Windows\\System32\\CMD.EXE\" /d /c echo %SENDER"),
		     wxString("on-chat.BAT %SENDER"),
		     wxString("on-chat.CmD prefix-%SENDER"),
		     wxString("\"on-chat.CmD. \" %SENDER"),
		     wxString("\"CMD.EXE. \" /c echo %SENDER") }) {
		for (const wxString &value :
			{ wxString("safe-looking"), wxString("x&echo INJECTED"), wxString("x|<>^()") }) {
			ASSERT_FALSE(ExternalCommand::Build(command,
				{ { "%SENDER", value } },
				nullptr,
				ExternalCommand::Platform::Windows)
					     .IsEmpty());
		}
		for (const wxString &value : { wxString("%EVIL%"),
			     wxString("!EVIL!"),
			     wxString("x\"&echo INJECTED"),
			     wxString("x\r"),
			     wxString("x\n") }) {
			ASSERT_TRUE(ExternalCommand::Build(command,
				{ { "%SENDER", value } },
				nullptr,
				ExternalCommand::Platform::Windows)
					    .IsEmpty());
		}
	}
	for (const wxString &command : { wxString("cmd /c %SENDER"),
		     wxString("cmd /c \"echo %SENDER\""),
		     wxString("cmd /c \"echo fixed\" %SENDER"),
		     wxString("cmd /s /c echo %SENDER"),
		     wxString("cmd /c powershell -Command %SENDER"),
		     wxString("cmd /c python3 -c %SENDER"),
		     wxString("cmd /c cmd /c %SENDER"),
		     wxString("cmd /c call %SENDER"),
		     wxString("cmd /c start fixed-title %SENDER"),
		     wxString("cmd /c for %SENDER"),
		     wxString("cmd /c if %SENDER"),
		     wxString("cmd /c echo prefix%SENDER%EVIL%"),
		     wxString("powershell -Command %SENDER"),
		     wxString("pwsh -File fixed.ps1 %SENDER") }) {
		ASSERT_TRUE(ExternalCommand::Build(command,
			{ { "%SENDER", "safe-looking" } },
			nullptr,
			ExternalCommand::Platform::Windows)
				    .IsEmpty());
	}
}

TEST(UserEventCommand, NativeWindowsProgramsKeepPercentExclamationAndQuotesLiteral)
{
	for (const wxString &value : { wxString("C:\\music\\100% Hits.mp3"),
		     wxString("C:\\videos\\Help!.avi"),
		     wxString("x\"&echo INJECTED"),
		     wxString("literal & | < > ^ %EVIL% !EVIL!\r\n") }) {
		const auto args = ExternalCommand::Build("player.exe %FILE",
			{ { "%FILE", value } },
			nullptr,
			ExternalCommand::Platform::Windows);
		ASSERT_EQUALS(size_t(2), args.size());
		ASSERT_EQUALS(value, args[1]);
		const auto fallback = ExternalCommand::Build(
			"player.exe", {}, nullptr, ExternalCommand::Platform::Windows, &value);
		ASSERT_EQUALS(size_t(2), fallback.size());
		ASSERT_EQUALS(value, fallback[1]);
	}
}

TEST(UserEventCommand, NativeExeWithCmdInItsFilenameIsNotABatchFile)
{
	const auto args = ExternalCommand::Build("player.cmd.exe %FILE",
		{ { "%FILE", "100% Hits!" } },
		nullptr,
		ExternalCommand::Platform::Windows);
	ASSERT_EQUALS(size_t(2), args.size());
}

TEST(UserEventCommand, RefusalReasonsDistinguishValidationFromLaunchErrors)
{
	using Reason = ExternalCommand::RejectionReason;
	Reason reason = Reason::EmbeddedNul;
	ASSERT_FALSE(ExternalCommand::Build("missing-program.exe %NAME",
		{ { "%NAME", "100% Hits!" } },
		nullptr,
		ExternalCommand::Platform::Windows,
		nullptr,
		&reason)
			     .IsEmpty());
	ASSERT_TRUE(reason == Reason::None); // missing executable is a later launch failure
	ASSERT_TRUE(ExternalCommand::DescribeRejection(reason).empty());
	ASSERT_TRUE(ExternalCommand::Build("%NAME",
		{ { "%NAME", "program" } },
		nullptr,
		ExternalCommand::Platform::Windows,
		nullptr,
		&reason)
			    .IsEmpty());
	ASSERT_TRUE(reason == Reason::ExecutableSubstitution);
	ASSERT_FALSE(ExternalCommand::DescribeRejection(reason).empty());
	ASSERT_TRUE(ExternalCommand::Build("cmd /c echo %NAME",
		{ { "%NAME", "%EVIL%" } },
		nullptr,
		ExternalCommand::Platform::Windows,
		nullptr,
		&reason)
			    .IsEmpty());
	ASSERT_TRUE(reason == Reason::UnsafeWindowsShellValue);
	ASSERT_TRUE(ExternalCommand::Build("sh -c %NAME",
		{ { "%NAME", "echo hostile" } },
		nullptr,
		ExternalCommand::Platform::Posix,
		nullptr,
		&reason)
			    .IsEmpty());
	ASSERT_TRUE(reason == Reason::InterpreterSubstitution);
	wxString nul = "before";
	nul += wxChar(0);
	ASSERT_TRUE(ExternalCommand::Build("notify %NAME",
		{ { "%NAME", nul } },
		nullptr,
		ExternalCommand::Platform::Posix,
		nullptr,
		&reason)
			    .IsEmpty());
	ASSERT_TRUE(reason == Reason::EmbeddedNul);
	ASSERT_TRUE(
		ExternalCommand::Build("", {}, nullptr, ExternalCommand::Platform::Posix, nullptr, &reason)
			.IsEmpty());
	ASSERT_TRUE(reason == Reason::EmptyCommand);
}

#ifdef __WINDOWS__
TEST(UserEventCommand, WindowsShortPathCannotHidePowerShell)
{
	const wxFileName powershell(
		wxGetOSDirectory() + "\\System32\\WindowsPowerShell\\v1.0\\powershell.exe");
	if (powershell.FileExists()) {
		const wxString shortPath = powershell.GetShortPath();
		ASSERT_TRUE(ExternalCommand::Build(
			"\"" + shortPath + "\" -Command %SENDER", { { "%SENDER", "Write-Output INJECTED" } })
				    .IsEmpty());
	}
}

TEST(UserEventCommand, NativeFixedCmdTemplatesRedirectAndChainCommands)
{
	const wxString path = wxFileName::CreateTempFileName("amule-fixed-command-");
	const wxString copy = path + ".copy";
	wxRemoveFile(path);
	for (const wxString &command : { "cmd /c echo done>>\"" + path + "\"",
		     "cmd /c copy /y \"" + path + "\" \"" + copy + "\" >NUL && del \"" + path + "\"" }) {
		bool substituted = true;
		const auto args = ExternalCommand::Build(command, {}, &substituted);
		ASSERT_FALSE(args.IsEmpty());
		ASSERT_FALSE(substituted);
		wxArrayString output, errors;
		ASSERT_EQUALS(0L,
			wxExecute(ExternalCommand::BuildWindowsCommandLine(args, &command),
				output,
				errors,
				wxEXEC_SYNC));
	}
	ASSERT_FALSE(wxFileExists(path));
	wxFFile file(copy, "rb");
	wxString contents;
	ASSERT_TRUE(file.IsOpened());
	ASSERT_TRUE(file.ReadAll(&contents));
	ASSERT_EQUALS("done\r\n", contents);
	file.Close();
	wxRemoveFile(copy);
}

TEST(UserEventCommand, NativeCmdAndBatchCannotExecuteASecondCommandFromData)
{
	wxString cmd;
	if (!wxGetEnv("COMSPEC", &cmd)) {
		cmd = "cmd.exe";
	}
	const wxString temporary = wxFileName::CreateTempFileName("amule event batch (test)-");
	wxFileName batch(temporary);
	batch.SetExt("cmd");
	wxRemoveFile(temporary);
	{
		wxFFile script(batch.GetFullPath(), "wb");
		ASSERT_TRUE(script.IsOpened());
		ASSERT_TRUE(script.Write("@echo off\r\necho \"%~1\"\r\n"));
	}
	for (const wxString &command : { "\"" + cmd + "\" /d /c echo %SENDER",
		     "\"" + batch.GetFullPath() + "\" %SENDER",
		     "\"" + cmd + "\" /d /c \"" + batch.GetFullPath() + "\" %SENDER" }) {
		for (const wxString &value : { wxString("x&echo INJECTED"),
			     wxString("x|echo INJECTED"),
			     wxString("x>NUL"),
			     wxString("x<NUL"),
			     wxString("x^&echo INJECTED"),
			     wxString("x&echo INJECTED\\"),
			     wxString("(x)|echo INJECTED"),
			     wxString("") }) {
			const auto args = ExternalCommand::Build(command, { { "%SENDER", value } });
			ASSERT_FALSE(args.IsEmpty());
			wxArrayString output, errors;
			const long status = wxExecute(
				ExternalCommand::BuildWindowsCommandLine(args), output, errors, wxEXEC_SYNC);
			ASSERT_EQUALS(0L, status);
			ASSERT_EQUALS(size_t(1), output.size());
			ASSERT_EQUALS("\"" + value + "\"", output[0]);
		}
	}
	wxRemoveFile(batch.GetFullPath());
}
#endif

TEST(UserEventCommand, InterpreterCodeAndScriptNamesMustRemainFixed)
{
	for (const wxString &command : { wxString("sh -c %SENDER"),
		     wxString("bash -lc %SENDER"),
		     wxString("sh %SENDER"),
		     wxString("python3 -c %SENDER"),
		     wxString("py -3 -c %SENDER"),
		     wxString("node --eval=%SENDER"),
		     wxString("env sh -c %SENDER"),
		     wxString("busybox sh -c %SENDER") }) {
		ASSERT_TRUE(ExternalCommand::Build(command,
			{ { "%SENDER", "echo INJECTED" } },
			nullptr,
			ExternalCommand::Platform::Posix)
				    .IsEmpty());
	}
	const auto args = ExternalCommand::Build("python3 /fixed/on-chat.py %SENDER",
		{ { "%SENDER", "--eval=hostile & ;" } },
		nullptr,
		ExternalCommand::Platform::Posix);
	ASSERT_EQUALS(size_t(3), args.size());
	ASSERT_EQUALS(wxString("--eval=hostile & ;"), args[2]);
}

TEST(UserEventCommand, PlayerFallbackUsesTheSameSafetyChecksAsPlaceholders)
{
	const wxString target = "/downloads/file with spaces.avi";
	auto args = ExternalCommand::Build(
		"player --fullscreen", {}, nullptr, ExternalCommand::Platform::Posix, &target);
	ASSERT_EQUALS(size_t(3), args.size());
	ASSERT_EQUALS(target, args[2]);
	args = ExternalCommand::Build("player %PARTFILE",
		{ { "%PARTFILE", target } },
		nullptr,
		ExternalCommand::Platform::Posix,
		&target);
	ASSERT_EQUALS(size_t(2), args.size()); // placeholder: do not append again
	ASSERT_TRUE(ExternalCommand::Build("sh -c", {}, nullptr, ExternalCommand::Platform::Posix, &target)
			    .IsEmpty());
	ASSERT_FALSE(ExternalCommand::Build(
		"cmd /c echo", {}, nullptr, ExternalCommand::Platform::Windows, &target)
			     .IsEmpty());
	ASSERT_FALSE(ExternalCommand::Build(
		"on-chat.cmd", {}, nullptr, ExternalCommand::Platform::Windows, &target)
			     .IsEmpty());
	const wxString expansion = "C:\\downloads\\%EVIL%.avi";
	ASSERT_FALSE(ExternalCommand::Build(
		"player.exe", {}, nullptr, ExternalCommand::Platform::Windows, &expansion)
			     .IsEmpty());
	ASSERT_TRUE(ExternalCommand::Build(
		"cmd /c echo", {}, nullptr, ExternalCommand::Platform::Windows, &expansion)
			    .IsEmpty());
	wxString nul = target;
	nul += wxChar(0);
	ASSERT_TRUE(ExternalCommand::Build("player", {}, nullptr, ExternalCommand::Platform::Posix, &nul)
			    .IsEmpty());
}

TEST(UserEventCommand, ReplaceProgramKeepsTheRestAsWritten)
{
	const wxString player = "C:\\Program Files\\VLC\\vlc.exe";
	ASSERT_EQUALS("\"" + player + "\"", ExternalCommand::ReplaceProgram("vlc", player));
	ASSERT_EQUALS("\"" + player + "\" --fullscreen \"%PARTFILE\"",
		ExternalCommand::ReplaceProgram("vlc --fullscreen \"%PARTFILE\"", player));
	// A quoted token ends at the first blank outside the quotes.
	ASSERT_EQUALS(
		"  \"" + player + "\"\tx", ExternalCommand::ReplaceProgram("  \"my player\"\tx", player));
	ASSERT_EQUALS("\"" + player + "\" /c echo done>>C:\\log.txt",
		ExternalCommand::ReplaceProgram("cmd /c echo done>>C:\\log.txt", player));
}

TEST(UserEventCommand, UnquotedProgramPathWithSpacesIsQuoted)
{
	const auto isFile = [](const wxString &path) {
		return path == "C:\\Program Files\\VideoLAN\\VLC\\vlc.exe" || path == "C:\\Tools\\mpv.exe" ||
		       path == "C:\\My  Apps\\a.exe" || path == "C:\\My  Apps\\a.exe b\\c.exe" ||
		       path == "Apps Dir\\p.exe";
	};
	const wxString vlc = "\"C:\\Program Files\\VideoLAN\\VLC\\vlc.exe\"";
	ASSERT_EQUALS(vlc,
		ExternalCommand::QuoteUnquotedProgram("C:\\Program Files\\VideoLAN\\VLC\\vlc.exe", isFile));
	ASSERT_EQUALS(vlc + " --fullscreen \"%PARTFILE\"",
		ExternalCommand::QuoteUnquotedProgram(
			"C:\\Program Files\\VideoLAN\\VLC\\vlc.exe --fullscreen \"%PARTFILE\"", isFile));
	// ".exe" is implied, as CreateProcess does.
	ASSERT_EQUALS("  \"C:\\Program Files\\VideoLAN\\VLC\\vlc\"\t%PARTFILE",
		ExternalCommand::QuoteUnquotedProgram(
			"  C:\\Program Files\\VideoLAN\\VLC\\vlc\t%PARTFILE", isFile));
	// The shortest run wins, and blanks inside it are kept as written.
	ASSERT_EQUALS("\"C:\\My  Apps\\a.exe\" b\\c.exe",
		ExternalCommand::QuoteUnquotedProgram("C:\\My  Apps\\a.exe b\\c.exe", isFile));

	const wxString unchanged[] = {
		"\"C:\\Program Files\\VideoLAN\\VLC\\vlc.exe\" %PARTFILE",
		"C:\\Tools\\mpv.exe --fs %PARTFILE",
		"C:\\Program Files\\Missing\\player.exe %PARTFILE",
		"Apps Dir\\p.exe %PARTFILE", // relative: CreateProcess would search the current directory
		"vlc",
		"",
	};
	for (const wxString &command : unchanged) {
		ASSERT_EQUALS(command, ExternalCommand::QuoteUnquotedProgram(command, isFile));
	}
}

#ifdef __WINDOWS__
TEST(UserEventCommand, BuildFindsAnUnquotedProgramPathWithSpaces)
{
	const TestScratchDir scratch("amule player ");
	const wxString dir = scratch.WxPath();
	const wxString player = wxFileName(dir, "player.exe").GetFullPath();
	ASSERT_TRUE(wxFFile(player, "w").Close());
	const wxString file = "C:\\Videos\\a b.part";
	const wxArrayString args =
		ExternalCommand::Build(player + " --fs %PARTFILE", { { "%PARTFILE", file } }, nullptr);
	ASSERT_EQUALS(3u, args.size());
	ASSERT_EQUALS(player, args[0]);
	ASSERT_EQUALS("--fs", args[1]);
	ASSERT_EQUALS(file, args[2]);
}
#endif

#ifdef __WINDOWS__
TEST(UserEventCommand, BareProgramIsNotTakenFromTheCurrentDirectory)
{
	const TestScratchDir scratch("amule-cwd-");
	const wxString dir = scratch.WxPath();
	const wxString planted = wxFileName(dir, "amule-cwd-probe.exe").GetFullPath();
	ASSERT_TRUE(wxCopyFile(wxGetOSDirectory() + "\\System32\\hostname.exe", planted));
	const wxString previous = wxGetCwd();
	ASSERT_TRUE(wxSetWorkingDirectory(dir));
	const wxString command = "amule-cwd-probe";
	const bool ran = ExternalCommand::RunDetached(command, wxArrayString{ command });
	const bool ranTemplate = ExternalCommand::RunDetached(command, wxArrayString{ command }, &command);
	wxSetWorkingDirectory(previous);
	ASSERT_FALSE(ran);
	ASSERT_FALSE(ranTemplate);
}
#endif
