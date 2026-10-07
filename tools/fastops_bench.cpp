// Times the fastops kernels and the skin filter coefficients at 1280x720.
// Usage: fastops-bench [opencv-threads]
#include "FastOps.h"
#include <opencv2/core/utility.hpp>
#include <opencv2/imgproc.hpp>
#include <cstdio>
#include <functional>
#include <cstdlib>
#include <vector>
static void timeit(const char* name, const std::function<void()>& fn)
{
    for (int i = 0; i < 5; ++i) fn();
    const int n = 200;
    const int64 t0 = cv::getTickCount();
    for (int i = 0; i < n; ++i) fn();
    std::printf("%-28s %7.3f ms\n", name, (cv::getTickCount() - t0) * 1000.0 / cv::getTickFrequency() / n);
}
int main(int argc, char** argv)
{
    if (argc > 1) cv::setNumThreads(atoi(argv[1]));
    cv::Mat f(720, 1280, CV_8UC3), g, q, tmp, I;
    cv::randu(f, 0, 255);
    std::vector<uchar> yuv(1280 * 720 * 3 / 2), yuy2(1280 * 720 * 2);
    timeit("i420ToBgr mirror", [&] { fastops::i420ToBgr(yuv.data(), 1280, yuv.data() + 1280 * 720, 640, yuv.data() + 1280 * 720 * 5 / 4, 640, 1280, 720, true, g, true); });
    timeit("bgrToYuy2", [&] { fastops::bgrToYuy2(f, yuy2.data(), 2560); });
    timeit("downscaleBox 4", [&] { fastops::downscaleBox(f, q, 4); });
    timeit("sharpen3x3", [&] { fastops::sharpen3x3(g, tmp, 0.3f); });
    timeit("guided coeffs", [&] {
        q.convertTo(I, CV_32FC3, 1 / 255.0);
        cv::Mat m, v, a, b, A, B; int r = 4;
        cv::Size k(2*r+1, 2*r+1);
        cv::boxFilter(I, m, -1, k); cv::boxFilter(I.mul(I), v, -1, k); v = v - m.mul(m);
        cv::divide(v, v + cv::Scalar::all(0.01), a); b = m - a.mul(m);
        cv::boxFilter(a, A, -1, k); cv::boxFilter(b, B, -1, k);
        A.convertTo(A, CV_8UC3, 255); B.convertTo(B, CV_8UC3, 255);
    });
}
