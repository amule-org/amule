// SPDX-License-Identifier: GPL-2.0-or-later
#include "sandbox.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#include <cstdio>
#include <string>
#include <vector>

namespace probe::sandbox
{
namespace
{
class Handle
{
public:
	HANDLE value = nullptr;
	~Handle()
	{
		if (value != nullptr && value != INVALID_HANDLE_VALUE)
			CloseHandle(value);
	}
	Handle() = default;
	Handle(const Handle &) = delete;
	Handle &operator=(const Handle &) = delete;
};

class PrivateDesktop
{
public:
	std::wstring name;
	~PrivateDesktop()
	{
		if (desktop != nullptr)
			CloseDesktop(desktop);
		if (station != nullptr)
			CloseWindowStation(station);
	}
	bool create(PSID user, PSID logon)
	{
		// Never grant the restricted token access to the interactive desktop.
		// Runtime initialization needs its own low-integrity desktop instead.
		LPWSTR userText = nullptr, logonText = nullptr;
		if (!ConvertSidToStringSidW(user, &userText))
			return false;
		if (!ConvertSidToStringSidW(logon, &logonText)) {
			LocalFree(userText);
			return false;
		}
		std::wstring sddl = L"D:(A;;GA;;;";
		sddl += userText;
		sddl += L")(A;;GA;;;";
		sddl += logonText;
		sddl += L")(A;;GA;;;S-1-0-0)S:(ML;;NW;;;LW)";
		LocalFree(userText);
		LocalFree(logonText);
		PSECURITY_DESCRIPTOR descriptor = nullptr;
		if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
			    sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
			return false;
		SECURITY_ATTRIBUTES attributes{ sizeof(attributes), descriptor, FALSE };
		const std::wstring stationName = L"AMuleProbe-" + std::to_wstring(GetCurrentProcessId()) +
						 L"-" + std::to_wstring(GetTickCount64());
		station = CreateWindowStationW(stationName.c_str(), 0, WINSTA_ALL_ACCESS, &attributes);
		const auto previous = GetProcessWindowStation();
		bool restored = true;
		if (station != nullptr && previous != nullptr && SetProcessWindowStation(station)) {
			desktop = CreateDesktopW(L"Parser", nullptr, nullptr, 0, GENERIC_ALL, &attributes);
			const DWORD error = GetLastError();
			restored = SetProcessWindowStation(previous) != 0;
			if (restored)
				SetLastError(error);
		}
		const DWORD error = GetLastError();
		LocalFree(descriptor);
		SetLastError(error);
		if (desktop == nullptr || !restored)
			return false;
		name = stationName + L"\\Parser";
		return true;
	}

private:
	HWINSTA station = nullptr;
	HDESK desktop = nullptr;
};

bool tokenInfo(HANDLE token, TOKEN_INFORMATION_CLASS type, std::vector<unsigned char> &buffer)
{
	DWORD size = 0;
	GetTokenInformation(token, type, nullptr, 0, &size);
	if (!size)
		return false;
	buffer.resize(size);
	return GetTokenInformation(token, type, buffer.data(), size, &size) != 0;
}

int failure(const char *stage)
{
	std::fprintf(stderr, "media-probe sandbox: %s failed (Windows error %lu)\n", stage, GetLastError());
	return 12;
}
} // namespace

int launch()
{
	Handle original, lockdown, startupPrimary, startup, job;
	// An internal helper must never leave a loader/crash dialog blocking a probe.
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
	// CreateRestrictedToken preserves this handle's access mask. The resulting
	// primary token needs integrity adjustment and assignment rights at launch.
	if (!OpenProcessToken(GetCurrentProcess(),
		    TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT,
		    &original.value))
		return failure("open startup token");
	std::vector<unsigned char> groupsBuffer, userBuffer, privilegesBuffer;
	if (!tokenInfo(original.value, TokenGroups, groupsBuffer) ||
		!tokenInfo(original.value, TokenUser, userBuffer) ||
		!tokenInfo(original.value, TokenPrivileges, privilegesBuffer))
		return failure("inspect startup token");
	const auto *groups = reinterpret_cast<const TOKEN_GROUPS *>(groupsBuffer.data());
	std::vector<SID_AND_ATTRIBUTES> disabled;
	for (DWORD i = 0; i < groups->GroupCount; ++i) {
		// Integrity labels are set separately. Keep the logon SID for startup.
		if (!(groups->Groups[i].Attributes & (SE_GROUP_INTEGRITY | SE_GROUP_LOGON_ID)))
			disabled.push_back(groups->Groups[i]);
	}
	disabled.push_back(reinterpret_cast<const TOKEN_USER *>(userBuffer.data())->User);
	unsigned char nullSid[SECURITY_MAX_SID_SIZE];
	DWORD nullSize = sizeof(nullSid);
	if (!CreateWellKnownSid(WinNullSid, nullptr, nullSid, &nullSize))
		return failure("create restricting SID");
	SID_AND_ATTRIBUTES restricting{ nullSid, 0 };
	auto *privileges = reinterpret_cast<TOKEN_PRIVILEGES *>(privilegesBuffer.data());
	if (!CreateRestrictedToken(original.value,
		    0,
		    static_cast<DWORD>(disabled.size()),
		    disabled.data(),
		    privileges->PrivilegeCount,
		    privileges->Privileges,
		    1,
		    &restricting,
		    &lockdown.value))
		return failure("create lockdown token");
	unsigned char integritySid[SECURITY_MAX_SID_SIZE];
	DWORD integritySize = sizeof(integritySid);
	// Windows startup needs low integrity; enter() lowers the primary token to
	// untrusted before parsing. Both stages already use the NULL restricting SID.
	if (!CreateWellKnownSid(WinLowLabelSid, nullptr, integritySid, &integritySize))
		return failure("create startup integrity label");
	TOKEN_MANDATORY_LABEL label{ { integritySid, SE_GROUP_INTEGRITY } };
	if (!SetTokenInformation(lockdown.value,
		    TokenIntegrityLevel,
		    &label,
		    static_cast<DWORD>(sizeof(label)) + GetLengthSid(integritySid)))
		return failure("set startup integrity label");
	// The startup impersonation token must itself be restricted when the primary
	// token is restricted. Restrict it to the caller's existing enabled SIDs, so
	// trusted loader initialization retains its normal access without adding any.
	std::vector<SID_AND_ATTRIBUTES> startupSids{
		{ reinterpret_cast<const TOKEN_USER *>(userBuffer.data())->User.Sid, 0 }
	};
	for (DWORD i = 0; i < groups->GroupCount; ++i) {
		if ((groups->Groups[i].Attributes & SE_GROUP_ENABLED) &&
			!(groups->Groups[i].Attributes & (SE_GROUP_INTEGRITY | SE_GROUP_USE_FOR_DENY_ONLY)))
			startupSids.push_back({ groups->Groups[i].Sid, 0 });
	}
	if (!CreateRestrictedToken(original.value,
		    0,
		    0,
		    nullptr,
		    0,
		    nullptr,
		    static_cast<DWORD>(startupSids.size()),
		    startupSids.data(),
		    &startupPrimary.value))
		return failure("create restricted startup token");
	// A higher-integrity impersonation token can be downgraded to identification
	// when assigned to this low-integrity process. Match the primary token here.
	if (!SetTokenInformation(startupPrimary.value,
		    TokenIntegrityLevel,
		    &label,
		    static_cast<DWORD>(sizeof(label)) + GetLengthSid(integritySid)))
		return failure("set startup impersonation integrity");
	if (!DuplicateTokenEx(startupPrimary.value,
		    TOKEN_IMPERSONATE | TOKEN_QUERY,
		    nullptr,
		    SecurityImpersonation,
		    TokenImpersonation,
		    &startup.value))
		return failure("duplicate startup impersonation token");
	PSID logon = nullptr;
	for (DWORD i = 0; i < groups->GroupCount; ++i) {
		if ((groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID)
			logon = groups->Groups[i].Sid;
	}
	PrivateDesktop desktop;
	if (logon == nullptr) {
		SetLastError(ERROR_NO_SUCH_LOGON_SESSION);
		return failure("find parser logon SID");
	}
	if (!desktop.create(reinterpret_cast<const TOKEN_USER *>(userBuffer.data())->User.Sid, logon))
		return failure("create private parser desktop");

	job.value = CreateJobObjectW(nullptr, nullptr);
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
	limits.BasicLimitInformation.LimitFlags =
		JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
		JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
	limits.BasicLimitInformation.ActiveProcessLimit = 1;
	limits.ProcessMemoryLimit = kMemoryLimitBytes;
	JOBOBJECT_BASIC_UI_RESTRICTIONS ui{};
	ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_ALL;
	if (job.value == nullptr ||
		!SetInformationJobObject(
			job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
		!SetInformationJobObject(job.value, JobObjectBasicUIRestrictions, &ui, sizeof(ui)))
		return failure("configure parser job");

	HANDLE inherited[] = { GetStdHandle(STD_INPUT_HANDLE),
		GetStdHandle(STD_OUTPUT_HANDLE),
		GetStdHandle(STD_ERROR_HANDLE) };
	for (const auto handle : inherited) {
		if (handle == nullptr || handle == INVALID_HANDLE_VALUE ||
			!SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
			return failure("prepare inherited handles");
	}
	STARTUPINFOEXW info{};
	info.StartupInfo.cb = sizeof(info);
	info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	info.StartupInfo.hStdInput = inherited[0];
	info.StartupInfo.hStdOutput = inherited[1];
	info.StartupInfo.hStdError = inherited[2];
	info.StartupInfo.lpDesktop = desktop.name.data();
	SIZE_T bytes = 0;
	InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
	std::vector<unsigned char> attributes(bytes);
	info.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
	if (!InitializeProcThreadAttributeList(info.lpAttributeList, 1, 0, &bytes))
		return failure("initialize handle list");
	if (!UpdateProcThreadAttribute(info.lpAttributeList,
		    0,
		    PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
		    inherited,
		    sizeof(inherited),
		    nullptr,
		    nullptr)) {
		DeleteProcThreadAttributeList(info.lpAttributeList);
		return failure("restrict handle inheritance");
	}
	std::vector<wchar_t> executable(32768);
	const DWORD length =
		GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
	std::wstring command = GetCommandLineW();
	command += L" --sandbox-child";
	std::vector<wchar_t> commandBuffer(command.begin(), command.end());
	commandBuffer.push_back(L'\0');
	// The parser needs the Windows runtime location, not the launcher's entire
	// environment (which may include credentials). No other values cross over.
	std::vector<wchar_t> systemRoot(32768);
	const DWORD rootLength = GetEnvironmentVariableW(
		L"SystemRoot", systemRoot.data(), static_cast<DWORD>(systemRoot.size()));
	if (!rootLength || rootLength >= systemRoot.size()) {
		DeleteProcThreadAttributeList(info.lpAttributeList);
		return failure("read Windows runtime location");
	}
	std::wstring environment = L"SystemRoot=";
	environment.append(systemRoot.data(), rootLength);
	environment.push_back(L'\0');
	environment.push_back(L'\0');
	PROCESS_INFORMATION child{};
	// CREATE_NO_WINDOW still allocates a console, whose KERNELBASE startup
	// requires access the primary token deliberately lacks. No console is needed:
	// inherited standard handles work in a detached process too.
	const BOOL created = length && length < executable.size() &&
			     CreateProcessAsUserW(lockdown.value,
				     executable.data(),
				     commandBuffer.data(),
				     nullptr,
				     nullptr,
				     TRUE,
				     CREATE_SUSPENDED | DETACHED_PROCESS | EXTENDED_STARTUPINFO_PRESENT |
					     CREATE_UNICODE_ENVIRONMENT,
				     environment.data(),
				     nullptr,
				     &info.StartupInfo,
				     &child);
	const DWORD spawnError = created ? ERROR_SUCCESS : GetLastError();
	DeleteProcThreadAttributeList(info.lpAttributeList);
	if (!created) {
		SetLastError(spawnError);
		return failure("launch restricted parser");
	}
	Handle process, thread;
	process.value = child.hProcess;
	thread.value = child.hThread;
	// Only trusted loader/runtime initialization uses this impersonation token.
	// enter() removes it before any media is read. It is never inherited as a handle.
	Handle assignedToken;
	SECURITY_IMPERSONATION_LEVEL level = SecurityAnonymous;
	DWORD levelSize = sizeof(level);
	bool confined =
		AssignProcessToJobObject(job.value, process.value) &&
		SetThreadToken(&thread.value, startup.value) &&
		OpenThreadToken(thread.value, TOKEN_QUERY, TRUE, &assignedToken.value) &&
		GetTokenInformation(
			assignedToken.value, TokenImpersonationLevel, &level, sizeof(level), &levelSize);
	if (confined && level < SecurityImpersonation) {
		SetLastError(ERROR_BAD_IMPERSONATION_LEVEL);
		confined = false;
	}
	if (!confined) {
		const int rc = failure("confine suspended parser");
		TerminateProcess(process.value, 12);
		WaitForSingleObject(process.value, 5000);
		return rc;
	}
	if (ResumeThread(thread.value) == DWORD(-1)) {
		const int rc = failure("resume confined parser");
		TerminateProcess(process.value, 12);
		WaitForSingleObject(process.value, 5000);
		return rc;
	}
	if (WaitForSingleObject(process.value, 30000) != WAIT_OBJECT_0) {
		std::fprintf(stderr, "media-probe sandbox: parser exceeded 30 second deadline\n");
		TerminateJobObject(job.value, 12);
		WaitForSingleObject(process.value, 5000);
		return 12;
	}
	DWORD code = 12;
	if (!GetExitCodeProcess(process.value, &code))
		return failure("read parser exit status");
	if (code != 0)
		std::fprintf(stderr, "media-probe sandbox: child exited with Windows status 0x%08lx\n", code);
	return static_cast<int>(code);
}

bool enter()
{
	Handle token;
	std::vector<unsigned char> restricted;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_DEFAULT, &token.value) ||
		!tokenInfo(token.value, TokenRestrictedSids, restricted)) {
		failure("inspect parser primary token");
		return false;
	}
	unsigned char integritySid[SECURITY_MAX_SID_SIZE];
	DWORD integritySize = sizeof(integritySid);
	if (!CreateWellKnownSid(WinUntrustedLabelSid, nullptr, integritySid, &integritySize)) {
		failure("create parser integrity label");
		return false;
	}
	TOKEN_MANDATORY_LABEL label{ { integritySid, SE_GROUP_INTEGRITY } };
	if (!SetTokenInformation(token.value,
		    TokenIntegrityLevel,
		    &label,
		    static_cast<DWORD>(sizeof(label)) + GetLengthSid(integritySid))) {
		failure("lower parser integrity label");
		return false;
	}
	const auto *groups = reinterpret_cast<const TOKEN_GROUPS *>(restricted.data());
	if (groups->GroupCount != 1 || !IsWellKnownSid(groups->Groups[0].Sid, WinNullSid)) {
		SetLastError(ERROR_ACCESS_DENIED);
		failure("verify parser restricting SID");
		return false;
	}
	// Reverting is essential: restricting only an impersonation token could be
	// undone by compromised code. The primary token here is already locked down.
	if (!RevertToSelf()) {
		failure("drop startup impersonation token");
		return false;
	}
	return true;
}
} // namespace probe::sandbox
#endif
