









#include "pch.h"
#include "overlay.h"

#include <d3d9.h>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx9.h"


extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{


    void Log(const char* fmt, ...)
    {
        static wchar_t path[MAX_PATH] = {};
        if (path[0] == 0)
        {
            HMODULE self = nullptr;
            ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCWSTR>(&Log), &self);
            ::GetModuleFileNameW(self, path, MAX_PATH);
            wchar_t* slash = wcsrchr(path, L'\\');
            if (slash) slash[1] = 0;
            wcscat_s(path, L"overlay.log");
        }

        FILE* f = nullptr;
        if (_wfopen_s(&f, path, L"a") != 0 || f == nullptr)
            return;

        SYSTEMTIME st = {};
        ::GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);

        fputc('\n', f);
        fclose(f);
    }




    constexpr int kIdxReset      = 16;
    constexpr int kIdxPresent    = 17;
    constexpr int kIdxBeginScene = 41;
    constexpr int kIdxEndScene   = 42;
    constexpr int kIdxClear      = 43;

    using EndSceneFn = HRESULT(WINAPI*)(IDirect3DDevice9*);
    using ResetFn    = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);


    struct HookedVtable
    {
        void** vt;
        void*  orgEndScene;
        void*  orgReset;
    };
    constexpr int kMaxHookedVtables = 8;


    HMODULE           g_hModule      = nullptr;
    HMODULE           g_hD3D9        = nullptr;
    HWND              g_hWnd         = nullptr;
    WNDPROC           g_OrgWndProc   = nullptr;
    bool              g_WndUnicode   = false;




    enum class FsMode
    {
        Off = 0,
        Borderless,
        Bordered,
    };
    FsMode            g_FsMode      = FsMode::Off;
    FsMode            g_FsWant      = FsMode::Off;
    int               g_FsWait      = 0;

    bool              g_WindowSaved = false;
    RECT              g_SavedRect   = {};

    int               g_ReqClientW  = 0;
    int               g_ReqClientH  = 0;
    float             g_PicScale    = 1.0f;

    HookedVtable      g_HookedVtables[kMaxHookedVtables] = {};
    int               g_HookedCount  = 0;

    EndSceneFn        g_OrgEndScene  = nullptr;
    ResetFn           g_OrgReset     = nullptr;

    IDirect3DDevice9* g_pDevice      = nullptr;
    bool              g_ImGuiReady   = false;

    bool              g_ShowMenu     = false;
    bool              g_ShowDemo     = false;




    float             g_UiScale      = 1.0f;
    float             g_UiScaleMin   = 0.5f;
    float             g_UiScaleMax   = 3.0f;
    bool              g_UiScaleDirty = false;
    ImGuiStyle        g_BaseStyle    = {};

    volatile LONG     g_InHook       = 0;
    volatile bool     g_Unloading    = false;
    volatile bool     g_UnloadReq    = false;

    bool              g_WasdToArrows = true;

    struct WasdKeyBinding
    {
        WPARAM src;
        WORD   dst;
        bool   held;
    };

    WasdKeyBinding g_WasdBindings[] =
    {
        { 'W', VK_UP,    false },
        { 'A', VK_LEFT,  false },
        { 'S', VK_DOWN,  false },
        { 'D', VK_RIGHT, false },
    };

    void SendArrowKey(WORD vk, bool down)
    {
        INPUT in = {};
        in.type       = INPUT_KEYBOARD;
        in.ki.wVk     = vk;
        in.ki.wScan   = static_cast<WORD>(::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        in.ki.dwFlags = KEYEVENTF_EXTENDEDKEY | (down ? 0u : KEYEVENTF_KEYUP);
        ::SendInput(1, &in, static_cast<UINT>(sizeof(in)));
    }

    void ReleaseWasdKeys()
    {
        for (WasdKeyBinding& binding : g_WasdBindings)
        {
            if (!binding.held)
                continue;
            binding.held = false;
            SendArrowKey(binding.dst, false);
        }
    }

    void HandleWasdKeyMessage(UINT msg, WPARAM wParam)
    {
        if (!g_WasdToArrows)
            return;

        for (WasdKeyBinding& binding : g_WasdBindings)
        {
            if (wParam != binding.src)
                continue;

            if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !binding.held)
            {
                binding.held = true;
                SendArrowKey(binding.dst, true);
            }
            else if ((msg == WM_KEYUP || msg == WM_SYSKEYUP) && binding.held)
            {
                binding.held = false;
                SendArrowKey(binding.dst, false);
            }
            break;
        }
    }


    float             g_PlayerX      = 0.0f;
    float             g_PlayerY      = 0.0f;
    bool              g_PlayerOk     = false;
    const char*       g_PlayerLabel  = "-";


    struct ViewState
    {
        bool  ok;
        bool  visible;
        int   current;
        float x;
        float y;
        float w;
        float h;
        int   xport;
        int   yport;
        int   wport;
        int   hport;
        float scaleX;
        float scaleY;
    };
    ViewState         g_View         = {};




    constexpr uint32_t kVaImageBase         = 0x00400000;
    constexpr uint32_t kVaObjectTable       = 0x0080861C;
    constexpr uint32_t kVaInstanceArray     = 0x00A1B010;
    constexpr uint32_t kVaInstanceCount     = 0x00A1B018;
    constexpr uint32_t kVaInstanceArray2    = 0x00A1B01C;
    constexpr uint32_t kVaInstanceCount2    = 0x00A1B024;
    constexpr uint32_t kVaInstanceManager   = 0x00A18EDC;
    constexpr uint32_t kOffInstanceId       = 0x60;
    constexpr uint32_t kOffInstanceObjIndex = 0x64;
    constexpr uint32_t kOffInstanceX        = 0x9C;
    constexpr uint32_t kOffInstanceY        = 0xA0;
    constexpr uint32_t kOffInstanceActiveA  = 0x4C;
    constexpr uint32_t kOffInstanceActiveB  = 0x4D;
    constexpr uint32_t kOffInstanceNext     = 0x160;
    constexpr uint32_t kOffObjEntryName     = 0x14;
    constexpr uint32_t kOffObjEntryInstList = 0xD0;
    constexpr uint32_t kOffManagerInstList  = 0x80;
    constexpr uint32_t kOffsetBucketStride  = 8;


    constexpr uint32_t kOffInstanceVisible  = 0x49;
    constexpr uint32_t kOffInstanceSprite   = 0x94;
    constexpr uint32_t kOffInstanceMask     = 0x74;
    constexpr uint32_t kOffInstanceXScale   = 0x80;
    constexpr uint32_t kOffInstanceYScale   = 0x84;
    constexpr uint32_t kOffInstanceAlpha    = 0x8C;
    constexpr uint32_t kOffInstanceBBoxL    = 0xD0;
    constexpr uint32_t kOffInstanceBBoxT    = 0xD4;
    constexpr uint32_t kOffInstanceBBoxR    = 0xD8;
    constexpr uint32_t kOffInstanceBBoxB    = 0xDC;



    constexpr uint32_t kVaSpriteArray       = 0x00A187E0;
    constexpr uint32_t kVaSpriteCount       = 0x00A187D8;
    constexpr uint32_t kOffSpriteBBoxL      = 0x08;
    constexpr uint32_t kOffSpriteBBoxT      = 0x0C;
    constexpr uint32_t kOffSpriteBBoxR      = 0x10;
    constexpr uint32_t kOffSpriteBBoxB      = 0x14;
    constexpr uint32_t kOffSpriteXOffset    = 0x24;
    constexpr uint32_t kOffSpriteYOffset    = 0x28;




    constexpr uint32_t kVaViewCurrent       = 0x00A18ED0;
    constexpr uint32_t kVaViewBase          = 0x00A18EDC;
    constexpr uint32_t kOffViewArray        = 0x48;
    constexpr uint32_t kViewCount           = 8;
    constexpr uint32_t kOffViewVisible      = 0x00;
    constexpr uint32_t kOffViewX            = 0x04;
    constexpr uint32_t kOffViewY            = 0x08;
    constexpr uint32_t kOffViewW            = 0x0C;
    constexpr uint32_t kOffViewH            = 0x10;
    constexpr uint32_t kOffViewXPort        = 0x14;
    constexpr uint32_t kOffViewYPort        = 0x18;
    constexpr uint32_t kOffViewWPort        = 0x1C;
    constexpr uint32_t kOffViewHPort        = 0x20;


    constexpr uint32_t kVaSetFullscreen     = 0x0043C650;
    constexpr uint32_t kVaRequestClientSize = 0x0043CC80;
    constexpr uint32_t kVaIsFullscreen      = 0x0043C6D0;




    constexpr uint32_t kVaSurfaceW          = 0x007B7A5C;
    constexpr uint32_t kVaSurfaceH          = 0x007B7A60;


    constexpr int      kGameBaseW           = 640;
    constexpr int      kGameBaseH           = 480;




    constexpr uint32_t kVaNameToIndex       = 0x0040F070;

    constexpr uint32_t kVaVarContext        = 0x0080894C;
    constexpr uint32_t kOffVarSlots         = 0x04;
    constexpr uint32_t kVarSlotStride       = 16;
    constexpr uint32_t kOffRValueValue      = 0x00;
    constexpr uint32_t kOffRValueType       = 0x0C;
    constexpr int      kVarGlobalBias       = 100000;

    uint32_t g_HeroObjIndex   = 0;
    uint32_t g_SoulObjIndex   = 0;
    int      g_ObjResolveCool = 0;


    uint32_t GameAddr(uint32_t va)
    {
        static uint32_t base = 0;
        if (base == 0)
        {
            HMODULE exe = ::GetModuleHandleW(nullptr);
            base = exe ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(exe)) : kVaImageBase;
        }
        return base + (va - kVaImageBase);
    }


    bool SafeRead8(uint32_t addr, uint8_t& out)
    {
        __try { out = *reinterpret_cast<volatile uint8_t*>(addr); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool SafeRead32(uint32_t addr, uint32_t& out)
    {
        __try { out = *reinterpret_cast<volatile uint32_t*>(addr); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool SafeReadFloat(uint32_t addr, float& out)
    {
        uint32_t bits = 0;
        if (!SafeRead32(addr, bits))
            return false;
        out = *reinterpret_cast<float*>(&bits);
        return true;
    }

    bool SafeReadDouble(uint32_t addr, double& out)
    {
        __try { out = *reinterpret_cast<volatile double*>(addr); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }


    bool StrEqualsAt(uint32_t addr, const char* s)
    {
        for (int i = 0; ; ++i)
        {
            uint8_t c = 0;
            if (!SafeRead8(addr + i, c))
                return false;
            if (c != static_cast<uint8_t>(s[i]))
                return false;
            if (s[i] == '\0')
                return true;
        }
    }


    uint32_t FindObjectIndex(const char* name)
    {
        uint32_t table = 0;
        if (!SafeRead32(GameAddr(kVaObjectTable), table) || table == 0)
            return 0;

        uint32_t buckets = 0;
        uint32_t mask    = 0;
        if (!SafeRead32(table, buckets) || buckets == 0)
            return 0;
        if (!SafeRead32(table + 4, mask) || mask > 0x100000)
            return 0;

        for (uint32_t i = 0; i <= mask; ++i)
        {
            uint32_t node = 0;
            if (!SafeRead32(buckets + i * kOffsetBucketStride, node))
                continue;

            while (node != 0)
            {
                uint32_t key   = 0;
                uint32_t entry = 0;
                if (!SafeRead32(node + 8, key) || !SafeRead32(node + 0xC, entry))
                    break;

                if (entry != 0)
                {
                    uint32_t namePtr = 0;
                    if (SafeRead32(entry + kOffObjEntryName, namePtr) && namePtr != 0 && StrEqualsAt(namePtr, name))
                        return key;
                }

                if (!SafeRead32(node + 4, node))
                    break;
            }
        }
        return 0;
    }


    const char* ObjectNameByIndex(uint32_t index)
    {
        uint32_t table = 0;
        if (!SafeRead32(GameAddr(kVaObjectTable), table) || table == 0)
            return "?";

        uint32_t buckets = 0;
        uint32_t mask    = 0;
        if (!SafeRead32(table, buckets) || buckets == 0)
            return "?";
        if (!SafeRead32(table + 4, mask) || mask > 0x100000)
            return "?";

        uint32_t node = 0;
        if (!SafeRead32(buckets + (index & mask) * kOffsetBucketStride, node))
            return "?";

        while (node != 0)
        {
            uint32_t key   = 0;
            uint32_t entry = 0;
            if (!SafeRead32(node + 8, key) || !SafeRead32(node + 0xC, entry))
                return "?";

            if (key == index)
            {
                uint32_t namePtr = 0;
                if (entry == 0 || !SafeRead32(entry + kOffObjEntryName, namePtr) || namePtr == 0)
                    return "<noname>";

                static char buf[64];
                int i = 0;
                for (; i < 63; ++i)
                {
                    uint8_t c = 0;
                    if (!SafeRead8(namePtr + i, c))
                        break;
                    buf[i] = static_cast<char>(c);
                    if (c == 0)
                        break;
                }
                buf[i] = '\0';
                return buf;
            }

            if (!SafeRead32(node + 4, node))
                break;
        }
        return "?";
    }


    void EnsurePlayerObjectIndexes()
    {
        if (g_HeroObjIndex != 0 && g_SoulObjIndex != 0)
            return;
        if (g_ObjResolveCool > 0)
        {
            --g_ObjResolveCool;
            return;
        }

        if (g_HeroObjIndex == 0)
            g_HeroObjIndex = FindObjectIndex("obj_mainchara");
        if (g_SoulObjIndex == 0)
            g_SoulObjIndex = FindObjectIndex("obj_heart_pl");

        if (g_HeroObjIndex == 0 || g_SoulObjIndex == 0)
            g_ObjResolveCool = 60;
    }


    bool IsInstanceActive(uint32_t inst)
    {
        uint8_t a = 0;
        uint8_t b = 0;
        SafeRead8(inst + kOffInstanceActiveA, a);
        SafeRead8(inst + kOffInstanceActiveB, b);
        return a == 0 && b == 0;
    }


    bool FindInstanceInArray(uint32_t objIndex, bool activeOnly, uint32_t& out)
    {
        uint32_t instBase  = 0;
        uint32_t instCount = 0;
        if (!SafeRead32(GameAddr(kVaInstanceArray), instBase) || instBase == 0)
            return false;
        if (!SafeRead32(GameAddr(kVaInstanceCount), instCount) || instCount == 0 || instCount > 100000)
            return false;

        for (uint32_t i = 0; i < instCount; ++i)
        {
            uint32_t inst = 0;
            if (!SafeRead32(instBase + i * 4, inst) || inst == 0)
                continue;

            uint32_t obj = 0;
            if (!SafeRead32(inst + kOffInstanceObjIndex, obj) || obj != objIndex)
                continue;

            if (activeOnly && !IsInstanceActive(inst))
                continue;

            out = inst;
            return true;
        }
        return false;
    }


    bool FindInstanceInManagerList(uint32_t objIndex, bool activeOnly, uint32_t& out)
    {
        uint32_t mgr = 0;
        if (!SafeRead32(GameAddr(kVaInstanceManager), mgr) || mgr == 0)
            return false;

        uint32_t inst = 0;
        if (!SafeRead32(mgr + kOffManagerInstList, inst))
            return false;

        for (int guard = 0; inst != 0 && guard < 100000; ++guard)
        {
            uint32_t obj = 0;
            if (!SafeRead32(inst + kOffInstanceObjIndex, obj))
                return false;

            if (obj == objIndex && (!activeOnly || IsInstanceActive(inst)))
            {
                out = inst;
                return true;
            }

            uint32_t next = 0;
            if (!SafeRead32(inst + kOffInstanceNext, next))
                return false;
            inst = next;
        }
        return false;
    }



    void DumpPlayerDiag()
    {

        static int cool = 0;
        if (cool > 0)
        {
            --cool;
            return;
        }
        cool = 120;

        uint32_t table = 0;
        uint32_t buckets = 0;
        uint32_t mask = 0;
        SafeRead32(GameAddr(kVaObjectTable), table);
        if (table != 0)
        {
            SafeRead32(table, buckets);
            SafeRead32(table + 4, mask);
        }
        Log("[diag] objTable=0x%08X buckets=0x%08X mask=0x%X", table, buckets, mask);
        Log("[diag] heroIdx=%u soulIdx=%u", g_HeroObjIndex, g_SoulObjIndex);

        uint32_t arr = 0;
        uint32_t cnt = 0;
        SafeRead32(GameAddr(kVaInstanceArray), arr);
        SafeRead32(GameAddr(kVaInstanceCount), cnt);
        Log("[diag] arrayA base=0x%08X count=%u", arr, cnt);

        if (arr != 0 && cnt > 0 && cnt <= 100000)
        {
            for (uint32_t i = 0; i < cnt && i < 24; ++i)
            {
                uint32_t inst = 0;
                if (!SafeRead32(arr + i * 4, inst) || inst == 0)
                    continue;

                uint32_t obj = 0;
                uint32_t id  = 0;
                float    x   = 0.0f;
                float    y   = 0.0f;
                SafeRead32(inst + kOffInstanceObjIndex, obj);
                SafeRead32(inst + kOffInstanceId, id);
                SafeReadFloat(inst + kOffInstanceX, x);
                SafeReadFloat(inst + kOffInstanceY, y);
                Log("[diag] A[%u] inst=0x%08X id=%u obj=%u(%s) x=%.1f y=%.1f act=%d",
                    i, inst, id, obj, ObjectNameByIndex(obj), x, y, IsInstanceActive(inst) ? 1 : 0);
            }
        }

        uint32_t mgr = 0;
        SafeRead32(GameAddr(kVaInstanceManager), mgr);
        Log("[diag] manager=0x%08X", mgr);

        if (mgr != 0)
        {
            uint32_t inst = 0;
            SafeRead32(mgr + kOffManagerInstList, inst);
            int n = 0;
            for (; inst != 0 && n < 24; ++n)
            {
                uint32_t obj = 0;
                float    x   = 0.0f;
                float    y   = 0.0f;
                if (!SafeRead32(inst + kOffInstanceObjIndex, obj))
                    break;

                SafeReadFloat(inst + kOffInstanceX, x);
                SafeReadFloat(inst + kOffInstanceY, y);
                Log("[diag] M[%d] inst=0x%08X obj=%u(%s) x=%.1f y=%.1f act=%d",
                    n, inst, obj, ObjectNameByIndex(obj), x, y, IsInstanceActive(inst) ? 1 : 0);

                uint32_t next = 0;
                if (!SafeRead32(inst + kOffInstanceNext, next))
                    break;
                inst = next;
            }
            Log("[diag] manager list walked=%d", n);
        }

        Log("[diag] player ok=%d x=%.1f y=%.1f label=%s",
            g_PlayerOk ? 1 : 0, g_PlayerX, g_PlayerY, g_PlayerLabel);
        Log("[diag] view ok=%d cur=%d vis=%d x=%.1f y=%.1f w=%.1f h=%.1f port=(%d,%d,%d,%d)",
            g_View.ok ? 1 : 0, g_View.current, g_View.visible ? 1 : 0,
            g_View.x, g_View.y, g_View.w, g_View.h,
            g_View.xport, g_View.yport, g_View.wport, g_View.hport);


        uint32_t rawSurfW = 0;
        uint32_t rawSurfH = 0;
        SafeRead32(GameAddr(kVaSurfaceW), rawSurfW);
        SafeRead32(GameAddr(kVaSurfaceH), rawSurfH);
        const ImVec2 disp = ::ImGui::GetIO().DisplaySize;
        Log("[diag] surface=%d x %d  client=%.0f x %.0f",
            static_cast<int32_t>(rawSurfW), static_cast<int32_t>(rawSurfH), disp.x, disp.y);
    }


    void UpdatePlayerPosition()
    {
        g_PlayerOk = false;
        EnsurePlayerObjectIndexes();


        const uint32_t targets[2] = { g_SoulObjIndex, g_HeroObjIndex };
        const char*    labels[2]  = { "红心(战斗)", "主角(地图)" };


        for (int pass = 0; pass < 2; ++pass)
        {
            const bool activeOnly = (pass == 0);

            for (int t = 0; t < 2; ++t)
            {
                if (targets[t] == 0)
                    continue;

                uint32_t inst = 0;
                if (!FindInstanceInArray(targets[t], activeOnly, inst) &&
                    !FindInstanceInManagerList(targets[t], activeOnly, inst))
                    continue;

                float x = 0.0f;
                float y = 0.0f;
                if (!SafeReadFloat(inst + kOffInstanceX, x) || !SafeReadFloat(inst + kOffInstanceY, y))
                    continue;

                g_PlayerX = x;
                g_PlayerY = y;
                g_PlayerOk = true;
                g_PlayerLabel = labels[t];
                return;
            }
        }
    }



    void UpdateViewState()
    {
        g_View.ok = false;

        uint32_t current = 0;
        if (!SafeRead32(GameAddr(kVaViewCurrent), current))
            return;


        const uint32_t index = (current < kViewCount) ? current : 0;

        uint32_t base = 0;
        if (!SafeRead32(GameAddr(kVaViewBase), base) || base == 0)
            return;

        uint32_t view = 0;
        if (!SafeRead32(base + kOffViewArray + index * 4, view) || view == 0)
            return;

        uint8_t  visible = 0;
        float    x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
        uint32_t xport = 0, yport = 0, wport = 0, hport = 0;
        if (!SafeRead8(view + kOffViewVisible, visible))
            return;
        if (!SafeReadFloat(view + kOffViewX, x) || !SafeReadFloat(view + kOffViewY, y) ||
            !SafeReadFloat(view + kOffViewW, w) || !SafeReadFloat(view + kOffViewH, h))
            return;
        if (!SafeRead32(view + kOffViewXPort, xport) || !SafeRead32(view + kOffViewYPort, yport) ||
            !SafeRead32(view + kOffViewWPort, wport) || !SafeRead32(view + kOffViewHPort, hport))
            return;

        g_View.ok      = true;
        g_View.visible = (visible != 0);
        g_View.current = static_cast<int>(current);
        g_View.x       = x;
        g_View.y       = y;
        g_View.w       = w;
        g_View.h       = h;
        g_View.xport   = static_cast<int>(xport);
        g_View.yport   = static_cast<int>(yport);
        g_View.wport   = static_cast<int>(wport);
        g_View.hport   = static_cast<int>(hport);
        g_View.scaleX  = (w > 0.0f) ? (static_cast<float>(wport) / w) : 1.0f;
        g_View.scaleY  = (h > 0.0f) ? (static_cast<float>(hport) / h) : 1.0f;
    }




    struct ScreenMap
    {
        float scale;
        float marginX;
        float marginY;
    };

    ScreenMap ComputeScreenMap()
    {
        const ImVec2 display = ::ImGui::GetIO().DisplaySize;


        float surfW = static_cast<float>(kGameBaseW);
        float surfH = static_cast<float>(kGameBaseH);
        uint32_t rawW = 0;
        uint32_t rawH = 0;
        if (SafeRead32(GameAddr(kVaSurfaceW), rawW) && SafeRead32(GameAddr(kVaSurfaceH), rawH))
        {
            const int32_t sw = static_cast<int32_t>(rawW);
            const int32_t sh = static_cast<int32_t>(rawH);
            if (sw > 0 && sh > 0 && sw <= 16384 && sh <= 16384)
            {
                surfW = static_cast<float>(sw);
                surfH = static_cast<float>(sh);
            }
        }

        ScreenMap map = {};
        map.scale = 1.0f;
        if (surfW > 0.0f && surfH > 0.0f && display.x > 0.0f && display.y > 0.0f)
        {
            const float kx = display.x / surfW;
            const float ky = display.y / surfH;
            map.scale   = (kx < ky) ? kx : ky;
            map.marginX = (display.x - surfW * map.scale) * 0.5f;
            map.marginY = (display.y - surfH * map.scale) * 0.5f;
        }
        return map;
    }


    void WorldToScreen(float wx, float wy, float& sx, float& sy)
    {
        const ScreenMap map = ComputeScreenMap();


        const float px = static_cast<float>(g_View.xport) + (wx - g_View.x) * g_View.scaleX;
        const float py = static_cast<float>(g_View.yport) + (wy - g_View.y) * g_View.scaleY;


        sx = map.marginX + px * map.scale;
        sy = map.marginY + py * map.scale;
    }



    constexpr int kMaxEntities = 1024;

    struct EntityEntry
    {
        uint32_t inst;
        uint32_t obj;
        float    x;
        float    y;
        float    left;
        float    top;
        float    right;
        float    bottom;
        bool     hasBox;
        bool     active;
        bool     visible;
        float    alpha;
    };

    EntityEntry g_Entities[kMaxEntities] = {};
    int         g_EntityCount            = 0;





    bool ResolveInstanceBox(uint32_t inst, float x, float y, float& l, float& t, float& r, float& b)
    {
        uint32_t raw[4] = {};
        if (SafeRead32(inst + kOffInstanceBBoxL, raw[0]) && SafeRead32(inst + kOffInstanceBBoxT, raw[1]) &&
            SafeRead32(inst + kOffInstanceBBoxR, raw[2]) && SafeRead32(inst + kOffInstanceBBoxB, raw[3]))
        {
            const int32_t il = static_cast<int32_t>(raw[0]);
            const int32_t it = static_cast<int32_t>(raw[1]);
            const int32_t ir = static_cast<int32_t>(raw[2]);
            const int32_t ib = static_cast<int32_t>(raw[3]);
            if (ir > il && ib > it)
            {
                l = static_cast<float>(il);
                t = static_cast<float>(it);
                r = static_cast<float>(ir);
                b = static_cast<float>(ib);
                return true;
            }
        }


        uint32_t spriteIdx = 0;
        if (!SafeRead32(inst + kOffInstanceSprite, spriteIdx))
            return false;
        int32_t si = static_cast<int32_t>(spriteIdx);
        if (si < 0)
        {
            if (!SafeRead32(inst + kOffInstanceMask, spriteIdx))
                return false;
            si = static_cast<int32_t>(spriteIdx);
        }
        if (si < 0)
            return false;

        uint32_t array = 0;
        uint32_t count = 0;
        if (!SafeRead32(GameAddr(kVaSpriteArray), array) || array == 0)
            return false;
        if (!SafeRead32(GameAddr(kVaSpriteCount), count) || static_cast<uint32_t>(si) >= count)
            return false;

        uint32_t sprite = 0;
        if (!SafeRead32(array + static_cast<uint32_t>(si) * 4, sprite) || sprite == 0)
            return false;

        uint32_t sbl = 0, sbt = 0, sbr = 0, sbb = 0, sxoff = 0, syoff = 0;
        if (!SafeRead32(sprite + kOffSpriteBBoxL, sbl) || !SafeRead32(sprite + kOffSpriteBBoxT, sbt) ||
            !SafeRead32(sprite + kOffSpriteBBoxR, sbr) || !SafeRead32(sprite + kOffSpriteBBoxB, sbb) ||
            !SafeRead32(sprite + kOffSpriteXOffset, sxoff) || !SafeRead32(sprite + kOffSpriteYOffset, syoff))
            return false;

        const int32_t il = static_cast<int32_t>(sbl);
        const int32_t it = static_cast<int32_t>(sbt);
        const int32_t ir = static_cast<int32_t>(sbr);
        const int32_t ib = static_cast<int32_t>(sbb);
        if (ir <= il || ib <= it)
            return false;

        float xs = 1.0f;
        float ys = 1.0f;
        SafeReadFloat(inst + kOffInstanceXScale, xs);
        SafeReadFloat(inst + kOffInstanceYScale, ys);
        if (xs == 0.0f) xs = 1.0f;
        if (ys == 0.0f) ys = 1.0f;

        l = x + (static_cast<float>(il) - static_cast<float>(static_cast<int32_t>(sxoff))) * xs;
        t = y + (static_cast<float>(it) - static_cast<float>(static_cast<int32_t>(syoff))) * ys;
        r = l + static_cast<float>(ir - il + 1) * xs;
        b = t + static_cast<float>(ib - it + 1) * ys;
        return true;
    }

    void UpdateEntityList()
    {
        g_EntityCount = 0;

        uint32_t mgr = 0;
        if (!SafeRead32(GameAddr(kVaInstanceManager), mgr) || mgr == 0)
            return;

        uint32_t inst = 0;
        if (!SafeRead32(mgr + kOffManagerInstList, inst))
            return;

        for (int guard = 0; inst != 0 && guard < 100000 && g_EntityCount < kMaxEntities; ++guard)
        {
            uint32_t obj = 0;
            if (!SafeRead32(inst + kOffInstanceObjIndex, obj))
                break;

            float x = 0.0f;
            float y = 0.0f;
            SafeReadFloat(inst + kOffInstanceX, x);
            SafeReadFloat(inst + kOffInstanceY, y);

            uint8_t visible = 0;
            SafeRead8(inst + kOffInstanceVisible, visible);

            float alpha = 1.0f;
            SafeReadFloat(inst + kOffInstanceAlpha, alpha);

            EntityEntry& e = g_Entities[g_EntityCount++];
            e.inst    = inst;
            e.obj     = obj;
            e.x       = x;
            e.y       = y;
            e.hasBox  = ResolveInstanceBox(inst, x, y, e.left, e.top, e.right, e.bottom);
            e.active  = IsInstanceActive(inst);
            e.visible = (visible != 0);
            e.alpha   = alpha;

            uint32_t next = 0;
            if (!SafeRead32(inst + kOffInstanceNext, next))
                break;
            inst = next;
        }
    }





    constexpr int kEspCatCount = 5;
    constexpr int kEspCatOther = kEspCatCount - 1;

    struct EspCategory
    {
        const char* label;
        bool        enabled;
        float       color[4];
        char        patterns[128];
    };




    EspCategory g_EspCats[kEspCatCount] =
    {
        { "玩家",   true,  { 0.20f, 1.00f, 0.35f, 1.00f }, "obj_mainchara,obj_heart_pl"   },
        { "敌人",   true,  { 1.00f, 0.30f, 0.30f, 1.00f }, ""                             },
        { "可交互", true,  { 0.30f, 0.80f, 1.00f, 1.00f }, "obj_readablesign*"            },
        { "地形",   false, { 0.60f, 0.60f, 0.60f, 1.00f }, "obj_fakewater*,obj_solid*"    },
        { "其它",   true,  { 1.00f, 0.60f, 0.20f, 1.00f }, ""                             },
    };


    bool WildcardMatch(const char* pat, const char* str)
    {
        while (*pat != '\0')
        {
            if (*pat == '*')
            {
                ++pat;
                if (*pat == '\0')
                    return true;
                for (; *str != '\0'; ++str)
                {
                    if (WildcardMatch(pat, str))
                        return true;
                }
                return false;
            }
            if (*str == '\0')
                return false;
            if (*pat != '?' && *pat != *str)
                return false;
            ++pat;
            ++str;
        }
        return *str == '\0';
    }


    bool NameMatchesPatterns(const char* patterns, const char* name)
    {
        const char* p = patterns;
        while (*p != '\0')
        {
            while (*p == ' ' || *p == ',')
                ++p;
            if (*p == '\0')
                break;

            const char* start = p;
            while (*p != '\0' && *p != ',')
                ++p;

            const char* end = p;
            while (end > start && end[-1] == ' ')
                --end;

            if (end > start)
            {
                char buf[128] = {};
                const size_t n = static_cast<size_t>(end - start);
                if (n < sizeof(buf))
                {
                    memcpy(buf, start, n);
                    if (WildcardMatch(buf, name))
                        return true;
                }
            }
        }
        return false;
    }


    int MatchEspCategory(const char* name)
    {
        for (int c = 0; c < kEspCatOther; ++c)
        {
            if (NameMatchesPatterns(g_EspCats[c].patterns, name))
                return c;
        }
        return kEspCatOther;
    }


    void DumpRoomObjectNames()
    {
        char names[128][64] = {};
        int  count = 0;

        for (int i = 0; i < g_EntityCount && count < 128; ++i)
        {
            const char* name = ObjectNameByIndex(g_Entities[i].obj);

            bool dup = false;
            for (int j = 0; j < count; ++j)
            {
                if (strcmp(names[j], name) == 0)
                {
                    dup = true;
                    break;
                }
            }
            if (dup)
                continue;

            size_t n = strlen(name);
            if (n > sizeof(names[count]) - 1)
                n = sizeof(names[count]) - 1;
            memcpy(names[count], name, n);
            names[count][n] = '\0';
            ++count;
        }

        Log("[dump] 当前房间对象名 %d 个（去重）:", count);
        for (int i = 0; i < count; ++i)
            Log("[dump]   %s", names[i]);
    }




    struct EspConfig
    {
        bool  on          = false;
        bool  drawBox     = true;
        bool  drawName    = true;
        bool  drawDist    = false;
        bool  drawEnemyHp = false;
        bool  onlyActive  = true;
        bool  onlyVisible = true;
        float maxDist     = 0.0f;
        char  filter[64]  = "";
    };

    EspConfig g_Esp      = {};
    int       g_EspDrawn = 0;
    int       g_EspShown[kEspCatCount] = {};




    const wchar_t* ConfigPath()
    {
        static wchar_t path[MAX_PATH] = {};
        if (path[0] == 0)
        {
            HMODULE self = nullptr;
            ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCWSTR>(&ConfigPath), &self);
            ::GetModuleFileNameW(self, path, MAX_PATH);
            wchar_t* slash = wcsrchr(path, L'\\');
            if (slash) slash[1] = 0;
            wcscat_s(path, L"overlay.ini");
        }
        return path;
    }

    int ConfigGetInt(const wchar_t* sec, const wchar_t* key, int def)
    {
        return static_cast<int>(::GetPrivateProfileIntW(sec, key, def, ConfigPath()));
    }

    void ConfigSetInt(const wchar_t* sec, const wchar_t* key, int val)
    {
        wchar_t buf[32] = {};
        swprintf_s(buf, L"%d", val);
        ::WritePrivateProfileStringW(sec, key, buf, ConfigPath());
    }

    float ConfigGetFloat(const wchar_t* sec, const wchar_t* key, float def)
    {
        wchar_t buf[64] = {};
        ::GetPrivateProfileStringW(sec, key, L"", buf, 64, ConfigPath());
        if (buf[0] == 0)
            return def;
        return static_cast<float>(wcstod(buf, nullptr));
    }

    void ConfigSetFloat(const wchar_t* sec, const wchar_t* key, float val)
    {
        wchar_t buf[64] = {};
        swprintf_s(buf, L"%.4f", val);
        ::WritePrivateProfileStringW(sec, key, buf, ConfigPath());
    }


    void ConfigGetString(const wchar_t* sec, const wchar_t* key, char* out, int outBytes, const char* def)
    {
        size_t nd = (def != nullptr) ? strlen(def) : 0;
        if (nd > static_cast<size_t>(outBytes) - 1)
            nd = static_cast<size_t>(outBytes) - 1;
        if (nd > 0 && def != nullptr)
            memcpy(out, def, nd);
        out[nd] = '\0';

        wchar_t buf[256] = {};
        ::GetPrivateProfileStringW(sec, key, L"", buf, 256, ConfigPath());
        if (buf[0] == 0)
            return;

        ::WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, outBytes, nullptr, nullptr);
        out[outBytes - 1] = '\0';
    }

    void ConfigSetString(const wchar_t* sec, const wchar_t* key, const char* val)
    {
        wchar_t buf[256] = {};
        ::MultiByteToWideChar(CP_UTF8, 0, val, -1, buf, 256);
        ::WritePrivateProfileStringW(sec, key, buf, ConfigPath());
    }

    void ConfigSetColor(const wchar_t* sec, const wchar_t* key, const float c[4])
    {
        wchar_t buf[128] = {};
        swprintf_s(buf, L"%.4f,%.4f,%.4f,%.4f", c[0], c[1], c[2], c[3]);
        ::WritePrivateProfileStringW(sec, key, buf, ConfigPath());
    }

    void ConfigGetColor(const wchar_t* sec, const wchar_t* key, float c[4])
    {
        wchar_t buf[128] = {};
        ::GetPrivateProfileStringW(sec, key, L"", buf, 128, ConfigPath());
        if (buf[0] == 0)
            return;

        float r = c[0];
        float g = c[1];
        float b = c[2];
        float a = c[3];
        if (swscanf_s(buf, L"%f,%f,%f,%f", &r, &g, &b, &a) < 3)
            return;

        const float v[4] = { r, g, b, a };
        for (int i = 0; i < 4; ++i)
            c[i] = (v[i] < 0.0f) ? 0.0f : ((v[i] > 1.0f) ? 1.0f : v[i]);
    }


    void LoadConfig()
    {
        g_UiScale    = ConfigGetFloat(L"ui", L"scale",    g_UiScale);
        g_UiScaleMin = ConfigGetFloat(L"ui", L"scaleMin", g_UiScaleMin);
        g_UiScaleMax = ConfigGetFloat(L"ui", L"scaleMax", g_UiScaleMax);
        if (g_UiScaleMin < 0.25f) g_UiScaleMin = 0.25f;
        if (g_UiScaleMax > 4.0f)  g_UiScaleMax = 4.0f;
        if (g_UiScaleMax < g_UiScaleMin) g_UiScaleMax = g_UiScaleMin;
        if (g_UiScale < g_UiScaleMin) g_UiScale = g_UiScaleMin;
        if (g_UiScale > g_UiScaleMax) g_UiScale = g_UiScaleMax;
        g_UiScaleDirty = true;

        g_WasdToArrows = ConfigGetInt(L"input", L"wasdToArrows", g_WasdToArrows ? 1 : 0) != 0;
        if (!g_WasdToArrows)
            ReleaseWasdKeys();

        g_PicScale = ConfigGetFloat(L"window", L"picScale", g_PicScale);
        if (g_PicScale < 0.5f) g_PicScale = 0.5f;
        if (g_PicScale > 8.0f) g_PicScale = 8.0f;


        if (g_PicScale < 0.999f || g_PicScale > 1.001f)
        {
            g_ReqClientW = static_cast<int>(kGameBaseW * g_PicScale + 0.5f);
            g_ReqClientH = static_cast<int>(kGameBaseH * g_PicScale + 0.5f);
        }

        const int fs = ConfigGetInt(L"window", L"fsMode", static_cast<int>(g_FsWant));
        if (fs >= 0 && fs <= 2)
            g_FsWant = static_cast<FsMode>(fs);

        g_Esp.on          = ConfigGetInt(L"esp", L"on",          g_Esp.on ? 1 : 0) != 0;
        g_Esp.drawBox     = ConfigGetInt(L"esp", L"drawBox",     g_Esp.drawBox ? 1 : 0) != 0;
        g_Esp.drawName    = ConfigGetInt(L"esp", L"drawName",    g_Esp.drawName ? 1 : 0) != 0;
        g_Esp.drawDist    = ConfigGetInt(L"esp", L"drawDist",    g_Esp.drawDist ? 1 : 0) != 0;
        g_Esp.drawEnemyHp = ConfigGetInt(L"esp", L"drawEnemyHp", g_Esp.drawEnemyHp ? 1 : 0) != 0;
        g_Esp.onlyActive  = ConfigGetInt(L"esp", L"onlyActive",  g_Esp.onlyActive ? 1 : 0) != 0;
        g_Esp.onlyVisible = ConfigGetInt(L"esp", L"onlyVisible", g_Esp.onlyVisible ? 1 : 0) != 0;
        g_Esp.maxDist     = ConfigGetFloat(L"esp", L"maxDist", g_Esp.maxDist);
        ConfigGetString(L"esp", L"filter", g_Esp.filter, sizeof(g_Esp.filter), g_Esp.filter);

        for (int c = 0; c < kEspCatCount; ++c)
        {
            wchar_t sec[32] = {};
            swprintf_s(sec, L"esp_cat_%d", c);

            g_EspCats[c].enabled = ConfigGetInt(sec, L"enabled", g_EspCats[c].enabled ? 1 : 0) != 0;
            ConfigGetColor(sec, L"color", g_EspCats[c].color);
            ConfigGetString(sec, L"patterns", g_EspCats[c].patterns, sizeof(g_EspCats[c].patterns), g_EspCats[c].patterns);
        }

        Log("config: loaded %ls", ConfigPath());
        Log("config: ui=%.2f[%.2f..%.2f] pic=%.2f fs=%d req=%dx%d esp=%d cats=%d%d%d%d%d",
            g_UiScale, g_UiScaleMin, g_UiScaleMax, g_PicScale, static_cast<int>(g_FsWant),
            g_ReqClientW, g_ReqClientH, g_Esp.on ? 1 : 0,
            g_EspCats[0].enabled ? 1 : 0, g_EspCats[1].enabled ? 1 : 0, g_EspCats[2].enabled ? 1 : 0,
            g_EspCats[3].enabled ? 1 : 0, g_EspCats[4].enabled ? 1 : 0);
    }

    void SaveConfig()
    {
        ConfigSetFloat(L"ui", L"scale",    g_UiScale);
        ConfigSetFloat(L"ui", L"scaleMin", g_UiScaleMin);
        ConfigSetFloat(L"ui", L"scaleMax", g_UiScaleMax);

        ConfigSetFloat(L"window", L"picScale", g_PicScale);
        ConfigSetInt(L"window", L"fsMode", static_cast<int>(g_FsWant));

        ConfigSetInt(L"input", L"wasdToArrows", g_WasdToArrows ? 1 : 0);

        ConfigSetInt(L"esp", L"on",          g_Esp.on ? 1 : 0);
        ConfigSetInt(L"esp", L"drawBox",     g_Esp.drawBox ? 1 : 0);
        ConfigSetInt(L"esp", L"drawName",    g_Esp.drawName ? 1 : 0);
        ConfigSetInt(L"esp", L"drawDist",    g_Esp.drawDist ? 1 : 0);
        ConfigSetInt(L"esp", L"drawEnemyHp", g_Esp.drawEnemyHp ? 1 : 0);
        ConfigSetInt(L"esp", L"onlyActive",  g_Esp.onlyActive ? 1 : 0);
        ConfigSetInt(L"esp", L"onlyVisible", g_Esp.onlyVisible ? 1 : 0);
        ConfigSetFloat(L"esp", L"maxDist", g_Esp.maxDist);
        ConfigSetString(L"esp", L"filter", g_Esp.filter);

        for (int c = 0; c < kEspCatCount; ++c)
        {
            wchar_t sec[32] = {};
            swprintf_s(sec, L"esp_cat_%d", c);

            ConfigSetInt(sec, L"enabled", g_EspCats[c].enabled ? 1 : 0);
            ConfigSetColor(sec, L"color", g_EspCats[c].color);
            ConfigSetString(sec, L"patterns", g_EspCats[c].patterns);
        }

        Log("config: saved %ls", ConfigPath());
    }







    enum
    {
        kStatHp = 0,
        kStatMaxHp,
        kStatLv,
        kStatLove,
        kStatXp,
        kStatGold,
        kStatMonsterHp,
        kStatMonsterMaxHp,
        kStatCount
    };

    struct VarStat
    {
        const char* name;
        double      value;
        int         rawIndex;
        uint32_t    type;
        bool        ok;
    };

    VarStat g_Stats[kStatCount] =
    {
        { "hp",            0.0, -1, 0, false },
        { "maxhp",         0.0, -1, 0, false },
        { "lv",            0.0, -1, 0, false },
        { "love",          0.0, -1, 0, false },
        { "xp",            0.0, -1, 0, false },
        { "gold",          0.0, -1, 0, false },
        { "monsterhp",     0.0, -1, 0, false },
        { "monstermaxhp",  0.0, -1, 0, false },
    };
    int g_StatDiagCool = 0;


    int NameToVarIndex(const char* name)
    {
        __try
        {
            auto fn = reinterpret_cast<int(__cdecl*)(const char*)>(GameAddr(kVaNameToIndex));
            return fn(name);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }


    bool ResolveGlobalSlot(const char* name, uint32_t& slot, int& rawIndex)
    {
        rawIndex = NameToVarIndex(name);
        if (rawIndex < 0)
            return false;

        const int slotIndex = (rawIndex >= kVarGlobalBias) ? (rawIndex - kVarGlobalBias) : rawIndex;

        uint32_t ctx = 0;
        if (!SafeRead32(GameAddr(kVaVarContext), ctx) || ctx == 0)
            return false;

        uint32_t slots = 0;
        if (!SafeRead32(ctx + kOffVarSlots, slots) || slots == 0)
            return false;

        slot = slots + kVarSlotStride * static_cast<uint32_t>(slotIndex);
        return true;
    }


    bool GetGlobalNumber(const char* name, double& out, int& rawIndex, uint32_t& type)
    {
        uint32_t slot = 0;
        if (!ResolveGlobalSlot(name, slot, rawIndex))
            return false;
        if (!SafeRead32(slot + kOffRValueType, type))
            return false;
        type &= 0x00FFFFFFu;
        return SafeReadDouble(slot + kOffRValueValue, out);
    }

    void UpdateStats()
    {
        for (int i = 0; i < kStatCount; ++i)
        {
            VarStat& s = g_Stats[i];
            s.ok = GetGlobalNumber(s.name, s.value, s.rawIndex, s.type);
        }


        if (g_StatDiagCool > 0)
        {
            --g_StatDiagCool;
            return;
        }
        g_StatDiagCool = 120;

        for (int i = 0; i < kStatCount; ++i)
        {
            const VarStat& s = g_Stats[i];
            Log("[stat] %-14s ok=%d idx=%d type=%u value=%.3f",
                s.name, s.ok ? 1 : 0, s.rawIndex, s.type, s.value);
        }
    }

    void DrawEsp()
    {
        g_EspDrawn = 0;
        for (int c = 0; c < kEspCatCount; ++c)
            g_EspShown[c] = 0;


        if (!g_Esp.on || !g_View.ok)
            return;

        const ImVec2 display = ::ImGui::GetIO().DisplaySize;
        if (display.x <= 1.0f || display.y <= 1.0f)
            return;

        ImDrawList* dl = ::ImGui::GetBackgroundDrawList();
        if (dl == nullptr)
            return;

        ImU32 catColors[kEspCatCount] = {};
        for (int c = 0; c < kEspCatCount; ++c)
        {
            catColors[c] = ::ImGui::ColorConvertFloat4ToU32(ImVec4(
                g_EspCats[c].color[0], g_EspCats[c].color[1], g_EspCats[c].color[2], g_EspCats[c].color[3]));
        }
        const ImU32 colShadow = IM_COL32(0, 0, 0, 200);

        for (int i = 0; i < g_EntityCount; ++i)
        {
            const EntityEntry& e = g_Entities[i];

            if (!e.hasBox)
                continue;
            if (g_Esp.onlyActive && !e.active)
                continue;
            if (g_Esp.onlyVisible && (!e.visible || e.alpha <= 0.01f))
                continue;

            const char* name = ObjectNameByIndex(e.obj);
            if (g_Esp.filter[0] != '\0' && strstr(name, g_Esp.filter) == nullptr)
                continue;

            const int cat = MatchEspCategory(name);
            if (!g_EspCats[cat].enabled)
                continue;

            float dist = 0.0f;
            if (g_PlayerOk)
            {
                const float dx = e.x - g_PlayerX;
                const float dy = e.y - g_PlayerY;
                dist = sqrtf(dx * dx + dy * dy);
                if (g_Esp.maxDist > 0.0f && dist > g_Esp.maxDist)
                    continue;
            }

            float sl = 0.0f, st = 0.0f, sr = 0.0f, sb = 0.0f;
            WorldToScreen(e.left, e.top, sl, st);
            WorldToScreen(e.right, e.bottom, sr, sb);


            if (sr < 0.0f || sb < 0.0f || sl > display.x || st > display.y)
                continue;

            const ImU32 col = catColors[cat];
            ++g_EspDrawn;
            ++g_EspShown[cat];

            if (g_Esp.drawBox)
                dl->AddRect(ImVec2(sl, st), ImVec2(sr, sb), col, 0.0f, 0, 1.5f);




            if (g_Esp.drawEnemyHp)
            {
                const VarStat& shp  = g_Stats[kStatMonsterHp];
                const VarStat& smax = g_Stats[kStatMonsterMaxHp];
                if (shp.ok && smax.ok && smax.value > 0.0 && shp.value >= 0.0 &&
                    shp.value <= smax.value + 0.001 && smax.value < 1.0e7)
                {
                    const float ratio = static_cast<float>(shp.value / smax.value);
                    const float barH  = 4.0f;
                    const float by    = st - barH - 2.0f;
                    const float bw    = sr - sl;
                    dl->AddRectFilled(ImVec2(sl, by), ImVec2(sr, by + barH), IM_COL32(0, 0, 0, 180));
                    dl->AddRectFilled(ImVec2(sl + 1.0f, by + 1.0f),
                                      ImVec2(sl + 1.0f + (bw - 2.0f) * ratio, by + barH - 1.0f),
                                      IM_COL32(220, 40, 40, 230));
                    dl->AddRect(ImVec2(sl, by), ImVec2(sr, by + barH), IM_COL32(0, 0, 0, 200));
                }
            }

            if (g_Esp.drawName)
            {

                const ImVec2 pos(sl, st - ::ImGui::GetFontSize());
                dl->AddText(ImVec2(pos.x + 1.0f, pos.y + 1.0f), colShadow, name);
                dl->AddText(pos, col, name);
            }

            if (g_Esp.drawDist && g_PlayerOk)
            {
                char buf[32] = {};
                snprintf(buf, IM_ARRAYSIZE(buf), "%.0f", dist);
                const ImVec2 pos(sl, sb + 1.0f);
                dl->AddText(ImVec2(pos.x + 1.0f, pos.y + 1.0f), colShadow, buf);
                dl->AddText(pos, col, buf);
            }
        }
    }



    struct WindowSearch
    {
        DWORD   pid;
        HWND    best;
        LONG64  bestArea;
    };

    BOOL CALLBACK EnumWindowsProc(HWND hWnd, LPARAM lParam)
    {
        WindowSearch* s = reinterpret_cast<WindowSearch*>(lParam);

        DWORD pid = 0;
        ::GetWindowThreadProcessId(hWnd, &pid);
        if (pid != s->pid || !::IsWindowVisible(hWnd) || ::GetWindow(hWnd, GW_OWNER) != nullptr)
            return TRUE;

        RECT rc = {};
        if (!::GetClientRect(hWnd, &rc))
            return TRUE;

        const LONG64 area = static_cast<LONG64>(rc.right - rc.left) * (rc.bottom - rc.top);
        if (area > s->bestArea)
        {
            s->bestArea = area;
            s->best     = hWnd;
        }
        return TRUE;
    }


    HWND FindGameWindow()
    {
        WindowSearch s = { ::GetCurrentProcessId(), nullptr, 0 };
        ::EnumWindows(&EnumWindowsProc, reinterpret_cast<LPARAM>(&s));
        return s.best;
    }



    bool PatchVtableEntry(void** vtable, int index, void* hook, void** original)
    {
        DWORD oldProtect = 0;
        if (!::VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
            return false;

        *original      = vtable[index];
        vtable[index]  = hook;

        DWORD ignored = 0;
        ::VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &ignored);
        return true;
    }



    void LoadFont(ImGuiIO& io)
    {

        static const char* kFonts[] =
        {
            "C:\\Windows\\Fonts\\msyh.ttc",
            "C:\\Windows\\Fonts\\msyh.ttf",
            "C:\\Windows\\Fonts\\simhei.ttf",
            "C:\\Windows\\Fonts\\simsun.ttc",
        };

        for (const char* path : kFonts)
        {
            if (::GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
                continue;
            if (io.Fonts->AddFontFromFileTTF(path, 18.0f, nullptr, io.Fonts->GetGlyphRangesChineseSimplifiedCommon()) != nullptr)
                return;
        }
        io.Fonts->AddFontDefault();
    }



    void ApplyUiScale()
    {
        ImGuiStyle& style = ImGui::GetStyle();


        const float dpiScale   = style.FontScaleDpi;
        const float fontSizeBase = style.FontSizeBase;

        style              = g_BaseStyle;
        style.FontScaleDpi = dpiScale;
        style.FontSizeBase = fontSizeBase;

        style.ScaleAllSizes(g_UiScale);
        style.FontScaleMain = g_UiScale;
    }


    bool InitImGui(IDirect3DDevice9* pDevice)
    {
        IMGUI_CHECKVERSION();
        if (ImGui::CreateContext() == nullptr)
            return false;

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename  = nullptr;

        ImGui::StyleColorsDark();
        g_BaseStyle = ImGui::GetStyle();
        ApplyUiScale();
        LoadFont(io);

        if (!ImGui_ImplWin32_Init(g_hWnd))
        {
            ImGui::DestroyContext();
            return false;
        }
        if (!ImGui_ImplDX9_Init(pDevice))
        {
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            return false;
        }

        g_ImGuiReady = true;
        return true;
    }





    void ClampCurrentWindowToDisplay()
    {
        const ImVec2 display = ::ImGui::GetIO().DisplaySize;
        if (display.x <= 0.0f || display.y <= 0.0f)
            return;

        const ImVec2 size = ::ImGui::GetWindowSize();
        const float fitW = size.x < display.x ? size.x : display.x;
        const float fitH = size.y < display.y ? size.y : display.y;
        if (fitW != size.x || fitH != size.y)
            ::ImGui::SetWindowSize(ImVec2(fitW, fitH));

        const ImVec2 pos = ::ImGui::GetWindowPos();
        const float maxX = display.x - fitW;
        const float maxY = display.y - fitH;

        float newX = pos.x < 0.0f ? 0.0f : pos.x;
        if (newX > maxX) newX = maxX;
        float newY = pos.y < 0.0f ? 0.0f : pos.y;
        if (newY > maxY) newY = maxY;

        if (newX != pos.x || newY != pos.y)
            ::ImGui::SetWindowPos(ImVec2(newX, newY));
    }







    void GameSetFullscreen(bool on)
    {
        using Fn = char(__cdecl*)(char);
        reinterpret_cast<Fn>(GameAddr(kVaSetFullscreen))(on ? 1 : 0);
    }

    void GameRequestClientSize(int w, int h)
    {
        using Fn = void(__cdecl*)(int, int);
        reinterpret_cast<Fn>(GameAddr(kVaRequestClientSize))(w, h);
    }

    bool GameIsFullscreen()
    {
        using Fn = char(__cdecl*)();
        return reinterpret_cast<Fn>(GameAddr(kVaIsFullscreen))() != 0;
    }



    void RequestFullscreenMode(FsMode mode)
    {
        g_FsWant = mode;
    }

    int ClampClientSize(int v)
    {
        if (v < 64)    return 64;
        if (v > 16384) return 16384;
        return v;
    }



    void MaximizeBorderedWindow()
    {
        if (g_hWnd == nullptr)
            return;

        if (!g_WindowSaved)
        {
            ::GetWindowRect(g_hWnd, &g_SavedRect);
            g_WindowSaved = true;
        }

        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        if (!::GetMonitorInfoW(::MonitorFromWindow(g_hWnd, MONITOR_DEFAULTTONEAREST), &mi))
            return;

        ::SetWindowPos(g_hWnd, nullptr,
                       mi.rcWork.left, mi.rcWork.top,
                       mi.rcWork.right - mi.rcWork.left,
                       mi.rcWork.bottom - mi.rcWork.top,
                       SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        Log("window: bordered fullscreen %ldx%ld",
            mi.rcWork.right - mi.rcWork.left, mi.rcWork.bottom - mi.rcWork.top);
    }

    void RestoreWindowedPlacement()
    {
        if (g_hWnd == nullptr || !g_WindowSaved)
            return;

        ::SetWindowPos(g_hWnd, nullptr,
                       g_SavedRect.left, g_SavedRect.top,
                       g_SavedRect.right - g_SavedRect.left,
                       g_SavedRect.bottom - g_SavedRect.top,
                       SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g_WindowSaved = false;
        Log("window: restored windowed placement");
    }



    void TickWindowMode()
    {
        if (g_hWnd == nullptr)
            return;

        if (g_FsWait > 0)
        {
            --g_FsWait;
            return;
        }

        if (g_FsWant != g_FsMode)
        {

            if (g_FsMode == FsMode::Borderless)
            {
                GameSetFullscreen(false);
                g_FsMode = FsMode::Off;
                g_FsWait = 4;
                Log("window: leaving borderless fullscreen");
                return;
            }
            if (g_FsMode == FsMode::Bordered)
            {
                RestoreWindowedPlacement();
                g_FsMode = FsMode::Off;
                g_FsWait = 4;
                return;
            }


            if (g_FsWant != FsMode::Borderless && GameIsFullscreen())
            {
                GameSetFullscreen(false);
                g_FsWait = 4;
                return;
            }


            switch (g_FsWant)
            {
            case FsMode::Borderless:
                GameSetFullscreen(true);
                g_FsMode = FsMode::Borderless;
                Log("window: entering borderless fullscreen");
                break;
            case FsMode::Bordered:
                MaximizeBorderedWindow();
                g_FsMode = FsMode::Bordered;
                break;
            default:
                g_FsMode = FsMode::Off;
                break;
            }
            return;
        }


        if (g_ReqClientW > 0 && g_ReqClientH > 0 && g_FsMode == FsMode::Off && !GameIsFullscreen())
        {
            const int w = ClampClientSize(g_ReqClientW);
            const int h = ClampClientSize(g_ReqClientH);
            GameRequestClientSize(w, h);
            Log("window: request client size %dx%d", w, h);
            g_ReqClientW = 0;
            g_ReqClientH = 0;
        }
    }

    void DrawMenu()
    {
        if (!g_ShowMenu)
            return;

        const ImGuiIO& io = ImGui::GetIO();

        ::ImGui::SetNextWindowSize(ImVec2(460.0f, 470.0f), ImGuiCond_FirstUseEver);
        ::ImGui::Begin("Undertale Internal", &g_ShowMenu);

        ClampCurrentWindowToDisplay();


        DumpPlayerDiag();

        ::ImGui::Text("FPS: %.1f  (%.2f ms/帧)", io.Framerate, io.Framerate > 0.0f ? 1000.0f / io.Framerate : 0.0f);
        ::ImGui::Text("D3D9 设备: 0x%08X", reinterpret_cast<UINT_PTR>(g_pDevice));
        ::ImGui::Text("窗口句柄: 0x%08X", reinterpret_cast<UINT_PTR>(g_hWnd));


        float uiScale = g_UiScale;
        ::ImGui::SetNextItemWidth(160.0f * g_UiScale);
        if (::ImGui::SliderFloat("界面缩放", &uiScale, g_UiScaleMin, g_UiScaleMax, "%.2fx"))
        {
            if (uiScale < g_UiScaleMin) uiScale = g_UiScaleMin;
            if (uiScale > g_UiScaleMax) uiScale = g_UiScaleMax;


            const float ratio = uiScale / g_UiScale;
            const ImVec2 size = ::ImGui::GetWindowSize();
            ::ImGui::SetWindowSize(ImVec2(size.x * ratio, size.y * ratio));

            g_UiScale      = uiScale;
            g_UiScaleDirty = true;
        }

        ::ImGui::TextUnformatted("缩放范围");
        ::ImGui::SameLine();
        ::ImGui::SetNextItemWidth(70.0f * g_UiScale);
        if (::ImGui::DragFloat("##ui_min", &g_UiScaleMin, 0.05f, 0.25f, 4.0f, "%.2f"))
        {
            if (g_UiScaleMin > g_UiScaleMax) g_UiScaleMin = g_UiScaleMax;
            if (g_UiScale < g_UiScaleMin) { g_UiScale = g_UiScaleMin; g_UiScaleDirty = true; }
        }
        ::ImGui::SameLine();
        ::ImGui::SetNextItemWidth(70.0f * g_UiScale);
        if (::ImGui::DragFloat("##ui_max", &g_UiScaleMax, 0.05f, 0.25f, 4.0f, "%.2f"))
        {
            if (g_UiScaleMax < g_UiScaleMin) g_UiScaleMax = g_UiScaleMin;
            if (g_UiScale > g_UiScaleMax) { g_UiScale = g_UiScaleMax; g_UiScaleDirty = true; }
        }


        int fsMode = static_cast<int>(g_FsWant);
        ::ImGui::TextUnformatted("全屏");
        ::ImGui::SameLine();
        if (::ImGui::RadioButton("关闭##fs", &fsMode, 0))
            RequestFullscreenMode(FsMode::Off);
        ::ImGui::SameLine();
        if (::ImGui::RadioButton("有边框全屏##fs", &fsMode, 2))
            RequestFullscreenMode(FsMode::Bordered);
        ::ImGui::SameLine();
        if (::ImGui::RadioButton("无边框全屏##fs", &fsMode, 1))
            RequestFullscreenMode(FsMode::Borderless);
        if (::ImGui::IsItemHovered())
            ::ImGui::SetTooltip(
                "有边框全屏：保留标题栏与边框 窗口撑满显示器工作区\n"
                "无边框全屏：调用游戏自己的全屏开关（等同 Alt+Enter） 由游戏重建 backbuffer\n"
                "两者都由游戏自己改窗口 不再和它的窗口逻辑抢控制权");


        if (::ImGui::CollapsingHeader("窗口与画面", ImGuiTreeNodeFlags_DefaultOpen))
        {
            static int reqW = 0;
            static int reqH = 0;
            if (reqW <= 0 || reqH <= 0)
            {
                RECT rc = {};
                if (::GetClientRect(g_hWnd, &rc) && rc.right > rc.left && rc.bottom > rc.top)
                {
                    reqW = rc.right - rc.left;
                    reqH = rc.bottom - rc.top;
                }
                else
                {
                    reqW = kGameBaseW;
                    reqH = kGameBaseH;
                }
            }

            ::ImGui::SetNextItemWidth(90.0f * g_UiScale);
            ::ImGui::InputInt("宽##client_w", &reqW, 0, 0);
            ::ImGui::SameLine();
            ::ImGui::SetNextItemWidth(90.0f * g_UiScale);
            ::ImGui::InputInt("高##client_h", &reqH, 0, 0);
            ::ImGui::SameLine();
            if (::ImGui::Button("应用##client_apply"))
            {
                g_ReqClientW = reqW;
                g_ReqClientH = reqH;
                g_FsWant     = FsMode::Off;
            }
            if (::ImGui::IsItemHovered())
                ::ImGui::SetTooltip("走游戏自己的 window_set_size 通道设置客户区大小（保留窗口边框）");

            float picScale = g_PicScale;
            ::ImGui::SetNextItemWidth(160.0f * g_UiScale);
            ::ImGui::SliderFloat("画面缩放", &picScale, 0.5f, 4.0f, "%.2fx");
            if (::ImGui::IsItemDeactivatedAfterEdit())
            {
                g_PicScale   = picScale;
                g_ReqClientW = static_cast<int>(kGameBaseW * picScale + 0.5f);
                g_ReqClientH = static_cast<int>(kGameBaseH * picScale + 0.5f);
                g_FsWant     = FsMode::Off;
            }
            if (::ImGui::IsItemHovered())
                ::ImGui::SetTooltip("把客户区按游戏原生 640x480 放大 N 倍 游戏会把整个画面拉伸铺满客户区\n松开滑条后生效");

            RECT rc = {};
            ::GetClientRect(g_hWnd, &rc);
            ::ImGui::Text("当前客户区: %ld x %ld", rc.right - rc.left, rc.bottom - rc.top);
            if (g_ReqClientW > 0)
                ::ImGui::Text("待应用: %d x %d", g_ReqClientW, g_ReqClientH);
        }

        ::ImGui::Separator();
        if (g_PlayerOk)
            ::ImGui::Text("自身坐标 [%s]: %.1f, %.1f", g_PlayerLabel, g_PlayerX, g_PlayerY);
        else
            ::ImGui::TextUnformatted("自身坐标: 未找到玩家实例");
        ::ImGui::Separator();


        {
            auto numStr = [](const VarStat& s, char* buf, size_t n)
            {
                if (s.ok)
                    snprintf(buf, n, "%.0f", s.value);
                else
                    snprintf(buf, n, "--");
            };
            char hpS[32] = {}, mhS[32] = {}, lvS[32] = {}, loveS[32] = {}, xpS[32] = {}, gdS[32] = {};
            char mhpS[32] = {}, mmaxS[32] = {};
            numStr(g_Stats[kStatHp], hpS, sizeof(hpS));
            numStr(g_Stats[kStatMaxHp], mhS, sizeof(mhS));
            numStr(g_Stats[kStatLv], lvS, sizeof(lvS));
            numStr(g_Stats[kStatLove], loveS, sizeof(loveS));
            numStr(g_Stats[kStatXp], xpS, sizeof(xpS));
            numStr(g_Stats[kStatGold], gdS, sizeof(gdS));
            numStr(g_Stats[kStatMonsterHp], mhpS, sizeof(mhpS));
            numStr(g_Stats[kStatMonsterMaxHp], mmaxS, sizeof(mmaxS));

            ::ImGui::Text("HP: %s / %s", hpS, mhS);
            ::ImGui::Text("LOVE(lv): %s    EXP(xp): %s    G(gold): %s", lvS, xpS, gdS);
            ::ImGui::Text("love: %s    怪物: HP %s / %s", loveS, mhpS, mmaxS);

            if (::ImGui::CollapsingHeader("数值诊断"))
            {
                for (int i = 0; i < kStatCount; ++i)
                {
                    const VarStat& s = g_Stats[i];
                    ::ImGui::Text("%-14s ok=%d idx=%d type=%u value=%.3f",
                                  s.name, s.ok ? 1 : 0, s.rawIndex, s.type, s.value);
                }
                ::ImGui::TextUnformatted("idx 为名字查找返回的原始变量索引；type 为 RValue 类型（0/13 实数、1 字符串、2 数组、5 undefined）");
            }
        }
        ::ImGui::Separator();

        if (g_View.ok)
        {
            ::ImGui::Text("view_current=%d  visible=%d", g_View.current, g_View.visible ? 1 : 0);
            ::ImGui::Text("view 偏移: %.1f, %.1f", g_View.x, g_View.y);
            ::ImGui::Text("view 尺寸: %.1f x %.1f", g_View.w, g_View.h);
            ::ImGui::Text("view 端口: (%d, %d) %d x %d", g_View.xport, g_View.yport, g_View.wport, g_View.hport);
            ::ImGui::Text("视口缩放: %.2f x %.2f", g_View.scaleX, g_View.scaleY);
            ::ImGui::Text("ImGui 显示区: %.0f x %.0f", io.DisplaySize.x, io.DisplaySize.y);
            const ScreenMap map = ComputeScreenMap();
            ::ImGui::Text("显示区缩放: %.3f  黑边: %.1f, %.1f", map.scale, map.marginX, map.marginY);
            if (g_PlayerOk)
            {
                float sx = 0.0f;
                float sy = 0.0f;
                WorldToScreen(g_PlayerX, g_PlayerY, sx, sy);
                ::ImGui::Text("自机屏幕坐标: %.1f, %.1f", sx, sy);
            }
        }
        else
        {
            ::ImGui::TextUnformatted("视图状态: 未读到");
        }
        ::ImGui::Separator();

        if (::ImGui::CollapsingHeader("实体列表", ImGuiTreeNodeFlags_DefaultOpen))
        {
            static bool activeOnly = false;
            static char filter[64] = "";

            ::ImGui::Checkbox("只显示活动实例", &activeOnly);
            ::ImGui::SameLine();
            ::ImGui::SetNextItemWidth(140.0f);
            ::ImGui::InputTextWithHint("##filter", "按对象名过滤", filter, IM_ARRAYSIZE(filter));


            int shown = 0;
            ::ImGui::BeginChild("entity_list", ImVec2(0.0f, 240.0f), true);
            for (int i = 0; i < g_EntityCount; ++i)
            {
                const EntityEntry& e = g_Entities[i];
                if (activeOnly && !e.active)
                    continue;

                const char* name = ObjectNameByIndex(e.obj);
                if (filter[0] != '\0' && strstr(name, filter) == nullptr)
                    continue;

                float sx = 0.0f;
                float sy = 0.0f;
                WorldToScreen(e.x, e.y, sx, sy);
                ++shown;
                ::ImGui::Text("%3d %-24s obj=%-4u 世界=(%.0f, %.0f) 屏幕=(%.0f, %.0f)%s",
                    i, name, e.obj, e.x, e.y, sx, sy, e.active ? "" : " [失活]");
            }
            ::ImGui::EndChild();
            ::ImGui::Text("共 %d 个实例 显示 %d 个", g_EntityCount, shown);
        }
        ::ImGui::Separator();


        if (::ImGui::CollapsingHeader("ESP", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ::ImGui::Checkbox("开启 ESP", &g_Esp.on);
            ::ImGui::SameLine();
            ::ImGui::Checkbox("方框", &g_Esp.drawBox);
            ::ImGui::SameLine();
            ::ImGui::Checkbox("名称", &g_Esp.drawName);
            ::ImGui::SameLine();
            ::ImGui::Checkbox("距离", &g_Esp.drawDist);

            ::ImGui::Checkbox("只显示活动实例", &g_Esp.onlyActive);
            ::ImGui::SameLine();
            ::ImGui::Checkbox("只显示可见实例", &g_Esp.onlyVisible);
            ::ImGui::SameLine();
            ::ImGui::Checkbox("敌人血条", &g_Esp.drawEnemyHp);
            if (::ImGui::IsItemHovered())
                ::ImGui::SetTooltip("在敌人方框上方绘制剩余 HP 血条\n需要能读到 global.monsterhp / monstermaxhp 读不到时不会绘制");

            ::ImGui::TextUnformatted("最大距离");
            ::ImGui::SameLine();
            ::ImGui::SetNextItemWidth(110.0f * g_UiScale);
            ::ImGui::DragFloat("##esp_dist", &g_Esp.maxDist, 8.0f, 0.0f, 4000.0f, "%.0f");
            if (::ImGui::IsItemHovered())
                ::ImGui::SetTooltip("超出该距离的实例不画；0 = 不限（需要先读到玩家实例）");

            ::ImGui::SetNextItemWidth(180.0f * g_UiScale);
            ::ImGui::InputTextWithHint("##esp_filter", "按对象名过滤（空=全部）", g_Esp.filter, IM_ARRAYSIZE(g_Esp.filter));

            ::ImGui::SeparatorText("目标分类");

            for (int c = 0; c < kEspCatCount; ++c)
            {
                EspCategory& cat = g_EspCats[c];
                ::ImGui::PushID(c);

                ::ImGui::Checkbox("##cat_on", &cat.enabled);
                ::ImGui::SameLine();
                ::ImGui::ColorEdit4("##cat_col", cat.color, ImGuiColorEditFlags_NoInputs);
                ::ImGui::SameLine();
                ::ImGui::Text("%s  (%d)", cat.label, g_EspShown[c]);

                if (c == kEspCatOther)
                {
                    ::ImGui::TextDisabled("      未命中上面任何分类的实例");
                }
                else
                {
                    ::ImGui::SetNextItemWidth(300.0f * g_UiScale);
                    ::ImGui::InputTextWithHint("##cat_pat", "对象名通配 逗号分隔（如 obj_*）",
                                               cat.patterns, IM_ARRAYSIZE(cat.patterns));
                }

                ::ImGui::PopID();
            }

            if (::ImGui::Button("预设: 全部"))
            {
                for (int c = 0; c < kEspCatCount; ++c)
                    g_EspCats[c].enabled = true;
            }
            ::ImGui::SameLine();
            if (::ImGui::Button("预设: 只看玩家"))
            {
                for (int c = 0; c < kEspCatCount; ++c)
                    g_EspCats[c].enabled = (c == 0);
            }
            ::ImGui::SameLine();
            if (::ImGui::Button("预设: 只看敌人"))
            {
                for (int c = 0; c < kEspCatCount; ++c)
                    g_EspCats[c].enabled = (c == 1);
            }

            if (::ImGui::Button("导出当前房间对象名到日志"))
                DumpRoomObjectNames();
            if (::ImGui::IsItemHovered())
                ::ImGui::SetTooltip("把当前房间出现过的对象名去重后写入 overlay.log\n用于补全上面各分类的通配符");

            ::ImGui::Text("本帧绘制 %d / %d 个实例", g_EspDrawn, g_EntityCount);
        }
        ::ImGui::Separator();

        if (::ImGui::Checkbox("WASD 转方向键", &g_WasdToArrows) && !g_WasdToArrows)
            ReleaseWasdKeys();
        if (::ImGui::IsItemHovered())
            ::ImGui::SetTooltip("W/A/S/D 分别对应上/左/下/右\n用 SendInput 发送方向键\n在菜单里输入文字时自动暂停");
        ::ImGui::Separator();

        ::ImGui::Checkbox("显示 ImGui 示例窗口", &g_ShowDemo);
        ::ImGui::Separator();

        ::ImGui::TextUnformatted("配置 (overlay.ini 与 DLL 同目录；END 卸载时自动保存)");
        static char s_ConfigMsg[32] = "";
        if (::ImGui::Button("保存配置"))
        {
            SaveConfig();
            strcpy_s(s_ConfigMsg, "已保存");
        }
        ::ImGui::SameLine();
        if (::ImGui::Button("重新载入"))
        {
            LoadConfig();
            strcpy_s(s_ConfigMsg, "已重新载入");
        }
        if (s_ConfigMsg[0] != '\0')
        {
            ::ImGui::SameLine();
            ::ImGui::TextDisabled("%s", s_ConfigMsg);
        }
        ::ImGui::Separator();

        ::ImGui::TextUnformatted("INS  显示 / 隐藏本菜单");
        ::ImGui::TextUnformatted("END  卸载并退出 DLL");
        ::ImGui::Spacing();

        if (::ImGui::Button("卸载 DLL (END)", ImVec2(-1.0f, 0.0f)))
            g_UnloadReq = true;

        ::ImGui::End();

        if (g_ShowDemo)
            ::ImGui::ShowDemoWindow(&g_ShowDemo);
    }



    void RenderFrame(IDirect3DDevice9* pDevice)
    {

        if (g_UiScaleDirty)
        {
            ApplyUiScale();
            g_UiScaleDirty = false;
        }

        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();


        TickWindowMode();


        UpdatePlayerPosition();
        UpdateViewState();
        UpdateEntityList();
        UpdateStats();

        DrawEsp();
        DrawMenu();

        ImGui::EndFrame();
        ImGui::Render();


        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    }



    HRESULT WINAPI HookedEndScene(IDirect3DDevice9* pDevice)
    {
        ::InterlockedIncrement(&g_InHook);

        if (!g_Unloading)
        {
            if (g_pDevice == nullptr)
                g_pDevice = pDevice;

            if (g_pDevice == pDevice)
            {
                if (!g_ImGuiReady)
                {

                    if (InitImGui(pDevice))
                        Log("imgui: init ok (display %.0fx%.0f)",
                            ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y);
                    else
                        Log("imgui: init failed");
                }

                if (g_ImGuiReady)
                    RenderFrame(pDevice);
            }
        }

        const HRESULT hr = g_OrgEndScene(pDevice);
        ::InterlockedDecrement(&g_InHook);
        return hr;
    }

    HRESULT WINAPI HookedReset(IDirect3DDevice9* pDevice, D3DPRESENT_PARAMETERS* pParams)
    {
        ::InterlockedIncrement(&g_InHook);

        HRESULT hr = D3DERR_INVALIDCALL;
        if (!g_Unloading && g_ImGuiReady && g_pDevice == pDevice)
        {
            ImGui_ImplDX9_InvalidateDeviceObjects();
            hr = g_OrgReset(pDevice, pParams);
            if (SUCCEEDED(hr))
                ImGui_ImplDX9_CreateDeviceObjects();
        }
        else
        {
            hr = g_OrgReset(pDevice, pParams);
        }

        ::InterlockedDecrement(&g_InHook);
        return hr;
    }

    LRESULT CallOrgWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        return g_WndUnicode
            ? ::CallWindowProcW(g_OrgWndProc, hWnd, msg, wParam, lParam)
            : ::CallWindowProcA(g_OrgWndProc, hWnd, msg, wParam, lParam);
    }

    LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        ::InterlockedIncrement(&g_InHook);

        const bool alive = !g_Unloading;


        if (alive && g_ImGuiReady && ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam) != 0)
        {
            ::InterlockedDecrement(&g_InHook);
            return TRUE;
        }


        if (alive && msg == WM_KEYDOWN && (lParam & (1 << 30)) == 0)
        {
            if (wParam == VK_INSERT)
            {
                g_ShowMenu = !g_ShowMenu;
                Log("key: INS menu=%d", g_ShowMenu ? 1 : 0);
                ::InterlockedDecrement(&g_InHook);
                return 0;
            }
            if (wParam == VK_END)
            {
                g_UnloadReq = true;
                Log("key: END -> unload");
                ::InterlockedDecrement(&g_InHook);
                return 0;
            }
        }


        if (alive && (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP))
        {
            bool imguiCapture = false;
            if (g_ImGuiReady && g_ShowMenu)
                imguiCapture = ImGui::GetIO().WantCaptureKeyboard;

            if (imguiCapture || !g_WasdToArrows)
                ReleaseWasdKeys();
            else
                HandleWasdKeyMessage(msg, wParam);
        }

        if (alive && msg == WM_KILLFOCUS)
            ReleaseWasdKeys();

        if (alive && g_ImGuiReady && g_ShowMenu)
        {
            const ImGuiIO& io = ImGui::GetIO();
            const bool kbMsg  = (msg >= WM_KEYFIRST && msg <= WM_KEYLAST);
            const bool msMsg  = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST);
            if ((kbMsg && io.WantCaptureKeyboard) || (msMsg && io.WantCaptureMouse))
            {
                ::InterlockedDecrement(&g_InHook);
                return 0;
            }
        }

        const LRESULT result = CallOrgWndProc(hWnd, msg, wParam, lParam);
        ::InterlockedDecrement(&g_InHook);
        return result;
    }



    void InstallWndProcHook()
    {
        g_WndUnicode = ::IsWindowUnicode(g_hWnd) != FALSE;
        g_OrgWndProc = reinterpret_cast<WNDPROC>(
            g_WndUnicode
                ? ::SetWindowLongPtrW(g_hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&HookedWndProc))
                : ::SetWindowLongPtrA(g_hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&HookedWndProc)));
    }

    void RestoreWndProc()
    {
        if (g_hWnd == nullptr || g_OrgWndProc == nullptr)
            return;

        if (g_WndUnicode)
            ::SetWindowLongPtrW(g_hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_OrgWndProc));
        else
            ::SetWindowLongPtrA(g_hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_OrgWndProc));

        g_OrgWndProc = nullptr;
    }

    void RestoreVtableSlot(void** vt, int index, void* original)
    {
        if (vt[index] == original)
            return;

        DWORD oldProtect = 0;
        if (!::VirtualProtect(&vt[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
            return;

        vt[index] = original;

        DWORD ignored = 0;
        ::VirtualProtect(&vt[index], sizeof(void*), oldProtect, &ignored);
    }





    int HookAllDeviceVtables(void** dummyVtable)
    {



        const void* kBeginScene = dummyVtable[kIdxBeginScene];
        const void* kClear      = dummyVtable[kIdxClear];
        if (kBeginScene == nullptr || kClear == nullptr)
            return 0;

        const SIZE_T kAnchorOffset = static_cast<SIZE_T>(kIdxBeginScene) * sizeof(void*);
        const SIZE_T kVtableSpan   = static_cast<SIZE_T>(kIdxClear + 1) * sizeof(void*);

        SYSTEM_INFO si = {};
        ::GetSystemInfo(&si);

        BYTE* p = static_cast<BYTE*>(si.lpMinimumApplicationAddress);
        BYTE* const pMax = static_cast<BYTE*>(si.lpMaximumApplicationAddress);

        while (p < pMax && g_HookedCount < kMaxHookedVtables)
        {
            MEMORY_BASIC_INFORMATION mbi = {};
            if (::VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi))
                break;

            BYTE* const regionBase = static_cast<BYTE*>(mbi.BaseAddress);
            BYTE* const regionEnd  = regionBase + mbi.RegionSize;



            const bool scannable =
                mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.RegionSize > kVtableSpan &&
                (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0 &&
                (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;

            if (scannable)
            {
                __try
                {
                    for (BYTE* slot = regionBase + kAnchorOffset;
                         slot + sizeof(void*) <= regionEnd;
                         slot += sizeof(void*))
                    {
                        if (*reinterpret_cast<const void**>(slot) != kBeginScene)
                            continue;

                        void** vt = reinterpret_cast<void**>(slot - kAnchorOffset);
                        if (vt == dummyVtable)
                            continue;
                        if (reinterpret_cast<BYTE*>(vt) + kVtableSpan > regionEnd)
                            continue;
                        if (vt[kIdxClear] != kClear)
                            continue;

                        void* orgEndScene = nullptr;
                        void* orgReset    = nullptr;
                        if (!PatchVtableEntry(vt, kIdxEndScene,
                                              reinterpret_cast<void*>(&HookedEndScene), &orgEndScene))
                            continue;
                        if (!PatchVtableEntry(vt, kIdxReset,
                                              reinterpret_cast<void*>(&HookedReset), &orgReset))
                        {
                            void* ignored = nullptr;
                            PatchVtableEntry(vt, kIdxEndScene, orgEndScene, &ignored);
                            continue;
                        }


                        if (g_OrgEndScene == nullptr)
                        {
                            g_OrgEndScene = reinterpret_cast<EndSceneFn>(orgEndScene);
                            g_OrgReset    = reinterpret_cast<ResetFn>(orgReset);
                        }

                        HookedVtable& entry = g_HookedVtables[g_HookedCount++];
                        entry.vt          = vt;
                        entry.orgEndScene = orgEndScene;
                        entry.orgReset    = orgReset;

                        Log("hooks: patched vt=%p", vt);
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {

                }
            }

            p = regionEnd;
        }

        return g_HookedCount;
    }


    bool InstallDeviceHooks()
    {
        HWND hDummy = ::CreateWindowExA(0, "STATIC", "undertale_overlay_dummy", WS_POPUP,
                                        0, 0, 4, 4, nullptr, nullptr, nullptr, nullptr);
        if (hDummy == nullptr)
            return false;

        IDirect3D9* pD3D = ::Direct3DCreate9(D3D_SDK_VERSION);
        if (pD3D == nullptr)
        {
            ::DestroyWindow(hDummy);
            return false;
        }

        D3DPRESENT_PARAMETERS pp = {};
        pp.Windowed       = TRUE;
        pp.SwapEffect     = D3DSWAPEFFECT_DISCARD;
        pp.BackBufferFormat = D3DFMT_UNKNOWN;
        pp.hDeviceWindow  = hDummy;

        IDirect3DDevice9* pDummy = nullptr;
        HRESULT hrHal = pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hDummy,
                                           D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &pDummy);
        HRESULT hrRef = 0;
        if (FAILED(hrHal))
        {

            hrRef = pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, hDummy,
                                       D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &pDummy);
        }
        pD3D->Release();

        if (FAILED(hrHal) && FAILED(hrRef))
        {
            Log("hooks: dummy CreateDevice failed HAL=0x%08X REF=0x%08X", (unsigned)hrHal, (unsigned)hrRef);
            ::DestroyWindow(hDummy);
            return false;
        }

        void** dummyVtable = *reinterpret_cast<void***>(pDummy);


        const int count = HookAllDeviceVtables(dummyVtable);

        pDummy->Release();
        ::DestroyWindow(hDummy);

        Log("hooks: %d vtable(s) hooked", count);
        return count > 0;
    }

    void RestoreDeviceHooks()
    {
        for (int i = 0; i < g_HookedCount; ++i)
        {
            const HookedVtable& entry = g_HookedVtables[i];
            RestoreVtableSlot(entry.vt, kIdxEndScene, entry.orgEndScene);
            RestoreVtableSlot(entry.vt, kIdxReset,    entry.orgReset);
        }
        g_HookedCount = 0;
    }




    DWORD WINAPI UnloadWatchdog(LPVOID)
    {
        while (!g_UnloadReq)
            ::Sleep(50);

        g_Unloading = true;
        ReleaseWasdKeys();


        SaveConfig();

        Log("unload: restoring hooks");


        if (GameIsFullscreen())
            GameSetFullscreen(false);
        if (g_FsMode == FsMode::Bordered)
            RestoreWindowedPlacement();


        RestoreWndProc();
        RestoreDeviceHooks();


        ::Sleep(300);


        while (::InterlockedCompareExchange(&g_InHook, 0, 0) != 0)
            ::Sleep(10);

        ReleaseWasdKeys();

        if (g_ImGuiReady)
        {
            ImGui_ImplDX9_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            g_ImGuiReady = false;
        }


        ::FreeLibraryAndExitThread(g_hModule, 0);
        return 0;
    }

    DWORD WINAPI InitThread(LPVOID)
    {

        LoadConfig();


        for (int i = 0; i < 120 && g_hWnd == nullptr; ++i)
        {
            g_hWnd = FindGameWindow();
            if (g_hWnd == nullptr)
                ::Sleep(500);
        }
        if (g_hWnd == nullptr)
        {
            Log("init: no game window found");
            return 0;
        }
        Log("init: hwnd=%p unicode=%d", g_hWnd, ::IsWindowUnicode(g_hWnd));


        if (g_hD3D9 == nullptr)
            g_hD3D9 = ::LoadLibraryA("d3d9.dll");
        if (g_hD3D9 == nullptr)
        {
            Log("init: d3d9.dll load failed");
            return 0;
        }

        InstallWndProcHook();
        if (!InstallDeviceHooks())
            Log("init: InstallDeviceHooks failed");

        HANDLE hWatchdog = ::CreateThread(nullptr, 0, &UnloadWatchdog, nullptr, 0, nullptr);
        if (hWatchdog != nullptr)
            ::CloseHandle(hWatchdog);

        return 0;
    }
}

namespace Overlay
{
    void Start(HMODULE hModule)
    {
        g_hModule = hModule;
        HANDLE hThread = ::CreateThread(nullptr, 0, &InitThread, nullptr, 0, nullptr);
        if (hThread != nullptr)
            ::CloseHandle(hThread);
    }

    void RequestUnload()
    {
        g_UnloadReq = true;
    }
}
