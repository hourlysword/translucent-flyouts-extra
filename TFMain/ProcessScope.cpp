#include "pch.h"
#include "resource.h"
#include "Utils.hpp"
#include "RegHelper.hpp"
#include "Application.hpp"
#include "ProcessScope.hpp"

using namespace TranslucentFlyouts;
using namespace std::literals;

namespace TranslucentFlyouts::ProcessScope
{
	namespace
	{
		std::wstring ToLower(std::wstring_view text)
		{
			std::wstring result{ text };
			if (!result.empty())
			{
				CharLowerBuffW(result.data(), static_cast<DWORD>(result.size()));
			}
			return result;
		}

		std::wstring_view BaseName(std::wstring_view path)
		{
			auto pos{ path.find_last_of(L"\\/") };
			return pos == std::wstring_view::npos ? path : path.substr(pos + 1);
		}

		const std::wstring& ExplorerImagePathLower()
		{
			static const std::wstring s_path
			{
				[]
				{
					WCHAR windowsDirectory[MAX_PATH + 1]{};
					GetWindowsDirectoryW(windowsDirectory, MAX_PATH);
					return ToLower(std::wstring{ windowsDirectory } + L"\\explorer.exe");
				} ()
			};
			return s_path;
		}

		// Folders that hold games, their launchers or their anti-cheat services. A Win32 menu is never worth
		// loading into one of these, and the loaded DLL would be blocked or flagged anyway. Matched against the
		// lower-cased full image path.
		constexpr std::array g_gamePathFragments
		{
			L"\\steamapps\\common\\"sv,
			L"\\riot games\\"sv,
			L"\\riot vanguard\\"sv,
			L"\\epic games\\"sv,
			L"\\easyanticheat"sv,
			L"\\battleye"sv,
			L"\\battle.net\\"sv,
			L"\\ubisoft\\"sv,
			L"\\ubisoft game launcher\\"sv,
			L"\\ea games\\"sv,
			L"\\electronic arts\\"sv,
			L"\\origin games\\"sv,
			L"\\gog galaxy\\games\\"sv,
			L"\\gog games\\"sv,
			L"\\wegame\\"sv,
			L"\\xboxgames\\"sv,
			L"\\hoyoverse\\"sv,
			L"\\mihoyo\\"sv,
			L"\\genshin impact"sv,
			L"\\star rail"sv,
			L"\\honkai"sv,
			L"\\rockstar games\\"sv,
			L"\\faceit\\"sv,
		};

		bool MatchesAllowListUnder(HKEY root, std::wstring_view lowerPath)
		{
			wil::unique_hkey key;
			if (RegOpenKeyExW(root, L"Software\\TranslucentFlyouts\\AllowList", 0, KEY_QUERY_VALUE, key.put()) != ERROR_SUCCESS)
			{
				return false;
			}

			auto lowerName{ BaseName(lowerPath) };
			for (DWORD index{ 0 };; index++)
			{
				WCHAR valueName[MAX_PATH + 1]{};
				DWORD nameLength{ _countof(valueName) };
				DWORD type{ 0 };
				DWORD data{ 0 };
				DWORD dataSize{ sizeof(data) };
				auto status{ RegEnumValueW(key.get(), index, valueName, &nameLength, nullptr, &type, reinterpret_cast<BYTE*>(&data), &dataSize) };
				if (status == ERROR_NO_MORE_ITEMS)
				{
					break;
				}
				if (status == ERROR_MORE_DATA)
				{
					continue;	// not a DWORD; skip it
				}
				if (status != ERROR_SUCCESS)
				{
					break;
				}
				if (type != REG_DWORD || data == 0)
				{
					continue;
				}

				auto lowerValue{ ToLower(std::wstring_view{ valueName, nameLength }) };
				bool isFullPath{ lowerValue.find(L'\\') != std::wstring::npos };
				if (isFullPath ? (lowerValue == lowerPath) : (lowerValue == lowerName))
				{
					return true;
				}
			}

			return false;
		}
	}
}

bool ProcessScope::IsLegacyGlobalHook()
{
	return RegHelper::Get<DWORD>({}, L"LegacyGlobalHook", 0) != 0;
}

bool ProcessScope::IsExplorerHookingEnabled()
{
	return RegHelper::Get<DWORD>({}, L"HookExplorer", 1) != 0;
}

bool ProcessScope::IsExplorerImage(std::wstring_view imagePath)
{
	return ToLower(imagePath) == ExplorerImagePathLower();
}

bool ProcessScope::IsGameOrLauncherPath(std::wstring_view imagePath)
{
	auto lower{ ToLower(imagePath) };
	// "X:\Games\..." at the root of any drive.
	if (lower.size() > 3 && lower[1] == L':' && lower.compare(2, 7, L"\\games\\") == 0)
	{
		return true;
	}
	for (auto fragment : g_gamePathFragments)
	{
		if (lower.find(fragment) != std::wstring::npos)
		{
			return true;
		}
	}
	return false;
}

bool ProcessScope::IsInUserAllowList(std::wstring_view imagePath)
{
	auto lower{ ToLower(imagePath) };
	return MatchesAllowListUnder(HKEY_CURRENT_USER, lower) || MatchesAllowListUnder(HKEY_LOCAL_MACHINE, lower);
}

const wchar_t* ProcessScope::DenyReasonForImagePath(std::wstring_view imagePath)
{
	if (imagePath.empty())
	{
		return L"unknown image path";
	}
	if (IsGameOrLauncherPath(imagePath))
	{
		return L"game or launcher folder";
	}
	if (RegHelper::Get<DWORD>({ L"BlockList" }, std::wstring{ BaseName(imagePath) }, 0) != 0)
	{
		return L"BlockList";
	}
	if (IsExplorerImage(imagePath))
	{
		return IsExplorerHookingEnabled() ? nullptr : L"HookExplorer is 0";
	}
	if (IsInUserAllowList(imagePath))
	{
		return nullptr;
	}
	return L"not in AllowList";
}

bool ProcessScope::IsCurrentProcessAllowed()
{
	if (IsLegacyGlobalHook())
	{
		return true;
	}
	WCHAR imagePath[MAX_PATH + 1]{};
	GetModuleFileNameW(nullptr, imagePath, MAX_PATH);
	return DenyReasonForImagePath(imagePath) == nullptr;
}

// ---------------------------------------------------------------------------------------------------------------
// Host side
// ---------------------------------------------------------------------------------------------------------------

namespace TranslucentFlyouts::ProcessScope::Host
{
	namespace
	{
		struct Target
		{
			wil::unique_handle process;
			HWINEVENTHOOK hook{ nullptr };
			HANDLE wait{ nullptr };
			bool isExplorer{ false };
		};

		HWND g_hostWindow{ nullptr };
		HMODULE g_hookModule{ nullptr };
		WINEVENTPROC g_inContextProc{ nullptr };
		HWINEVENTHOOK g_watcherHook{ nullptr };
		HWINEVENTHOOK g_legacyHook{ nullptr };

		std::unordered_map<DWORD, Target> g_targets{};
		// Process id -> creation time of the process that was judged. A recycled id has a different creation time
		// and is judged again.
		std::unordered_map<DWORD, ULONGLONG> g_decided{};

		DWORD g_sessionId{ 0 };
		USHORT g_hostMachine{ IMAGE_FILE_MACHINE_UNKNOWN };
		DWORD g_hostIntegrity{ 0 };
		std::vector<BYTE> g_hostUserSid{};

		std::vector<ULONGLONG> g_explorerCrashTimes{};
		bool g_explorerSuspended{ false };

		ULONGLONG CreationTimeOf(HANDLE process)
		{
			FILETIME creation{}, exit{}, kernel{}, user{};
			if (!GetProcessTimes(process, &creation, &exit, &kernel, &user))
			{
				return 0;
			}
			return (static_cast<ULONGLONG>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
		}

		std::wstring ImagePathOf(HANDLE process)
		{
			WCHAR path[MAX_PATH * 2]{};
			DWORD size{ _countof(path) };
			if (!QueryFullProcessImageNameW(process, 0, path, &size))
			{
				return {};
			}
			return std::wstring{ path, size };
		}

		USHORT MachineOf(HANDLE process)
		{
			using IsWow64Process2_t = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
			static const auto s_isWow64Process2{ reinterpret_cast<IsWow64Process2_t>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2")) };

			USHORT processMachine{ IMAGE_FILE_MACHINE_UNKNOWN }, nativeMachine{ IMAGE_FILE_MACHINE_UNKNOWN };
			if (s_isWow64Process2 && s_isWow64Process2(process, &processMachine, &nativeMachine))
			{
				return processMachine == IMAGE_FILE_MACHINE_UNKNOWN ? nativeMachine : processMachine;
			}
			BOOL wow64{ FALSE };
			IsWow64Process(process, &wow64);
			return wow64 ? IMAGE_FILE_MACHINE_I386 : IMAGE_FILE_MACHINE_AMD64;
		}

		struct TokenFacts
		{
			bool ok{ false };
			DWORD integrity{ 0 };
			bool appContainer{ false };
			std::vector<BYTE> userSid{};
		};

		TokenFacts TokenFactsOf(HANDLE process)
		{
			TokenFacts facts{};
			wil::unique_handle token;
			if (!OpenProcessToken(process, TOKEN_QUERY, token.put()))
			{
				return facts;
			}

			DWORD length{ 0 };
			BYTE labelBuffer[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE]{};
			if (!GetTokenInformation(token.get(), TokenIntegrityLevel, labelBuffer, sizeof(labelBuffer), &length))
			{
				return facts;
			}
			auto label{ reinterpret_cast<TOKEN_MANDATORY_LABEL*>(labelBuffer) };
			facts.integrity = *GetSidSubAuthority(label->Label.Sid, *GetSidSubAuthorityCount(label->Label.Sid) - 1);

			DWORD isAppContainer{ 0 };
			if (GetTokenInformation(token.get(), TokenIsAppContainer, &isAppContainer, sizeof(isAppContainer), &length))
			{
				facts.appContainer = isAppContainer != 0;
			}

			BYTE userBuffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]{};
			if (!GetTokenInformation(token.get(), TokenUser, userBuffer, sizeof(userBuffer), &length))
			{
				return facts;
			}
			auto user{ reinterpret_cast<TOKEN_USER*>(userBuffer) };
			auto sid{ reinterpret_cast<BYTE*>(user->User.Sid) };
			facts.userSid.assign(sid, sid + GetLengthSid(user->User.Sid));
			facts.ok = true;
			return facts;
		}

		bool AlreadyDecided(DWORD processId, ULONGLONG creationTime)
		{
			auto it{ g_decided.find(processId) };
			return it != g_decided.end() && it->second == creationTime;
		}

		void CALLBACK OnWaitSatisfied(PVOID context, BOOLEAN /*timedOut*/)
		{
			// Threadpool thread: hand over to the host thread, which owns all of the state above.
			PostMessageW(g_hostWindow, GetTargetExitedMsg(), reinterpret_cast<WPARAM>(context), 0);
		}

		void Consider(DWORD processId)
		{
			if (!processId || processId == GetCurrentProcessId() || g_targets.contains(processId))
			{
				return;
			}

			wil::unique_handle process{ OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, processId) };
			if (!process)
			{
				g_decided[processId] = 0;
				return;
			}
			auto creationTime{ CreationTimeOf(process.get()) };
			if (AlreadyDecided(processId, creationTime))
			{
				return;
			}
			g_decided[processId] = creationTime;

			// Hard denies first. Each one is a process we could not safely or meaningfully load into.
			DWORD sessionId{ 0 };
			if (!ProcessIdToSessionId(processId, &sessionId) || sessionId != g_sessionId)
			{
				return;
			}
			if (MachineOf(process.get()) != g_hostMachine)
			{
				return;	// the host of the other bitness takes care of it
			}
			auto token{ TokenFactsOf(process.get()) };
			if (!token.ok || token.appContainer || token.integrity > g_hostIntegrity)
			{
				return;
			}
			if (token.userSid.size() != g_hostUserSid.size() || !EqualSid(token.userSid.data(), g_hostUserSid.data()))
			{
				return;	// a different user's process in this session ("Run as different user")
			}

			auto imagePath{ ImagePathOf(process.get()) };
			if (auto reason{ DenyReasonForImagePath(imagePath) })
			{
#ifdef _DEBUG
				OutputDebugStringW(std::format(L"[TranslucentFlyouts] skip {} ({}): {}\n", processId, imagePath, reason).c_str());
#endif
				return;
			}

			bool isExplorer{ IsExplorerImage(imagePath) };
			if (isExplorer && g_explorerSuspended)
			{
				return;
			}

			Target target{};
			target.process = std::move(process);
			target.isExplorer = isExplorer;
			// The DLL is mapped into this process the first time one of its threads raises an event in this range.
			target.hook = SetWinEventHook(
				EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE,
				g_hookModule,
				g_inContextProc,
				processId, 0,
				WINEVENT_INCONTEXT
			);
			if (!target.hook)
			{
				LOG_LAST_ERROR();
				return;
			}
			if (!RegisterWaitForSingleObject(&target.wait, target.process.get(), OnWaitSatisfied, reinterpret_cast<PVOID>(static_cast<ULONG_PTR>(processId)), INFINITE, WT_EXECUTEONLYONCE))
			{
				LOG_LAST_ERROR();
				UnhookWinEvent(target.hook);
				return;
			}
			g_targets.emplace(processId, std::move(target));
		}

		void CALLBACK WatcherProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG idChild, DWORD eventThread, DWORD)
		{
			// Out-of-context: delivered on the host thread, nothing is mapped into the process that raised it.
			if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF)
			{
				return;
			}
			DWORD processId{ 0 };
			GetWindowThreadProcessId(hwnd, &processId);
			if (!processId && eventThread)
			{
				// The window may already be gone; the thread id still identifies the process.
				wil::unique_handle thread{ OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, eventThread) };
				if (thread)
				{
					processId = GetProcessIdOfThread(thread.get());
				}
			}
			Consider(processId);
		}

		void NoteExplorerCrash()
		{
			auto now{ GetTickCount64() };
			std::erase_if(g_explorerCrashTimes, [now](ULONGLONG t) { return now - t > g_explorerCrashWindowMs; });
			g_explorerCrashTimes.push_back(now);
			if (g_explorerCrashTimes.size() < 2)
			{
				return;
			}
			g_explorerCrashTimes.clear();

			// Stop touching new Explorer instances until the user decides; this replaces the old check that polled
			// GetShellWindow() from inside every hook callback.
			g_explorerSuspended = true;
			if (
				MessageBoxW(
					nullptr,
					Utils::GetResWString<IDS_STRING109>().c_str(),
					nullptr,
					MB_ICONERROR | MB_SYSTEMMODAL | MB_SERVICE_NOTIFICATION | MB_SETFOREGROUND | MB_YESNO
				) == IDYES
			)
			{
				std::thread{ [] { Application::StopService(); } }.detach();
			}
			else
			{
				g_explorerSuspended = false;
				Resync();
			}
		}
	}
}

HRESULT ProcessScope::Host::Start(HWND hostWindow, HMODULE hookModule, WINEVENTPROC inContextProc)
{
	RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), IsRunning());
	RETURN_HR_IF_NULL(E_INVALIDARG, hookModule);
	RETURN_HR_IF_NULL(E_INVALIDARG, inContextProc);

	g_hostWindow = hostWindow;
	g_hookModule = hookModule;
	g_inContextProc = inContextProc;

	if (IsLegacyGlobalHook())
	{
		g_legacyHook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE, hookModule, inContextProc, 0, 0, WINEVENT_INCONTEXT);
		RETURN_LAST_ERROR_IF_NULL(g_legacyHook);
		return S_OK;
	}

	ProcessIdToSessionId(GetCurrentProcessId(), &g_sessionId);
	g_hostMachine = MachineOf(GetCurrentProcess());
	auto facts{ TokenFactsOf(GetCurrentProcess()) };
	RETURN_HR_IF(E_ACCESSDENIED, !facts.ok);
	g_hostIntegrity = facts.integrity;
	g_hostUserSid = facts.userSid;

	g_watcherHook = SetWinEventHook(
		EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE,
		nullptr,
		WatcherProc,
		0, 0,
		WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
	);
	RETURN_LAST_ERROR_IF_NULL(g_watcherHook);

	Resync();
	return S_OK;
}

void ProcessScope::Host::Stop()
{
	for (auto& [processId, target] : g_targets)
	{
		if (target.wait)
		{
			UnregisterWaitEx(target.wait, INVALID_HANDLE_VALUE);
		}
		if (target.hook)
		{
			UnhookWinEvent(target.hook);
		}
	}
	g_targets.clear();
	g_decided.clear();

	if (g_watcherHook)
	{
		UnhookWinEvent(g_watcherHook);
		g_watcherHook = nullptr;
	}
	if (g_legacyHook)
	{
		UnhookWinEvent(g_legacyHook);
		g_legacyHook = nullptr;
	}
	g_hostWindow = nullptr;
}

bool ProcessScope::Host::IsRunning()
{
	return g_watcherHook != nullptr || g_legacyHook != nullptr;
}

HWINEVENTHOOK ProcessScope::Host::GetPrimaryHook()
{
	return g_watcherHook ? g_watcherHook : g_legacyHook;
}

UINT ProcessScope::Host::GetTargetExitedMsg()
{
	static const UINT s_message{ RegisterWindowMessageW(L"TranslucentFlyouts.Host.TargetExited") };
	return s_message;
}

void ProcessScope::Host::OnTargetExited(DWORD processId)
{
	auto it{ g_targets.find(processId) };
	if (it == g_targets.end())
	{
		return;
	}
	auto target{ std::move(it->second) };
	g_targets.erase(it);

	if (target.wait)
	{
		// The callback has already run (WT_EXECUTEONLYONCE); this only releases the wait object.
		UnregisterWaitEx(target.wait, INVALID_HANDLE_VALUE);
	}
	DWORD exitCode{ 0 };
	GetExitCodeProcess(target.process.get(), &exitCode);
	if (target.hook)
	{
		UnhookWinEvent(target.hook);
	}
	// 'target.process' closes when 'target' goes out of scope, after the unhook, so the id cannot be recycled
	// while a hook still names it.

	// NTSTATUS error-severity exit codes (0xC0000005 and friends) mean a crash; "Restart Explorer" exits cleanly.
	if (target.isExplorer && (exitCode & 0xC0000000) == 0xC0000000)
	{
		NoteExplorerCrash();
	}
}

void ProcessScope::Host::Resync()
{
	if (!g_watcherHook)
	{
		return;
	}
	EnumWindows([](HWND hwnd, LPARAM) -> BOOL
	{
		DWORD processId{ 0 };
		GetWindowThreadProcessId(hwnd, &processId);
		Consider(processId);
		return TRUE;
	}, 0);
}
