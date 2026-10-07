#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <opencv2/core.hpp>
#include <string>
#include <thread>

// Per-pixel probabilities at model resolution (CV_32F, 256x256)
struct SegResult {
    cv::Mat person;  // 1 - p(background)
    cv::Mat skin;    // p(face skin) + p(body skin)
    uint64_t seq = 0;
    bool valid() const { return !person.empty(); }
};

// Runs the multiclass selfie segmentation model on a worker thread.
// Only the newest submitted frame is processed; older pending frames are dropped.
class Segmenter {
public:
    explicit Segmenter(const std::string& modelPath);
    ~Segmenter();

    bool ok() const { return ok_; }
    bool usingGpu() const { return gpu_; }
    const std::string& error() const { return error_; }
    double inferenceMs() const { return inferMs_.load(); }

    static constexpr int kSize = 256;

    // Queue a BGR frame for segmentation; it is resized to the model input with bilinear
    // interpolation, so a frame close to the model size gives the best result
    void submit(const cv::Mat& bgr, uint64_t seq);

    // Copy the newest result into `out`. Wait up to `timeout` for a result with seq >= minSeq.
    // Return false if no result exists yet.
    bool latest(SegResult& out, uint64_t minSeq, std::chrono::microseconds timeout);

private:
    void run();

    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool ok_ = false;
    bool gpu_ = false;
    std::string error_;
    std::atomic<double> inferMs_{0.0};

    std::mutex m_;
    std::condition_variable cvIn_, cvOut_;
    cv::Mat pending_;
    uint64_t pendingSeq_ = 0;
    bool hasPending_ = false;
    SegResult result_;
    bool quit_ = false;
    std::thread thread_;
};
