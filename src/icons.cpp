#include "icons.h"

#include <algorithm>
#include <cmath>

// Icons are rasterized from signed distance functions in a unit square, which
// gives anti-aliased edges and a real alpha channel without GDI+ or assets.

namespace {

constexpr float kPi = 3.14159265f;

struct Vec { float x, y; };
struct Color { float r, g, b, a; };

Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y}; }
Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y}; }
Vec operator*(Vec a, float s) { return {a.x * s, a.y * s}; }
float Dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y; }
float Len(Vec a) { return std::sqrt(Dot(a, a)); }

Color Rgb(uint32_t hex, float a = 1.0f) {
    return {((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f, a};
}

float SdCircle(Vec p, Vec c, float r) { return Len(p - c) - r; }

float SdRoundRect(Vec p, Vec c, Vec half, float r) {
    Vec q = {std::fabs(p.x - c.x) - half.x + r, std::fabs(p.y - c.y) - half.y + r};
    Vec m = {std::max(q.x, 0.0f), std::max(q.y, 0.0f)};
    return Len(m) + std::min(std::max(q.x, q.y), 0.0f) - r;
}

float SdCapsule(Vec p, Vec a, Vec b, float r) {
    Vec pa = p - a, ba = b - a;
    float h = std::clamp(Dot(pa, ba) / Dot(ba, ba), 0.0f, 1.0f);
    return Len(pa - ba * h) - r;
}

// Arc of radius R centred at c, from angle a0 to a1 (radians, clockwise in screen space).
float SdArc(Vec p, Vec c, float R, float a0, float a1, float w) {
    Vec d = p - c;
    float ang = std::atan2(d.y, d.x);
    while (ang < a0) ang += 2 * kPi;
    if (ang <= a1) return std::fabs(Len(d) - R) - w;
    Vec e0 = c + Vec{std::cos(a0), std::sin(a0)} * R;
    Vec e1 = c + Vec{std::cos(a1), std::sin(a1)} * R;
    return std::min(Len(p - e0), Len(p - e1)) - w;
}

float SdTriangle(Vec p, Vec a, Vec b, Vec c) {
    Vec e0 = b - a, e1 = c - b, e2 = a - c;
    Vec v0 = p - a, v1 = p - b, v2 = p - c;
    auto seg = [](Vec v, Vec e) {
        float h = std::clamp(Dot(v, e) / Dot(e, e), 0.0f, 1.0f);
        return v - e * h;
    };
    Vec pq0 = seg(v0, e0), pq1 = seg(v1, e1), pq2 = seg(v2, e2);
    float s = (e0.x * e2.y - e0.y * e2.x) > 0 ? 1.0f : -1.0f;
    float dx = std::min({Dot(pq0, pq0), Dot(pq1, pq1), Dot(pq2, pq2)});
    float dy = std::min({s * (v0.x * e0.y - v0.y * e0.x),
                         s * (v1.x * e1.y - v1.y * e1.x),
                         s * (v2.x * e2.y - v2.y * e2.x)});
    return -std::sqrt(dx) * (dy > 0 ? 1.0f : -1.0f);
}

class Canvas {
public:
    explicit Canvas(int size) : size_(size), px_(size * size, Color{0, 0, 0, 0}) {}

    template <typename Sdf>
    void Fill(Color col, Sdf sdf) {
        for (int y = 0; y < size_; ++y) {
            for (int x = 0; x < size_; ++x) {
                Vec p = {(x + 0.5f) / size_, (y + 0.5f) / size_};
                float cov = std::clamp(0.5f - sdf(p) * size_, 0.0f, 1.0f);
                if (cov <= 0) continue;
                float a = col.a * cov;
                Color& d = px_[y * size_ + x];
                d.r = col.r * a + d.r * (1 - a);
                d.g = col.g * a + d.g * (1 - a);
                d.b = col.b * a + d.b * (1 - a);
                d.a = a + d.a * (1 - a);
            }
        }
    }

    void Export(std::vector<uint32_t>& out) const {
        out.resize(px_.size());
        for (size_t i = 0; i < px_.size(); ++i) {
            const Color& c = px_[i];
            auto ch = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
            // Icon bitmaps use straight (non-premultiplied) alpha.
            float inv = c.a > 0 ? 1.0f / c.a : 0.0f;
            out[i] = (ch(c.a) << 24) | (ch(c.r * inv) << 16) | (ch(c.g * inv) << 8) | ch(c.b * inv);
        }
    }

private:
    int size_;
    std::vector<Color> px_;
};

struct Palette { Color body, shade, fan; };

Palette PaletteFor(IconState s) {
    switch (s) {
    case IconState::On:   return {Rgb(0x76B900), Rgb(0x3E6200), Rgb(0xFFFFFF)};
    case IconState::Busy: return {Rgb(0xF0A020), Rgb(0x8A5600), Rgb(0xFFFFFF)};
    default:              return {Rgb(0x8A8A8A), Rgb(0x4E4E4E), Rgb(0xEDEDED)};
    }
}

void DrawFan(Canvas& cv, const Palette& pal, Vec c, float r, bool detailed) {
    cv.Fill(pal.fan, [=](Vec p) { return SdCircle(p, c, r); });
    cv.Fill(pal.shade, [=](Vec p) { return SdCircle(p, c, r * 0.80f); });
    if (detailed) {
        for (int i = 0; i < 3; ++i) {
            float a = -kPi / 2 + i * 2 * kPi / 3;
            Vec tip = c + Vec{std::cos(a + 0.45f), std::sin(a + 0.45f)} * (r * 0.68f);
            Vec mid = c + Vec{std::cos(a), std::sin(a)} * (r * 0.38f);
            cv.Fill(pal.fan, [=](Vec p) {
                return std::min(SdCapsule(p, c, mid, r * 0.13f), SdCapsule(p, mid, tip, r * 0.10f));
            });
        }
    }
    cv.Fill(pal.fan, [=](Vec p) { return SdCircle(p, c, r * (detailed ? 0.20f : 0.32f)); });
}

void DrawCard(Canvas& cv, const Palette& pal, bool detailed) {
    if (detailed) {
        // PCIe edge connector teeth below the card.
        cv.Fill(pal.shade, [](Vec p) { return SdRoundRect(p, {0.42f, 0.745f}, {0.24f, 0.06f}, 0.02f); });
        cv.Fill(Rgb(0xE8C547), [](Vec p) {
            float d = 1e9f;
            for (int i = 0; i < 7; ++i)
                d = std::min(d, SdRoundRect(p, {0.22f + i * 0.067f, 0.755f}, {0.022f, 0.04f}, 0.008f));
            return d;
        });
        cv.Fill(pal.body, [](Vec p) { return SdRoundRect(p, {0.5f, 0.45f}, {0.45f, 0.26f}, 0.08f); });
        DrawFan(cv, pal, {0.36f, 0.45f}, 0.19f, true);
        // Vent slots on the right half.
        cv.Fill(pal.shade, [](Vec p) {
            float d = 1e9f;
            for (int i = 0; i < 3; ++i)
                d = std::min(d, SdCapsule(p, {0.66f + i * 0.09f, 0.32f}, {0.66f + i * 0.09f, 0.58f}, 0.022f));
            return d;
        });
    } else {
        cv.Fill(pal.body, [](Vec p) { return SdRoundRect(p, {0.5f, 0.5f}, {0.5f, 0.34f}, 0.14f); });
        DrawFan(cv, pal, {0.36f, 0.5f}, 0.25f, false);
        cv.Fill(pal.shade, [](Vec p) {
            return std::min(SdRoundRect(p, {0.73f, 0.5f}, {0.045f, 0.17f}, 0.02f),
                            SdRoundRect(p, {0.87f, 0.5f}, {0.045f, 0.17f}, 0.02f));
        });
    }
}

void DrawSlash(Canvas& cv, bool detailed) {
    float w = detailed ? 0.045f : 0.07f;
    Vec a = {0.12f, 0.12f}, b = {0.88f, 0.88f};
    cv.Fill(Rgb(0xFFFFFF), [=](Vec p) { return SdCapsule(p, a, b, w + (detailed ? 0.03f : 0.05f)); });
    cv.Fill(Rgb(0xE0302A), [=](Vec p) { return SdCapsule(p, a, b, w); });
}

void DrawBusyBadge(Canvas& cv, bool detailed) {
    Vec c = detailed ? Vec{0.755f, 0.745f} : Vec{0.73f, 0.73f};
    float r = detailed ? 0.20f : 0.27f;
    cv.Fill(Rgb(0xFFFFFF), [=](Vec p) { return SdCircle(p, c, r + (detailed ? 0.025f : 0.0f)); });
    cv.Fill(Rgb(0x2A2A2A), [=](Vec p) { return SdCircle(p, c, r - (detailed ? 0.0f : 0.05f)); });
    float R = r * 0.55f, w = r * (detailed ? 0.11f : 0.24f);
    float a0 = -kPi * 0.35f, a1 = kPi * 1.25f;
    cv.Fill(Rgb(0xFFFFFF), [=](Vec p) { return SdArc(p, c, R, a0, a1, w); });
    if (!detailed) return; // a plain gapped ring reads as a spinner at tray size
    // Arrow head at the start of the arc, pointing clockwise.
    Vec e = c + Vec{std::cos(a0), std::sin(a0)} * R;
    Vec tangent = {-std::sin(a0), std::cos(a0)};
    Vec normal = {std::cos(a0), std::sin(a0)};
    float h = r * 0.42f;
    Vec t0 = e + normal * h, t1 = e - normal * h, t2 = e + tangent * h;
    cv.Fill(Rgb(0xFFFFFF), [=](Vec p) { return SdTriangle(p, t0, t1, t2); });
}

void DrawErrorBadge(Canvas& cv, bool detailed) {
    Vec c = detailed ? Vec{0.755f, 0.745f} : Vec{0.73f, 0.73f};
    float r = detailed ? 0.20f : 0.27f;
    cv.Fill(Rgb(0xFFFFFF), [=](Vec p) { return SdCircle(p, c, r + (detailed ? 0.025f : 0.0f)); });
    cv.Fill(Rgb(0xE0302A), [=](Vec p) { return SdCircle(p, c, r - (detailed ? 0.0f : 0.04f)); });
    float w = r * (detailed ? 0.14f : 0.22f);
    cv.Fill(Rgb(0xFFFFFF), [=](Vec p) {
        return std::min(SdCapsule(p, {c.x, c.y - r * 0.55f}, {c.x, c.y + r * 0.10f}, w),
                        SdCircle(p, {c.x, c.y + r * 0.50f}, w * 1.1f));
    });
}

} // namespace

void RenderStateIcon(IconState state, int size, std::vector<uint32_t>& pixels) {
    Canvas cv(size);
    bool detailed = size > 24;
    DrawCard(cv, PaletteFor(state), detailed);
    switch (state) {
    case IconState::Off:   DrawSlash(cv, detailed); break;
    case IconState::Busy:  DrawBusyBadge(cv, detailed); break;
    case IconState::Error: DrawErrorBadge(cv, detailed); break;
    case IconState::On:    break;
    }
    cv.Export(pixels);
}

HICON MakeStateIcon(IconState state, int size) {
    std::vector<uint32_t> pixels;
    RenderStateIcon(state, size, pixels);

    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) return nullptr;
    memcpy(bits, pixels.data(), pixels.size() * sizeof(uint32_t));

    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}
