#pragma once

#include "Params.h"

#include <QImage>
#include <QObject>
#include <QString>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <opencv2/core.hpp>
#include <thread>

typedef struct _GstElement GstElement;
typedef struct _GstSample GstSample;

class Segmenter;
class Effects;

// Latency figures in milliseconds, averaged over the last reporting interval
struct PipelineStats {
    bool active = false;     // camera is open and frames are processed
    bool outputOk = false;   // virtual camera device is open
    bool gpu = false;        // segmentation runs on CUDA
    int consumers = 0;       // applications streaming from the virtual camera
    double fps = 0;
    double captureMs = 0;    // sensor timestamp -> frame available in the app
    double segWaitMs = 0;    // wait for the segmentation mask of the current frame
    double effectsMs = 0;    // color conversion and all image effects
    double outputMs = 0;     // YUY2 conversion and hand-off until the frame reaches the device
    double totalMs = 0;      // sensor timestamp -> frame reaches the virtual camera device
    double inferenceMs = 0;  // model inference time on the segmentation thread
    QString camera;
    QString status;
};
Q_DECLARE_METATYPE(PipelineStats)

// Camera -> effects -> virtual camera. The output side runs for the lifetime of the
// object; the camera side runs while a consumer, the preview, or a forced run needs it.
class Pipeline : public QObject {
    Q_OBJECT
public:
    static constexpr int kWidth = 1280;
    static constexpr int kHeight = 720;
    static constexpr int kFps = 30;

    Pipeline(const QString& outputDevice, const QString& modelPath, QObject* parent = nullptr);
    ~Pipeline() override;

    void setParams(const Params& p);
    void setBypass(bool on);
    void setCamera(const QString& device);
    void setPreviewEnabled(bool on);
    // Width in pixels of the preview frames (16:9, at most the output width)
    void setPreviewWidth(int width);
    void setConsumerCount(int n);
    void setForceActive(bool on);

    // Called on the worker thread for every processed frame right before output.
    // Arguments: the frame, its sequence number, and its capture time (CLOCK_MONOTONIC ns).
    using FrameHook = std::function<void(cv::Mat&, uint64_t, int64_t)>;
    void setFrameHook(FrameHook hook);

    static int64_t nowNs();

signals:
    void previewFrame(const QImage& image);
    void statsUpdated(const PipelineStats& stats);

private:
    void run();
    bool startOutput();
    void stopOutput();
    bool startInput(const QString& device);
    void stopInput();
    bool inputHasError();
    void processSample(GstSample* sample);
    void pushFrame(const cv::Mat& bgr, int64_t captureNs);
    void pushPlaceholder();
    void reportStats(bool force);
    void onSinkBuffer(uint64_t pts);
    void wake();

    QString outputDevice_;
    QString modelPath_;

    std::mutex m_;
    std::condition_variable cv_;
    std::shared_ptr<const Params> params_;
    QString camera_;
    bool cameraChanged_ = false;
    FrameHook frameHook_;

    std::atomic<bool> bypass_{false};
    std::atomic<bool> preview_{false};
    std::atomic<int> previewWidth_{640};
    std::atomic<bool> force_{false};
    std::atomic<int> consumers_{0};
    std::atomic<bool> quit_{false};

    // Worker-thread state
    GstElement* outPipe_ = nullptr;
    GstElement* appsrc_ = nullptr;
    GstElement* inPipe_ = nullptr;
    GstElement* appsink_ = nullptr;
    std::unique_ptr<Segmenter> segmenter_;
    std::unique_ptr<Effects> effects_;
    cv::Mat frame_;
    cv::Mat inScratch_;  // camera frame before scaling, when its size differs from the output
    cv::Mat placeholder_;
    uint64_t seq_ = 0;
    int64_t lastPreviewNs_ = 0;
    int64_t lastStatsNs_ = 0;
    QString status_;

    // Accumulators for the current stats interval
    struct Acc {
        int frames = 0;
        double capture = 0, segWait = 0, effects = 0;
        int outFrames = 0;
        double output = 0, total = 0;
    };
    std::mutex accMutex_;
    Acc acc_;

    // Capture time and push time of recent frames, keyed by output PTS
    struct InFlight {
        uint64_t pts = UINT64_MAX;
        int64_t captureNs = 0;
        int64_t pushNs = 0;
    };
    InFlight inFlight_[16];
    std::mutex inFlightMutex_;

    std::thread thread_;
};
