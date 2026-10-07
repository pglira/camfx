#include "Lut3D.h"

#include <QFile>
#include <QTextStream>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>

namespace {

using Rgb = Lut3D::Rgb;
using LookFn = std::function<Rgb(Rgb)>;

float luma(const Rgb& c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }
float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

// Smooth S-curve around mid gray; amount 0 is identity
float sCurve(float x, float amount)
{
    const float s = x * x * (3.0f - 2.0f * x);
    return x + amount * (s - x);
}

Rgb saturate(Rgb c, float s)
{
    const float y = luma(c);
    for (float& v : c)
        v = y + s * (v - y);
    return c;
}

struct BuiltinLook {
    const char* name;
    LookFn fn;
};

const std::vector<BuiltinLook>& builtinLooks()
{
    static const std::vector<BuiltinLook> looks = {
        {"Warm", [](Rgb c) { return Rgb{c[0] * 1.06f + 0.01f, c[1] * 1.01f, c[2] * 0.90f}; }},
        {"Cool", [](Rgb c) { return Rgb{c[0] * 0.92f, c[1] * 1.0f, c[2] * 1.07f + 0.01f}; }},
        {"Teal & Orange",
         [](Rgb c) {
             const float y = luma(c);
             const float sh = 1.0f - y, hi = y;
             c = {c[0] - 0.05f * sh + 0.07f * hi, c[1] + 0.02f * sh + 0.01f * hi,
                  c[2] + 0.07f * sh - 0.06f * hi};
             for (float& v : c)
                 v = sCurve(clamp01(v), 0.35f);
             return c;
         }},
        {"Vivid",
         [](Rgb c) {
             c = saturate(c, 1.3f);
             for (float& v : c)
                 v = sCurve(clamp01(v), 0.3f);
             return c;
         }},
        {"Fade",
         [](Rgb c) {
             c = saturate(c, 0.8f);
             for (float& v : c)
                 v = 0.07f + 0.88f * v;
             c[2] += 0.02f;
             return c;
         }},
        {"Mono",
         [](Rgb c) {
             const float y = sCurve(clamp01(luma(c)), 0.25f);
             return Rgb{y, y, y};
         }},
    };
    return looks;
}

}  // namespace

QStringList Lut3D::builtinNames()
{
    QStringList names;
    for (const auto& l : builtinLooks())
        names << QString::fromLatin1(l.name);
    return names;
}

Lut3D Lut3D::builtin(const QString& name)
{
    for (const auto& l : builtinLooks())
        if (name == QLatin1String(l.name))
            return fromFunction(33, l.fn);
    return {};
}

Lut3D Lut3D::fromFunction(int n, const std::function<Rgb(Rgb)>& fn)
{
    Lut3D lut;
    lut.size_ = n;
    lut.data_.resize(size_t(n) * n * n * 3);
    float* d = lut.data_.data();
    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r) {
                const Rgb out = fn({r / float(n - 1), g / float(n - 1), b / float(n - 1)});
                *d++ = clamp01(out[0]);
                *d++ = clamp01(out[1]);
                *d++ = clamp01(out[2]);
            }
    lut.buildIndex();
    return lut;
}

void Lut3D::buildIndex()
{
    for (int v = 0; v < 256; ++v) {
        const float x = v * (size_ - 1) / 255.0f;
        idx_[v] = std::min(int(x), size_ - 2);
        frac_[v] = x - idx_[v];
    }
}

Lut3D::Rgb Lut3D::sample(Rgb c) const
{
    int i[3];
    float f[3];
    for (int k = 0; k < 3; ++k) {
        const float x = clamp01(c[k]) * (size_ - 1);
        i[k] = std::min(int(x), size_ - 2);
        f[k] = x - i[k];
    }
    const int n = size_, n2 = n * n;
    const float* c000 = data_.data() + 3 * (i[2] * n2 + i[1] * n + i[0]);
    const float* c010 = c000 + 3 * n;
    const float* c001 = c000 + 3 * n2;
    const float* c011 = c001 + 3 * n;
    Rgb out;
    for (int ch = 0; ch < 3; ++ch) {
        const float c00 = c000[ch] + f[0] * (c000[ch + 3] - c000[ch]);
        const float c10 = c010[ch] + f[0] * (c010[ch + 3] - c010[ch]);
        const float c01 = c001[ch] + f[0] * (c001[ch + 3] - c001[ch]);
        const float c11 = c011[ch] + f[0] * (c011[ch + 3] - c011[ch]);
        const float lo = c00 + f[1] * (c10 - c00);
        const float hi = c01 + f[1] * (c11 - c01);
        out[ch] = lo + f[2] * (hi - lo);
    }
    return out;
}

Lut3D Lut3D::fromCube(const QString& path, QString* error)
{
    auto fail = [&](const QString& msg) {
        if (error)
            *error = msg;
        return Lut3D{};
    };
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return fail(QStringLiteral("cannot open %1").arg(path));

    Lut3D lut;
    float domMin[3] = {0, 0, 0}, domMax[3] = {1, 1, 1};
    QTextStream in(&f);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        const QStringList tok = line.split(QChar(' '), Qt::SkipEmptyParts);
        if (tok[0] == QLatin1String("LUT_3D_SIZE") && tok.size() >= 2) {
            lut.size_ = tok[1].toInt();
            if (lut.size_ < 2 || lut.size_ > 256)
                return fail(QStringLiteral("invalid LUT_3D_SIZE"));
            lut.data_.reserve(size_t(lut.size_) * lut.size_ * lut.size_ * 3);
        } else if (tok[0] == QLatin1String("DOMAIN_MIN") && tok.size() >= 4) {
            for (int i = 0; i < 3; ++i)
                domMin[i] = tok[i + 1].toFloat();
        } else if (tok[0] == QLatin1String("DOMAIN_MAX") && tok.size() >= 4) {
            for (int i = 0; i < 3; ++i)
                domMax[i] = tok[i + 1].toFloat();
        } else if (tok.size() == 3 && (tok[0][0].isDigit() || tok[0][0] == '-' || tok[0][0] == '.')) {
            for (int i = 0; i < 3; ++i)
                lut.data_.push_back(clamp01((tok[i].toFloat() - domMin[i]) / (domMax[i] - domMin[i])));
        }
        // TITLE, LUT_1D_SIZE and unknown keywords are ignored
    }
    if (lut.size_ < 2 || lut.data_.size() != size_t(lut.size_) * lut.size_ * lut.size_ * 3)
        return fail(QStringLiteral("not a valid 3D .cube file"));
    lut.buildIndex();
    return lut;
}
