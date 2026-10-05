#include "pch.h"
#include "resource.h"
#include "Utils.hpp"
#include "RegHelper.hpp"
#include "Framework.hpp"
#include "DwmThumbnailAPI.hpp"
#include "Application.hpp"
#include "MenuHandler.hpp"
#include "TooltipHandler.hpp"
#include "DropDownHandler.hpp"

using namespace TranslucentFlyouts;
namespace TranslucentFlyouts::Framework
{
	std::array g_winEventCallbacks
	{
		MenuHandler::HandleWinEvent,
		TooltipHandler::HandleWinEvent,
		DropDownHandler::HandleWinEvent
	};
	std::array g_startupRoutines
	{
		MenuHandler::Startup,
		TooltipHandler::Startup,
		DropDownHandler::Startup
	};
	std::array g_shutdownRoutines
	{
		MenuHandler::Shutdown,
		TooltipHandler::Shutdown,
		DropDownHandler::Shutdown
	};
	std::array g_prepareRoutines
	{
		MenuHandler::Prepare,
		TooltipHandler::Prepare,
		DropDownHandler::Prepare
	};
	std::array g_updateRoutines
	{
		MenuHandler::Update,
		TooltipHandler::Update,
		DropDownHandler::Update
	};

	bool g_startup{ false };

	// Settings used to be re-read from the registry on every single window event, on the UI thread of every
	// hooked process. Throttle that to a few times a second per thread; a menu re-reads its own settings when it
	// actually opens (MN_SIZEWINDOW), so nothing the user sees is delayed.
	constexpr ULONGLONG g_updateIntervalMs{ 250 };
	void ThrottledUpdate()
	{
		static thread_local ULONGLONG s_lastUpdate{ 0 };
		const auto now{ GetTickCount64() };
		if (now - s_lastUpdate < g_updateIntervalMs)
		{
			return;
		}
		s_lastUpdate = now;
		Update();
	}
}

void CALLBACK Framework::HandleWinEvent(
	HWINEVENTHOOK hWinEventHook, DWORD dwEvent, HWND hWnd,
	LONG idObject, LONG idChild,
	DWORD dwEventThread, DWORD dwmsEventTime
)
{
	// The service host installs the hooks but must never style its own windows. In the default per-process
	// model the host is not hooked at all; this guard still matters in the opt-in legacy global-hook mode,
	// where one in-context hook reaches every process including the host. Explorer-crash handling now lives in
	// ProcessScope::Host (it watches each hooked process's exit code) instead of polling here on every event.
	if (Api::IsHostProcess(Application::g_serviceName))
	{
		return;
	}
	if (!g_startup)
	{
		return;
	}
	ThrottledUpdate();

	DWORD processId{ 0 };
	GetWindowThreadProcessId(hWnd, &processId);
	if(
		idObject != OBJID_WINDOW ||
		idChild != CHILDID_SELF ||
		!hWnd || !IsWindow(hWnd) ||
		processId != GetCurrentProcessId() ||
		dwEventThread != GetCurrentThreadId()
	)
	{
		return;
	}

	for (auto callback : g_winEventCallbacks)
	{
		callback(hWinEventHook, dwEvent, hWnd, idObject, idChild, dwEventThread, dwmsEventTime);
	}
}

void Framework::Startup()
{
	if (g_startup)
	{
		return;
	}

	LOG_IF_FAILED(DwmThumbnailAPI::Initialize());
	for (auto startup : g_startupRoutines)
	{
		startup();
	}

	g_startup = true;
}

void Framework::Shutdown()
{
	if (!g_startup)
	{
		return;
	}

	for (auto shutdown : g_shutdownRoutines)
	{
		shutdown();
	}

	g_startup = false;
}

void Framework::Prepare()
{
	for (auto prepare : g_prepareRoutines)
	{
		prepare();
	}
}

void Framework::Update()
{
	for (auto update : g_updateRoutines)
	{
		update();
	}
}
