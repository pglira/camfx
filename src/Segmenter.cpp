#include "Segmenter.h"

#include <onnxruntime_cxx_api.h>

#include <array>
#include <cstdio>
#include <opencv2/imgproc.hpp>
#include <vector>

struct Segmenter::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "camfx"};
    std::unique_ptr<Ort::Session> session;
    std::string inputName, outputName;
    std::vector<float> input = std::vector<float>(kSize * kSize * 3);
};

namespace {

std::unique_ptr<Ort::Session> createSession(Ort::Env& env, const std::string& path, bool gpu)
{
    Ort::SessionOptions so;
    so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    // Threads must not spin while waiting: they would compete with the effect threads
    so.AddConfigEntry("session.intra_op.allow_spinning", "0");
    so.AddConfigEntry("session.inter_op.allow_spinning", "0");
    if (gpu) {
        so.SetIntraOpNumThreads(1);
        OrtCUDAProviderOptions cuda{};
        cuda.device_id = 0;
        cuda.cudnn_conv_algo_search = OrtCudnnConvAlgoSearchHeuristic;
        so.AppendExecutionProvider_CUDA(cuda);
    } else {
        so.SetIntraOpNumThreads(4);
    }
    return std::make_unique<Ort::Session>(env, path.c_str(), so);
}

}  // namespace

Segmenter::Segmenter(const std::string& modelPath) : impl_(std::make_unique<Impl>())
{
    try {
        impl_->session = createSession(impl_->env, modelPath, true);
        gpu_ = true;
    } catch (const Ort::Exception& e) {
        std::fprintf(stderr, "[segmenter] CUDA unavailable (%s), using CPU\n", e.what());
        try {
            impl_->session = createSession(impl_->env, modelPath, false);
        } catch (const Ort::Exception& e2) {
            error_ = e2.what();
            return;
        }
    }
    Ort::AllocatorWithDefaultOptions alloc;
    impl_->inputName = impl_->session->GetInputNameAllocated(0, alloc).get();
    impl_->outputName = impl_->session->GetOutputNameAllocated(0, alloc).get();
    ok_ = true;
    thread_ = std::thread([this] { run(); });
}

Segmenter::~Segmenter()
{
    {
        std::lock_guard lk(m_);
        quit_ = true;
    }
    cvIn_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

void Segmenter::submit(const cv::Mat& bgr, uint64_t seq)
{
    cv::Mat small;
    cv::resize(bgr, small, cv::Size(kSize, kSize), 0, 0, cv::INTER_LINEAR);
    {
        std::lock_guard lk(m_);
        pending_ = small;
        pendingSeq_ = seq;
        hasPending_ = true;
    }
    cvIn_.notify_one();
}

bool Segmenter::latest(SegResult& out, uint64_t minSeq, std::chrono::microseconds timeout)
{
    std::unique_lock lk(m_);
    if (timeout.count() > 0)
        cvOut_.wait_for(lk, timeout, [&] { return result_.seq >= minSeq || quit_; });
    if (!result_.valid())
        return false;
    out = result_;  // cv::Mat copies are shallow; the worker allocates new mats per result
    return true;
}

void Segmenter::run()
{
    // Warm-up: the first CUDA inference allocates memory and picks kernels
    try {
        std::array<int64_t, 4> shape{1, kSize, kSize, 3};
        auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value in = Ort::Value::CreateTensor<float>(mem, impl_->input.data(), impl_->input.size(),
                                                        shape.data(), shape.size());
        const char* inN = impl_->inputName.c_str();
        const char* outN = impl_->outputName.c_str();
        for (int i = 0; i < 3; ++i)
            impl_->session->Run(Ort::RunOptions{nullptr}, &inN, &in, 1, &outN, 1);
    } catch (const Ort::Exception& e) {
        std::fprintf(stderr, "[segmenter] warm-up failed: %s\n", e.what());
    }

    for (;;) {
        cv::Mat frame;
        uint64_t seq;
        {
            std::unique_lock lk(m_);
            cvIn_.wait(lk, [&] { return hasPending_ || quit_; });
            if (quit_)
                return;
            frame = pending_;
            seq = pendingSeq_;
            hasPending_ = false;
        }

        const auto t0 = std::chrono::steady_clock::now();

        // BGR uint8 -> RGB float in [-1, 1], NHWC
        float* dst = impl_->input.data();
        for (int y = 0; y < kSize; ++y) {
            const uchar* p = frame.ptr<uchar>(y);
            for (int x = 0; x < kSize; ++x, p += 3) {
                *dst++ = p[2] / 127.5f - 1.0f;
                *dst++ = p[1] / 127.5f - 1.0f;
                *dst++ = p[0] / 127.5f - 1.0f;
            }
        }

        SegResult r;
        try {
            std::array<int64_t, 4> shape{1, kSize, kSize, 3};
            auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            Ort::Value in = Ort::Value::CreateTensor<float>(mem, impl_->input.data(), impl_->input.size(),
                                                            shape.data(), shape.size());
            const char* inN = impl_->inputName.c_str();
            const char* outN = impl_->outputName.c_str();
            auto outs = impl_->session->Run(Ort::RunOptions{nullptr}, &inN, &in, 1, &outN, 1);

            // Model output [1, 256, 256, 2]: p(person), p(skin)
            const cv::Mat masks(kSize, kSize, CV_32FC2, const_cast<float*>(outs[0].GetTensorData<float>()));
            cv::Mat planes[2];
            cv::split(masks, planes);
            r.person = planes[0];
            r.skin = planes[1];
            r.seq = seq;
        } catch (const Ort::Exception& e) {
            std::fprintf(stderr, "[segmenter] inference failed: %s\n", e.what());
            continue;
        }

        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        inferMs_.store(inferMs_.load() * 0.9 + ms * 0.1);
        {
            std::lock_guard lk(m_);
            result_ = std::move(r);
        }
        cvOut_.notify_all();
    }
}
