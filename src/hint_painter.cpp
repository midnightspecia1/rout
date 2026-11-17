#include "hint_painter.h"

namespace {

// Label box padding around the text, at 96 DPI.
constexpr int kPadX = 5;
constexpr int kPadY = 1;

// Restores the DC's selected objects on scope exit.
class Selection {
public:
    explicit Selection(HDC hdc) : hdc_(hdc), saved_(SaveDC(hdc)) {}
    ~Selection() { RestoreDC(hdc_, saved_); }

private:
    HDC hdc_;
    int saved_;
};

}  // namespace

HintPainter::HintPainter(const Theme& theme, UINT dpi) : theme_(theme), dpi_(dpi) {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi_);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfWeight = FW_BOLD;
    if (theme_.fontSize > 0) lf.lfHeight = -MulDiv(theme_.fontSize, dpi_, 72);
    font_ = CreateFontIndirectW(&lf);

    boxBrush_ = CreateSolidBrush(theme_.background);
    const int width = Scale(theme_.borderWidth);
    borderPen_ = CreatePen(PS_SOLID, width, theme_.border);
    scrollPen_ = CreatePen(PS_SOLID, width, theme_.scrollBorder);
}

HintPainter::~HintPainter() {
    DeleteObject(font_);
    DeleteObject(boxBrush_);
    DeleteObject(borderPen_);
    DeleteObject(scrollPen_);
}

SIZE HintPainter::Measure(HDC hdc, const std::wstring& text) const {
    Selection keep(hdc);
    SelectObject(hdc, font_);
    SIZE extent{};
    GetTextExtentPoint32W(hdc, text.c_str(), static_cast<int>(text.size()), &extent);
    return {extent.cx + 2 * Scale(kPadX), extent.cy + 2 * Scale(kPadY)};
}

void HintPainter::DrawBox(HDC hdc, POINT topLeft, const std::wstring& text, bool scroll) const {
    const SIZE box = Measure(hdc, text);
    Selection keep(hdc);
    SelectObject(hdc, font_);
    SelectObject(hdc, boxBrush_);
    SelectObject(hdc, scroll ? scrollPen_ : borderPen_);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, theme_.text);

    RECT r{topLeft.x, topLeft.y, topLeft.x + box.cx, topLeft.y + box.cy};
    // GDI's RoundRect takes the ellipse diameter, not the radius.
    const int d = Scale(theme_.cornerRadius) * 2;
    RoundRect(hdc, r.left, r.top, r.right, r.bottom, d, d);
    DrawTextW(hdc, text.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void HintPainter::DrawFrame(HDC hdc, const RECT& r) const {
    HPEN pen = CreatePen(PS_INSIDEFRAME, Scale(2), theme_.scrollBorder);
    {
        Selection keep(hdc);  // Deselects the pen before it is deleted.
        SelectObject(hdc, pen);
        SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, r.left, r.top, r.right, r.bottom);
    }
    DeleteObject(pen);
}
