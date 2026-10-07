#include "FastOps.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>
#include <opencv2/core/utility.hpp>

namespace fastops {

namespace {

// Split rows into chunks of a few dozen rows to keep the thread pool overhead low
constexpr double kStripes = 16;

}  // namespace

struct YuvCoeffs {
    int ys, yo, rv, gu, gv, bu;  // fixed point, 1/1024 units
};

static void i420Row(const uchar* __restrict py, const uchar* __restrict pu, const uchar* __restrict pv,
                    uchar* __restrict d, int width, YuvCoeffs c, bool mirror)
{
    for (int x = 0; x < width; ++x) {
        const int Y = (py[x] - c.yo) * c.ys;
        const int U = pu[x >> 1] - 128, V = pv[x >> 1] - 128;
        uchar* o = d + 3 * (mirror ? width - 1 - x : x);
        o[0] = uchar(std::clamp((Y + c.bu * U + 512) >> 10, 0, 255));
        o[1] = uchar(std::clamp((Y - c.gu * U - c.gv * V + 512) >> 10, 0, 255));
        o[2] = uchar(std::clamp((Y + c.rv * V + 512) >> 10, 0, 255));
    }
}

void i420ToBgr(const uchar* y, int strideY, const uchar* u, int strideU, const uchar* v, int strideV, int width,
               int height, bool fullRange, cv::Mat& dst, bool mirror)
{
    dst.create(height, width, CV_8UC3);
    const YuvCoeffs c = fullRange ? YuvCoeffs{1024, 0, 1436, 352, 731, 1815} : YuvCoeffs{1192, 16, 1634, 401, 832, 2066};
    cv::parallel_for_(
        cv::Range(0, height),
        [=, &dst](const cv::Range& rows) {
            for (int r = rows.start; r < rows.end; ++r)
                i420Row(y + size_t(r) * strideY, u + size_t(r / 2) * strideU, v + size_t(r / 2) * strideV,
                        dst.ptr<uchar>(r), width, c, mirror);
        },
        kStripes);
}

static void yuy2Row(const uchar* __restrict s, uchar* __restrict d, int width)
{
    for (int x = 0; x < width; x += 2, s += 6, d += 4) {
        // BT.601 limited range, coefficients in 1/256 units
        const int b0 = s[0], g0 = s[1], r0 = s[2], b1 = s[3], g1 = s[4], r1 = s[5];
        const int y0 = (66 * r0 + 129 * g0 + 25 * b0 + 128) >> 8;
        const int y1 = (66 * r1 + 129 * g1 + 25 * b1 + 128) >> 8;
        const int rs = r0 + r1, gs = g0 + g1, bs = b0 + b1;
        const int uu = (-38 * rs - 74 * gs + 112 * bs + 256) >> 9;
        const int vv = (112 * rs - 94 * gs - 18 * bs + 256) >> 9;
        d[0] = uchar(y0 + 16);
        d[1] = uchar(std::clamp(uu + 128, 0, 255));
        d[2] = uchar(y1 + 16);
        d[3] = uchar(std::clamp(vv + 128, 0, 255));
    }
}

void bgrToYuy2(const cv::Mat& src, uchar* dst, int dstStride)
{
    CV_Assert(src.type() == CV_8UC3 && src.cols % 2 == 0);
    cv::parallel_for_(
        cv::Range(0, src.rows),
        [&src, dst, dstStride](const cv::Range& rows) {
            for (int r = rows.start; r < rows.end; ++r)
                yuy2Row(src.ptr<uchar>(r), dst + size_t(r) * dstStride, src.cols);
        },
        kStripes);
}

void downscaleBox(const cv::Mat& src, cv::Mat& dst, int k)
{
    CV_Assert(src.type() == CV_8UC3 && src.cols % k == 0 && src.rows % k == 0);
    dst.create(src.rows / k, src.cols / k, CV_8UC3);
    const int area = k * k;
    cv::parallel_for_(
        cv::Range(0, dst.rows),
        [&](const cv::Range& rows) {
            std::vector<int> acc(size_t(dst.cols) * 3);
            for (int y = rows.start; y < rows.end; ++y) {
                std::fill(acc.begin(), acc.end(), 0);
                for (int r = 0; r < k; ++r) {
                    const uchar* s = src.ptr<uchar>(y * k + r);
                    int* a = acc.data();
                    for (int x = 0; x < dst.cols; ++x, a += 3)
                        for (int i = 0; i < k; ++i, s += 3) {
                            a[0] += s[0];
                            a[1] += s[1];
                            a[2] += s[2];
                        }
                }
                uchar* d = dst.ptr<uchar>(y);
                for (int i = 0; i < dst.cols * 3; ++i)
                    d[i] = uchar((acc[i] + area / 2) / area);
            }
        },
        kStripes);
}

// Row kernels take restrict-qualified pointers and plain values so that the compiler
// can vectorize them (uchar stores could otherwise alias any captured variable)
static void sharpenRow(const uchar* __restrict r0, const uchar* __restrict r1, const uchar* __restrict r2,
                       int16_t* __restrict vs, uchar* __restrict d, int n, float k)
{
    for (int i = 0; i < n; ++i)
        vs[i] = int16_t(r0[i] + r1[i] + r2[i]);
    const float k9 = k / 9.0f, k1 = 1.0f + k;
    for (int i = 3; i < n - 3; ++i) {
        const float sum = float(vs[i - 3] + vs[i] + vs[i + 3]);
        const float out = float(r1[i]) * k1 - sum * k9 + 0.5f;
        d[i] = uchar(std::min(std::max(out, 0.0f), 255.0f));
    }
}

void sharpen3x3(cv::Mat& f, cv::Mat& tmp, float amount)
{
    CV_Assert(f.type() == CV_8UC3);
    f.copyTo(tmp);
    const int n = f.cols * 3, h = f.rows;
    cv::parallel_for_(
        cv::Range(0, h),
        [&f, &tmp, n, h, amount](const cv::Range& rows) {
            std::vector<int16_t> vs(n);
            for (int y = rows.start; y < rows.end; ++y)
                sharpenRow(tmp.ptr<uchar>(std::max(y - 1, 0)), tmp.ptr<uchar>(y), tmp.ptr<uchar>(std::min(y + 1, h - 1)),
                           vs.data(), f.ptr<uchar>(y), n, amount);
        },
        kStripes);
}

}  // namespace fastops
