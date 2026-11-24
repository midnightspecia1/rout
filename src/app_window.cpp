#include "app_window.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <wincodec.h>

#include <string>

#include "com_ptr.h"
#include "hint_painter.h"
#include "resource.h"

namespace {

constexpr wchar_t kClassName[] = L"RoutAppWindow";
constexpr UINT WM_TRAY = WM_APP + 100;
constexpr UINT kTrayId = 1;
enum : UINT { kCmdOpen = 1, kCmdExit = 2 };
enum : int { kIdPreset = 101, kIdAccent, kIdSwatch, kIdOpacity, kIdFontSize, kIdRadius, kIdPreview, kIdLogo };

// Layout at 96 DPI; scaled for the window's actual DPI.
constexpr int kClientWidth = 440;
constexpr int kPadding = 16;
constexpr int kLogoHeight = 120;
constexpr int kHelpHeight = 150;
constexpr int kRowHeight = 30;
constexpr int kLabelWidth = 100;
constexpr int kValueWidth = 50;
constexpr int kPreviewHeight = 110;
constexpr int kRows = 5;
constexpr int kClientHeight =
    kPadding + kLogoHeight + kPadding + kHelpHeight + kRows * kRowHeight + 10 + kPreviewHeight + kPadding;

constexpr wchar_t kHelpText[] =
    L"rout is running in the background.\r\n"
    L"\r\n"
    L"Ctrl+Shift+Space — show hints; letters pick one\r\n"
    L"/ — search hints by name, Backspace — erase\r\n"
    L"Esc or Ctrl+Shift+Space — close hints\r\n"
    L"Ctrl+Alt+Space — scroll mode (Ctrl+N/P, Ctrl+V/Alt+V)\r\n"
    L"In scroll mode: Ctrl+Shift+Space — hints inside it\r\n"
    L"\r\n"
    L"Closing this window does not quit rout; use the tray icon menu.";

constexpr const wchar_t* kRowNames[kRows] = {L"Preset", L"Accent", L"Opacity", L"Font size", L"Corner radius"};

HICON AppIcon() { return LoadIconW(nullptr, IDI_APPLICATION); }

// The system UI font size in points: what a theme with fontSize 0 uses.
int SystemFontPoints() {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, 96);
    return MulDiv(-ncm.lfMessageFont.lfHeight, 72, 96);
}

HWND Child(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
}

void SetSlider(HWND slider, int lo, int hi, int value) {
    SendMessageW(slider, TBM_SETRANGE, FALSE, MAKELPARAM(lo, hi));
    SendMessageW(slider, TBM_SETPOS, TRUE, value);
}

int SliderValue(HWND slider) { return static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0)); }

// The embedded PNG logo scaled to `height` px, as a premultiplied 32-bit
// DIB for AlphaBlend; null on failure (the window just has no logo).
// WIC runs in the process MTA the scroller thread keeps alive.
HBITMAP LoadLogo(int height) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_LOGO), MAKEINTRESOURCEW(10));  // RT_RCDATA
    HGLOBAL data = res ? LoadResource(nullptr, res) : nullptr;
    if (!data) return nullptr;

    Com<IWICImagingFactory> factory;
    Com<IWICStream> stream;
    Com<IWICBitmapDecoder> decoder;
    Com<IWICBitmapFrameDecode> frame;
    Com<IWICBitmapScaler> scaler;
    Com<IWICFormatConverter> converter;
    UINT w = 0, h = 0;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                reinterpret_cast<void**>(factory.Put()))) ||
        FAILED(factory->CreateStream(stream.Put())) ||
        FAILED(stream->InitializeFromMemory(static_cast<BYTE*>(LockResource(data)), SizeofResource(nullptr, res))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.Put())) ||
        FAILED(decoder->GetFrame(0, frame.Put())) || FAILED(frame->GetSize(&w, &h)) || h == 0) {
        return nullptr;
    }
    const int width = MulDiv(static_cast<int>(w), height, static_cast<int>(h));
    if (FAILED(factory->CreateBitmapScaler(scaler.Put())) ||
        FAILED(scaler->Initialize(frame.Get(), width, height, WICBitmapInterpolationModeFant)) ||
        FAILED(factory->CreateFormatConverter(converter.Put())) ||
        FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                     0, WICBitmapPaletteTypeCustom))) {
        return nullptr;
    }

    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};  // Top-down.
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) return nullptr;
    const UINT stride = width * 4;
    if (FAILED(converter->CopyPixels(nullptr, stride, stride * height, static_cast<BYTE*>(bits)))) {
        DeleteObject(bitmap);
        return nullptr;
    }
    return bitmap;
}

}  // namespace

bool AppWindow::Create(HINSTANCE instance, const Settings& settings, ThemeHandler onTheme) {
    settings_ = settings;
    onTheme_ = std::move(onTheme);
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &AppWindow::WndProc;
    wc.hInstance = instance;
    wc.hIcon = AppIcon();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) return false;

    // Size is set in ApplyDpi once the window knows its monitor's DPI.
    if (!CreateWindowExW(0, kClassName, L"rout", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                         CW_USEDEFAULT, CW_USEDEFAULT, 0, 0, nullptr, nullptr, instance, this)) {
        return false;
    }
    CreateControls();
    ApplyDpi(GetDpiForWindow(hwnd_));
    SyncControls();
    AddTrayIcon();
    return true;
}

void AppWindow::CreateControls() {
    logo_ = Child(hwnd_, L"STATIC", L"", SS_OWNERDRAW, kIdLogo);
    help_ =Child(hwnd_, L"STATIC", kHelpText, SS_LEFT, 0);
    for (int i = 0; i < kRows; ++i) rowLabels_[i] = Child(hwnd_, L"STATIC", kRowNames[i], SS_LEFT, 0);
    preset_ = Child(hwnd_, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, kIdPreset);
    for (const ThemePreset& p : kThemePresets) SendMessageW(preset_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p.name));
    accent_ = Child(hwnd_, WC_BUTTONW, L"Change…", BS_PUSHBUTTON | WS_TABSTOP, kIdAccent);
    swatch_ = Child(hwnd_, L"STATIC", L"", SS_OWNERDRAW, kIdSwatch);
    const DWORD slider = TBS_HORZ | TBS_NOTICKS | WS_TABSTOP;
    opacity_ = Child(hwnd_, TRACKBAR_CLASSW, L"", slider, kIdOpacity);
    fontSize_ = Child(hwnd_, TRACKBAR_CLASSW, L"", slider, kIdFontSize);
    radius_ = Child(hwnd_, TRACKBAR_CLASSW, L"", slider, kIdRadius);
    for (HWND& v : values_) v = Child(hwnd_, L"STATIC", L"", SS_LEFT, 0);
    preview_ = Child(hwnd_, L"STATIC", L"", SS_OWNERDRAW, kIdPreview);
}

void AppWindow::SyncControls() {
    const Theme theme = ResolveTheme(settings_);
    for (int i = 0; i < static_cast<int>(std::size(kThemePresets)); ++i) {
        if (settings_.preset == kThemePresets[i].name) SendMessageW(preset_, CB_SETCURSEL, i, 0);
    }
    SetSlider(opacity_, Settings::kMinOpacity, 100, MulDiv(theme.opacity, 100, 255));
    SetSlider(fontSize_, Settings::kMinFontSize, Settings::kMaxFontSize,
              theme.fontSize > 0 ? theme.fontSize : SystemFontPoints());
    SetSlider(radius_, 0, Settings::kMaxCornerRadius, theme.cornerRadius);
    Changed(false);
}

// Refreshes everything that shows the settings and hands the theme on.
void AppWindow::Changed(bool save) {
    const Theme theme = ResolveTheme(settings_);
    SetWindowTextW(values_[0], (std::to_wstring(SliderValue(opacity_)) + L"%").c_str());
    SetWindowTextW(values_[1], (std::to_wstring(SliderValue(fontSize_)) + L" pt").c_str());
    SetWindowTextW(values_[2], (std::to_wstring(SliderValue(radius_)) + L" px").c_str());
    InvalidateRect(swatch_, nullptr, FALSE);
    InvalidateRect(preview_, nullptr, FALSE);
    if (onTheme_) onTheme_(theme);
    if (save) SaveSettings(settings_);
}

void AppWindow::OnCommand(int id, int code) {
    if (id == kIdPreset && code == CBN_SELCHANGE) {
        const auto i = SendMessageW(preset_, CB_GETCURSEL, 0, 0);
        if (i < 0) return;
        // A preset is a complete look: picking one drops the tweaks.
        settings_ = Settings{};
        settings_.preset = kThemePresets[i].name;
        SyncControls();
        SaveSettings(settings_);
    } else if (id == kIdAccent && code == BN_CLICKED) {
        PickAccent();
    }
}

void AppWindow::OnSlider(HWND slider, bool done) {
    const int value = SliderValue(slider);
    if (slider == opacity_) settings_.opacityPercent = value;
    if (slider == fontSize_) settings_.fontSize = value;
    if (slider == radius_) settings_.cornerRadius = value;
    Changed(done);
}

void AppWindow::PickAccent() {
    static COLORREF custom[16]{};
    CHOOSECOLORW cc{sizeof(cc)};
    cc.hwndOwner = hwnd_;
    cc.rgbResult = ResolveTheme(settings_).border;
    cc.lpCustColors = custom;
    cc.Flags = CC_RGBINIT | CC_FULLOPEN;
    if (!ChooseColorW(&cc)) return;
    settings_.accent = cc.rgbResult;
    Changed(true);
}

void AppWindow::DrawLogo(const DRAWITEMSTRUCT& di) {
    FillRect(di.hDC, &di.rcItem, GetSysColorBrush(COLOR_BTNFACE));
    if (!logoBitmap_) return;
    BITMAP bm{};
    GetObjectW(logoBitmap_, sizeof(bm), &bm);
    HDC mem = CreateCompatibleDC(di.hDC);
    HGDIOBJ old = SelectObject(mem, logoBitmap_);
    const int x = di.rcItem.left + (di.rcItem.right - di.rcItem.left - bm.bmWidth) / 2;
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    AlphaBlend(di.hDC, x, di.rcItem.top, bm.bmWidth, bm.bmHeight, mem, 0, 0, bm.bmWidth, bm.bmHeight, blend);
    SelectObject(mem, old);
    DeleteDC(mem);
}

void AppWindow::DrawSwatch(const DRAWITEMSTRUCT& di) {
    HBRUSH brush = CreateSolidBrush(ResolveTheme(settings_).border);
    FillRect(di.hDC, &di.rcItem, brush);
    DeleteObject(brush);
    FrameRect(di.hDC, &di.rcItem, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
}

// A mock window with a few hints, drawn by the overlay's own painter and
// blended at the theme's opacity, as the overlay would show it.
void AppWindow::DrawPreview(const DRAWITEMSTRUCT& di) {
    const RECT& rc = di.rcItem;
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    auto s = [this](int px) { return MulDiv(px, dpi_, 96); };

    HDC base = CreateCompatibleDC(di.hDC);
    HBITMAP baseBmp = CreateCompatibleBitmap(di.hDC, w, h);
    SelectObject(base, baseBmp);
    const RECT all{0, 0, w, h};
    FillRect(base, &all, GetSysColorBrush(COLOR_WINDOW));
    // Mock content: a toolbar strip and some text lines.
    RECT bar{0, 0, w, s(26)};
    FillRect(base, &bar, GetSysColorBrush(COLOR_BTNFACE));
    for (int y = s(40); y < h - s(10); y += s(16)) {
        RECT line{s(16), y, s(16) + (w - s(32)) * (y % 3 + 2) / 4, y + s(6)};
        FillRect(base, &line, GetSysColorBrush(COLOR_BTNSHADOW));
    }
    FrameRect(base, &all, GetSysColorBrush(COLOR_BTNSHADOW));

    HDC top = CreateCompatibleDC(di.hDC);
    HBITMAP topBmp = CreateCompatibleBitmap(di.hDC, w, h);
    SelectObject(top, topBmp);
    BitBlt(top, 0, 0, w, h, base, 0, 0, SRCCOPY);
    const Theme theme = ResolveTheme(settings_);
    HintPainter painter(theme, dpi_);
    painter.DrawBox(top, {s(10), s(4)}, L"A", false);
    painter.DrawBox(top, {s(60), s(4)}, L"S", false);
    painter.DrawBox(top, {s(110), s(4)}, L"DF", false);
    painter.DrawBox(top, {s(24), s(52)}, L"GH", false);
    const std::wstring scrollLabel = L"J";
    painter.DrawBox(top, {w - s(8) - painter.Measure(top, scrollLabel).cx, s(34)}, scrollLabel, true);

    BLENDFUNCTION blend{AC_SRC_OVER, 0, theme.opacity, 0};
    AlphaBlend(base, 0, 0, w, h, top, 0, 0, w, h, blend);
    BitBlt(di.hDC, rc.left, rc.top, w, h, base, 0, 0, SRCCOPY);

    DeleteDC(top);
    DeleteObject(topBmp);
    DeleteDC(base);
    DeleteObject(baseBmp);
}

void AppWindow::ShowAndActivate() {
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd_);
}

void AppWindow::AddTrayIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = AppIcon();
    lstrcpynW(nid.szTip, L"rout — Ctrl+Shift+Space", ARRAYSIZE(nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &nid);

    // Version 4: clean NIN_SELECT / WM_CONTEXTMENU events with the
    // anchor point in wParam, keyboard activation included.
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void AppWindow::RemoveTrayIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void AppWindow::ShowTrayMenu(POINT at) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kCmdOpen, L"Settings…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdExit, L"Exit");
    SetMenuDefaultItem(menu, kCmdOpen, FALSE);

    // Without this the menu won't close when clicking elsewhere.
    SetForegroundWindow(hwnd_);
    const UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                      at.x, at.y, hwnd_, nullptr);
    DestroyMenu(menu);

    if (cmd == kCmdOpen) ShowAndActivate();
    if (cmd == kCmdExit) DestroyWindow(hwnd_);
}

void AppWindow::ApplyDpi(UINT dpi) {
    dpi_ = dpi;
    auto s = [dpi](int px) { return MulDiv(px, dpi, 96); };

    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
    HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
    for (HWND child = GetWindow(hwnd_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    if (font_) DeleteObject(font_);
    font_ = font;

    RECT r{0, 0, s(kClientWidth), s(kClientHeight)};
    AdjustWindowRectExForDpi(&r, GetWindowLongW(hwnd_, GWL_STYLE), FALSE, 0, dpi);
    SetWindowPos(hwnd_, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    const int pad = s(kPadding);
    const int inner = s(kClientWidth) - 2 * pad;
    if (logoBitmap_) DeleteObject(logoBitmap_);
    logoBitmap_ = LoadLogo(s(kLogoHeight));
    MoveWindow(logo_, pad, pad, inner, s(kLogoHeight), TRUE);
    int y = pad + s(kLogoHeight) + pad;
    MoveWindow(help_, pad, y, inner, s(kHelpHeight), TRUE);

    const int controlX = pad + s(kLabelWidth);
    const int controlW = inner - s(kLabelWidth) - s(kValueWidth);
    y += s(kHelpHeight);
    const int rowH = s(kRowHeight);
    const int ctlH = s(24);
    for (int i = 0; i < kRows; ++i) MoveWindow(rowLabels_[i], pad, y + i * rowH + s(4), s(kLabelWidth), ctlH, TRUE);
    MoveWindow(preset_, controlX, y, controlW, s(200), TRUE);  // Height includes the drop-down.
    MoveWindow(accent_, controlX, y + rowH, s(90), ctlH, TRUE);
    MoveWindow(swatch_, controlX + s(100), y + rowH + s(2), s(40), ctlH - s(4), TRUE);
    HWND sliders[] = {opacity_, fontSize_, radius_};
    for (int i = 0; i < 3; ++i) {
        const int rowY = y + (i + 2) * rowH;
        MoveWindow(sliders[i], controlX, rowY, controlW, ctlH, TRUE);
        MoveWindow(values_[i], controlX + controlW + s(6), rowY + s(4), s(kValueWidth) - s(6), ctlH, TRUE);
    }
    y += kRows * rowH + s(10);
    MoveWindow(preview_, pad, y, inner, s(kPreviewHeight), TRUE);
}

LRESULT AppWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == taskbarCreated_ && taskbarCreated_ != 0) {
        AddTrayIcon();
        return 0;
    }
    if (msg >= kAppMessageFirst && msg <= kAppMessageLast) {
        if (onAppMessage_) onAppMessage_(msg, wParam, lParam);
        return 0;
    }
    switch (msg) {
        case WM_COMMAND:
            OnCommand(LOWORD(wParam), HIWORD(wParam));
            return 0;
        case WM_HSCROLL:
            if (lParam) OnSlider(reinterpret_cast<HWND>(lParam), LOWORD(wParam) == TB_ENDTRACK);
            return 0;
        case WM_DRAWITEM: {
            const auto* di = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (di->CtlID == kIdPreview) DrawPreview(*di);
            if (di->CtlID == kIdSwatch) DrawSwatch(*di);
            if (di->CtlID == kIdLogo) DrawLogo(*di);
            return TRUE;
        }
        case WM_TRAY:
            switch (LOWORD(lParam)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                    ShowAndActivate();
                    break;
                case WM_CONTEXTMENU:
                    ShowTrayMenu({static_cast<short>(LOWORD(wParam)), static_cast<short>(HIWORD(wParam))});
                    break;
            }
            return 0;
        case WM_DPICHANGED: {
            ApplyDpi(HIWORD(wParam));
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_CLOSE:
            ShowWindow(hwnd_, SW_HIDE);  // Keep running in the tray.
            return 0;
        case WM_DESTROY:
            RemoveTrayIcon();
            if (font_) DeleteObject(font_);
            if (logoBitmap_) DeleteObject(logoBitmap_);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

LRESULT CALLBACK AppWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppWindow* self;
    if (msg == WM_NCCREATE) {
        self = static_cast<AppWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->HandleMessage(msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}
