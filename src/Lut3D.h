#pragma once

#include <QString>
#include <QStringList>
#include <array>
#include <functional>
#include <opencv2/core.hpp>
#include <vector>

// A 3D color lookup table with RGB entries in [0,1], red index varying fastest
// (the layout of the .cube format).
class Lut3D {
public:
    using Rgb = std::array<float, 3>;

    bool valid() const { return size_ >= 2; }
    int size() const { return size_; }

    static Lut3D fromCube(const QString& path, QString* error = nullptr);
    static Lut3D builtin(const QString& name);
    static QStringList builtinNames();
    // Tabulate an RGB -> RGB function on an n^3 grid
    static Lut3D fromFunction(int n, const std::function<Rgb(Rgb)>& fn);

    // Trilinear lookup for RGB in [0,1]
    Rgb sample(Rgb c) const;

    // Trilinear lookup for one 8-bit BGR pixel; writes BGR values in [0,255]
    inline void mapPixel(const uchar* bgr, float* out) const
    {
        const int ri = idx_[bgr[2]], gi = idx_[bgr[1]], bi = idx_[bgr[0]];
        const float fr = frac_[bgr[2]], fg = frac_[bgr[1]], fb = frac_[bgr[0]];
        const int n = size_, n2 = n * n;
        const float* c000 = data_.data() + 3 * (bi * n2 + gi * n + ri);
        const float* c010 = c000 + 3 * n;
        const float* c001 = c000 + 3 * n2;
        const float* c011 = c001 + 3 * n;
        for (int ch = 0; ch < 3; ++ch) {
            const float c00 = c000[ch] + fr * (c000[ch + 3] - c000[ch]);
            const float c10 = c010[ch] + fr * (c010[ch + 3] - c010[ch]);
            const float c01 = c001[ch] + fr * (c001[ch + 3] - c001[ch]);
            const float c11 = c011[ch] + fr * (c011[ch + 3] - c011[ch]);
            const float lo = c00 + fg * (c10 - c00);
            const float hi = c01 + fg * (c11 - c01);
            out[2 - ch] = (lo + fb * (hi - lo)) * 255.0f;  // LUT order is RGB, pixel order is BGR
        }
    }

private:
    void buildIndex();

    int size_ = 0;
    std::vector<float> data_;
    // Grid cell index and interpolation weight for each 8-bit channel value
    std::array<int, 256> idx_{};
    std::array<float, 256> frac_{};
};
