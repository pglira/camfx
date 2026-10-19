#include "Effects.h"

#include "FastOps.h"

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <opencv2/core/utility.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace {

cv::Mat box(const cv::Mat& src, int r)
{
    cv::Mat dst;
    cv::boxFilter(src, dst, -1, cv::Size(2 * r + 1, 2 * r + 1), cv::Point(-1, -1), true, cv::BORDER_REFLECT);
    return dst;
}

// Map a soft mask through a contrast curve around 0.5 to control edge hardness
void hardenMask(cv::Mat& m, float gain)
{
    m.forEach<float>([gain](float& v, const int*) { v = std::clamp((v - 0.5f) * gain + 0.5f, 0.0f, 1.0f); });
}

// Row kernels take restrict-qualified pointers and plain values so that the compiler
// can vectorize them (uchar stores could otherwise alias any captured variable)

// px += m * (bright + amount * (A * px + B - px)), with A, B, m stored as value * 255
void skinRow(uchar* __restrict px, const uchar* __restrict pm, const uchar* __restrict pa,
             const uchar* __restrict pb, int cols, float amount, float bright)
{
    for (int x = 0; x < cols; ++x) {
        const float m = pm[x] * (1.0f / 255.0f);
        for (int c = 0; c < 3; ++c) {
            const int i = 3 * x + c;
            const float v = px[i];
            const float q = pa[i] * (1.0f / 255.0f) * v + pb[i];
            const float out = v + m * (bright + amount * (q - v)) + 0.5f;
            px[i] = uchar(std::min(std::max(out, 0.0f), 255.0f));
        }
    }
}

void brightenRow(uchar* __restrict px, const uchar* __restrict pm, int cols, float bright)
{
    for (int x = 0; x < cols; ++x) {
        const float d = pm[x] * (1.0f / 255.0f) * bright;
        for (int c = 0; c < 3; ++c) {
            const float out = px[3 * x + c] + d + 0.5f;
            px[3 * x + c] = uchar(std::min(out, 255.0f));
        }
    }
}

// px = bg + a * (px - bg) with a in 0..255
void compositeRow(uchar* __restrict px, const uchar* __restrict bg, const uchar* __restrict pa, int cols)
{
    for (int x = 0; x < cols; ++x) {
        const int a = pa[x];
        for (int c = 0; c < 3; ++c) {
            const int i = 3 * x + c;
            px[i] = uchar((bg[i] * (255 - a) + px[i] * a + 127) / 255);
        }
    }
}

const char* kStageNames[6] = {"prepare", "masks", "skin", "background", "final", "total"};

}  // namespace

void Effects::Profile::lap(int i)
{
    const int64_t n = cv::getTickCount();
    acc[i] += (n - t) * 1000.0 / cv::getTickFrequency();
    t = n;
}

void Effects::Profile::report()
{
    if (++frames < 100)
        return;
    std::fprintf(stderr, "[profile ms]");
    for (int i = 0; i < 5; ++i)
        std::fprintf(stderr, " %s %.2f", kStageNames[i], acc[i] / frames);
    std::fprintf(stderr, "\n");
    std::fill(acc, acc + 6, 0.0);
    frames = 0;
}

QStringList Effects::lutDirectories()
{
    QStringList dirs;
    dirs << QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/luts";
    dirs << QCoreApplication::applicationDirPath() + "/../share/camfx/luts";
    return dirs;
}

const cv::Mat& Effects::beginFrame(const cv::Mat& f)
{
    CV_Assert(f.type() == CV_8UC3 && f.cols % 8 == 0 && f.rows % 8 == 0);
    size_ = f.size();
    fastops::downscaleBox(f, quarter_, 4);
    return quarter_;
}

void Effects::prepare(cv::Mat& f, const Params& p)
{
    CV_Assert(f.size() == size_);
    if (!profileInit_) {
        // Set CAMFX_PROFILE=1 to print the average time of each stage every 100 frames
        profile_ = qEnvironmentVariableIntValue("CAMFX_PROFILE") != 0;
        profileInit_ = true;
    }
    prof_.t = cv::getTickCount();

    if (p.toneEnabled && p.sharpen > 0.01) {
        fastops::sharpen3x3(f, scratch_, float(p.sharpen * 1.5));
    }

    skinReady_ = false;
    if (p.skinEnabled && p.smoothing * (1.0 - p.detail) >= 0.01) {
        // Self-guided filter (He et al.) per channel at 1/4 resolution; applied at full
        // resolution as q = A * I + B in smoothSkin()
        cv::Mat I;
        quarter_.convertTo(I, CV_32FC3, 1.0 / 255.0);
        const int r = 2 + int(std::lround(p.smoothing * 4.0));
        const double e = 0.03 + 0.1 * p.smoothing;
        cv::Mat meanI = box(I, r);
        cv::Mat var = box(I.mul(I), r) - meanI.mul(meanI);
        cv::Mat a;
        cv::divide(var, var + cv::Scalar::all(e * e), a);
        cv::Mat b = meanI - a.mul(meanI);
        box(a, r).convertTo(skinA_, CV_8UC3, 255.0);
        box(b, r).convertTo(skinB_, CV_8UC3, 255.0);
        skinReady_ = true;
    }
    if (profile_)
        prof_.lap(0);
}

void Effects::finish(cv::Mat& f, const Params& p, const SegResult* seg)
{
    if (profile_)
        prof_.t = cv::getTickCount();
    const bool haveMasks = (p.skinEnabled || p.backgroundEnabled) && seg && seg->valid();
    if (haveMasks)
        updateMasks(*seg, p);
    if (profile_)
        prof_.lap(1);

    if (p.skinEnabled && haveMasks)
        smoothSkin(f, p);
    if (profile_)
        prof_.lap(2);

    const bool composite = p.backgroundEnabled && haveMasks && buildBackground(f, p);
    if (profile_)
        prof_.lap(3);

    finalPass(f, p, composite);
    if (profile_) {
        prof_.lap(4);
        prof_.report();
    }
}

void Effects::updateMasks(const SegResult& seg, const Params& p)
{
    // Exponential moving average over new results reduces flicker
    if (seg.seq != lastSeq_ || personEma_.empty()) {
        if (personEma_.empty() || personEma_.size() != seg.person.size()) {
            personEma_ = seg.person.clone();
            skinEma_ = seg.skin.clone();
        } else {
            cv::addWeighted(personEma_, 0.4, seg.person, 0.6, 0.0, personEma_);
            cv::addWeighted(skinEma_, 0.4, seg.skin, 0.6, 0.0, skinEma_);
        }
        lastSeq_ = seg.seq;
    }

    const cv::Size quarter = size_ / 4;
    if (p.backgroundEnabled) {
        cv::Mat person;
        cv::GaussianBlur(personEma_, person, cv::Size(0, 0), 0.6 + p.edgeSoftness * 3.0);
        hardenMask(person, float(1.0 + (1.0 - p.edgeSoftness) * 5.0));
        cv::resize(person, person, quarter, 0, 0, cv::INTER_LINEAR);
        person.convertTo(personQ_, CV_8U, 255.0);
        cv::resize(personQ_, personFull_, size_, 0, 0, cv::INTER_LINEAR);
    }
    if (p.skinEnabled) {
        cv::Mat skin;
        cv::GaussianBlur(skinEma_, skin, cv::Size(0, 0), 1.5);
        cv::resize(skin, skin, quarter, 0, 0, cv::INTER_LINEAR);
        skin.convertTo(skinQ_, CV_8U, 255.0);
    }
}

void Effects::smoothSkin(cv::Mat& f, const Params& p)
{
    const float amount = float(p.smoothing * (1.0 - p.detail));
    const float bright = float(p.skinBrighten * 25.0);
    if (!skinReady_ && bright < 0.1f)
        return;

    // Restrict all full-resolution work to the bounding box of the skin mask
    cv::Mat present;
    cv::threshold(skinQ_, present, 5, 255, cv::THRESH_BINARY);
    cv::Rect roiQ = cv::boundingRect(present);
    if (roiQ.empty())
        return;
    roiQ = (roiQ + cv::Size(4, 4) - cv::Point(2, 2)) & cv::Rect(cv::Point(), skinQ_.size());
    const cv::Rect roi(roiQ.x * 4, roiQ.y * 4, roiQ.width * 4, roiQ.height * 4);

    cv::Mat mask, A, B;
    cv::resize(skinQ_(roiQ), mask, roi.size(), 0, 0, cv::INTER_LINEAR);
    if (skinReady_) {
        cv::resize(skinA_(roiQ), A, roi.size(), 0, 0, cv::INTER_LINEAR);
        cv::resize(skinB_(roiQ), B, roi.size(), 0, 0, cv::INTER_LINEAR);
    }
    const bool smooth = skinReady_;
    cv::Mat fr = f(roi);

    cv::parallel_for_(cv::Range(0, fr.rows), [&](const cv::Range& rows) {
        for (int y = rows.start; y < rows.end; ++y) {
            if (smooth)
                skinRow(fr.ptr<uchar>(y), mask.ptr<uchar>(y), A.ptr<uchar>(y), B.ptr<uchar>(y), fr.cols, amount, bright);
            else
                brightenRow(fr.ptr<uchar>(y), mask.ptr<uchar>(y), fr.cols, bright);
        }
    });
}

const cv::Mat& Effects::backgroundImage(const QString& path, cv::Size size)
{
    if (path == bgPath_ && size == bgSize_)
        return bgImage_;
    bgPath_ = path;
    bgSize_ = size;
    bgImage_.release();
    cv::Mat img = cv::imread(path.toStdString(), cv::IMREAD_COLOR);
    if (img.empty()) {
        if (!path.isEmpty())
            std::fprintf(stderr, "[effects] cannot read background image %s\n", qPrintable(path));
        return bgImage_;
    }
    // Scale to cover the frame, then crop the center
    const double s = std::max(double(size.width) / img.cols, double(size.height) / img.rows);
    cv::Mat scaled;
    cv::resize(img, scaled, cv::Size(), s, s, s < 1 ? cv::INTER_AREA : cv::INTER_LINEAR);
    const int x = (scaled.cols - size.width) / 2, y = (scaled.rows - size.height) / 2;
    bgImage_ = scaled(cv::Rect(x, y, size.width, size.height)).clone();
    return bgImage_;
}

bool Effects::buildBackground(const cv::Mat& f, const Params& p)
{
    switch (p.backgroundMode) {
    case BackgroundMode::Image:
        bg_ = backgroundImage(p.backgroundImage, f.size());
        return !bg_.empty();
    case BackgroundMode::Color: {
        const QColor& c = p.backgroundColor;
        bg_.create(f.size(), CV_8UC3);
        bg_.setTo(cv::Scalar(c.blue(), c.green(), c.red()));
        return true;
    }
    case BackgroundMode::Blur:
        break;
    }

    // Blur at 1/8 resolution, weighted by the background mask so that the person
    // does not bleed into the blurred background
    const cv::Size eighth = f.size() / 8;
    cv::Mat small, w;
    fastops::downscaleBox(quarter_, small, 2);
    small.convertTo(small, CV_32FC3);
    cv::resize(personQ_, w, eighth, 0, 0, cv::INTER_AREA);
    w.convertTo(w, CV_32F, -1.0 / 255.0, 1.0);
    cv::Mat w3;
    cv::merge(std::vector<cv::Mat>{w, w, w}, w3);

    const double sigma = 0.5 + p.blurStrength * 5.0;
    cv::Mat num, den, plain;
    cv::GaussianBlur(small.mul(w3), num, cv::Size(0, 0), sigma);
    cv::GaussianBlur(w3, den, cv::Size(0, 0), sigma);
    cv::GaussianBlur(small, plain, cv::Size(0, 0), sigma);
    constexpr float k = 0.02f;
    cv::Mat blurred = (num + plain * k) / (den + cv::Scalar::all(k));
    blurred.convertTo(small, CV_8UC3);
    cv::resize(small, bg_, f.size(), 0, 0, cv::INTER_LINEAR);
    return true;
}

void Effects::updateColor(const Params& p)
{
    // 1D tone curves: warmth gains, gamma, contrast, brightness
    curvesIdentity_ = !p.toneEnabled || (std::abs(p.brightness) < 1e-3 && std::abs(p.contrast) < 1e-3 &&
                                         std::abs(p.warmth) < 1e-3 && std::abs(p.gamma - 1.0) < 1e-3);
    if (!curvesIdentity_) {
        const double c = p.contrast >= 0 ? 1.0 + p.contrast * 1.5 : 1.0 + p.contrast * 0.8;
        const double gains[3] = {1.0 - 0.12 * p.warmth, 1.0 + 0.02 * p.warmth, 1.0 + 0.10 * p.warmth};  // B, G, R
        for (int ch = 0; ch < 3; ++ch)
            for (int v = 0; v < 256; ++v) {
                double x = std::clamp(v / 255.0 * gains[ch], 0.0, 1.0);
                x = std::pow(x, 1.0 / std::max(p.gamma, 0.1));
                x = (x - 0.5) * c + 0.5 + p.brightness * 0.3;
                curves_[ch][v] = cv::saturate_cast<uchar>(x * 255.0);
            }
    }

    // 3D LUT: saturation followed by the look, blended by the look intensity
    const double sat = p.toneEnabled ? p.saturation : 1.0;
    const bool useSat = std::abs(sat - 1.0) > 0.01;
    const bool useLook = p.lookEnabled && p.lookIntensity > 0.001;
    if (useLook && p.look != lookKey_) {
        lookKey_ = p.look;
        if (p.look.endsWith(QLatin1String(".cube"), Qt::CaseInsensitive)) {
            QString err;
            look_ = Lut3D::fromCube(p.look, &err);
            if (!look_.valid())
                std::fprintf(stderr, "[effects] LUT %s: %s\n", qPrintable(p.look), qPrintable(err));
        } else {
            look_ = Lut3D::builtin(p.look);
        }
    }
    const bool lookOk = useLook && look_.valid();
    const QString key = QStringLiteral("%1|%2|%3").arg(useSat ? sat : 1.0).arg(lookOk ? p.look : QString()).arg(lookOk ? p.lookIntensity : 0.0);
    if (key == colorKey_)
        return;
    colorKey_ = key;
    if (!useSat && !lookOk) {
        colorLut_ = Lut3D();
        return;
    }
    const float s = float(sat), k = float(p.lookIntensity);
    colorLut_ = Lut3D::fromFunction(33, [&](Lut3D::Rgb c) {
        if (useSat) {
            const float y = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
            for (float& v : c)
                v = std::clamp(y + s * (v - y), 0.0f, 1.0f);
        }
        if (lookOk) {
            const Lut3D::Rgb l = look_.sample(c);
            for (int i = 0; i < 3; ++i)
                c[i] += k * (l[i] - c[i]);
        }
        return c;
    });
}

void Effects::finalPass(cv::Mat& f, const Params& p, bool composite)
{
    updateColor(p);
    const bool curves = !curvesIdentity_;
    const Lut3D* lut = colorLut_.valid() ? &colorLut_ : nullptr;
    if (!composite && !curves && !lut)
        return;

    // A blurred background comes from the camera frame and gets the same tone curves and
    // color LUT as the person. A replacement image or color stays as it is: the
    // composite comes after the color pass.
    const bool gradeBackground = p.backgroundMode == BackgroundMode::Blur;

    // One pass over the frame: background composite, tone curves, color LUT
    cv::parallel_for_(cv::Range(0, f.rows), [&](const cv::Range& rows) {
        for (int y = rows.start; y < rows.end; ++y) {
            uchar* row = f.ptr<uchar>(y);
            if (composite && gradeBackground)
                compositeRow(row, bg_.ptr<uchar>(y), personFull_.ptr<uchar>(y), f.cols);
            if (curves || lut) {
                uchar* px = row;
                for (int x = 0; x < f.cols; ++x, px += 3) {
                    if (curves) {
                        px[0] = curves_[0][px[0]];
                        px[1] = curves_[1][px[1]];
                        px[2] = curves_[2][px[2]];
                    }
                    if (lut) {
                        float out[3];
                        lut->mapPixel(px, out);
                        px[0] = uchar(out[0] + 0.5f);
                        px[1] = uchar(out[1] + 0.5f);
                        px[2] = uchar(out[2] + 0.5f);
                    }
                }
            }
            if (composite && !gradeBackground)
                compositeRow(row, bg_.ptr<uchar>(y), personFull_.ptr<uchar>(y), f.cols);
        }
    });
}
