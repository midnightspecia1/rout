#include "overlay.h"

namespace {

// Arbitrary color used only as the "this pixel is transparent" marker
// for LWA_COLORKEY. Never used for real hint content.
constexpr COLORREF kColorKey = RGB(1, 1, 1);
constexpr wchar_t kClassName[] = L"RoutOverlayWindow";

// Label box padding around the text, at 96 DPI.
constexpr int kPadX = 5;
constexpr int kPadY = 1;

}  // namespace

bool Overlay::Create() {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &Overlay::WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hbrBackground = CreateSolidBrush(kColorKey);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) return false;

    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    origin_ = {x, y};

    dpi_ = GetDpiForSystem();
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi_);
    ncm.lfMessageFont.lfWeight = FW_BOLD;
    font_ = CreateFontIndirectW(&ncm.lfMessageFont);

    // WS_EX_TRANSPARENT: clicks pass through to whatever is beneath.
    // WS_EX_NOACTIVATE: showing the overlay must never steal focus
    // from the target app (e.g. Rider's editor caret has to stay put).
    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        kClassName, L"rout overlay", WS_POPUP,
        x, y, w, h, nullptr, nullptr, wc.hInstance, this);
    if (!hwnd) return false;

    ApplyLayeredAttributes();
    return true;
}

void Overlay::SetTheme(const Theme& theme) {
    theme_ = theme;
    ApplyLayeredAttributes();
    if (visible_) InvalidateRect(hwnd_, nullptr, TRUE);
}

void Overlay::ApplyLayeredAttributes() {
    SetLayeredWindowAttributes(hwnd_, kColorKey, theme_.opacity, LWA_COLORKEY | LWA_ALPHA);
}

void Overlay::Show(const std::vector<HintBox>& hints) {
    hints_ = hints;
    visible_ = true;
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void Overlay::Hide() {
    visible_ = false;
    ShowWindow(hwnd_, SW_HIDE);
}

void Overlay::Paint() {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd_, &ps);

    RECT client;
    GetClientRect(hwnd_, &client);
    HBRUSH keyBrush = CreateSolidBrush(kColorKey);
    FillRect(hdc, &client, keyBrush);
    DeleteObject(keyBrush);

    HBRUSH boxBrush = CreateSolidBrush(theme_.background);
    HPEN borderPen = CreatePen(PS_SOLID, theme_.borderWidth, theme_.border);
    HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, borderPen));
    HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(hdc, boxBrush));
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, font_));

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, theme_.text);

    for (const auto& hint : hints_) {
        SIZE text{};
        GetTextExtentPoint32W(hdc, hint.label.c_str(), static_cast<int>(hint.label.size()), &text);
        const LONG left = hint.anchor.x - origin_.x;
        const LONG top = hint.anchor.y - origin_.y;
        RECT r{left, top, left + text.cx + 2 * Scale(kPadX), top + text.cy + 2 * Scale(kPadY)};
        // GDI's RoundRect takes the ellipse diameter, not the radius.
        const int d = Scale(theme_.cornerRadius) * 2;
        RoundRect(hdc, r.left, r.top, r.right, r.bottom, d, d);
        DrawTextW(hdc, hint.label.c_str(), -1, &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    SelectObject(hdc, oldFont);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(borderPen);
    DeleteObject(boxBrush);

    EndPaint(hwnd_, &ps);
}

LRESULT CALLBACK Overlay::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Overlay* self;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<Overlay*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<Overlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self && msg == WM_PAINT) {
        self->Paint();
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
