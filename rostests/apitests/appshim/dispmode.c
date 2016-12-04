/*
 * Copyright 2016 Mark Jansen
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <windows.h>
#ifdef __REACTOS__
#include <ntndk.h>
#else
#include <winternl.h>
#endif
#include <stdio.h>
#include <strsafe.h>
#include "wine/test.h"

extern DWORD get_host_winver(void);
static DWORD g_WinVersion;
#define WINVER_ANY     0
#define WINVER_VISTA   0x0600


/* apphelp.dll */
static BOOL(WINAPI* pSdbGetAppPatchDir)(PVOID, LPWSTR, DWORD);

/* aclayers.dll */
static PVOID(WINAPI* pGetHookAPIs)(LPCSTR, LPCWSTR, PDWORD);
static BOOL(WINAPI* pNotifyShims)(DWORD fdwReason, PVOID ptr);


static LONG g_Count;
static DEVMODEA g_LastDevmode;
static DWORD g_LastFlags;

static LONG (WINAPI *pChangeDisplaySettingsA)(_In_opt_ PDEVMODEA lpDevMode, _In_ DWORD dwflags);
LONG WINAPI mChangeDisplaySettingsA(_In_opt_ PDEVMODEA lpDevMode, _In_ DWORD dwflags)
{
    g_Count++;
    g_LastDevmode = *lpDevMode;
    g_LastFlags = dwflags;

    return DISP_CHANGE_FAILED;
}

static LONG g_ThemeCount;
static DWORD g_LastThemeFlags;

static void (WINAPI *pSetThemeAppProperties)(DWORD dwFlags);
void WINAPI mSetThemeAppProperties(DWORD dwFlags)
{
    g_ThemeCount++;
    g_LastThemeFlags = dwFlags;
}


static const WCHAR* shim_dll(const WCHAR* name)
{
    static WCHAR buf[MAX_PATH];
    pSdbGetAppPatchDir(NULL, buf, MAX_PATH);
    StringCchCatW(buf, _countof(buf), name);
    return buf;
}

static void pre_8bit(void)
{
    g_Count = 0;
    memset(&g_LastDevmode, 0, sizeof(g_LastDevmode));
    g_LastFlags = 0xffffffff;
}

static void post_8bit(void)
{
    ok_int(g_Count, 1);
    ok_hex(g_LastDevmode.dmFields & DM_BITSPERPEL, DM_BITSPERPEL);
    ok_int(g_LastDevmode.dmBitsPerPel, 8);
    ok_hex(g_LastFlags, CDS_FULLSCREEN);
}

static void pre_640(void)
{
    g_Count = 0;
    memset(&g_LastDevmode, 0, sizeof(g_LastDevmode));
    g_LastFlags = 0xffffffff;
}

static void post_640(void)
{
    ok_int(g_Count, 1);
    ok_hex(g_LastDevmode.dmFields & (DM_PELSWIDTH | DM_PELSHEIGHT), (DM_PELSWIDTH | DM_PELSHEIGHT));
    ok_int(g_LastDevmode.dmPelsWidth, 640);
    ok_int(g_LastDevmode.dmPelsHeight, 480);
    ok_hex(g_LastFlags, CDS_FULLSCREEN);
}

static void pre_theme(void)
{
    g_ThemeCount = 0;
    g_LastThemeFlags = 0xffffffff;
}

static void post_theme(void)
{
    ok_int(g_ThemeCount, 1);
    ok_hex(g_LastThemeFlags, 0);
}

static PIMAGE_IMPORT_DESCRIPTOR FindImportDescriptor(PBYTE DllBase, PCSTR DllName)
{
    ULONG Size;
    PIMAGE_IMPORT_DESCRIPTOR ImportDescriptor = RtlImageDirectoryEntryToData((HMODULE)DllBase, TRUE, IMAGE_DIRECTORY_ENTRY_IMPORT, &Size);
    while (ImportDescriptor->Name && ImportDescriptor->OriginalFirstThunk)
    {
        PCHAR Name = (PCHAR)(DllBase + ImportDescriptor->Name);
        if (!lstrcmpiA(Name, DllName))
        {
            return ImportDescriptor;
        }
        ImportDescriptor++;
    }
    return NULL;
}

static BOOL RedirectIat(HMODULE TargetDll, PCSTR DllName, PCSTR FunctionName, ULONG_PTR NewFunction, ULONG_PTR* OriginalFunction)
{
    PBYTE DllBase = (PBYTE)TargetDll;
    PIMAGE_IMPORT_DESCRIPTOR ImportDescriptor = FindImportDescriptor(DllBase, DllName);
    if (ImportDescriptor)
    {
        // On loaded images, OriginalFirstThunk points to the name / ordinal of the function
        PIMAGE_THUNK_DATA OriginalThunk = (PIMAGE_THUNK_DATA)(DllBase + ImportDescriptor->OriginalFirstThunk);
        // FirstThunk points to the resolved address.
        PIMAGE_THUNK_DATA FirstThunk = (PIMAGE_THUNK_DATA)(DllBase + ImportDescriptor->FirstThunk);
        while (OriginalThunk->u1.AddressOfData && FirstThunk->u1.Function)
        {
            if (!IMAGE_SNAP_BY_ORDINAL32(OriginalThunk->u1.AddressOfData))
            {
                PIMAGE_IMPORT_BY_NAME ImportName = (PIMAGE_IMPORT_BY_NAME)(DllBase + OriginalThunk->u1.AddressOfData);
                if (!lstrcmpiA((PCSTR)ImportName->Name, FunctionName))
                {
                    DWORD dwOld;
                    VirtualProtect(&FirstThunk->u1.Function, sizeof(ULONG_PTR), PAGE_EXECUTE_READWRITE, &dwOld);
                    *OriginalFunction = FirstThunk->u1.Function;
                    FirstThunk->u1.Function = NewFunction;
                    VirtualProtect(&FirstThunk->u1.Function, sizeof(ULONG_PTR), dwOld, &dwOld);
                    return TRUE;
                }
            }
            OriginalThunk++;
            FirstThunk++;
        }
        skip("Unable to find the Import %s!%s\n", DllName, FunctionName);
    }
    else
    {
        skip("Unable to find the ImportDescriptor for %s\n", DllName);
    }
    return FALSE;
}

static BOOL hook_disp(HMODULE dll)
{
    return RedirectIat(dll, "user32.dll", "ChangeDisplaySettingsA", (ULONG_PTR)mChangeDisplaySettingsA, (ULONG_PTR*)&pChangeDisplaySettingsA);
}

static BOOL hook_theme(HMODULE dll)
{
    return RedirectIat(dll, "uxtheme.dll", "SetThemeAppProperties", (ULONG_PTR)mSetThemeAppProperties, (ULONG_PTR*)&pSetThemeAppProperties);
}


static void test_one(LPCSTR shim, DWORD dwReason, void(*pre)(), void(*post)())
{
    DWORD num_shims = 0;
    WCHAR wide_shim[50] = { 0 };
    PVOID hook;
    BOOL ret;
    MultiByteToWideChar(CP_ACP, 0, shim, -1, wide_shim, 50);

    if (pre)
        pre();

    hook = pGetHookAPIs("", wide_shim, &num_shims);
    if (hook == NULL)
    {
        skip("Skipping tests for layers (%s) not present in this os (0x%x)\n", shim, g_WinVersion);
        return;
    }
    ok(hook != NULL, "Expected hook to be a valid pointer for %s\n", shim);
    ok(num_shims == 0, "Expected not to find any apihooks, got: %u for %s\n", num_shims, shim);

    ret = pNotifyShims(dwReason, NULL);

    /* Win7 and Win10 return 1, w2k3 returns a pointer */
    ok(ret != 0, "Expected pNotifyShims to succeed (%i)\n", ret);

    if (post)
        post();
}

static struct test_info
{
    const char* name;
    const WCHAR* dll;
    DWORD winver;
    DWORD reason;
    BOOL(*hook)(HMODULE);
    void(*pre)(void);
    void(*post)(void);
} tests[] =
{
    { "Force8BitColor", L"\\aclayers.dll", WINVER_ANY, 1, hook_disp, pre_8bit, post_8bit },
    { "Force8BitColor", L"\\aclayers.dll", WINVER_VISTA, 100, hook_disp, pre_8bit, post_8bit },
    { "Force640x480", L"\\aclayers.dll", WINVER_ANY, 1, hook_disp, pre_640, post_640 },
    { "Force640x480", L"\\aclayers.dll", WINVER_VISTA, 100, hook_disp, pre_640, post_640 },
    { "DisableThemes", L"\\acgenral.dll", WINVER_ANY, 1, hook_theme, pre_theme, post_theme },
    { "DisableThemes", L"\\acgenral.dll", WINVER_VISTA, 100, hook_theme, pre_theme, post_theme },
};


static void run_test(size_t n, BOOL unload)
{
    BOOL ret;
    HMODULE dll;
    const WCHAR* buf = shim_dll(tests[n].dll);

    dll = LoadLibraryW(shim_dll(tests[n].dll));
    pGetHookAPIs = (void*)GetProcAddress(dll, "GetHookAPIs");
    pNotifyShims = (void*)GetProcAddress(dll, "NotifyShims");

    if (!pGetHookAPIs || !pNotifyShims)
    {
        skip("aclayers.dll not loaded, or does not export GetHookAPIs or pNotifyShims (%s, %p, %p)\n",
            tests[n].name, pGetHookAPIs, pNotifyShims);
        return;
    }

    ret = tests[n].hook(dll);
    if (ret)
    {
        test_one(tests[n].name, tests[n].reason, tests[n].pre, tests[n].post);
    }
    else
    {
        ok(0, "Unable to redirect functions!\n");
    }
    FreeLibrary(dll);
    if (unload)
    {
        dll = GetModuleHandleW(buf);
        ok(dll == NULL, "Unable to unload %s\n", wine_dbgstr_w(buf));
    }
}


START_TEST(dispmode)
{
    HMODULE dll = LoadLibraryA("apphelp.dll");
    size_t n;
    int argc;
    char **argv;

    pSdbGetAppPatchDir = (void*)GetProcAddress(dll, "SdbGetAppPatchDir");
    if (!pSdbGetAppPatchDir)
    {
        skip("apphelp.dll not loaded, or does not export SdbGetAppPatchDir\n");
        return;
    }

    g_WinVersion = get_host_winver();

    argc = winetest_get_mainargs(&argv);
    if (argc < 3)
    {
        WCHAR path[MAX_PATH];
        GetModuleFileNameW(NULL, path, _countof(path));
        dll = GetModuleHandleW(shim_dll(L"\\aclayers.dll"));
        if (!dll)
            dll = GetModuleHandleW(shim_dll(L"\\acgenral.dll"));
        if (dll != NULL)
            trace("Loaded under a shim, running each test in it's own process\n");

        for (n = 0; n < _countof(tests); ++n)
        {
            LONG failures = winetest_get_failures();

            if (g_WinVersion < tests[n].winver)
                continue;

            if (dll == NULL)
            {
                run_test(n, TRUE);
            }
            else
            {
                WCHAR buf[MAX_PATH+40];
                STARTUPINFOW si = { sizeof(si) };
                PROCESS_INFORMATION pi;
                BOOL created;

                StringCchPrintfW(buf, _countof(buf), L"\"%ls\" dispmode %u", path, n);
                created = CreateProcessW(NULL, buf, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
                ok(created, "Expected CreateProcess to succeed\n");
                if (created)
                {
                    winetest_wait_child_process(pi.hProcess);
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                }
            }

            if (failures != winetest_get_failures())
            {
                trace("Failures from %d (%s)\n", n, tests[n].name);
            }
        }
    }
    else
    {
        n = (size_t)atoi(argv[2]);
        if (n >= 0 && n < _countof(tests))
        {
            run_test(n, FALSE);
        }
        else
        {
            ok(0, "Test out of range: %u\n", n);
        }
    }
}
