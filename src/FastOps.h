#pragma once

#include <opencv2/core.hpp>

// Parallel single-pass image operations for BGR 8-bit frames
namespace fastops {

// Convert planar YUV 4:2:0 (BT.601) to BGR, optionally mirrored horizontally.
// fullRange selects JPEG-style 0-255 levels instead of 16-235.
void i420ToBgr(const uchar* y, int strideY, const uchar* u, int strideU, const uchar* v, int strideV, int width,
               int height, bool fullRange, cv::Mat& dst, bool mirror);

// Convert BGR to packed YUY2 (BT.601, 16-235 levels); width must be even
void bgrToYuy2(const cv::Mat& src, uchar* dst, int dstStride);

// Downscale by an integer factor k with a k x k box average; src size must be divisible by k
void downscaleBox(const cv::Mat& src, cv::Mat& dst, int k);

// Unsharp mask with a 3x3 box blur: f = f + amount * (f - blur(f)). `tmp` is scratch memory.
void sharpen3x3(cv::Mat& f, cv::Mat& tmp, float amount);

}  // namespace fastops
