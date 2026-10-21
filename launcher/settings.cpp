#include "settings.h"
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <string>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace MorrowindModernSettings {
namespace {

constexpr int IdMode = 100, IdResolution = 101, IdScale = 102;
constexpr int IdRam = 111, IdTrace = 112;
constexpr int IdTitle = 200, IdSubtitle = 201, IdModeLabel = 202;
constexpr int IdResolutionLabel = 203, IdScaleLabel = 204, IdFooter = 205;
constexpr double Scales[] = { 1.0, 1.25, 1.5, 1.75, 2.0 };

struct Resolution {
    int width;
    int height;
};

struct Context {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    HWND dialog = nullptr;
    HWND tooltip = nullptr;
    HFONT font = nullptr;
    HFONT titleFont = nullptr;
    UINT dpi = 96;
    int desktopWidth = 0;
    int desktopHeight = 0;
    std::wstring settingsPath;
    std::vector<Resolution> resolutions;
    LaunchSettings settings;

    ~Context() {
        if (font) DeleteObject(font);
        if (titleFont) DeleteObject(titleFont);
    }
    int Pixel(int value) const { return MulDiv(value, static_cast<int>(dpi), 96); }
    HWND Item(int id) const { return GetDlgItem(dialog, id); }
};

void EnableDpiAwareness() {
    const HMODULE user = GetModuleHandleW(L"user32.dll");
    using SetContext = BOOL(WINAPI*)(HANDLE);
    const auto setContext = reinterpret_cast<SetContext>(GetProcAddress(user, "SetProcessDpiAwarenessContext"));
    if (setContext) {
        setContext(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)));
    } else {
        SetProcessDPIAware();
    }
}

UINT SystemDpi() {
    using GetDpi = UINT(WINAPI*)();
    const auto getDpi = reinterpret_cast<GetDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"));
    const UINT dpi = getDpi ? getDpi() : 96;
    return dpi ? dpi : 96;
}

std::wstring SettingsPath() {
    wchar_t module[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, module, static_cast<DWORD>(std::size(module)));
    if (!length || length >= std::size(module)) return {};
    std::wstring directory(module, length);
    const size_t slash = directory.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    directory.resize(slash);
    const size_t parent = directory.find_last_of(L"\\/");
    const std::wstring folder = directory.substr(parent == std::wstring::npos ? 0 : parent + 1);
    if (!_wcsicmp(folder.c_str(), L"bin") && parent != std::wstring::npos) directory.resize(parent);
    return directory + L"\\settings.ini";
}

int ClosestScale(double requested) {
    int nearest = 0;
    for (int i = 1; i < static_cast<int>(std::size(Scales)); ++i) {
        if (std::abs(Scales[i] - requested) < std::abs(Scales[nearest] - requested)) nearest = i;
    }
    return nearest;
}

void LoadSettings(Context& context) {
    LaunchSettings& value = context.settings;
    if (value.width < 640 || value.height < 480) {
        value.width = context.desktopWidth;
        value.height = context.desktopHeight;
    }
    if (!std::isfinite(value.uiScale) || value.uiScale <= 0.0) {
        value.uiScale = Scales[ClosestScale(std::clamp(context.desktopHeight / 900.0, 1.0, 2.0))];
    }
    if (context.settingsPath.empty()) return;
    const wchar_t* path = context.settingsPath.c_str();
    const int width = static_cast<int>(GetPrivateProfileIntW(L"Display", L"Width", value.width, path));
    const int height = static_cast<int>(GetPrivateProfileIntW(L"Display", L"Height", value.height, path));
    if (width >= 640 && height >= 480 && width <= 16384 && height <= 16384) {
        value.width = width;
        value.height = height;
    }
    const int mode = static_cast<int>(GetPrivateProfileIntW(L"Display", L"ScreenMode", value.screenMode, path));
    value.screenMode = mode >= 0 && mode <= 2 ? mode : 2;
    const int percent = static_cast<int>(GetPrivateProfileIntW(
        L"Display", L"UIScalePercent", static_cast<int>(std::lround(value.uiScale * 100)), path));
    value.uiScale = Scales[ClosestScale(std::clamp(percent / 100.0, 1.0, 2.0))];
    value.trace = GetPrivateProfileIntW(L"Game", L"Trace", value.trace ? 1 : 0, path) != 0;
    value.lockRam = GetPrivateProfileIntW(L"Game", L"LockRam", value.lockRam ? 1 : 0, path) != 0;
}

bool SaveSettings(const Context& context) {
    if (context.settingsPath.empty()) return false;
    bool success = true;
    const auto save = [&](const wchar_t* section, const wchar_t* key, int value) {
        const std::wstring text = std::to_wstring(value);
        if (!WritePrivateProfileStringW(section, key, text.c_str(), context.settingsPath.c_str())) success = false;
    };
    save(L"Display", L"Width", context.settings.width);
    save(L"Display", L"Height", context.settings.height);
    save(L"Display", L"ScreenMode", context.settings.screenMode);
    save(L"Display", L"UIScalePercent", static_cast<int>(std::lround(context.settings.uiScale * 100)));
    save(L"Game", L"PreloadAdjacentCells", 0);
    save(L"Game", L"Trace", context.settings.trace ? 1 : 0);
    save(L"Game", L"LockRam", context.settings.lockRam ? 1 : 0);
    return success;
}

HWND AddControl(Context& context, const wchar_t* type, const wchar_t* text, DWORD style, int id) {
    return CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style,
        0, 0, 0, 0, context.dialog, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), context.instance, nullptr);
}

void Tooltip(Context& context, int id, const wchar_t* text) {
    TOOLINFOW info{ sizeof(info) };
    info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    info.hwnd = context.dialog;
    info.uId = reinterpret_cast<UINT_PTR>(context.Item(id));
    info.lpszText = const_cast<wchar_t*>(text);
    SendMessageW(context.tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
}

void Position(Context& context, int id, int x, int y, int width, int height) {
    SetWindowPos(context.Item(id), nullptr, context.Pixel(x), context.Pixel(y),
        context.Pixel(width), context.Pixel(height), SWP_NOACTIVATE | SWP_NOZORDER);
}

void ApplyFonts(Context& context) {
    const HFONT oldFont = context.font, oldTitle = context.titleFont;
    context.font = CreateFontW(-MulDiv(10, static_cast<int>(context.dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    context.titleFont = CreateFontW(-MulDiv(15, static_cast<int>(context.dpi), 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    for (HWND child = GetWindow(context.dialog, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        const HFONT font = GetDlgCtrlID(child) == IdTitle ? context.titleFont : context.font;
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    if (oldFont) DeleteObject(oldFont);
    if (oldTitle) DeleteObject(oldTitle);
}

void Layout(Context& context) {
    Position(context, IdTitle, 24, 18, 452, 32);
    Position(context, IdSubtitle, 24, 52, 452, 24);
    Position(context, IdModeLabel, 24, 92, 172, 24);
    Position(context, IdMode, 202, 86, 274, 220);
    Position(context, IdResolutionLabel, 24, 134, 172, 24);
    Position(context, IdResolution, 202, 128, 274, 300);
    Position(context, IdScaleLabel, 24, 176, 172, 24);
    Position(context, IdScale, 202, 170, 274, 200);
    Position(context, IdRam, 24, 220, 452, 24);
    Position(context, IdTrace, 24, 250, 452, 24);
    Position(context, IdFooter, 24, 322, 452, 36);
    Position(context, IDOK, 252, 370, 108, 32);
    Position(context, IDCANCEL, 368, 370, 108, 32);
}

void FillResolutions(Context& context) {
    std::vector<Resolution> available;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    for (DWORD index = 0; EnumDisplaySettingsW(nullptr, index, &mode); ++index) {
        if (mode.dmPelsWidth < 640 || mode.dmPelsHeight < 480) continue;
        available.push_back({ static_cast<int>(mode.dmPelsWidth), static_cast<int>(mode.dmPelsHeight) });
    }
    std::sort(available.begin(), available.end(), [](const Resolution& first, const Resolution& second) {
        if (first.width != second.width) return first.width < second.width;
        return first.height < second.height;
    });
    available.erase(std::unique(available.begin(), available.end(), [](const Resolution& first, const Resolution& second) {
        return first.width == second.width && first.height == second.height;
    }), available.end());
    context.resolutions.push_back({ context.desktopWidth, context.desktopHeight });
    for (const Resolution& resolution : available) {
        if (resolution.width != context.desktopWidth || resolution.height != context.desktopHeight) {
            context.resolutions.push_back(resolution);
        }
    }
    int selected = 0;
    for (size_t i = 0; i < context.resolutions.size(); ++i) {
        const Resolution& resolution = context.resolutions[i];
        std::wstring text = std::to_wstring(resolution.width) + L" × " + std::to_wstring(resolution.height);
        if (i == 0) text += L" (рабочий стол)";
        SendMessageW(context.Item(IdResolution), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        if (resolution.width == context.settings.width && resolution.height == context.settings.height) {
            selected = static_cast<int>(i);
        }
    }
    SendMessageW(context.Item(IdResolution), CB_SETCURSEL, selected, 0);
    SendMessageW(context.Item(IdResolution), CB_SETMINVISIBLE, 12, 0);
}

bool Accept(Context& context) {
    const int mode = static_cast<int>(SendMessageW(context.Item(IdMode), CB_GETCURSEL, 0, 0));
    const int resolution = static_cast<int>(SendMessageW(context.Item(IdResolution), CB_GETCURSEL, 0, 0));
    const int scale = static_cast<int>(SendMessageW(context.Item(IdScale), CB_GETCURSEL, 0, 0));
    if (mode < 0 || mode > 2 || resolution < 0 || resolution >= static_cast<int>(context.resolutions.size()) ||
        scale < 0 || scale >= static_cast<int>(std::size(Scales))) return false;
    const Resolution chosen = context.resolutions[resolution];
    if (chosen.width / Scales[scale] < 640 || chosen.height / Scales[scale] < 480) {
        wchar_t message[256]{};
        swprintf_s(message, L"Для масштаба %d%% нужно разрешение не ниже %d × %d.\n\nВыберите меньший масштаб или большее разрешение.",
            static_cast<int>(std::lround(Scales[scale] * 100)),
            static_cast<int>(std::ceil(640 * Scales[scale])), static_cast<int>(std::ceil(480 * Scales[scale])));
        MessageBoxW(context.dialog, message, L"Размер интерфейса", MB_OK | MB_ICONINFORMATION);
        SetFocus(context.Item(IdScale));
        return false;
    }
    context.settings.width = chosen.width;
    context.settings.height = chosen.height;
    context.settings.screenMode = mode;
    context.settings.uiScale = Scales[scale];
    context.settings.lockRam = SendMessageW(context.Item(IdRam), BM_GETCHECK, 0, 0) == BST_CHECKED;
    context.settings.trace = SendMessageW(context.Item(IdTrace), BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (!SaveSettings(context)) {
        MessageBoxW(context.dialog,
            L"Не удалось сохранить настройки для следующего запуска.\nИгра будет запущена с выбранными параметрами.",
            L"Настройки Morrowind", MB_OK | MB_ICONINFORMATION);
    }
    return true;
}

INT_PTR CALLBACK DialogProcedure(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    Context* context = reinterpret_cast<Context*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
    if (message == WM_INITDIALOG) {
        context = reinterpret_cast<Context*>(lParam);
        context->dialog = dialog;
        SetWindowLongPtrW(dialog, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
        SetWindowTextW(dialog, L"Morrowind — настройки");
        AddControl(*context, L"STATIC", L"Настройки Morrowind", SS_LEFT, IdTitle);
        AddControl(*context, L"STATIC", L"Экран, интерфейс и загрузка мира", SS_LEFT, IdSubtitle);
        AddControl(*context, L"STATIC", L"Режим экрана", SS_LEFT, IdModeLabel);
        AddControl(*context, L"STATIC", L"Разрешение", SS_LEFT, IdResolutionLabel);
        AddControl(*context, L"STATIC", L"Размер интерфейса", SS_LEFT, IdScaleLabel);
        AddControl(*context, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, IdMode);
        AddControl(*context, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, IdResolution);
        AddControl(*context, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, IdScale);
        AddControl(*context, L"BUTTON", L"Удерживать ресурсы в оперативной памяти", BS_AUTOCHECKBOX | WS_TABSTOP, IdRam);
        AddControl(*context, L"BUTTON", L"Записывать журнал работы", BS_AUTOCHECKBOX | WS_TABSTOP, IdTrace);
        AddControl(*context, L"STATIC", L"Выбранные настройки сохраняются после нажатия «Играть».", SS_LEFT, IdFooter);
        AddControl(*context, L"BUTTON", L"Играть", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK);
        AddControl(*context, L"BUTTON", L"Отмена", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL);
        for (const wchar_t* name : { L"Полноэкранный (масштабирование)", L"В окне", L"Окно без рамки" }) {
            SendMessageW(context->Item(IdMode), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
        }
        SendMessageW(context->Item(IdMode), CB_SETCURSEL, context->settings.screenMode, 0);
        FillResolutions(*context);
        for (double scale : Scales) {
            const std::wstring name = std::to_wstring(static_cast<int>(std::lround(scale * 100))) + L"%";
            SendMessageW(context->Item(IdScale), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        }
        SendMessageW(context->Item(IdScale), CB_SETCURSEL, ClosestScale(context->settings.uiScale), 0);
        SendMessageW(context->Item(IdRam), BM_SETCHECK, context->settings.lockRam ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(context->Item(IdTrace), BM_SETCHECK, context->settings.trace ? BST_CHECKED : BST_UNCHECKED, 0);
        context->tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, dialog, nullptr, context->instance, nullptr);
        SendMessageW(context->tooltip, TTM_SETMAXTIPWIDTH, 0, context->Pixel(340));
        SendMessageW(context->tooltip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 15000);
        Tooltip(*context, IdMode, L"Полноэкранный: изображение растягивается на монитор. Без рамки: выбранный размер по центру монитора. В окне: обычная рамка и заголовок.");
        Tooltip(*context, IdResolution, L"Разрешение изображения во всех режимах. Полноэкранный режим масштабирует его на монитор; окно без рамки сохраняет выбранный размер.");
        Tooltip(*context, IdScale, L"Размер текста, меню и окон в игре.");
        Tooltip(*context, IdRam, L"Удерживать игровые ресурсы в памяти, чтобы Windows не выгружала их на диск.");
        Tooltip(*context, IdTrace, L"Сохранить журнал запуска и переходов между участками мира.");
        ApplyFonts(*context);
        Layout(*context);
        RECT rectangle{ 0, 0, context->Pixel(500), context->Pixel(408) };
        AdjustWindowRectEx(&rectangle, static_cast<DWORD>(GetWindowLongW(dialog, GWL_STYLE)), FALSE,
            static_cast<DWORD>(GetWindowLongW(dialog, GWL_EXSTYLE)));
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const int width = rectangle.right - rectangle.left, height = rectangle.bottom - rectangle.top;
        SetWindowPos(dialog, nullptr, work.left + (work.right - work.left - width) / 2,
            work.top + (work.bottom - work.top - height) / 2, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        SetFocus(context->Item(IdMode));
        return FALSE;
    }
    if (!context) return FALSE;
    if (message == WM_COMMAND) {
        if (LOWORD(wParam) == IDOK) {
            if (Accept(*context)) EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
    }
    if (message == WM_CLOSE) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    if (message == WM_DPICHANGED) {
        context->dpi = LOWORD(wParam);
        ApplyFonts(*context);
        Layout(*context);
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(dialog, nullptr, suggested->left, suggested->top,
            suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return TRUE;
    }
    return FALSE;
}

} // namespace

bool ConfigureLaunch(LaunchSettings& settings) {
    EnableDpiAwareness();
    INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&controls);
    Context context;
    context.settings = settings;
    context.dpi = SystemDpi();
    context.desktopWidth = GetSystemMetrics(SM_CXSCREEN);
    context.desktopHeight = GetSystemMetrics(SM_CYSCREEN);
    context.settingsPath = SettingsPath();
    LoadSettings(context);

    // Empty dialog template: children are native controls created in WM_INITDIALOG.
    struct EmptyDialog {
        DLGTEMPLATE dialog;
        WORD menu = 0;
        WORD windowClass = 0;
        WORD title = 0;
    } empty{};
    empty.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    empty.dialog.dwExtendedStyle = WS_EX_APPWINDOW | WS_EX_CONTROLPARENT;
    empty.dialog.cx = 300;
    empty.dialog.cy = 250;
    const INT_PTR result = DialogBoxIndirectParamW(context.instance, &empty.dialog, nullptr,
        DialogProcedure, reinterpret_cast<LPARAM>(&context));
    if (result == -1) {
        const DWORD error = GetLastError();
        wchar_t message[160];
        swprintf_s(message, L"Не удалось открыть настройки. Код Windows: %lu.", error);
        MessageBoxW(nullptr, message, L"Morrowind Modernizer", MB_OK | MB_ICONERROR);
        SetLastError(error);
    }
    if (result != IDOK) return false;
    settings = context.settings;
    return true;
}

} // namespace MorrowindModernSettings
