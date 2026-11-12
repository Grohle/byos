/* ByOS Desktop.exe - the Windows launcher of ByOS Desktop (one file, nothing to install first).
 *
 * The exe carries everything the app needs as a zip appended to itself, followed by a 16-byte
 * trailer ("BYOSPAK1" + the zip's offset, little endian): the web app (app/), byos-cli.jar,
 * Node.js (runtime/node), a trimmed Java runtime (runtime/java) and adb (platform-tools/).
 * tools/package_desktop.py builds it.
 *
 *  1. The first start of a version unpacks the zip into %LOCALAPPDATA%\ByOS Desktop\<version>
 *     with Windows' own tar.exe (a small "Preparing" window shows meanwhile). Other versions'
 *     folders are removed afterwards.
 *  2. It starts the local server (node app\server.mjs) without a console window, inside a job
 *     object: the server and everything it starts (byos-cli, java, adb) end with the launcher.
 *  3. It opens the app in its own Microsoft Edge window (app mode, its own profile folder, so the
 *     launcher can wait for that window) and, when the window closes, stops the server.
 *     Without Edge it opens the default browser and a message box keeps the app running.
 *
 * Build: tools/wsl/zbuild_desktop_exe.sh (zig cc, x86_64-windows-gnu, GUI subsystem).
 */
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdint.h>
#include <wchar.h>

#ifndef BYOS_VERSION
#define BYOS_VERSION L"dev"
#endif
#define TITLE L"ByOS Desktop"

static HWND splash;
static const wchar_t *splash_text = L"Opening ByOS Desktop\x2026";

static void fail(const wchar_t *msg)
{
    if (splash) DestroyWindow(splash);
    MessageBoxW(0, msg, TITLE, MB_OK | MB_ICONERROR);
    ExitProcess(1);
}

/* keeps the splash responsive while waiting for a process (or for ms milliseconds) */
static DWORD pump_wait(HANDLE h, DWORD ms)
{
    DWORD t0 = GetTickCount();
    for (;;) {
        DWORD left = ms == INFINITE ? INFINITE : (GetTickCount() - t0 >= ms ? 0 : ms - (GetTickCount() - t0));
        DWORD r = MsgWaitForMultipleObjects(h ? 1 : 0, h ? &h : 0, FALSE, left, QS_ALLINPUT);
        if (h && r == WAIT_OBJECT_0) return r;
        if (r == WAIT_TIMEOUT) return r;
        MSG m;
        while (PeekMessageW(&m, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
}

static LRESULT CALLBACK splash_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(w, &ps);
        RECT r; GetClientRect(w, &r);
        HBRUSH bg = CreateSolidBrush(RGB(28, 28, 30)); FillRect(dc, &r, bg); DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
        HFONT f = CreateFontW(-18, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT old = SelectObject(dc, f);
        DrawTextW(dc, splash_text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old); DeleteObject(f);
        EndPaint(w, &ps);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

static void splash_show(HINSTANCE inst)
{
    if (splash) { InvalidateRect(splash, 0, TRUE); pump_wait(0, 10); return; }   /* new text */
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = splash_proc; wc.hInstance = inst; wc.lpszClassName = L"ByOSSplash";
    wc.hCursor = LoadCursor(0, IDC_WAIT); wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);
    int w = 360, h = 110, sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
    splash = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"ByOSSplash", TITLE, WS_POPUP | WS_BORDER,
                             (sx - w) / 2, (sy - h) / 2, w, h, 0, 0, inst, 0);
    ShowWindow(splash, SW_SHOW); UpdateWindow(splash);
    pump_wait(0, 10);
}

static int exists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

/* runs a command line hidden and waits; returns its exit code (or -1) */
static int run_hidden(wchar_t *cmdline, const wchar_t *cwd)
{
    STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
    if (!CreateProcessW(0, cmdline, 0, 0, FALSE, CREATE_NO_WINDOW, 0, cwd, &si, &pi)) return -1;
    pump_wait(pi.hProcess, INFINITE);
    DWORD rc = 1; GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)rc;
}

static void remove_tree(const wchar_t *dir)
{
    if (!exists(dir)) return;
    wchar_t sys[MAX_PATH], cmd[3 * MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    swprintf(cmd, 3 * MAX_PATH, L"\"%ls\\cmd.exe\" /c rmdir /s /q \"%ls\"", sys, dir);
    run_hidden(cmd, 0);
}

/* copies the zip appended to this exe into zip_path */
static int extract_payload(const wchar_t *self, const wchar_t *zip_path)
{
    HANDLE in = CreateFileW(self, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (in == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER size; GetFileSizeEx(in, &size);
    unsigned char tr[16]; DWORD got = 0;
    LARGE_INTEGER pos; pos.QuadPart = size.QuadPart - 16;
    if (size.QuadPart < 32 || !SetFilePointerEx(in, pos, 0, FILE_BEGIN) || !ReadFile(in, tr, 16, &got, 0) || got != 16 ||
        memcmp(tr, "BYOSPAK1", 8)) { CloseHandle(in); return 0; }
    uint64_t off = 0; for (int i = 7; i >= 0; i--) off = off << 8 | tr[8 + i];
    uint64_t len = (uint64_t)size.QuadPart - 16 - off;
    pos.QuadPart = (LONGLONG)off;
    if (off >= (uint64_t)size.QuadPart || !SetFilePointerEx(in, pos, 0, FILE_BEGIN)) { CloseHandle(in); return 0; }
    HANDLE out = CreateFileW(zip_path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
    if (out == INVALID_HANDLE_VALUE) { CloseHandle(in); return 0; }
    static unsigned char buf[1 << 20];
    int ok = 1;
    while (len && ok) {
        DWORD want = len > sizeof buf ? (DWORD)sizeof buf : (DWORD)len, w = 0;
        if (!ReadFile(in, buf, want, &got, 0) || !got) { ok = 0; break; }
        if (!WriteFile(out, buf, got, &w, 0) || w != got) ok = 0;
        len -= got;
        pump_wait(0, 0);
    }
    CloseHandle(in); CloseHandle(out);
    return ok;
}

/* unpacks this version once; other versions' folders are removed */
static void ensure_unpacked(const wchar_t *self, const wchar_t *base, const wchar_t *appdir, HINSTANCE inst)
{
    wchar_t mark[MAX_PATH], tmp[MAX_PATH], zip[MAX_PATH], sys[MAX_PATH], cmd[4 * MAX_PATH];
    swprintf(mark, MAX_PATH, L"%ls\\installed.ok", appdir);
    if (exists(mark)) return;
    splash_text = L"Preparing ByOS Desktop (first start)\x2026";
    splash_show(inst);
    swprintf(tmp, MAX_PATH, L"%ls.unpacking", appdir);
    swprintf(zip, MAX_PATH, L"%ls\\payload.zip", base);
    remove_tree(tmp); remove_tree(appdir);
    if (!CreateDirectoryW(tmp, 0)) fail(L"Could not create the ByOS Desktop folder in your user profile.");
    if (!extract_payload(self, zip)) { DeleteFileW(zip); fail(L"This copy of ByOS Desktop.exe is damaged. Please download it again."); }
    GetSystemDirectoryW(sys, MAX_PATH);
    swprintf(cmd, 4 * MAX_PATH, L"\"%ls\\tar.exe\" -xf \"%ls\" -C \"%ls\"", sys, zip, tmp);
    int rc = run_hidden(cmd, tmp);
    DeleteFileW(zip);
    if (rc != 0) fail(L"Could not unpack ByOS Desktop (Windows 10 version 1803 or newer is needed).");
    if (!MoveFileExW(tmp, appdir, 0)) fail(L"Could not finish unpacking ByOS Desktop.");
    HANDLE m = CreateFileW(mark, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
    if (m != INVALID_HANDLE_VALUE) CloseHandle(m);
    /* older versions */
    wchar_t pat[MAX_PATH]; swprintf(pat, MAX_PATH, L"%ls\\*", base);
    WIN32_FIND_DATAW fd; HANDLE fh = FindFirstFileW(pat, &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            if (fd.cFileName[0] < L'0' || fd.cFileName[0] > L'9' || !wcscmp(fd.cFileName, BYOS_VERSION)) continue;
            wchar_t old[MAX_PATH]; swprintf(old, MAX_PATH, L"%ls\\%ls", base, fd.cFileName);
            remove_tree(old);
        } while (FindNextFileW(fh, &fd));
        FindClose(fh);
    }
}

static int find_edge(wchar_t *out)
{
    const wchar_t *vars[] = { L"ProgramFiles(x86)", L"ProgramFiles", L"LOCALAPPDATA" };
    for (int i = 0; i < 3; i++) {
        wchar_t root[MAX_PATH];
        if (!GetEnvironmentVariableW(vars[i], root, MAX_PATH)) continue;
        swprintf(out, MAX_PATH, L"%ls\\Microsoft\\Edge\\Application\\msedge.exe", root);
        if (exists(out)) return 1;
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR args, int show)
{
    (void)prev; (void)args; (void)show;
    HANDLE once = CreateMutexW(0, TRUE, L"Local\\ByOSDesktop");
    if (once && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(0, L"ByOS Desktop is already open.", TITLE, MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    wchar_t self[MAX_PATH], local[MAX_PATH], base[MAX_PATH], appdir[MAX_PATH];
    GetModuleFileNameW(0, self, MAX_PATH);
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) fail(L"LOCALAPPDATA is not set.");
    swprintf(base, MAX_PATH, L"%ls\\ByOS Desktop", local);
    CreateDirectoryW(base, 0);
    splash_show(inst);                 /* until the app's window is there (the server needs a few seconds) */
    swprintf(appdir, MAX_PATH, L"%ls\\%ls", base, BYOS_VERSION);
    ensure_unpacked(self, base, appdir, inst);

    /* the server, hidden, in a job that ends with us */
    wchar_t node[MAX_PATH], urlf[MAX_PATH], logf[MAX_PATH], java[MAX_PATH], cmd[8 * MAX_PATH];
    swprintf(node, MAX_PATH, L"%ls\\runtime\\node\\node.exe", appdir);
    swprintf(urlf, MAX_PATH, L"%ls\\url.txt", base);
    swprintf(logf, MAX_PATH, L"%ls\\server.log", base);
    swprintf(java, MAX_PATH, L"%ls\\runtime\\java", appdir);
    if (!exists(node)) { wchar_t mark[MAX_PATH]; swprintf(mark, MAX_PATH, L"%ls\\installed.ok", appdir); DeleteFileW(mark);
                         fail(L"ByOS Desktop is incomplete. Start it again to unpack it again."); }
    SetEnvironmentVariableW(L"JAVA_HOME", java);
    DeleteFileW(urlf);
    swprintf(cmd, 8 * MAX_PATH,
             L"\"%ls\" \"%ls\\app\\server.mjs\" --cli \"%ls\\byos-cli.jar\" --adb \"%ls\\platform-tools\\adb.exe\" --url-file \"%ls\"",
             node, appdir, appdir, appdir, urlf);

    SECURITY_ATTRIBUTES sa = { sizeof sa, 0, TRUE };
    HANDLE log = CreateFileW(logf, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, 0);
    HANDLE job = CreateJobObjectW(0, 0);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl = { 0 };
    jl.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jl, sizeof jl);

    STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE); si.hStdOutput = log; si.hStdError = log;
    if (!CreateProcessW(0, cmd, 0, 0, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, 0, appdir, &si, &pi))
        fail(L"Could not start ByOS Desktop's server.");
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);

    /* its private address (with the access token) */
    char url[600] = "";
    for (int i = 0; i < 300 && !url[0]; i++) {
        if (pump_wait(pi.hProcess, 100) == WAIT_OBJECT_0) {
            wchar_t m[MAX_PATH + 120];
            swprintf(m, MAX_PATH + 120, L"ByOS Desktop's server stopped at start. Details: %ls", logf);
            fail(m);
        }
        FILE *f = _wfopen(urlf, L"rb");
        if (f) { size_t n = fread(url, 1, sizeof url - 1, f); fclose(f); url[n] = 0; char *e = strpbrk(url, "\r\n"); if (e) *e = 0; else url[0] = 0; }
    }
    if (!url[0]) { if (job) TerminateJobObject(job, 1); fail(L"ByOS Desktop's server did not answer."); }
    wchar_t wurl[600]; MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 600);

    wchar_t edge[MAX_PATH];
    if (find_edge(edge)) {
        wchar_t prof[MAX_PATH], ecmd[4 * MAX_PATH];
        swprintf(prof, MAX_PATH, L"%ls\\window", base);
        swprintf(ecmd, 4 * MAX_PATH,
                 L"\"%ls\" --app=%ls --user-data-dir=\"%ls\" --window-size=1180,760 --no-first-run --no-default-browser-check",
                 edge, wurl, prof);
        STARTUPINFOW es = { sizeof es }; PROCESS_INFORMATION ep;
        if (CreateProcessW(0, ecmd, 0, 0, FALSE, 0, 0, 0, &es, &ep)) {
            WaitForInputIdle(ep.hProcess, 8000); pump_wait(0, 600);   /* its window is up: the splash goes */
            if (splash) { DestroyWindow(splash); splash = 0; }
            HANDLE two[2] = { ep.hProcess, pi.hProcess };
            for (;;) {   /* until the window closes (or the server ends) */
                DWORD r = MsgWaitForMultipleObjects(2, two, FALSE, INFINITE, QS_ALLINPUT);
                if (r == WAIT_OBJECT_0 || r == WAIT_OBJECT_0 + 1) break;
                MSG m; while (PeekMessageW(&m, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
            }
            CloseHandle(ep.hProcess); CloseHandle(ep.hThread);
            goto done;
        }
    }
    if (splash) { DestroyWindow(splash); splash = 0; }
    ShellExecuteW(0, L"open", wurl, 0, 0, SW_SHOWNORMAL);
    MessageBoxW(0, L"ByOS Desktop is open in your web browser.\n\nPress OK to close ByOS Desktop.", TITLE, MB_OK | MB_ICONINFORMATION);
done:
    if (job) TerminateJobObject(job, 0); else TerminateProcess(pi.hProcess, 0);
    DeleteFileW(urlf);
    return 0;
}
