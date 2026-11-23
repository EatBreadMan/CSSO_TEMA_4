/* t4_gui.cpp - Native Win32 GUI Coordinator for CSSO Tema 4
 * FII UAIC · CSSO 2026-2027 · Assignment 4 GUI Bonus (R11, G1-G5)
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <windows.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "t4_engine.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "shell32.lib")

/* Control IDs */
enum ControlIDs {
	ID_EDIT_DAYS = 101,
	ID_BTN_BROWSE_DAYS = 102,
	ID_EDIT_RESULTS = 103,
	ID_BTN_BROWSE_RESULTS = 104,
	ID_EDIT_WORKERS = 105,
	ID_SPIN_WORKERS = 106,
	ID_EDIT_DELAY = 107,
	ID_SPIN_DELAY = 108,
	ID_EDIT_CAPACITY = 109,
	ID_SPIN_CAPACITY = 110,
	ID_CHECK_EXPERIMENT = 111,
	ID_EDIT_REPETITIONS = 112,
	ID_SPIN_REPETITIONS = 113,
	ID_BTN_START = 114,
	ID_BTN_STOP = 115,
	ID_BTN_KILL = 116,
	ID_BTN_CLEAR_LOG = 117,
	ID_LISTVIEW = 118,
	ID_PROGRESS = 119,
	ID_EDIT_LOG = 120,
	ID_LABEL_DAY_STATUS = 121
};

/* Custom Win32 Messages for Thread -> UI Communication */
enum AppMessages {
	WM_APP_LOG_LINE = WM_APP + 1,       /* lParam: wchar_t* (heap allocated, freed by window proc) */
	WM_APP_WORKER_STARTED = WM_APP + 2, /* wParam: worker_id, lParam: pid */
	WM_APP_DAY_START = WM_APP + 3,      /* wParam: day, lParam: total_jobs */
	WM_APP_PROGRESS = WM_APP + 4,       /* lParam: T4_PROGRESS_MSG* (heap allocated, freed by window proc) */
	WM_APP_DAY_DONE = WM_APP + 5,       /* wParam: day, lParam: duration_ms */
	WM_APP_WORKER_DEAD = WM_APP + 6,    /* wParam: worker_id, lParam: pid */
	WM_APP_FINISHED = WM_APP + 7        /* wParam: exit_code */
};

typedef struct {
	int day;
	uint32_t done_jobs;
	uint32_t total_jobs;
	uint32_t worker_done[T4_MAX_WORKERS + 1];
} T4_PROGRESS_MSG;

/* Global GUI handles and state */
static HWND g_hWnd = NULL;
static HWND g_hEditDays = NULL;
static HWND g_hBtnBrowseDays = NULL;
static HWND g_hEditResults = NULL;
static HWND g_hBtnBrowseResults = NULL;
static HWND g_hEditWorkers = NULL;
static HWND g_hSpinWorkers = NULL;
static HWND g_hEditDelay = NULL;
static HWND g_hSpinDelay = NULL;
static HWND g_hEditCapacity = NULL;
static HWND g_hSpinCapacity = NULL;
static HWND g_hCheckExp = NULL;
static HWND g_hEditReps = NULL;
static HWND g_hSpinReps = NULL;
static HWND g_hBtnStart = NULL;
static HWND g_hBtnStop = NULL;
static HWND g_hBtnKill = NULL;
static HWND g_hBtnClearLog = NULL;
static HWND g_hLabelDay = NULL;
static HWND g_hProgressBar = NULL;
static HWND g_hListView = NULL;
static HWND g_hEditLog = NULL;

static HFONT g_hFontUI = NULL;
static HFONT g_hFontLog = NULL;

static HANDLE g_hCoordThread = NULL;
static volatile BOOL g_bRunning = FALSE;
static DWORD g_worker_pids[T4_MAX_WORKERS + 1];
static int g_num_workers = 4;

/* Forward declarations */
static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static void DoBrowseFolder(HWND hEdit, const wchar_t* title);
static DWORD WINAPI CoordinatorThreadProc(LPVOID lpParam);

/* Engine Callbacks (called from coordinator background thread)
 * Strictly use PostMessage with documented ownership:
 * The background thread allocates data, the window procedure frees after processing.
 */
static void engine_cb_worker_started(void* ctx, int worker_id, DWORD pid)
{
	HWND hwnd = (HWND)ctx;
	PostMessageW(hwnd, WM_APP_WORKER_STARTED, (WPARAM)worker_id, (LPARAM)pid);
}

static void engine_cb_day_start(void* ctx, int day, uint32_t total_jobs)
{
	HWND hwnd = (HWND)ctx;
	PostMessageW(hwnd, WM_APP_DAY_START, (WPARAM)day, (LPARAM)total_jobs);
}

static void engine_cb_progress(void* ctx, int day, uint32_t done_jobs, uint32_t total_jobs, const uint32_t* worker_done_counts)
{
	HWND hwnd = (HWND)ctx;
	T4_PROGRESS_MSG* msg = (T4_PROGRESS_MSG*)malloc(sizeof(T4_PROGRESS_MSG));
	if (!msg) return;
	msg->day = day;
	msg->done_jobs = done_jobs;
	msg->total_jobs = total_jobs;
	memset(msg->worker_done, 0, sizeof(msg->worker_done));
	if (worker_done_counts) {
		memcpy(msg->worker_done, worker_done_counts, sizeof(msg->worker_done));
	}
	PostMessageW(hwnd, WM_APP_PROGRESS, 0, (LPARAM)msg);
}

static void engine_cb_day_done(void* ctx, int day, uint32_t jobs, unsigned long long duration_ms)
{
	HWND hwnd = (HWND)ctx;
	(void)jobs;
	PostMessageW(hwnd, WM_APP_DAY_DONE, (WPARAM)day, (LPARAM)duration_ms);
}

static void engine_cb_worker_dead(void* ctx, int worker_id, DWORD pid)
{
	HWND hwnd = (HWND)ctx;
	PostMessageW(hwnd, WM_APP_WORKER_DEAD, (WPARAM)worker_id, (LPARAM)pid);
}

static void engine_cb_log_line(void* ctx, const char* line)
{
	HWND hwnd = (HWND)ctx;
	if (!line) return;
	int len = MultiByteToWideChar(CP_UTF8, 0, line, -1, NULL, 0);
	if (len <= 0) return;
	wchar_t* wline = (wchar_t*)malloc(len * sizeof(wchar_t));
	if (!wline) return;
	MultiByteToWideChar(CP_UTF8, 0, line, -1, wline, len);
	PostMessageW(hwnd, WM_APP_LOG_LINE, 0, (LPARAM)wline);
}

static void engine_cb_finished(void* ctx, int exit_code)
{
	HWND hwnd = (HWND)ctx;
	PostMessageW(hwnd, WM_APP_FINISHED, (WPARAM)exit_code, 0);
}

/* WinMain entry point */
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
	(void)hPrevInstance; (void)lpCmdLine;

	/* G5: Per-Monitor v2 DPI awareness */
	typedef BOOL(WINAPI* PFN_SetProcessDpiAwarenessContext)(DPI_AWARENESS_CONTEXT);
	HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
	if (hUser32) {
		PFN_SetProcessDpiAwarenessContext pfnSetDpi =
			(PFN_SetProcessDpiAwarenessContext)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
		if (pfnSetDpi) {
			pfnSetDpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
		}
	}

	/* Initialize Common Controls (ListView, ProgressBar, UpDown) */
	INITCOMMONCONTROLSEX icex;
	icex.dwSize = sizeof(icex);
	icex.dwICC = ICC_WIN95_CLASSES | ICC_PROGRESS_CLASS | ICC_LISTVIEW_CLASSES | ICC_UPDOWN_CLASS;
	InitCommonControlsEx(&icex);

	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

	WNDCLASSEXW wc = { 0 };
	wc.cbSize = sizeof(wc);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInstance;
	wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	wc.lpszClassName = L"CSSO_T4_GUI_MAIN";
	if (!RegisterClassExW(&wc)) return 1;

	/* Fonts */
	NONCLIENTMETRICSW ncm = { sizeof(ncm) };
	SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
	g_hFontUI = CreateFontIndirectW(&ncm.lfMessageFont);
	g_hFontLog = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

	HWND hwnd = CreateWindowExW(
		0,
		L"CSSO_T4_GUI_MAIN",
		L"CSSO T4 - Interprocess Work Queue Coordinator",
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, 980, 720,
		NULL, NULL, hInstance, NULL);

	if (!hwnd) return 1;
	g_hWnd = hwnd;

	ShowWindow(hwnd, nCmdShow);
	UpdateWindow(hwnd);

	MSG msg;
	while (GetMessageW(&msg, NULL, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	if (g_hFontUI) DeleteObject(g_hFontUI);
	if (g_hFontLog) DeleteObject(g_hFontLog);
	CoUninitialize();
	return (int)msg.wParam;
}

/* Helper to set default UI font on controls */
static void SetFont(HWND hCtrl)
{
	if (hCtrl && g_hFontUI) {
		SendMessageW(hCtrl, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
	}
}

/* Folder Picker using IFileOpenDialog with FOS_PICKFOLDERS (G1) */
static void DoBrowseFolder(HWND hEdit, const wchar_t* title)
{
	IFileOpenDialog* pFileOpen = NULL;
	HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, (void**)&pFileOpen);
	if (SUCCEEDED(hr)) {
		DWORD dwOptions;
		if (SUCCEEDED(pFileOpen->GetOptions(&dwOptions))) {
			pFileOpen->SetOptions(dwOptions | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
		}
		if (title) pFileOpen->SetTitle(title);
		if (SUCCEEDED(pFileOpen->Show(g_hWnd))) {
			IShellItem* pItem = NULL;
			if (SUCCEEDED(pFileOpen->GetResult(&pItem))) {
				PWSTR pszPath = NULL;
				if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) {
					SetWindowTextW(hEdit, pszPath);
					CoTaskMemFree(pszPath);
				}
				pItem->Release();
			}
		}
		pFileOpen->Release();
	}
}

/* Append text to multiline EDIT without reloading all text (G2) */
static void AppendLogText(const wchar_t* text)
{
	if (!g_hEditLog || !text) return;
	int len = GetWindowTextLengthW(g_hEditLog);
	SendMessageW(g_hEditLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
	SendMessageW(g_hEditLog, EM_REPLACESEL, FALSE, (LPARAM)text);
}

/* Refresh / Reset ListView columns and rows for K workers */
static void SetupWorkersListView(int K)
{
	ListView_DeleteAllItems(g_hListView);
	for (int i = 1; i <= K; i++) {
		wchar_t buf[32];
		LVITEMW item = { 0 };
		item.mask = LVIF_TEXT;
		item.iItem = i - 1;
		item.iSubItem = 0;
		swprintf(buf, 32, L"W%d", i);
		item.pszText = buf;
		ListView_InsertItem(g_hListView, &item);

		ListView_SetItemText(g_hListView, i - 1, 1, (LPWSTR)L"-");
		ListView_SetItemText(g_hListView, i - 1, 2, (LPWSTR)L"0");
		ListView_SetItemText(g_hListView, i - 1, 3, (LPWSTR)L"Waiting");
	}
}

/* Coordinator Background Thread */
static DWORD WINAPI CoordinatorThreadProc(LPVOID lpParam)
{
	T4_COORD_CONFIG* pCfg = (T4_COORD_CONFIG*)lpParam;
	int rc = t4_engine_run(pCfg);
	free(pCfg);
	return (DWORD)rc;
}

/* Layout Management on WM_SIZE (G5) */
static void LayoutControls(int width, int height)
{
	if (!g_hEditDays) return;

	int m = 12;            /* margin */
	int labelW = 90;
	int btnW = 85;
	int rowH = 25;
	int spacing = 8;
	int curY = m;

	/* Row 1: Days dir */
	MoveWindow(GetDlgItem(g_hWnd, 201), m, curY + 3, labelW, rowH, TRUE);
	int editW = width - (m * 2 + labelW + btnW + spacing * 2);
	MoveWindow(g_hEditDays, m + labelW + spacing, curY, editW, rowH, TRUE);
	MoveWindow(g_hBtnBrowseDays, m + labelW + spacing + editW + spacing, curY, btnW, rowH, TRUE);
	curY += rowH + spacing;

	/* Row 2: Results dir */
	MoveWindow(GetDlgItem(g_hWnd, 202), m, curY + 3, labelW, rowH, TRUE);
	MoveWindow(g_hEditResults, m + labelW + spacing, curY, editW, rowH, TRUE);
	MoveWindow(g_hBtnBrowseResults, m + labelW + spacing + editW + spacing, curY, btnW, rowH, TRUE);
	curY += rowH + spacing;

	/* Row 3: Parameters */
	int paramX = m;
	/* Workers */
	MoveWindow(GetDlgItem(g_hWnd, 203), paramX, curY + 3, 60, rowH, TRUE); paramX += 65;
	MoveWindow(g_hEditWorkers, paramX, curY, 45, rowH, TRUE);
	MoveWindow(g_hSpinWorkers, paramX + 45, curY, 18, rowH, TRUE); paramX += 75;

	/* Delay */
	MoveWindow(GetDlgItem(g_hWnd, 204), paramX, curY + 3, 65, rowH, TRUE); paramX += 70;
	MoveWindow(g_hEditDelay, paramX, curY, 55, rowH, TRUE);
	MoveWindow(g_hSpinDelay, paramX + 55, curY, 18, rowH, TRUE); paramX += 85;

	/* Capacity */
	MoveWindow(GetDlgItem(g_hWnd, 205), paramX, curY + 3, 60, rowH, TRUE); paramX += 65;
	MoveWindow(g_hEditCapacity, paramX, curY, 45, rowH, TRUE);
	MoveWindow(g_hSpinCapacity, paramX + 45, curY, 18, rowH, TRUE); paramX += 75;

	/* Experiment Checkbox */
	MoveWindow(g_hCheckExp, paramX, curY + 2, 160, rowH, TRUE); paramX += 165;

	/* Repetitions */
	MoveWindow(GetDlgItem(g_hWnd, 206), paramX, curY + 3, 40, rowH, TRUE); paramX += 45;
	MoveWindow(g_hEditReps, paramX, curY, 40, rowH, TRUE);
	MoveWindow(g_hSpinReps, paramX + 40, curY, 18, rowH, TRUE);

	curY += rowH + spacing;

	/* Row 4: Action Buttons */
	int actX = m;
	MoveWindow(g_hBtnStart, actX, curY, 100, 30, TRUE); actX += 110;
	MoveWindow(g_hBtnStop, actX, curY, 100, 30, TRUE); actX += 110;
	MoveWindow(g_hBtnKill, actX, curY, 150, 30, TRUE); actX += 160;
	MoveWindow(g_hBtnClearLog, width - m - 90, curY, 90, 30, TRUE);
	curY += 35 + spacing;

	/* Row 5: Day status and Progress bar */
	MoveWindow(g_hLabelDay, m, curY, width - 2 * m, 20, TRUE);
	curY += 22;
	MoveWindow(g_hProgressBar, m, curY, width - 2 * m, 24, TRUE);
	curY += 24 + spacing;

	/* Split area: Left = ListView, Right = Edit Log */
	int bottomH = height - curY - m;
	if (bottomH < 120) bottomH = 120;
	int leftW = (width - 2 * m - spacing) * 42 / 100;
	if (leftW < 330) leftW = 330;
	int rightW = width - 2 * m - spacing - leftW;

	MoveWindow(g_hListView, m, curY, leftW, bottomH, TRUE);
	MoveWindow(g_hEditLog, m + leftW + spacing, curY, rightW, bottomH, TRUE);

	/* Adjust ListView columns */
	int col0 = 65, col1 = 75, col2 = 105;
	int col3 = leftW - (col0 + col1 + col2 + 25);
	if (col3 < 70) col3 = 70;
	ListView_SetColumnWidth(g_hListView, 0, col0);
	ListView_SetColumnWidth(g_hListView, 1, col1);
	ListView_SetColumnWidth(g_hListView, 2, col2);
	ListView_SetColumnWidth(g_hListView, 3, col3);
}

/* Main Window Procedure */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg) {
	case WM_CREATE: {
		HINSTANCE hInst = ((LPCREATESTRUCT)lParam)->hInstance;

		/* Row 1: Days dir */
		HWND hLbl1 = CreateWindowW(L"STATIC", L"Days Dir:", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)201, hInst, NULL);
		g_hEditDays = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"C:\\T4\\days", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd, (HMENU)ID_EDIT_DAYS, hInst, NULL);
		g_hBtnBrowseDays = CreateWindowW(L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, (HMENU)ID_BTN_BROWSE_DAYS, hInst, NULL);

		/* Row 2: Results dir */
		HWND hLbl2 = CreateWindowW(L"STATIC", L"Results Dir:", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)202, hInst, NULL);
		g_hEditResults = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"C:\\Faculty\\CSSO\\T4\\Results", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd, (HMENU)ID_EDIT_RESULTS, hInst, NULL);
		g_hBtnBrowseResults = CreateWindowW(L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, (HMENU)ID_BTN_BROWSE_RESULTS, hInst, NULL);

		/* Row 3: Parameters */
		HWND hLbl3 = CreateWindowW(L"STATIC", L"Workers:", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)203, hInst, NULL);
		g_hEditWorkers = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"4", WS_CHILD | WS_VISIBLE | ES_NUMBER, 0, 0, 0, 0, hwnd, (HMENU)ID_EDIT_WORKERS, hInst, NULL);
		g_hSpinWorkers = CreateWindowW(UPDOWN_CLASS, NULL, WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS, 0, 0, 0, 0, hwnd, (HMENU)ID_SPIN_WORKERS, hInst, NULL);
		SendMessageW(g_hSpinWorkers, UDM_SETBUDDY, (WPARAM)g_hEditWorkers, 0);
		SendMessageW(g_hSpinWorkers, UDM_SETRANGE32, 1, 16);
		SendMessageW(g_hSpinWorkers, UDM_SETPOS32, 0, 4);

		HWND hLbl4 = CreateWindowW(L"STATIC", L"Delay (ms):", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)204, hInst, NULL);
		g_hEditDelay = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0", WS_CHILD | WS_VISIBLE | ES_NUMBER, 0, 0, 0, 0, hwnd, (HMENU)ID_EDIT_DELAY, hInst, NULL);
		g_hSpinDelay = CreateWindowW(UPDOWN_CLASS, NULL, WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS, 0, 0, 0, 0, hwnd, (HMENU)ID_SPIN_DELAY, hInst, NULL);
		SendMessageW(g_hSpinDelay, UDM_SETBUDDY, (WPARAM)g_hEditDelay, 0);
		SendMessageW(g_hSpinDelay, UDM_SETRANGE32, 0, 5000);
		SendMessageW(g_hSpinDelay, UDM_SETPOS32, 0, 0);

		HWND hLbl5 = CreateWindowW(L"STATIC", L"Capacity:", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)205, hInst, NULL);
		g_hEditCapacity = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"16", WS_CHILD | WS_VISIBLE | ES_NUMBER, 0, 0, 0, 0, hwnd, (HMENU)ID_EDIT_CAPACITY, hInst, NULL);
		g_hSpinCapacity = CreateWindowW(UPDOWN_CLASS, NULL, WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS, 0, 0, 0, 0, hwnd, (HMENU)ID_SPIN_CAPACITY, hInst, NULL);
		SendMessageW(g_hSpinCapacity, UDM_SETBUDDY, (WPARAM)g_hEditCapacity, 0);
		SendMessageW(g_hSpinCapacity, UDM_SETRANGE32, 1, 16);
		SendMessageW(g_hSpinCapacity, UDM_SETPOS32, 0, 16);

		g_hCheckExp = CreateWindowW(L"BUTTON", L"Experiment E1 Mode", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, (HMENU)ID_CHECK_EXPERIMENT, hInst, NULL);

		HWND hLbl6 = CreateWindowW(L"STATIC", L"Reps:", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)206, hInst, NULL);
		g_hEditReps = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"5", WS_CHILD | WS_VISIBLE | ES_NUMBER, 0, 0, 0, 0, hwnd, (HMENU)ID_EDIT_REPETITIONS, hInst, NULL);
		g_hSpinReps = CreateWindowW(UPDOWN_CLASS, NULL, WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS, 0, 0, 0, 0, hwnd, (HMENU)ID_SPIN_REPETITIONS, hInst, NULL);
		SendMessageW(g_hSpinReps, UDM_SETBUDDY, (WPARAM)g_hEditReps, 0);
		SendMessageW(g_hSpinReps, UDM_SETRANGE32, 1, 30);
		SendMessageW(g_hSpinReps, UDM_SETPOS32, 0, 5);

		/* Row 4: Buttons */
		g_hBtnStart = CreateWindowW(L"BUTTON", L"▶ Start", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, (HMENU)ID_BTN_START, hInst, NULL);
		g_hBtnStop = CreateWindowW(L"BUTTON", L"⏹ Stop", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED, 0, 0, 0, 0, hwnd, (HMENU)ID_BTN_STOP, hInst, NULL);
		g_hBtnKill = CreateWindowW(L"BUTTON", L"☠ Kill Selected Worker", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED, 0, 0, 0, 0, hwnd, (HMENU)ID_BTN_KILL, hInst, NULL);
		g_hBtnClearLog = CreateWindowW(L"BUTTON", L"Clear Log", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, (HMENU)ID_BTN_CLEAR_LOG, hInst, NULL);

		/* Row 5: Day Progress and Status */
		g_hLabelDay = CreateWindowW(L"STATIC", L"Status: Ready to start.", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)ID_LABEL_DAY_STATUS, hInst, NULL);
		g_hProgressBar = CreateWindowExW(0, PROGRESS_CLASS, NULL, WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 0, 0, 0, 0, hwnd, (HMENU)ID_PROGRESS, hInst, NULL);

		/* Bottom: ListView & Multiline Log Edit */
		g_hListView = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
			WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | WS_BORDER,
			0, 0, 0, 0, hwnd, (HMENU)ID_LISTVIEW, hInst, NULL);
		ListView_SetExtendedListViewStyle(g_hListView, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

		LVCOLUMNW col = { 0 };
		col.mask = LVCF_TEXT | LVCF_WIDTH;
		col.pszText = (LPWSTR)L"Worker"; col.cx = 65; ListView_InsertColumn(g_hListView, 0, &col);
		col.pszText = (LPWSTR)L"PID"; col.cx = 75; ListView_InsertColumn(g_hListView, 1, &col);
		col.pszText = (LPWSTR)L"Completed"; col.cx = 105; ListView_InsertColumn(g_hListView, 2, &col);
		col.pszText = (LPWSTR)L"Status"; col.cx = 90; ListView_InsertColumn(g_hListView, 3, &col);

		g_hEditLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
			WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL | WS_BORDER,
			0, 0, 0, 0, hwnd, (HMENU)ID_EDIT_LOG, hInst, NULL);
		SendMessageW(g_hEditLog, EM_SETLIMITTEXT, 0, 0);

		/* Set Fonts */
		SetFont(hLbl1); SetFont(g_hEditDays); SetFont(g_hBtnBrowseDays);
		SetFont(hLbl2); SetFont(g_hEditResults); SetFont(g_hBtnBrowseResults);
		SetFont(hLbl3); SetFont(g_hEditWorkers); SetFont(g_hSpinWorkers);
		SetFont(hLbl4); SetFont(g_hEditDelay); SetFont(g_hSpinDelay);
		SetFont(hLbl5); SetFont(g_hEditCapacity); SetFont(g_hSpinCapacity);
		SetFont(g_hCheckExp);
		SetFont(hLbl6); SetFont(g_hEditReps); SetFont(g_hSpinReps);
		SetFont(g_hBtnStart); SetFont(g_hBtnStop); SetFont(g_hBtnKill); SetFont(g_hBtnClearLog);
		SetFont(g_hLabelDay); SetFont(g_hListView);

		if (g_hFontLog) {
			SendMessageW(g_hEditLog, WM_SETFONT, (WPARAM)g_hFontLog, TRUE);
		}

		SetupWorkersListView(4);
		return 0;
	}

	case WM_SIZE: {
		int w = LOWORD(lParam);
		int h = HIWORD(lParam);
		LayoutControls(w, h);
		return 0;
	}

	case WM_COMMAND: {
		int wmId = LOWORD(wParam);
		switch (wmId) {
		case ID_BTN_BROWSE_DAYS:
			DoBrowseFolder(g_hEditDays, L"Select Days Directory");
			break;

		case ID_BTN_BROWSE_RESULTS:
			DoBrowseFolder(g_hEditResults, L"Select Results Directory");
			break;

		case ID_CHECK_EXPERIMENT: {
			BOOL isExp = (SendMessageW(g_hCheckExp, BM_GETCHECK, 0, 0) == BST_CHECKED);
			if (isExp) {
				SetWindowTextW(g_hEditDays, L"C:\\T4\\throughput");
				SetWindowTextW(g_hEditResults, L"C:\\Faculty\\CSSO\\T4\\Experiments");
			}
			else {
				SetWindowTextW(g_hEditDays, L"C:\\T4\\days");
				SetWindowTextW(g_hEditResults, L"C:\\Faculty\\CSSO\\T4\\Results");
			}
			break;
		}

		case ID_BTN_CLEAR_LOG:
			SetWindowTextW(g_hEditLog, L"");
			break;

		case ID_BTN_START: {
			if (g_bRunning) break;

			wchar_t days[MAX_PATH], results[MAX_PATH];
			GetWindowTextW(g_hEditDays, days, MAX_PATH);
			GetWindowTextW(g_hEditResults, results, MAX_PATH);

			int K = (int)SendMessageW(g_hSpinWorkers, UDM_GETPOS32, 0, 0);
			if (K < 1) K = 1; if (K > 16) K = 16;
			g_num_workers = K;

			DWORD delay = (DWORD)SendMessageW(g_hSpinDelay, UDM_GETPOS32, 0, 0);
			uint32_t cap = (uint32_t)SendMessageW(g_hSpinCapacity, UDM_GETPOS32, 0, 0);
			if (cap < 1 || cap > 16) cap = 16;

			BOOL isExp = (SendMessageW(g_hCheckExp, BM_GETCHECK, 0, 0) == BST_CHECKED);
			uint32_t reps = (uint32_t)SendMessageW(g_hSpinReps, UDM_GETPOS32, 0, 0);
			if (reps < 1) reps = 5;

			memset(g_worker_pids, 0, sizeof(g_worker_pids));
			SetupWorkersListView(K);

			SendMessageW(g_hProgressBar, PBM_SETRANGE32, 0, 100);
			SendMessageW(g_hProgressBar, PBM_SETPOS, 0, 0);
			SetWindowTextW(g_hLabelDay, L"Starting session...");

			T4_COORD_CONFIG* pCfg = (T4_COORD_CONFIG*)calloc(1, sizeof(T4_COORD_CONFIG));
			if (!pCfg) break;

			wcsncpy(pCfg->days_dir, days, MAX_PATH - 1);
			wcsncpy(pCfg->results_dir, results, MAX_PATH - 1);
			pCfg->K = K;
			pCfg->capacity = cap;
			pCfg->delay_ms = delay;
			pCfg->experiment = isExp ? 1 : 0;
			pCfg->job_log = isExp ? 0 : 1;
			pCfg->repetitions = reps;

			pCfg->cb_ctx = hwnd;
			pCfg->on_worker_started = engine_cb_worker_started;
			pCfg->on_day_start = engine_cb_day_start;
			pCfg->on_progress = engine_cb_progress;
			pCfg->on_day_done = engine_cb_day_done;
			pCfg->on_worker_dead = engine_cb_worker_dead;
			pCfg->on_log_line = engine_cb_log_line;
			pCfg->on_finished = engine_cb_finished;

			g_bRunning = TRUE;
			EnableWindow(g_hBtnStart, FALSE);
			EnableWindow(g_hBtnStop, TRUE);
			EnableWindow(g_hBtnKill, TRUE);
			EnableWindow(g_hEditDays, FALSE);
			EnableWindow(g_hBtnBrowseDays, FALSE);
			EnableWindow(g_hEditResults, FALSE);
			EnableWindow(g_hBtnBrowseResults, FALSE);
			EnableWindow(g_hEditWorkers, FALSE);
			EnableWindow(g_hSpinWorkers, FALSE);
			EnableWindow(g_hEditDelay, FALSE);
			EnableWindow(g_hSpinDelay, FALSE);
			EnableWindow(g_hEditCapacity, FALSE);
			EnableWindow(g_hSpinCapacity, FALSE);
			EnableWindow(g_hCheckExp, FALSE);
			EnableWindow(g_hEditReps, FALSE);
			EnableWindow(g_hSpinReps, FALSE);

			AppendLogText(L"=== Starting Session ===\r\n");

			g_hCoordThread = CreateThread(NULL, 0, CoordinatorThreadProc, pCfg, 0, NULL);
			if (!g_hCoordThread) {
				free(pCfg);
				g_bRunning = FALSE;
				EnableWindow(g_hBtnStart, TRUE);
				EnableWindow(g_hBtnStop, FALSE);
				EnableWindow(g_hBtnKill, FALSE);
				SetWindowTextW(g_hLabelDay, L"Failed to create coordinator thread.");
			}
			break;
		}

		case ID_BTN_STOP: {
			if (!g_bRunning) break;
			AppendLogText(L"=== Stop requested by user ===\r\n");
			SetWindowTextW(g_hLabelDay, L"Stopping coordinator...");
			t4_engine_request_stop();
			EnableWindow(g_hBtnStop, FALSE);
			break;
		}

		case ID_BTN_KILL: {
			/* G3: Kill a worker button (TerminateProcess on selected worker) */
			if (!g_bRunning) break;
			int sel = ListView_GetNextItem(g_hListView, -1, LVNI_SELECTED);
			if (sel < 0) {
				MessageBoxW(hwnd, L"Please select a worker from the list to kill.", L"Kill Worker", MB_OK | MB_ICONINFORMATION);
				break;
			}
			int worker_id = sel + 1;
			DWORD pid = g_worker_pids[worker_id];
			if (pid == 0) {
				MessageBoxW(hwnd, L"Selected worker does not have an active PID.", L"Kill Worker", MB_OK | MB_ICONWARNING);
				break;
			}
			wchar_t confirmMsg[128];
			swprintf(confirmMsg, 128, L"Kill worker W%d (PID %lu) with TerminateProcess?", worker_id, pid);
			if (MessageBoxW(hwnd, confirmMsg, L"Confirm Kill Worker", MB_YESNO | MB_ICONQUESTION) == IDYES) {
				HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
				if (hProc) {
					TerminateProcess(hProc, 99);
					CloseHandle(hProc);
					wchar_t logBuf[128];
					swprintf(logBuf, 128, L"[GUI] Terminated worker W%d (PID %lu)\r\n", worker_id, pid);
					AppendLogText(logBuf);
				}
				else {
					MessageBoxW(hwnd, L"Could not open process with PROCESS_TERMINATE.", L"Error", MB_OK | MB_ICONERROR);
				}
			}
			break;
		}
		}
		return 0;
	}

	case WM_APP_WORKER_STARTED: {
		int wid = (int)wParam;
		DWORD pid = (DWORD)lParam;
		if (wid >= 1 && wid <= T4_MAX_WORKERS) {
			g_worker_pids[wid] = pid;
			wchar_t sPid[32];
			swprintf(sPid, 32, L"%lu", pid);
			ListView_SetItemText(g_hListView, wid - 1, 1, sPid);
			ListView_SetItemText(g_hListView, wid - 1, 3, (LPWSTR)L"Active");
		}
		return 0;
	}

	case WM_APP_DAY_START: {
		int day = (int)wParam;
		uint32_t total_jobs = (uint32_t)lParam;
		SendMessageW(g_hProgressBar, PBM_SETRANGE32, 0, (LPARAM)total_jobs);
		SendMessageW(g_hProgressBar, PBM_SETPOS, 0, 0);
		wchar_t buf[128];
		swprintf(buf, 128, L"Day %d started (%u jobs)...", day, total_jobs);
		SetWindowTextW(g_hLabelDay, buf);
		for (int i = 1; i <= g_num_workers; i++) {
			ListView_SetItemText(g_hListView, i - 1, 2, (LPWSTR)L"0");
		}
		return 0;
	}

	case WM_APP_PROGRESS: {
		T4_PROGRESS_MSG* p = (T4_PROGRESS_MSG*)lParam;
		if (p) {
			SendMessageW(g_hProgressBar, PBM_SETPOS, (WPARAM)p->done_jobs, 0);
			wchar_t buf[128];
			unsigned pct = p->total_jobs ? (unsigned)(p->done_jobs * 100 / p->total_jobs) : 100;
			swprintf(buf, 128, L"Day %d: %u / %u jobs (%u%%)", p->day, p->done_jobs, p->total_jobs, pct);
			SetWindowTextW(g_hLabelDay, buf);
			for (int i = 1; i <= g_num_workers; i++) {
				wchar_t sDone[32];
				swprintf(sDone, 32, L"%u", p->worker_done[i]);
				ListView_SetItemText(g_hListView, i - 1, 2, sDone);
			}
			free(p);
		}
		return 0;
	}

	case WM_APP_DAY_DONE: {
		int day = (int)wParam;
		unsigned long long ms = (unsigned long long)lParam;
		wchar_t buf[128];
		swprintf(buf, 128, L"Day %d completed in %llu ms.", day, ms);
		SetWindowTextW(g_hLabelDay, buf);
		return 0;
	}

	case WM_APP_WORKER_DEAD: {
		int wid = (int)wParam;
		DWORD pid = (DWORD)lParam;
		if (wid >= 1 && wid <= T4_MAX_WORKERS) {
			ListView_SetItemText(g_hListView, wid - 1, 3, (LPWSTR)L"DEAD");
		}
		wchar_t buf[128];
		swprintf(buf, 128, L"Worker W%d (PID %lu) DIED! Stopping session...", wid, pid);
		SetWindowTextW(g_hLabelDay, buf);
		return 0;
	}

	case WM_APP_LOG_LINE: {
		wchar_t* wline = (wchar_t*)lParam;
		if (wline) {
			AppendLogText(wline);
			free(wline);
		}
		return 0;
	}

	case WM_APP_FINISHED: {
		int exitCode = (int)wParam;
		g_bRunning = FALSE;

		EnableWindow(g_hBtnStart, TRUE);
		EnableWindow(g_hBtnStop, FALSE);
		EnableWindow(g_hBtnKill, FALSE);
		EnableWindow(g_hEditDays, FALSE);
		EnableWindow(g_hBtnBrowseDays, TRUE);
		EnableWindow(g_hEditResults, TRUE);
		EnableWindow(g_hBtnBrowseResults, TRUE);
		EnableWindow(g_hEditWorkers, TRUE);
		EnableWindow(g_hSpinWorkers, TRUE);
		EnableWindow(g_hEditDelay, TRUE);
		EnableWindow(g_hSpinDelay, TRUE);
		EnableWindow(g_hEditCapacity, TRUE);
		EnableWindow(g_hSpinCapacity, TRUE);
		EnableWindow(g_hCheckExp, TRUE);
		EnableWindow(g_hEditReps, TRUE);
		EnableWindow(g_hSpinReps, TRUE);
		EnableWindow(g_hEditDays, TRUE);

		wchar_t buf[128];
		if (exitCode == 0) {
			swprintf(buf, 128, L"Session finished successfully (exit code 0).");
		}
		else if (exitCode == 1) {
			swprintf(buf, 128, L"Session stopped by user / Ctrl+C (exit code 1).");
		}
		else {
			swprintf(buf, 128, L"Session terminated with code %d (worker died / error).", exitCode);
		}
		SetWindowTextW(g_hLabelDay, buf);

		for (int i = 1; i <= g_num_workers; i++) {
			wchar_t curStat[32] = L"";
			ListView_GetItemText(g_hListView, i - 1, 3, curStat, 32);
			if (wcscmp(curStat, L"DEAD") != 0) {
				ListView_SetItemText(g_hListView, i - 1, 3, (LPWSTR)L"Stopped");
			}
		}

		if (g_hCoordThread) {
			CloseHandle(g_hCoordThread);
			g_hCoordThread = NULL;
		}
		return 0;
	}

	case WM_CLOSE: {
		/* G4: Closing during execution: stop event, responsive UI, no access to destroyed windows/memory */
		if (g_bRunning) {
			t4_engine_request_stop();
			DWORD t0 = GetTickCount();
			while (g_bRunning && (GetTickCount() - t0 < 6000)) {
				MSG msg;
				while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
					TranslateMessage(&msg);
					DispatchMessageW(&msg);
				}
				Sleep(20);
			}
		}
		if (g_hCoordThread) {
			WaitForSingleObject(g_hCoordThread, 2000);
			CloseHandle(g_hCoordThread);
			g_hCoordThread = NULL;
		}
		DestroyWindow(hwnd);
		return 0;
	}

	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}

	return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}
