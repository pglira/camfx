#include "Bench.h"

#include "Cameras.h"
#include "LoopbackMonitor.h"
#include "Pipeline.h"
#include "PresetStore.h"

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <opencv2/imgcodecs.hpp>

#include <QCoreApplication>
#include <QFileInfo>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <numeric>
#include <thread>
#include <vector>

namespace {

// The frame sequence number is drawn as 32 black/white blocks along the top edge,
// which survives the BGR -> YUY2 conversion and lets the consumer side identify frames.
constexpr int kBits = 32;
constexpr int kBlock = 16;

void stamp(cv::Mat& f, uint32_t seq)
{
    for (int i = 0; i < kBits; ++i)
        f(cv::Rect(i * kBlock, 0, kBlock, kBlock)).setTo((seq >> i) & 1 ? cv::Scalar::all(255) : cv::Scalar::all(0));
}

uint32_t readStamp(const cv::Mat& bgr)
{
    uint32_t seq = 0;
    const uint8_t* row = bgr.ptr<uint8_t>(kBlock / 2);
    for (int i = 0; i < kBits; ++i)
        if (row[3 * (i * kBlock + kBlock / 2) + 1] > 128)
            seq |= 1u << i;
    return seq;
}

struct Summary {
    double mean = 0, p50 = 0, p95 = 0, max = 0;
    size_t n = 0;
};

Summary summarize(std::vector<double> v)
{
    Summary s;
    s.n = v.size();
    if (v.empty())
        return s;
    std::sort(v.begin(), v.end());
    s.mean = std::accumulate(v.begin(), v.end(), 0.0) / v.size();
    s.p50 = v[v.size() / 2];
    s.p95 = v[std::min(v.size() - 1, size_t(v.size() * 0.95))];
    s.max = v.back();
    return s;
}

void printRow(const char* name, const Summary& s)
{
    std::printf("  %-28s %7.1f %7.1f %7.1f %7.1f\n", name, s.mean, s.p50, s.p95, s.max);
}

}  // namespace

int runBench(const BenchOptions& opts)
{
    QString camera = opts.camera;
    if (camera.isEmpty()) {
        const auto cams = listCameras();
        if (cams.isEmpty()) {
            std::fprintf(stderr, "no camera found\n");
            return 1;
        }
        camera = cams.first().device;
    }
    if (!QFileInfo::exists(opts.outputDevice)) {
        std::fprintf(stderr, "virtual camera %s not found (run install.sh)\n", qPrintable(opts.outputDevice));
        return 1;
    }

    Params params;
    QString presetName = opts.preset;
    if (presetName.isEmpty()) {
        presetName = QStringLiteral("all effects");
        params.backgroundEnabled = true;
        params.lookEnabled = true;
        params.sharpen = 0.2;
        params.saturation = 1.1;
    } else if (const auto p = PresetStore().load(presetName)) {
        params = *p;
    } else {
        std::fprintf(stderr, "preset %s not found\n", qPrintable(presetName));
        return 1;
    }

    std::printf("Benchmark: camera %s -> %s, preset \"%s\", %d s\n", qPrintable(camera),
                qPrintable(opts.outputDevice), qPrintable(presetName), opts.seconds);

    // Declared before the pipeline so they outlive its worker thread
    std::mutex mu;
    std::map<uint32_t, int64_t> captureTimes;  // seq -> capture time
    std::atomic<int64_t> measureFromNs{INT64_MAX};
    cv::Mat lastFrame;  // last frame received by the consumer
    std::vector<double> endToEnd, capture, segWait, effects, output, total, fps, infer;
    bool gpu = false;

    Pipeline pipeline(opts.outputDevice, opts.modelPath);
    pipeline.setParams(params);
    pipeline.setCamera(camera);
    pipeline.setFrameHook([&](cv::Mat& f, uint64_t seq, int64_t captureNs) {
        stamp(f, uint32_t(seq));
        std::lock_guard lk(mu);
        captureTimes[uint32_t(seq)] = captureNs;
        while (captureTimes.size() > 300)
            captureTimes.erase(captureTimes.begin());
    });

    QObject::connect(&pipeline, &Pipeline::statsUpdated, qApp, [&](const PipelineStats& s) {
        if (!s.active || Pipeline::nowNs() < measureFromNs)
            return;
        capture.push_back(s.captureMs);
        segWait.push_back(s.segWaitMs);
        effects.push_back(s.effectsMs);
        output.push_back(s.outputMs);
        total.push_back(s.totalMs);
        fps.push_back(s.fps);
        infer.push_back(s.inferenceMs);
        gpu = s.gpu;
    });

    // Consumer: read the virtual camera like a video call application would
    std::atomic<bool> stop{false};
    std::atomic<int> consumerFrames{0};
    std::thread consumer([&] {
        // Wait until the output side streams, otherwise the capture side cannot start
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        const QString desc = QStringLiteral("v4l2src device=%1 ! videoconvert ! video/x-raw,format=BGR "
                                            "! appsink name=sink max-buffers=1 drop=true sync=false")
                                 .arg(opts.outputDevice);
        GError* err = nullptr;
        GstElement* pipe = gst_parse_launch(desc.toUtf8().constData(), &err);
        if (err) {
            std::fprintf(stderr, "consumer: %s\n", err->message);
            g_error_free(err);
            return;
        }
        GstElement* sink = gst_bin_get_by_name(GST_BIN(pipe), "sink");
        gst_element_set_state(pipe, GST_STATE_PLAYING);
        while (!stop) {
            GstSample* s = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 200 * GST_MSECOND);
            if (!s)
                continue;
            const int64_t now = Pipeline::nowNs();
            GstBuffer* buf = gst_sample_get_buffer(s);
            GstMapInfo map;
            if (gst_buffer_map(buf, &map, GST_MAP_READ)) {
                const cv::Mat img(Pipeline::kHeight, Pipeline::kWidth, CV_8UC3, map.data);
                const uint32_t seq = readStamp(img);
                if (!opts.snapshot.isEmpty()) {
                    std::lock_guard lk(mu);
                    img.copyTo(lastFrame);
                }
                gst_buffer_unmap(buf, &map);
                int64_t cap = 0;
                {
                    std::lock_guard lk(mu);
                    auto it = captureTimes.find(seq);
                    if (it != captureTimes.end())
                        cap = it->second;
                }
                if (cap > 0) {
                    consumerFrames++;
                    if (now >= measureFromNs) {
                        std::lock_guard lk(mu);
                        endToEnd.push_back((now - cap) / 1e6);
                    }
                }
            }
            gst_sample_unref(s);
        }
        gst_element_set_state(pipe, GST_STATE_NULL);
        gst_object_unref(sink);
        gst_object_unref(pipe);
    });

    LoopbackMonitor monitor(opts.outputDevice, [&](int n) { pipeline.setConsumerCount(n); });

    // Skip camera start-up and model warm-up before measuring
    QTimer::singleShot(4000, [&] { measureFromNs = Pipeline::nowNs(); });
    QTimer::singleShot(4000 + opts.seconds * 1000, [] { QCoreApplication::quit(); });
    QCoreApplication::exec();

    stop = true;
    consumer.join();

    if (!opts.snapshot.isEmpty()) {
        std::lock_guard lk(mu);
        if (!lastFrame.empty() && cv::imwrite(opts.snapshot.toStdString(), lastFrame))
            std::printf("Saved last consumer frame to %s\n", qPrintable(opts.snapshot));
    }
    if (total.empty()) {
        std::fprintf(stderr, "no frames processed; is the camera busy?\n");
        return 1;
    }
    std::printf("\nSegmentation on %s, %.1f fps\n", gpu ? "GPU (CUDA)" : "CPU", summarize(fps).mean);
    std::printf("\n  %-28s %7s %7s %7s %7s   [ms]\n", "stage", "mean", "p50", "p95", "max");
    printRow("capture -> app", summarize(capture));
    printRow("mask wait", summarize(segWait));
    printRow("effects", summarize(effects));
    printRow("output (convert + write)", summarize(output));
    printRow("capture -> virtual camera", summarize(total));
    printRow("model inference (parallel)", summarize(infer));
    {
        std::lock_guard lk(mu);
        if (endToEnd.empty()) {
            std::printf("  consumer: no frames received from %s\n", qPrintable(opts.outputDevice));
        } else {
            printRow("capture -> consumer app", summarize(endToEnd));
            std::printf("\n  (stage rows: mean of 0.5 s averages; consumer row: per frame, n=%zu)\n", endToEnd.size());
        }
    }
    return 0;
}
