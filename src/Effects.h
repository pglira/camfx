#pragma once

#include "Lut3D.h"
#include "Params.h"
#include "Segmenter.h"

#include <opencv2/core.hpp>

// Applies all image effects in place to a BGR 8-bit frame. Holds caches
// (LUTs, background image, temporally smoothed masks) between frames.
// Not thread-safe; use one instance per processing thread.
//
// Work is split in two phases so that the mask-independent part can run while
// the segmentation of the same frame is still in progress:
//   beginFrame(): the 1/4 resolution copy of the frame
//   prepare(): sharpening and the skin filter coefficients
//   finish():  everything that needs the masks, then one fused color pass
class Effects {
public:
    // Start a frame: compute the 1/4 resolution copy, which also serves as segmentation input
    const cv::Mat& beginFrame(const cv::Mat& frame);
    void prepare(cv::Mat& frame, const Params& p);
    void finish(cv::Mat& frame, const Params& p, const SegResult* seg);

    // Directories searched for .cube files; a look name ending in ".cube" is a file path
    static QStringList lutDirectories();

private:
    void updateMasks(const SegResult& seg, const Params& p);
    void smoothSkin(cv::Mat& f, const Params& p);
    bool buildBackground(const cv::Mat& f, const Params& p);
    void finalPass(cv::Mat& f, const Params& p, bool composite);
    void updateColor(const Params& p);
    const cv::Mat& backgroundImage(const QString& path, cv::Size size);

    struct Profile {
        double acc[6] = {};
        int frames = 0;
        int64_t t = 0;
        void lap(int i);
        void report();
    };
    Profile prof_;
    cv::Size size_;  // frame size of the current frame
    bool profile_ = false;
    bool profileInit_ = false;

    cv::Mat quarter_;  // current frame at 1/4 resolution, before any effect
    cv::Mat scratch_;

    // Self-guided filter coefficients at 1/4 resolution, 8-bit (value / 255)
    cv::Mat skinA_, skinB_;
    bool skinReady_ = false;

    // Temporally smoothed masks at model resolution, CV_32F
    cv::Mat personEma_, skinEma_;
    uint64_t lastSeq_ = 0;
    // Masks at 1/4 frame resolution, CV_8U
    cv::Mat personQ_, skinQ_;
    // Person mask at full resolution, CV_8U
    cv::Mat personFull_;
    // Background layer for compositing, BGR 8-bit at full resolution
    cv::Mat bg_;

    // Per-channel tone curves (BGR) and the combined saturation + look LUT
    uchar curves_[3][256];
    bool curvesIdentity_ = true;
    Lut3D colorLut_;
    QString colorKey_;

    QString lookKey_;
    Lut3D look_;

    QString bgPath_;
    cv::Size bgSize_;
    cv::Mat bgImage_;
};
