/*
 * Fighting Layer Setup: builds the game from the player's own ROM and installs it.
 *
 * Layout of the Setup folder (made by tools/install/make_package.py):
 *     Setup.exe
 *     files/python/   Python + the disassembler packages
 *     files/clang/    32-bit clang + linker + Windows headers/libraries
 *     files/source/   this project's source (no ROM data)
 *     files/menu_art.png, files/fgtlayer.ico (optional)
 *
 * Setup runs files/source/tools/install/build_from_rom.py, shows its "@@progress" lines, writes everything
 * else to <install folder>/setup.log, then adds shortcuts and an "Apps & features" entry (per user, no admin).
 * Copied into the install folder as Uninstall.exe it removes the game again.
 *
 * Optional: the player's Street Fighter EX2 Plus set (sfex2p.zip) adds Pullum Purna as an extra fighter
 * (build_from_rom.py --extra-rom); PULLUM_portrait.png next to Setup.exe becomes her select-screen picture.
 *
 * Built with -DNO_EXTRA (make_package.py --public): no Street Fighter EX2 Plus row, Fighting Layer only.
 * Command line (for testing):  /rom=<zip> /ex2p=<zip> /dest=<folder> /silent /noshortcuts /noregister /shot=<bmp>
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define APP_TITLE L"ReLayer Setup"
#define EXE_NAME L"ReLayer.exe"
#define UNINSTALL_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ReLayer"

enum { ID_ROM = 100, ID_ROM_BROWSE, ID_DEST, ID_DEST_BROWSE, ID_DESKTOP, ID_STARTMENU, ID_PROGRESS, ID_STATUS,
       ID_GO, ID_CANCEL, ID_HINT_ROM, ID_HINT_FOOT, ID_EX2P, ID_EX2P_BROWSE, ID_HINT_EX2P };
enum { WM_APP_PROGRESS = WM_APP + 1, WM_APP_DONE };

static HINSTANCE g_inst;
static HWND g_wnd, g_rom, g_dest, g_desktop, g_startmenu, g_progress, g_status, g_go, g_cancel, g_rom_browse,
    g_dest_browse, g_ex2p, g_ex2p_browse;
static HFONT g_font, g_font_bold, g_font_small, g_font_title, g_font_sub;
static HBITMAP g_banner;
static int g_dpi = 96;
static wchar_t g_setup_dir[MAX_PATH], g_files[MAX_PATH];
static HANDLE g_job, g_child;
static volatile LONG g_running;
static wchar_t g_error[1024], g_done_exe[MAX_PATH], g_log_path[MAX_PATH];
static int g_state;     /* 0 = ready, 1 = installing, 2 = finished, 3 = failed */
static int g_silent, g_no_shortcuts, g_no_register, g_exit_code;
static wchar_t g_shot[MAX_PATH];

static int S(int v) { return MulDiv(v, g_dpi, 96); }

static void path_join(wchar_t *out, const wchar_t *a, const wchar_t *b) {
    swprintf(out, MAX_PATH, L"%ls\\%ls", a, b);
}

static int file_exists(const wchar_t *p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* ------------------------------------------------------------------ banner: the menu art behind the title */
static HBITMAP make_banner(int w, int h) {
    wchar_t art[MAX_PATH];
    path_join(art, g_files, L"menu_art.png");
    BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB}};
    void *bits;
    HBITMAP bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp) return NULL;
    uint32_t *px = bits;
    for (int i = 0; i < w * h; i++) px[i] = 0x0E0F15;

    IWICImagingFactory *f = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *fr = NULL;
    IWICBitmapScaler *sc = NULL;
    IWICFormatConverter *cv = NULL;
    static const GUID clsid = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
    static const GUID iid = {0xec5ec8a9, 0xc395, 0x4314, {0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70}};
    static const GUID bgra = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0e}};
    if (file_exists(art) && SUCCEEDED(CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &iid, (void **)&f)) &&
        SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(f, art, NULL, GENERIC_READ,
                                                               WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &fr))) {
        UINT iw, ih;
        IWICBitmapFrameDecode_GetSize(fr, &iw, &ih);
        /* scale to the banner's width, then show a band around the face (about 28% down the picture) */
        UINT sw = (UINT)w, sh = (UINT)((double)ih * w / iw);
        if (sh < (UINT)h) { sh = (UINT)h; sw = (UINT)((double)iw * h / ih); }
        if (SUCCEEDED(IWICImagingFactory_CreateBitmapScaler(f, &sc)) &&
            SUCCEEDED(IWICBitmapScaler_Initialize(sc, (IWICBitmapSource *)fr, sw, sh, WICBitmapInterpolationModeFant)) &&
            SUCCEEDED(IWICImagingFactory_CreateFormatConverter(f, &cv)) &&
            SUCCEEDED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)sc, &bgra, WICBitmapDitherTypeNone, NULL,
                                                     0, WICBitmapPaletteTypeCustom))) {
            int top = (int)(sh * 0.28) - h / 2;
            if (top < 0) top = 0;
            if (top > (int)sh - h) top = (int)sh - h;
            int left = (int)sw - w;
            WICRect r = {left, top, w, h};
            IWICFormatConverter_CopyPixels(cv, &r, (UINT)w * 4, (UINT)(w * h * 4), bits);
        }
    }
    if (cv) IWICFormatConverter_Release(cv);
    if (sc) IWICBitmapScaler_Release(sc);
    if (fr) IWICBitmapFrameDecode_Release(fr);
    if (dec) IWICBitmapDecoder_Release(dec);
    if (f) IWICImagingFactory_Release(f);

    /* dark on the left where the title goes, fading out to the right; a little darker at the bottom */
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float t = (float)x / (float)w;
            float k = t < 0.25f ? 0.06f : 0.06f + (t - 0.25f) / 0.75f * 0.6f;
            k *= 1.0f - 0.35f * (float)y / (float)h;
            uint32_t c = px[y * w + x];
            int r = (int)(((c >> 16) & 255) * k + 0x0E * (1 - k)), g = (int)(((c >> 8) & 255) * k + 0x0F * (1 - k)),
                b = (int)((c & 255) * k + 0x15 * (1 - k));
            px[y * w + x] = (uint32_t)(r << 16 | g << 8 | b);
        }
    return bmp;
}

/* ------------------------------------------------------------------ logging + the build process */
static void log_line(const char *s) {
    HANDLE h = CreateFileW(g_log_path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD n;
    WriteFile(h, s, (DWORD)strlen(s), &n, NULL);
    WriteFile(h, "\r\n", 2, &n, NULL);
    CloseHandle(h);
}

static wchar_t *utf8_to_wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = malloc(sizeof(wchar_t) * (size_t)(n > 0 ? n : 1));
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    else w[0] = 0;
    return w;
}

static void handle_line(char *line) {
    log_line(line);
    if (!strncmp(line, "@@progress ", 11)) {
        char *end;
        long pct = strtol(line + 11, &end, 10);
        while (*end == ' ') end++;
        PostMessageW(g_wnd, WM_APP_PROGRESS, (WPARAM)pct, (LPARAM)utf8_to_wide(end));
    } else if (!strncmp(line, "@@error ", 8)) {
        wchar_t *w = utf8_to_wide(line + 8);
        wcsncpy(g_error, w, 1023);
        free(w);
    } else if (!strncmp(line, "@@done ", 7)) {
        wchar_t *w = utf8_to_wide(line + 7);
        wcsncpy(g_done_exe, w, MAX_PATH - 1);
        free(w);
    }
}

static DWORD WINAPI reader(LPVOID pipe) {
    char buf[4096], line[8192];
    size_t len = 0;
    DWORD n;
    while (ReadFile(pipe, buf, sizeof buf, &n, NULL) && n) {
        for (DWORD i = 0; i < n; i++) {
            if (buf[i] == '\n' || len == sizeof line - 1) {
                while (len && line[len - 1] == '\r') len--;
                line[len] = 0;
                handle_line(line);
                len = 0;
            } else line[len++] = buf[i];
        }
    }
    if (len) { line[len] = 0; handle_line(line); }
    CloseHandle(pipe);
    WaitForSingleObject(g_child, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(g_child, &code);
    PostMessageW(g_wnd, WM_APP_DONE, code, 0);
    return 0;
}

static int start_build(const wchar_t *rom, const wchar_t *ex2p, const wchar_t *dest) {
    wchar_t py[MAX_PATH], script[MAX_PATH], clang[MAX_PATH], art[MAX_PATH], portrait[MAX_PATH], cmd[8 * MAX_PATH];
    path_join(py, g_files, L"python\\python.exe");
    path_join(script, g_files, L"source\\tools\\install\\build_from_rom.py");
    path_join(clang, g_files, L"clang\\bin");
    path_join(art, g_files, L"menu_art.png");
    if (!file_exists(py) || !file_exists(script)) {
        wcscpy(g_error, L"Setup's files folder is missing or incomplete. Unzip the whole Setup download and run Setup.exe from there.");
        return 0;
    }
    SHCreateDirectoryExW(NULL, dest, NULL);
    path_join(g_log_path, dest, L"setup.log");
    DeleteFileW(g_log_path);
    swprintf(cmd, sizeof cmd / sizeof cmd[0], L"\"%ls\" -u -X utf8 \"%ls\" --rom \"%ls\" --dest \"%ls\" --clang \"%ls\"%ls%ls%ls",
             py, script, rom, dest, clang, file_exists(art) ? L" --art \"" : L"", file_exists(art) ? art : L"",
             file_exists(art) ? L"\"" : L"");
    if (ex2p[0]) {                              /* Pullum Purna from Street Fighter EX2 Plus */
        size_t n = wcslen(cmd);
        swprintf(cmd + n, sizeof cmd / sizeof cmd[0] - n, L" --extra-rom \"%ls\"", ex2p);
        path_join(portrait, g_setup_dir, L"PULLUM_portrait.png");
        n = wcslen(cmd);
        if (file_exists(portrait)) swprintf(cmd + n, sizeof cmd / sizeof cmd[0] - n, L" --portrait \"%ls\"", portrait);
    }

    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return 0;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi;
    /* a job object so Cancel (or closing Setup) stops the whole build, compilers included */
    g_job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {0};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof li);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, dest, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        swprintf(g_error, 1024, L"Could not start the build (error %lu).", GetLastError());
        return 0;
    }
    AssignProcessToJobObject(g_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(wr);
    g_child = pi.hProcess;
    InterlockedExchange(&g_running, 1);
    CreateThread(NULL, 0, reader, rd, 0, NULL);
    return 1;
}

/* ------------------------------------------------------------------ shortcuts, Apps & features, uninstall */
static void make_shortcut(int csidl, const wchar_t *exe, const wchar_t *dir) {
    wchar_t folder[MAX_PATH], lnk[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, csidl, NULL, 0, folder))) return;
    path_join(lnk, folder, L"ReLayer.lnk");
    IShellLinkW *sl;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl))) return;
    IShellLinkW_SetPath(sl, exe);
    IShellLinkW_SetWorkingDirectory(sl, dir);
    IShellLinkW_SetDescription(sl, L"ReLayer: Fighting Layer on PC");
    IPersistFile *pf;
    if (SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) {
        IPersistFile_Save(pf, lnk, TRUE);
        IPersistFile_Release(pf);
    }
    IShellLinkW_Release(sl);
}

static void remove_shortcut(int csidl) {
    wchar_t folder[MAX_PATH], lnk[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, csidl, NULL, 0, folder))) return;
    path_join(lnk, folder, L"ReLayer.lnk");
    DeleteFileW(lnk);
}

static void reg_str(HKEY k, const wchar_t *name, const wchar_t *v) {
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)v, (DWORD)((wcslen(v) + 1) * sizeof(wchar_t)));
}

static void register_uninstall(const wchar_t *dest, const wchar_t *exe) {
    wchar_t self[MAX_PATH], un[MAX_PATH], cmd[MAX_PATH + 32];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    path_join(un, dest, L"Uninstall.exe");
    CopyFileW(self, un, FALSE);
    swprintf(cmd, MAX_PATH + 32, L"\"%ls\" /uninstall", un);
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, UNINSTALL_KEY, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL)) return;
    reg_str(k, L"DisplayName", L"ReLayer (Fighting Layer PC port)");
    reg_str(k, L"DisplayIcon", exe);
    reg_str(k, L"InstallLocation", dest);
    reg_str(k, L"UninstallString", cmd);
    reg_str(k, L"Publisher", L"ReLayer project");
    DWORD one = 1, kb = 60 * 1024;
    RegSetValueExW(k, L"NoModify", 0, REG_DWORD, (const BYTE *)&one, sizeof one);
    RegSetValueExW(k, L"NoRepair", 0, REG_DWORD, (const BYTE *)&one, sizeof one);
    RegSetValueExW(k, L"EstimatedSize", 0, REG_DWORD, (const BYTE *)&kb, sizeof kb);
    RegCloseKey(k);
}

static void delete_in(const wchar_t *dir, const wchar_t *name) {
    wchar_t p[MAX_PATH];
    path_join(p, dir, name);
    DeleteFileW(p);
}

static int uninstall(const wchar_t *dir) {
    if (MessageBoxW(NULL, L"Remove ReLayer from this PC?", L"ReLayer", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return 1;
    int saves = MessageBoxW(NULL, L"Also delete your settings, controls and the game's saved settings and scores?\n\n"
                                  L"Choose No to keep them for next time.",
                            L"ReLayer", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
    static const wchar_t *files[] = {EXE_NAME, L"setup.log", L"fgtlayer.log", L"fgtlayer.old.log", L"screen.bmp", L"vram.bmp",
                                     L"missing_entries.txt", L"menu_art.png", L"menu_art.jpg", L"menu_art.jpeg"};
    for (unsigned i = 0; i < sizeof files / sizeof files[0]; i++) delete_in(dir, files[i]);
    static const wchar_t *roms[] = {L"prg.bin", L"banked.bin", L"main.bin", L"sound.bin", L"wave.bin"};
    wchar_t rom[MAX_PATH];
    path_join(rom, dir, L"rom");
    for (unsigned i = 0; i < sizeof roms / sizeof roms[0]; i++) delete_in(rom, roms[i]);
    RemoveDirectoryW(rom);
    /* folders: the extra fighter's files (made from the player's EX2 Plus ROM), and the (empty) log folder the
     * NVIDIA driver leaves in the folder a game runs from */
    static const wchar_t *dirs[] = {L"extra", L"NVIDIA Corporation"};
    for (unsigned i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        wchar_t d[MAX_PATH + 2] = {0};
        path_join(d, dir, dirs[i]);
        d[wcslen(d) + 1] = 0;                   /* SHFileOperation wants a double-NUL-terminated list */
        SHFILEOPSTRUCTW op = {0};
        op.wFunc = FO_DELETE;
        op.pFrom = d;
        op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
        if (GetFileAttributesW(d) != INVALID_FILE_ATTRIBUTES) SHFileOperationW(&op);
    }
    if (saves) {
        delete_in(dir, L"settings.ini");
        delete_in(dir, L"controls.ini");
        delete_in(dir, L"eeprom.bin");
    }
    remove_shortcut(CSIDL_DESKTOPDIRECTORY);
    remove_shortcut(CSIDL_PROGRAMS);
    RegDeleteKeyW(HKEY_CURRENT_USER, UNINSTALL_KEY);
    MessageBoxW(NULL, saves ? L"ReLayer has been removed." :
                              L"ReLayer has been removed. Your settings were kept in its folder.",
                L"ReLayer", MB_OK | MB_ICONINFORMATION);
    /* this program is <dir>\Uninstall.exe and can't delete itself while running: a hidden cmd does it after */
    wchar_t cmd[3 * MAX_PATH], sysdir[MAX_PATH];
    GetSystemDirectoryW(sysdir, MAX_PATH);
    swprintf(cmd, 3 * MAX_PATH, L"\"%ls\\cmd.exe\" /c ping 127.0.0.1 -n 3 > nul & del \"%ls\\Uninstall.exe\" & rmdir \"%ls\"",
             sysdir, dir, dir);
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, sysdir, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return 0;
}

/* ------------------------------------------------------------------ the window */
static void set_state(int st) {
    g_state = st;
    int idle = st == 0 || st == 3;
    EnableWindow(g_rom, idle);
    EnableWindow(g_dest, idle);
    EnableWindow(g_rom_browse, idle);
    EnableWindow(g_ex2p, idle);
    EnableWindow(g_ex2p_browse, idle);
    EnableWindow(g_dest_browse, idle);
    EnableWindow(g_desktop, idle);
    EnableWindow(g_startmenu, idle);
    SetWindowTextW(g_go, st == 2 ? L"Play" : st == 3 ? L"Try again" : L"Install");
    EnableWindow(g_go, st != 1);
    SetWindowTextW(g_cancel, st == 2 ? L"Close" : L"Cancel");
}

static void browse_zip(HWND edit, const wchar_t *title) {
    wchar_t file[MAX_PATH] = L"";
    GetWindowTextW(edit, file, MAX_PATH);
    OPENFILENAMEW of = {sizeof of};
    of.hwndOwner = g_wnd;
    of.lpstrFilter = L"MAME ROM set (*.zip)\0*.zip\0All files\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH;
    of.lpstrTitle = title;
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&of)) SetWindowTextW(edit, file);
}

static void browse_dest(void) {
    IFileOpenDialog *d;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d)))
        return;
    DWORD opt;
    IFileOpenDialog_GetOptions(d, &opt);
    IFileOpenDialog_SetOptions(d, opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    IFileOpenDialog_SetTitle(d, L"Install ReLayer to");
    if (SUCCEEDED(IFileOpenDialog_Show(d, g_wnd))) {
        IShellItem *it;
        if (SUCCEEDED(IFileOpenDialog_GetResult(d, &it))) {
            wchar_t *p;
            if (SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_FILESYSPATH, &p))) {
                /* picking "D:\Games" installs into "D:\Games\Fighting Layer" */
                wchar_t full[MAX_PATH];
                const wchar_t *base = wcsrchr(p, L'\\');
                if (base && !_wcsicmp(base + 1, L"ReLayer")) wcsncpy(full, p, MAX_PATH);
                else swprintf(full, MAX_PATH, L"%ls%ls%ls", p, p[wcslen(p) - 1] == L'\\' ? L"" : L"\\", L"ReLayer");
                SetWindowTextW(g_dest, full);
                CoTaskMemFree(p);
            }
            IShellItem_Release(it);
        }
    }
    IFileOpenDialog_Release(d);
}

static void go(void) {
    if (g_state == 2) {
        ShellExecuteW(NULL, L"open", g_done_exe, NULL, NULL, SW_SHOWNORMAL);
        DestroyWindow(g_wnd);
        return;
    }
    wchar_t rom[MAX_PATH], ex2p[MAX_PATH] = L"", dest[MAX_PATH];
    GetWindowTextW(g_rom, rom, MAX_PATH);
    if (g_ex2p) GetWindowTextW(g_ex2p, ex2p, MAX_PATH);
    GetWindowTextW(g_dest, dest, MAX_PATH);
    if (!file_exists(rom)) {
        if (!g_silent) MessageBoxW(g_wnd, L"Choose your Fighting Layer ROM first: the MAME set fgtlayer.zip.", APP_TITLE, MB_OK | MB_ICONINFORMATION);
        else g_exit_code = 2, DestroyWindow(g_wnd);
        return;
    }
    if (ex2p[0] && !file_exists(ex2p)) {
        if (!g_silent) MessageBoxW(g_wnd, L"The Street Fighter EX2 Plus ROM was not found. Choose sfex2p.zip again, or clear "
                                          L"the box to install without Pullum.", APP_TITLE, MB_OK | MB_ICONINFORMATION);
        else g_exit_code = 2, DestroyWindow(g_wnd);
        return;
    }
    if (!dest[0]) return;
    g_error[0] = 0;
    g_done_exe[0] = 0;
    SendMessageW(g_progress, PBM_SETPOS, 0, 0);
    SetWindowTextW(g_status, L"Starting");
    set_state(1);
    if (!start_build(rom, ex2p, dest)) {
        SetWindowTextW(g_status, g_error);
        set_state(3);
        if (g_silent) g_exit_code = 1, DestroyWindow(g_wnd);
    }
}

static void finished(DWORD code) {
    InterlockedExchange(&g_running, 0);
    if (g_job) { CloseHandle(g_job); g_job = NULL; }
    if (g_child) { CloseHandle(g_child); g_child = NULL; }
    if (code == 0 && g_done_exe[0]) {
        wchar_t dest[MAX_PATH];
        GetWindowTextW(g_dest, dest, MAX_PATH);
        if (!g_no_shortcuts) {
            if (SendMessageW(g_desktop, BM_GETCHECK, 0, 0) == BST_CHECKED) make_shortcut(CSIDL_DESKTOPDIRECTORY, g_done_exe, dest);
            if (SendMessageW(g_startmenu, BM_GETCHECK, 0, 0) == BST_CHECKED) make_shortcut(CSIDL_PROGRAMS, g_done_exe, dest);
        }
        if (!g_no_register) register_uninstall(dest, g_done_exe);
        SendMessageW(g_progress, PBM_SETPOS, 100, 0);
        SetWindowTextW(g_status, L"Installed. Press Play to start (Back + Start or Tab opens the menu in game).");
        set_state(2);
        g_exit_code = 0;
    } else {
        wchar_t msg[1200];
        swprintf(msg, 1200, L"%ls", g_error[0] ? g_error : L"Setup stopped (see setup.log in the install folder).");
        SetWindowTextW(g_status, msg);
        set_state(3);
        g_exit_code = 1;
    }
    if (g_silent) DestroyWindow(g_wnd);
}

static HWND add(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id, HFONT font) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_wnd,
                             (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

#define WIN_W 620
#define BANNER_H 150

static void create_controls(void) {
    g_font = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_font_bold = CreateFontW(-S(15), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_font_small = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_font_title = CreateFontW(-S(34), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_font_sub = CreateFontW(-S(15), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_banner = make_banner(S(WIN_W), S(BANNER_H));

    int y = BANNER_H + 20;
    add(L"STATIC", L"Your Fighting Layer ROM", 0, 24, y, 400, 20, 0, g_font_bold);
    g_rom = add(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 24, y + 24, 470, 26, ID_ROM, g_font);
    g_rom_browse = add(L"BUTTON", L"Browse...", WS_TABSTOP, 502, y + 23, 94, 28, ID_ROM_BROWSE, g_font);
    add(L"STATIC", L"The MAME set fgtlayer.zip. It is checked before anything is installed.", 0, 24, y + 54, 572, 18,
        ID_HINT_ROM, g_font_small);
    y += 86;
#ifndef NO_EXTRA
    add(L"STATIC", L"Street Fighter EX2 Plus ROM (optional)", 0, 24, y, 400, 20, 0, g_font_bold);
    g_ex2p = add(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 24, y + 24, 470, 26, ID_EX2P, g_font);
    g_ex2p_browse = add(L"BUTTON", L"Browse...", WS_TABSTOP, 502, y + 23, 94, 28, ID_EX2P_BROWSE, g_font);
    add(L"STATIC", L"The MAME set sfex2p.zip adds Pullum Purna as an extra fighter. Leave it empty to skip.", 0, 24,
        y + 54, 572, 18, ID_HINT_EX2P, g_font_small);
    y += 86;
#endif
    add(L"STATIC", L"Install to", 0, 24, y, 400, 20, 0, g_font_bold);
    g_dest = add(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 24, y + 24, 470, 26, ID_DEST, g_font);
    g_dest_browse = add(L"BUTTON", L"Browse...", WS_TABSTOP, 502, y + 23, 94, 28, ID_DEST_BROWSE, g_font);
    y += 64;
    g_desktop = add(L"BUTTON", L"Desktop shortcut", BS_AUTOCHECKBOX | WS_TABSTOP, 24, y, 180, 22, ID_DESKTOP, g_font);
    g_startmenu = add(L"BUTTON", L"Start menu shortcut", BS_AUTOCHECKBOX | WS_TABSTOP, 214, y, 200, 22, ID_STARTMENU, g_font);
    SendMessageW(g_desktop, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(g_startmenu, BM_SETCHECK, BST_CHECKED, 0);
    y += 40;
    g_progress = add(PROGRESS_CLASSW, L"", 0, 24, y, 572, 14, ID_PROGRESS, g_font);
    SendMessageW(g_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    g_status = add(L"STATIC", L"Setup makes the game from your ROM on this PC. It takes a minute or two.", SS_LEFT, 24,
                   y + 22, 572, 38, ID_STATUS, g_font_small);
    y += 70;
    g_go = add(L"BUTTON", L"Install", BS_DEFPUSHBUTTON | WS_TABSTOP, 392, y, 98, 32, ID_GO, g_font_bold);
    g_cancel = add(L"BUTTON", L"Cancel", WS_TABSTOP, 498, y, 98, 32, ID_CANCEL, g_font);
    add(L"STATIC", L"No game data comes with Setup.", 0, 24, y + 8, 340, 18, ID_HINT_FOOT, g_font_small);
}

static void paint_dc(HWND h, HDC dc) {
    RECT cr;
    GetClientRect(h, &cr);
    HBRUSH white = GetSysColorBrush(COLOR_WINDOW);
    RECT body = {0, S(BANNER_H), cr.right, cr.bottom};
    FillRect(dc, &body, white);
    if (g_banner) {
        HDC m = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(m, g_banner);
        BitBlt(dc, 0, 0, S(WIN_W), S(BANNER_H), m, 0, 0, SRCCOPY);
        SelectObject(m, old);
        DeleteDC(m);
    }
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_font_title);
    SetTextColor(dc, RGB(0xF4, 0xF5, 0xF8));
    TextOutW(dc, S(26), S(42), L"ReLayer", 7);
    HBRUSH gold = CreateSolidBrush(RGB(0xE8, 0xB6, 0x4C));
    RECT bar = {S(28), S(88), S(28 + 64), S(88 + 4)};
    FillRect(dc, &bar, gold);
    DeleteObject(gold);
    SelectObject(dc, g_font_sub);
    SetTextColor(dc, RGB(0xC9, 0xCB, 0xD3));
    TextOutW(dc, S(26), S(102), L"Fighting Layer on PC: setup", 27);
}

static void paint(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    paint_dc(h, dc);
    EndPaint(h, &ps);
}

static void save_shot(void) {
    RECT r;
    GetWindowRect(g_wnd, &r);
    int w = r.right - r.left, h = r.bottom - r.top;
    HDC sdc = GetDC(NULL), m = CreateCompatibleDC(sdc);
    BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), w, h, 1, 24, BI_RGB}};
    void *bits;
    HBITMAP b = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(m, b);
    SendMessageW(g_wnd, WM_PRINT, (WPARAM)m, PRF_NONCLIENT | PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
    int stride = (w * 3 + 3) & ~3;
    BITMAPFILEHEADER fh = {0x4D42, (DWORD)(sizeof fh + sizeof bi.bmiHeader + stride * h), 0, 0, sizeof fh + sizeof bi.bmiHeader};
    FILE *f = _wfopen(g_shot, L"wb");
    if (f) {
        fwrite(&fh, sizeof fh, 1, f);
        fwrite(&bi.bmiHeader, sizeof bi.bmiHeader, 1, f);
        fwrite(bits, (size_t)stride * h, 1, f);
        fclose(f);
    }
    DeleteObject(b);
    DeleteDC(m);
    ReleaseDC(NULL, sdc);
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: paint(h); return 0;
    case WM_PRINTCLIENT: paint_dc(h, (HDC)wp); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        int id = GetDlgCtrlID((HWND)lp);
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        SetTextColor(dc, id == ID_HINT_ROM || id == ID_HINT_FOOT || id == ID_HINT_EX2P ? RGB(0x6B, 0x6E, 0x78) :
                         id == ID_STATUS && g_state == 3 ? RGB(0xC0, 0x30, 0x30) : GetSysColor(COLOR_WINDOWTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_ROM_BROWSE: browse_zip(g_rom, L"Choose your Fighting Layer ROM (fgtlayer.zip)"); break;
        case ID_EX2P_BROWSE: browse_zip(g_ex2p, L"Choose your Street Fighter EX2 Plus ROM (sfex2p.zip)"); break;
        case ID_DEST_BROWSE: browse_dest(); break;
        case ID_GO: go(); break;
        case ID_CANCEL:
        case IDCANCEL:
            if (g_state == 1 && MessageBoxW(h, L"Stop installing?", APP_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES) break;
            DestroyWindow(h);
            break;
        }
        return 0;
    case WM_APP_PROGRESS:
        SendMessageW(g_progress, PBM_SETPOS, wp, 0);
        SetWindowTextW(g_status, (wchar_t *)lp);
        free((void *)lp);
        return 0;
    case WM_APP_DONE: finished((DWORD)wp); return 0;
    case WM_CLOSE:
        if (g_state == 1 && MessageBoxW(h, L"Stop installing?", APP_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        if (g_job) CloseHandle(g_job);      /* kills the build if it is still running */
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* a ROM set, if it's lying next to Setup or in Downloads */
static void guess_rom(wchar_t *out, const wchar_t *name) {
    wchar_t p[MAX_PATH], dl[MAX_PATH];
    path_join(p, g_setup_dir, name);
    if (file_exists(p)) { wcscpy(out, p); return; }
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, dl))) {
        swprintf(p, MAX_PATH, L"%ls\\Downloads\\%ls", dl, name);
        if (file_exists(p)) { wcscpy(out, p); return; }
    }
    out[0] = 0;
}

static const wchar_t *arg_value(const wchar_t *arg, const wchar_t *name) {
    size_t n = wcslen(name);
    return !_wcsnicmp(arg, name, n) ? arg + n : NULL;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show) {
    (void)prev; (void)cmdline;
    g_inst = inst;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    GetModuleFileNameW(NULL, g_setup_dir, MAX_PATH);
    *wcsrchr(g_setup_dir, L'\\') = 0;
    path_join(g_files, g_setup_dir, L"files");

    int argc;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t rom[MAX_PATH] = L"", ex2p[MAX_PATH] = L"", dest[MAX_PATH] = L"";
    int ex2p_given = 0;
    for (int i = 1; i < argc; i++) {
        const wchar_t *v;
        if (!_wcsicmp(argv[i], L"/uninstall")) return uninstall(g_setup_dir);
        if ((v = arg_value(argv[i], L"/rom="))) wcsncpy(rom, v, MAX_PATH - 1);
        else if ((v = arg_value(argv[i], L"/ex2p="))) wcsncpy(ex2p, v, MAX_PATH - 1), ex2p_given = 1;
        else if ((v = arg_value(argv[i], L"/dest="))) wcsncpy(dest, v, MAX_PATH - 1);
        else if ((v = arg_value(argv[i], L"/shot="))) wcsncpy(g_shot, v, MAX_PATH - 1);
        else if (!_wcsicmp(argv[i], L"/silent")) g_silent = 1;
        else if (!_wcsicmp(argv[i], L"/noshortcuts")) g_no_shortcuts = 1;
        else if (!_wcsicmp(argv[i], L"/noregister")) g_no_register = 1;
    }
    if (!rom[0]) guess_rom(rom, L"fgtlayer.zip");
#ifdef NO_EXTRA
    ex2p[0] = 0;
    (void)ex2p_given;
#else
    if (!ex2p_given) guess_rom(ex2p, L"sfex2p.zip");
#endif
    if (!dest[0]) {
        wchar_t la[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, la)))
            swprintf(dest, MAX_PATH, L"%ls\\Programs\\ReLayer", la);
    }

    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    HDC sdc = GetDC(NULL);
    g_dpi = GetDeviceCaps(sdc, LOGPIXELSY);
    ReleaseDC(NULL, sdc);

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"ReLayerSetup";
    RegisterClassW(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
#ifdef NO_EXTRA
    RECT r = {0, 0, S(WIN_W), S(BANNER_H + 20 + 86 + 64 + 40 + 70 + 32 + 22)};
#else
    RECT r = {0, 0, S(WIN_W), S(BANNER_H + 20 + 86 + 86 + 64 + 40 + 70 + 32 + 22)};
#endif
    AdjustWindowRect(&r, style, FALSE);
    g_wnd = CreateWindowW(L"ReLayerSetup", APP_TITLE, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                          r.bottom - r.top, NULL, NULL, inst, NULL);
    create_controls();
    SetWindowTextW(g_rom, rom);
    SetWindowTextW(g_ex2p, ex2p);
    SetWindowTextW(g_dest, dest);
    /* centre on the screen */
    RECT wr, wa;
    GetWindowRect(g_wnd, &wr);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    SetWindowPos(g_wnd, NULL, wa.left + (wa.right - wa.left - (wr.right - wr.left)) / 2,
                 wa.top + (wa.bottom - wa.top - (wr.bottom - wr.top)) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    ShowWindow(g_wnd, g_silent ? SW_SHOWMINNOACTIVE : show);
    UpdateWindow(g_wnd);
    if (g_shot[0]) {
        MSG m;
        for (int i = 0; i < 20; i++) {
            while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
            Sleep(15);
        }
        save_shot();
        return 0;
    }
    if (g_silent) go();

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(g_wnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    CoUninitialize();
    return g_exit_code;
}
