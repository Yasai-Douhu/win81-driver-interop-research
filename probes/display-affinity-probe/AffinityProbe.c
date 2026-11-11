// VM probe for KxUser Immersive03: a topmost solid red window asks for WDA_EXCLUDEFROMCAPTURE (0x11),
// then reads the affinity back, checks the VxKex property and samples the screen under the window with
// BitBlt (as the KxCom WGC GDI backend does). Built under the Computer Use helper name (opted-in) and a
// control name. Writes <exe>.txt next to itself.
#include <windows.h>
#include <stdio.h>

static LRESULT CALLBACK Proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
	if (m == WM_PAINT) {
		PAINTSTRUCT ps; RECT r; HDC dc = BeginPaint(w, &ps); HBRUSH b = CreateSolidBrush(RGB(255, 0, 0));
		GetClientRect(w, &r); FillRect(dc, &r, b); DeleteObject(b); EndPaint(w, &ps); return 0;
	}
	return DefWindowProcW(w, m, wp, lp);
}

static void Pump(DWORD ms)
{
	DWORD end = GetTickCount() + ms; MSG msg;
	while (GetTickCount() < end) {
		while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
		Sleep(10);
	}
}

static int RedPercent(HWND w)
{
	RECT r; HDC screen, dc; HBITMAP bmp; BITMAPINFO bi; DWORD *px; int i, n, red = 0;
	GetWindowRect(w, &r); n = (r.right - r.left) * (r.bottom - r.top);
	ZeroMemory(&bi, sizeof(bi)); bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = r.right - r.left;
	bi.bmiHeader.biHeight = -(r.bottom - r.top); bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
	screen = GetDC(NULL); dc = CreateCompatibleDC(screen); bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void **) &px, NULL, 0);
	SelectObject(dc, bmp); BitBlt(dc, 0, 0, r.right - r.left, r.bottom - r.top, screen, r.left, r.top, SRCCOPY | CAPTUREBLT); GdiFlush();
	for (i = 0; i < n; ++i) if ((px[i] & 0xffffff) == 0xff0000) ++red;
	DeleteObject(bmp); DeleteDC(dc); ReleaseDC(NULL, screen);
	return n ? red * 100 / n : -1;
}

int wmain(void)
{
	WCHAR path[MAX_PATH]; FILE *out; WNDCLASSW wc = {0}; HWND w; DWORD aff = 0xffff; BOOL set, get;
	GetModuleFileNameW(NULL, path, MAX_PATH); lstrcatW(path, L".txt");
	if (_wfopen_s(&out, path, L"w, ccs=UTF-8") != 0) return 2;
	wc.lpfnWndProc = Proc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"CuaAffinityProbe";
	RegisterClassW(&wc);
	w = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"probe", WS_POPUP | WS_VISIBLE, 100, 100, 200, 200, NULL, NULL, wc.hInstance, NULL);
	Pump(600);
	fwprintf(out, L"before red%%=%d\n", RedPercent(w));
	set = SetWindowDisplayAffinity(w, 0x11);
	fwprintf(out, L"set(0x11)=%d err=%lu\n", set, set ? 0 : GetLastError());
	get = GetWindowDisplayAffinity(w, &aff);
	fwprintf(out, L"get=%d affinity=0x%lx prop=%p\n", get, aff, GetPropW(w, L"VxKex.RequestedExcludeFromCapture"));
	Pump(600);
	fwprintf(out, L"after red%%=%d\n", RedPercent(w));
	SetWindowDisplayAffinity(w, 0);
	GetWindowDisplayAffinity(w, &aff);
	fwprintf(out, L"reset affinity=0x%lx prop=%p\n", aff, GetPropW(w, L"VxKex.RequestedExcludeFromCapture"));
	DestroyWindow(w); fclose(out); return 0;
}
