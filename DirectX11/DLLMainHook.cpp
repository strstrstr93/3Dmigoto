#include "DLLMainHook.h"

#include "log.h"
#include "HookedDXGI.h"
#include "globals.h"


// Add in Deviare in-proc for hooking system traps using a Detours approach.  We need access to the
// LoadLibrary call to fix the problem of nvapi.dll bypassing our local patches to the d3d11, when
// it does GetSystemDirectory to get System32, and directly access ..\System32\d3d11.dll
// If we get a failure, we'll just log it, it's not fatal.
//
// Pretty sure this is safe at DLLMain, because we are only accessing kernel32 stuff which is sure
// to be loaded.

// Used for other hooking. extern in the .h file.
// Only one instance of CNktHookLib is allowed for a given process.
CNktHookLib cHookMgr;

// Use this logging when at DLLMain which is too early to do anything with the file system.
#if _DEBUG
bool bLog = true;
#else
bool bLog = false;
#endif

static HMODULE(WINAPI *trampoline_LoadLibraryExW)( _In_ LPCWSTR lpLibFileName,
		_Reserved_ HANDLE hFile, _In_ DWORD dwFlags) = LoadLibraryExW;

static BOOL(WINAPI *trampoline_IsDebuggerPresent)(VOID) = IsDebuggerPresent;


// ----------------------------------------------------------------------------

static HMODULE ReplaceOnMatch(LPCWSTR lpLibFileName, HANDLE hFile,
		DWORD dwFlags, LPCWSTR our_name, LPCWSTR library)
{
	WCHAR fullPath[MAX_PATH];
	UINT ret;

	// We can use System32 for all cases, because it will be properly rerouted
	// to SysWow64 by LoadLibraryEx itself.

	ret = GetSystemDirectoryW(fullPath, ARRAYSIZE(fullPath));
	if (ret == 0 || ret >= ARRAYSIZE(fullPath))
		return NULL;
	wcscat_s(fullPath, MAX_PATH, L"\\");
	wcscat_s(fullPath, MAX_PATH, library);

	// Bypass the known expected call from our wrapped d3d11 & nvapi, where it needs
	// to call to the system to get APIs. This is a bit of a hack, but if the string
	// comes in as original_d3d11/nvapi/nvapi64, that's from us, and needs to switch 
	// to the real one. The test string should have no path attached.
	
	if (_wcsicmp(lpLibFileName, our_name) == 0)
	{
		LogInfoW(L"Hooked_LoadLibraryExW switching to original dll: %s to %s.\n",
			lpLibFileName, fullPath);

		return trampoline_LoadLibraryExW(fullPath, hFile, dwFlags);
	}

	// For this case, we want to see if it's the game loading d3d11 or nvapi directly
	// from the system directory, and redirect it to the game folder if so, by stripping
	// the system path. This is to be case insensitive as we don't know if NVidia will 
	// change that and otherwise break it it with a driver upgrade. 
	
	if (_wcsicmp(lpLibFileName, fullPath) == 0)
	{
		LogInfoW(L"Replaced Hooked_LoadLibraryExW for: %s to %s.\n", lpLibFileName, library);

		return trampoline_LoadLibraryExW(library, hFile, dwFlags);
	}

	return NULL;
}

// Function called for every LoadLibraryExW call once we have hooked it.
// We want to look for overrides to System32 that we can circumvent.  This only happens
// in the current process, not system wide.
// 
// We need to do two things here.  First, we need to bypass all calls that go
// directly to the System32 folder, because that will circumvent our wrapping 
// of the d3d11 and nvapi APIs. The nvapi itself does this specifically as fake
// security to avoid proxy DLLs like us. 
// Second, because we are now forcing all LoadLibraryExW calls back to the game
// folder, we need somehow to allow us access to the original dlls so that we can
// get the original proc addresses to call.  We do this with the original_* names
// passed in to this routine.
//
// There three use cases:
// x32 game on x32 OS
//	 LoadLibraryExW("C:\Windows\system32\d3d11.dll", NULL, 0)
//	 LoadLibraryExW("C:\Windows\system32\nvapi.dll", NULL, 0)
// x64 game on x64 OS
//	 LoadLibraryExW("C:\Windows\system32\d3d11.dll", NULL, 0)
//	 LoadLibraryExW("C:\Windows\system32\nvapi64.dll", NULL, 0)
// x32 game on x64 OS
//	 LoadLibraryExW("C:\Windows\SysWOW64\d3d11.dll", NULL, 0)
//	 LoadLibraryExW("C:\Windows\SysWOW64\nvapi.dll", NULL, 0)
//
// To be general and simplify the init, we are going to specifically do the bypass 
// for all variants, even though we only know of this happening on x64 games.  
//
// An important thing to remember here is that System32 is automatically rerouted
// to SysWow64 by the OS as necessary, so we can use System32 in all cases.
//
// It's not clear if we should also hook LoadLibraryW, but we don't have examples
// where we need that yet.

static HMODULE WINAPI Hooked_LoadLibraryExW(_In_ LPCWSTR lpLibFileName, _Reserved_ HANDLE hFile, _In_ DWORD dwFlags)
{
	HMODULE module;
	static bool hook_enabled = true;

	// This is late enough that we can look for standard logging.
	LogDebugW(L"   Hooked_LoadLibraryExW load: %s.\n", lpLibFileName);

	if (_wcsicmp(lpLibFileName, L"SUPPRESS_3DMIGOTO_REDIRECT") == 0) {
		// Something (like Origin's IGO32.dll hook in ntdll.dll
		// LdrLoadDll) is interfering with our hook and the caller is
		// about to attempt the load again using the full path. Disable
		// our redirect for the next call to make sure we don't give
		// them a reference to themselves. Subsequent calls will be
		// armed again in case we still need the redirect.
		hook_enabled = false;
		return NULL;
	}

	if (hook_enabled) {
		module = ReplaceOnMatch(lpLibFileName, hFile, dwFlags, L"original_d3d11.dll", L"d3d11.dll");
		if (module)
			return module;

		module = ReplaceOnMatch(lpLibFileName, hFile, dwFlags, L"original_nvapi64.dll", L"nvapi64.dll");
		if (module)
			return module;

		module = ReplaceOnMatch(lpLibFileName, hFile, dwFlags, L"original_nvapi.dll", L"nvapi.dll");
		if (module)
			return module;
	} else
		hook_enabled = true;

	// Normal unchanged case.
	return trampoline_LoadLibraryExW(lpLibFileName, hFile, dwFlags);
}


// Function to be called whenever real IsDebuggerPresent is called, so that we can force it to false.

//static BOOL WINAPI Hooked_IsDebuggerPresent()
//{
//	return trampoline_IsDebuggerPresent();
//}

static void LogHooks(bool LogInfo_is_safe, char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);

	if (LogInfo_is_safe)
		vLogInfo(fmt, ap);
	else if (bLog)
		NktHookLibHelpers::DebugVPrint(fmt, ap);

	va_end(ap);
}

static int InstallHook(HINSTANCE module, char *func, void **trampoline, void *hook, bool LogInfo_is_safe)
{
	SIZE_T hook_id;
	DWORD dwOsErr;
	void *fnOrig;

	// Early exit with error so the caller doesn't need to explicitly deal
	// with errors getting the module handle:
	if (!module)
		return 1;

	fnOrig = NktHookLibHelpers::GetProcedureAddress(module, func);
	if (fnOrig == NULL) {
		LogHooks(LogInfo_is_safe, "Failed to get address of %s\n", func);
		return 1;
	}

	dwOsErr = cHookMgr.Hook(&hook_id, trampoline, fnOrig, hook);
	if (dwOsErr) {
		LogHooks(LogInfo_is_safe, "Failed to hook %s: 0x%x\n", func, dwOsErr);
		return 1;
	}

	return 0;
}

static bool InstallHooks()
{
	HINSTANCE hKernel32;
	int fail = 0;

	LogHooks(false, "Attempting to hook LoadLibraryExW using Deviare in-proc.\n");
	cHookMgr.SetEnableDebugOutput(bLog);

	hKernel32 = NktHookLibHelpers::GetModuleBaseAddress(L"Kernel32.dll");
	if (hKernel32 == NULL)
	{
		LogHooks(false, "Failed to get Kernel32 module for Loadlibrary hook.\n");
		return false;
	}

	// Only ExW version for now, used by nvapi.
	fail |= InstallHook(hKernel32, "LoadLibraryExW", (LPVOID*)&trampoline_LoadLibraryExW, Hooked_LoadLibraryExW, false);

	// Next hook IsDebuggerPresent to force it false. Same Kernel32.dll
	// fail |= InstallHook(hKernel32, "IsDebuggerPresent", (LPVOID*)&trampoline_IsDebuggerPresent, Hooked_IsDebuggerPresent, false);

	if (fail)
	{
		LogHooks(false, "InstallHooks for LoadLibraryExW using Deviare in-proc failed\n");
		return false;
	}

	LogHooks(false, "InstallHooks for LoadLibraryExW using Deviare in-proc succeeded\n");

	return true;
}

static BOOL(WINAPI *trampoline_SetWindowPos)(_In_ HWND hWnd, _In_opt_ HWND hWndInsertAfter,
		_In_ int X, _In_ int Y, _In_ int cx, _In_ int cy, _In_ UINT uFlags)
	= SetWindowPos;

static BOOL WINAPI Hooked_SetWindowPos(
    _In_ HWND hWnd,
    _In_opt_ HWND hWndInsertAfter,
    _In_ int X,
    _In_ int Y,
    _In_ int cx,
    _In_ int cy,
    _In_ UINT uFlags)
{
	if (G->SCREEN_UPSCALING != 0) {
		// Force desired upscaled resolution (only when desired resolution is provided!)
		if (cx != 0 && cy != 0) {
			cx = G->SCREEN_WIDTH;
			cy = G->SCREEN_HEIGHT;
			X = 0;
			Y = 0;
		}
	}
	else if (G->SCREEN_FULLSCREEN == 2) {
		// Do nothing - passing this call through could change the game
		// to a borderless window. Needed for The Witness.
		return true;
	}
	
	return trampoline_SetWindowPos(hWnd, hWndInsertAfter, X, Y, cx, cy, uFlags);
}

void InstallSetWindowPosHook()
{
	HINSTANCE hUser32;
	static bool hook_installed = false;
	int fail = 0;

	// Only attempt to hook it once:
	if (hook_installed)
		return;
	hook_installed = true;

	hUser32 = NktHookLibHelpers::GetModuleBaseAddress(L"User32.dll");
	fail |= InstallHook(hUser32, "SetWindowPos", (void**)&trampoline_SetWindowPos, Hooked_SetWindowPos, true);

	if (fail) {
		LogInfo("Failed to hook SetWindowPos for full_screen=2\n");
		BeepFailure2();
		return;
	}

	LogInfo("Successfully hooked SetWindowPos for full_screen=2\n");
	return;
}

//////////////////////////// HARDWARE MOUSE CURSOR SUPPRESSION //////////////////////////
// To suppress the hardware mouse cursor you would think we just have to call
// ShowCursor(FALSE) somewhere to decrement the count by one, but doing so from
// DLL initialization permanently disables the cursor (in Dreamfall Chapters at
// least), and calling it from RunFrameActions or elsewhere has no effect
// (though the call does not indicate an error and does seem to affect a
// counter).
//
// My first attempt to solve this was to hook ShowCursor and keep a separate
// counter for the software curser visibility, but it turns out the Steam
// Overlay also hooks this function, but has a bug where it calls the original
// vs hooked versions inconsistently when showing vs hiding the overlay,
// leading to it reading the visibility count of the *hardware* cursor when the
// overlay is shown, then setting the visibility count of the *software* cursor
// to match when the overlay is hidden, leading to the cursor disappearing.
//
// This is a second attempt to suppress the hardware cursor - this time we
// leave the visibility count alone and instead replace the cursor icon with a
// completely invisible one. Since the hardware cursor technically is
// displayed, the visibility counts for software and hardware cursors match, so
// we no longer need to manage them separately. We hook into SetCursor,
// GetCursor and GetCursorInfo to keep a handle of the cursor the game set and
// return it whenever something (including our own software mouse
// implementation) asks for it.

HCURSOR current_cursor = NULL;

typedef LRESULT(WINAPI *lpfnDefWindowProc)(_In_ HWND hWnd,
	_In_ UINT Msg, _In_ WPARAM wParam, _In_ LPARAM lParam);

static lpfnDefWindowProc trampoline_DefWindowProcA = DefWindowProcA;
static lpfnDefWindowProc trampoline_DefWindowProcW = DefWindowProcW;

static HCURSOR(WINAPI *trampoline_SetCursor)(_In_opt_ HCURSOR hCursor) = SetCursor;
static HCURSOR(WINAPI *trampoline_GetCursor)(void) = GetCursor;
static BOOL(WINAPI *trampoline_GetCursorInfo)(_Inout_ PCURSORINFO pci) = GetCursorInfo;
static BOOL(WINAPI* trampoline_SetCursorPos)(_In_ int X, _In_ int Y) = SetCursorPos;
static BOOL(WINAPI* trampoline_GetCursorPos)(_Out_ LPPOINT lpPoint) = GetCursorPos;
static BOOL(WINAPI* trampoline_ScreenToClient)(_In_ HWND hWnd,LPPOINT lpPoint) = ScreenToClient;
static BOOL(WINAPI* trampoline_GetClientRect)(_In_ HWND hWnd, _Out_ LPRECT lpRect) = GetClientRect;

// This routine creates an invisible cursor that we can set whenever we are
// hiding the cursor. It is static, so will only be created the first time this
// is called.
static HCURSOR InvisibleCursor()
{
	static HCURSOR cursor = NULL;
	int width, height;
	unsigned pitch, size;
	char *and, *xor;

	if (!cursor) {
		width = GetSystemMetrics(SM_CXCURSOR);
		height = GetSystemMetrics(SM_CYCURSOR);
		pitch = ((width + 31) / 32) * 4;
		size = pitch * height;

		and = new char[size];
		xor = new char[size];

		memset(and, 0xff, size);
		memset(xor, 0x00, size);

		cursor = CreateCursor(GetModuleHandle(NULL), 0, 0, width, height, and, xor);

		delete [] and;
		delete [] xor;
	}

	return cursor;
}

// We hook the SetCursor call so that we can catch the current cursor that the
// game has set and return it in the GetCursorInfo call whenever the software
// cursor is visible but the hardware cursor is not.
static HCURSOR WINAPI Hooked_SetCursor(
    _In_opt_ HCURSOR hCursor)
{
	current_cursor = hCursor;

	if (G->hide_cursor)
		return trampoline_SetCursor(InvisibleCursor());
	else
		return trampoline_SetCursor(hCursor);
}

static HCURSOR WINAPI Hooked_GetCursor(void)
{
	if (G->hide_cursor)
		return current_cursor;
	else
		return trampoline_GetCursor();
}

static BOOL WINAPI HideCursor_GetCursorInfo(
    _Inout_ PCURSORINFO pci)
{
	BOOL rc = trampoline_GetCursorInfo(pci);

	if (rc && (pci->flags & CURSOR_SHOWING))
		pci->hCursor = current_cursor;

	return rc;
}

static BOOL WINAPI Hooked_GetCursorInfo(
    _Inout_ PCURSORINFO pci)
{
	BOOL rc = HideCursor_GetCursorInfo(pci);
	RECT client;

	if (rc && G->SCREEN_UPSCALING > 0 && trampoline_GetClientRect(G->hWnd, &client) && client.right && client.bottom)
	{
		pci->ptScreenPos.x = pci->ptScreenPos.x * G->GAME_INTERNAL_WIDTH / client.right;
		pci->ptScreenPos.y = pci->ptScreenPos.y * G->GAME_INTERNAL_HEIGHT / client.bottom;
	}

	return rc;
}

BOOL WINAPI CursorUpscalingBypass_GetCursorInfo(
    _Inout_ PCURSORINFO pci)
{
	if (G->cursor_upscaling_bypass)
	{
		// Still need to process hide_cursor logic:
		return HideCursor_GetCursorInfo(pci);
	}
	return GetCursorInfo(pci);
}

static BOOL WINAPI Hooked_ScreenToClient(_In_ HWND hWnd, LPPOINT lpPoint)
{
	BOOL rc;
	RECT client;
	bool translate = G->SCREEN_UPSCALING > 0 && lpPoint
		&& trampoline_GetClientRect(G->hWnd, &client)
		&& client.right && client.bottom
		&& G->GAME_INTERNAL_WIDTH && G->GAME_INTERNAL_HEIGHT;

	if (translate)
	{
		// Scale back to original screen coordinates:
		lpPoint->x = lpPoint->x * client.right / G->GAME_INTERNAL_WIDTH;
		lpPoint->y = lpPoint->y * client.bottom / G->GAME_INTERNAL_HEIGHT;
	}

	rc = trampoline_ScreenToClient(hWnd, lpPoint);

	if (translate)
	{
		// Now scale to fake game coordinates:
		lpPoint->x = lpPoint->x * G->GAME_INTERNAL_WIDTH / client.right;
		lpPoint->y = lpPoint->y * G->GAME_INTERNAL_HEIGHT / client.bottom;
	}

	return rc;
}

BOOL WINAPI CursorUpscalingBypass_ScreenToClient(_In_ HWND hWnd, LPPOINT lpPoint)
{
	if (G->cursor_upscaling_bypass)
		return trampoline_ScreenToClient(hWnd, lpPoint);
	return ScreenToClient(hWnd, lpPoint);
}

static BOOL WINAPI Hooked_GetClientRect(_In_ HWND hWnd, _Out_ LPRECT lpRect)
{
	BOOL rc = trampoline_GetClientRect(hWnd, lpRect);

	if (G->upscaling_hooks_armed && rc && G->SCREEN_UPSCALING > 0 && lpRect != NULL)
	{
		lpRect->right = G->GAME_INTERNAL_WIDTH;
		lpRect->bottom = G->GAME_INTERNAL_HEIGHT;
	}

	return rc;
}

BOOL WINAPI CursorUpscalingBypass_GetClientRect(_In_ HWND hWnd, _Out_ LPRECT lpRect)
{
	if (G->cursor_upscaling_bypass)
		return trampoline_GetClientRect(hWnd, lpRect);
	return GetClientRect(hWnd, lpRect);
}

static BOOL WINAPI Hooked_GetCursorPos(_Out_ LPPOINT lpPoint)
{
	BOOL res = trampoline_GetCursorPos(lpPoint);
	RECT client;

	if (lpPoint && res && G->SCREEN_UPSCALING > 0 && trampoline_GetClientRect(G->hWnd, &client) && client.right && client.bottom)
	{
		// This should work with all games that uses this function to gatter the mouse coords
		// Tested with witcher 3 and dreamfall chapters
		// TODO: Maybe there is a better way than use globals for the original game resolution
		lpPoint->x = lpPoint->x * G->GAME_INTERNAL_WIDTH / client.right;
		lpPoint->y = lpPoint->y * G->GAME_INTERNAL_HEIGHT / client.bottom;
	}

	return res;
}

static BOOL WINAPI Hooked_SetCursorPos(_In_ int X, _In_ int Y)
{
	RECT client;

	if (G->SCREEN_UPSCALING > 0 && trampoline_GetClientRect(G->hWnd, &client) && G->GAME_INTERNAL_WIDTH && G->GAME_INTERNAL_HEIGHT)
	{
		// TODO: Maybe there is a better way than use globals for the original game resolution
		const int new_x = X * client.right / G->GAME_INTERNAL_WIDTH;
		const int new_y = Y * client.bottom / G->GAME_INTERNAL_HEIGHT;
		return trampoline_SetCursorPos(new_x, new_y);
	}
	else
		return trampoline_SetCursorPos(X, Y);
}

// DefWindowProc can bypass our SetCursor hook, which means that some games
// such would continue showing the hardware cursor, and our knowledge of what
// cursor was supposed to be set may be inaccurate (e.g. Akiba's Trip doesn't
// hide the cursor and sometimes the software cursor uses the busy cursor
// instead of the arrow cursor). We fix this by hooking DefWindowProc and
// processing WM_SETCURSOR message just as the original DefWindowProc would
// have done, but without bypassing our SetCursor hook.
//
// An alternative to hooking DefWindowProc in this manner might be to use
// SetWindowsHookEx since it can also hook window messages.
static LRESULT WINAPI Hooked_DefWindowProc(
	_In_ HWND   hWnd,
	_In_ UINT   Msg,
	_In_ WPARAM wParam,
	_In_ LPARAM lParam,
	lpfnDefWindowProc trampoline_DefWindowProc)
{

	HWND parent = NULL;
	HCURSOR cursor = NULL;
	LPARAM ret = 0;

	if (Msg == WM_SETCURSOR) {
		// XXX: Should we use GetParent or GetAncestor? GetParent can
		// return an "owner" window, while GetAncestor only returns
		// parents... Not sure which the official DefWindowProc uses,
		// but I suspect the answer is GetAncestor, so go with that:
		parent = GetAncestor(hWnd, GA_PARENT);

		if (parent) {
			// Pass the message to the parent window, just like the
			// real DefWindowProc does. This may call back in here
			// if the parent also doesn't handle this message, and
			// we stop processing if the parent handled it.
			ret = SendMessage(parent, Msg, wParam, lParam);
			if (ret)
				return ret;
		}

		// If the mouse is in the client area and the window class has
		// a cursor associated with it we set that. This will call into
		// our hooked version of SetCursor (whereas the real
		// DefWindowProc would bypass that) so that we can track the
		// current cursor set by the game and force the hardware cursor
		// to remain hidden.
		if ((lParam & 0xffff) == HTCLIENT) {
			cursor = (HCURSOR)GetClassLongPtr(hWnd, GCLP_HCURSOR);
			if (cursor)
				SetCursor(cursor);
		} else {
			// Not in client area. We could continue emulating
			// DefWindowProc by setting an arrow cursor, bypassing
			// our hook to set the *real* hardware cursor, but
			// since the real DefWindowProc already bypasses our
			// hook let's just call that and allow it to take care
			// of any other edge cases we may not know about (like
			// HTERROR):
			return trampoline_DefWindowProc(hWnd, Msg, wParam, lParam);
		}

		// Return false to allow children to set their class cursor:
		return FALSE;
	}

	return trampoline_DefWindowProc(hWnd, Msg, wParam, lParam);
}

static LRESULT WINAPI Hooked_DefWindowProcA(_In_ HWND hWnd, _In_ UINT Msg, _In_ WPARAM wParam, _In_ LPARAM lParam)
{
	return Hooked_DefWindowProc(hWnd, Msg, wParam, lParam, trampoline_DefWindowProcA);
}

static LRESULT WINAPI Hooked_DefWindowProcW(_In_ HWND hWnd, _In_ UINT Msg, _In_ WPARAM wParam, _In_ LPARAM lParam)
{
	return Hooked_DefWindowProc(hWnd, Msg, wParam, lParam, trampoline_DefWindowProcW);
}

void InstallMouseHooks(bool hide)
{
	HINSTANCE hUser32;
	static bool hook_installed = false;
	int fail = 0;

	// Only attempt to hook it once:
	if (hook_installed)
		return;
	hook_installed = true;

	// Init our handle to the current cursor now before installing the
	// hooks, and from now on it will be kept up to date from SetCursor:
	current_cursor = GetCursor();
	if (hide)
		SetCursor(InvisibleCursor());

	hUser32 = NktHookLibHelpers::GetModuleBaseAddress(L"User32.dll");
	fail |= InstallHook(hUser32, "SetCursor", (void**)&trampoline_SetCursor, Hooked_SetCursor, true);
	fail |= InstallHook(hUser32, "GetCursor", (void**)&trampoline_GetCursor, Hooked_GetCursor, true);
	fail |= InstallHook(hUser32, "GetCursorInfo", (void**)&trampoline_GetCursorInfo, Hooked_GetCursorInfo, true);
	fail |= InstallHook(hUser32, "DefWindowProcA", (void**)&trampoline_DefWindowProcA, Hooked_DefWindowProcA, true);
	fail |= InstallHook(hUser32, "DefWindowProcW", (void**)&trampoline_DefWindowProcW, Hooked_DefWindowProcW, true);
	fail |= InstallHook(hUser32, "SetCursorPos", (void**)&trampoline_SetCursorPos, Hooked_SetCursorPos, true);
	fail |= InstallHook(hUser32, "GetCursorPos", (void**)&trampoline_GetCursorPos, Hooked_GetCursorPos, true);
	fail |= InstallHook(hUser32, "ScreenToClient", (void**)&trampoline_ScreenToClient, Hooked_ScreenToClient, true);
	fail |= InstallHook(hUser32, "GetClientRect", (void**)&trampoline_GetClientRect, Hooked_GetClientRect, true);

	if (fail) {
		LogInfo("Failed to hook mouse cursor functions - hide_cursor will not work\n");
		BeepFailure2();
		return;
	}

	LogInfo("Successfully hooked mouse cursor functions for hide_cursor\n");
}

static void RemoveHooks()
{
	cHookMgr.UnhookAll();
}


// Now doing hooking for every build, x32 and x64.  Not positive this is the
// best idea, but so far it's been stable and does not seem to introduce any
// problems. The original thought was that this was only useful for some games.
//
// The fact that we are hooking to install DXGI now makes this necessary for 
// all builds.

BOOL WINAPI DllMain(
	_In_  HINSTANCE hinstDLL,
	_In_  DWORD fdwReason,
	_In_  LPVOID lpvReserved)
{
	bool result = true;

	switch (fdwReason)
	{
		case DLL_PROCESS_ATTACH:
			InstallDXGIHooks();
			result = InstallHooks();
			break;

		case DLL_PROCESS_DETACH:
			RemoveHooks();
			break;

		case DLL_THREAD_ATTACH:
			// Do thread-specific initialization.
			break;

		case DLL_THREAD_DETACH:
			// Do thread-specific cleanup.
			break;
	}

	return result;
}
