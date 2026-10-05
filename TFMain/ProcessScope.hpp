#pragma once
#include "pch.h"

// Decides which processes TranslucentFlyouts loads into, and tracks them from the host.
//
// Before 3.2 the host installed one global in-context WinEvent hook, so TFMain was mapped into every process that
// raised a window event and could only refuse afterwards, in DllMain. That put the DLL inside games, anti-cheat
// protected processes and .NET/Electron apps that never show a Win32 menu. Now the host watches for new windows
// with an out-of-context hook (which maps nothing into other processes) and installs a per-process in-context hook
// only for processes that pass the policy below. The DLL re-checks the same policy before it initialises anything,
// so a mapping that slips through stays inert.
//
// Registry (HKCU\SOFTWARE\TranslucentFlyouts, HKLM as fallback):
//   HookExplorer       DWORD, default 1   style every %SystemRoot%\explorer.exe in the session
//   AllowList\<name>   DWORD != 0         additional processes; <name> is an exe name or a full image path
//   BlockList\<name>   DWORD != 0         never load into this process (pre-existing)
//   LegacyGlobalHook   DWORD, default 0   pre-3.2 behaviour: one global hook into every process (not recommended)
//   ElevatedHost       DWORD, default 0   register the autorun task with highest privileges; only needed to reach
//                                         elevated apps, Explorer-only mode works at medium integrity
namespace TranslucentFlyouts::ProcessScope
{
	// Two abnormal Explorer exits within this window suspend Explorer hooking and ask the user what to do.
	constexpr ULONGLONG g_explorerCrashWindowMs{ 30'000 };

	bool IsLegacyGlobalHook();
	bool IsExplorerHookingEnabled();
	bool IsExplorerImage(std::wstring_view imagePath);
	bool IsGameOrLauncherPath(std::wstring_view imagePath);
	bool IsInUserAllowList(std::wstring_view imagePath);
	// Path-only policy shared by the host (before mapping) and the DLL (before initialising).
	// Returns the reason the process is denied, or nullptr if it is allowed.
	const wchar_t* DenyReasonForImagePath(std::wstring_view imagePath);
	bool IsCurrentProcessAllowed();

	// Host side. Everything here runs on the host's UI thread, the thread that pumps the host window.
	namespace Host
	{
		HRESULT Start(HWND hostWindow, HMODULE hookModule, WINEVENTPROC inContextProc);
		void Stop();
		bool IsRunning();
		HWINEVENTHOOK GetPrimaryHook();
		// Posted to the host window (wParam = process id) when a hooked process exits.
		UINT GetTargetExitedMsg();
		void OnTargetExited(DWORD processId);
		// Re-evaluates every process that owns a top-level window; used at start and on TaskbarCreated.
		void Resync();
	}
}
