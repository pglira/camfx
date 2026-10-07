#include "Pipeline.h"

#include "Effects.h"
#include "FastOps.h"
#include "Segmenter.h"

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <QFileInfo>
#include <algorithm>
#include <cstdio>
#include <string>
#include <ctime>
#include <opencv2/imgproc.hpp>

namespace {

constexpr int64_t kMs = 1000000;
constexpr int64_t kIdleGraceNs = 3000 * kMs;     // keep the camera open after the last user leaves
constexpr int64_t kPlaceholderPeriodNs = 1000 * kMs;
constexpr int64_t kStatsPeriodNs = 500 * kMs;
constexpr int64_t kPreviewPeriodNs = 33 * kMs;

GstElement* parseLaunch(const QString& desc, QString* error)
{
    GError* err = nullptr;
    GstElement* e = gst_parse_launch(desc.toUtf8().constData(), &err);
    if (err) {
        if (error)
            *error = QString::fromUtf8(err->message);
        g_error_free(err);
        if (e)
            gst_object_unref(e);
        return nullptr;
    }
    return e;
}

void destroyPipeline(GstElement*& p)
{
    if (!p)
        return;
    gst_element_set_state(p, GST_STATE_NULL);
    gst_object_unref(p);
    p = nullptr;
}

QString popError(GstElement* pipe)
{
    GstBus* bus = gst_element_get_bus(pipe);
    QString result;
    while (GstMessage* msg = gst_bus_pop_filtered(bus, GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_EOS))) {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError* err = nullptr;
            gchar* dbg = nullptr;
            gst_message_parse_error(msg, &err, &dbg);
            result = QString::fromUtf8(err->message);
            std::fprintf(stderr, "[pipeline] %s (%s)\n", err->message, dbg ? dbg : "");
            g_error_free(err);
            g_free(dbg);
        } else {
            result = QStringLiteral("end of stream");
        }
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
    return result;
}

double ms(int64_t ns) { return ns / 1e6; }

// Profiling: for each element of a pipeline, print the average age of buffers
// (now - capture timestamp) when they leave the element's src pad.
struct ProbeStat {
    GstElement* pipe;
    std::string name;
    double sum = 0;
    int n = 0;
};

void addProfileProbes(GstElement* pipe)
{
    GstIterator* it = gst_bin_iterate_elements(GST_BIN(pipe));
    GValue item = G_VALUE_INIT;
    while (gst_iterator_next(it, &item) == GST_ITERATOR_OK) {
        GstElement* e = GST_ELEMENT(g_value_get_object(&item));
        if (GstPad* pad = gst_element_get_static_pad(e, "src")) {
            auto* st = new ProbeStat{pipe, GST_ELEMENT_NAME(e)};
            gst_pad_add_probe(
                pad, GST_PAD_PROBE_TYPE_BUFFER,
                [](GstPad*, GstPadProbeInfo* info, gpointer d) -> GstPadProbeReturn {
                    auto* st = static_cast<ProbeStat*>(d);
                    GstBuffer* b = GST_PAD_PROBE_INFO_BUFFER(info);
                    if (GST_BUFFER_PTS_IS_VALID(b)) {
                        const int64_t cap = int64_t(gst_element_get_base_time(st->pipe) + GST_BUFFER_PTS(b));
                        st->sum += ms(Pipeline::nowNs() - cap);
                        if (++st->n == 100) {
                            std::fprintf(stderr, "[profile ms] %-14s age %.2f\n", st->name.c_str(), st->sum / st->n);
                            st->sum = 0;
                            st->n = 0;
                        }
                    }
                    return GST_PAD_PROBE_OK;
                },
                st, [](gpointer d) { delete static_cast<ProbeStat*>(d); });
            gst_object_unref(pad);
        }
        g_value_reset(&item);
    }
    g_value_unset(&item);
    gst_iterator_free(it);
}

}  // namespace

int64_t Pipeline::nowNs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

Pipeline::Pipeline(const QString& outputDevice, const QString& modelPath, QObject* parent)
    : QObject(parent), outputDevice_(outputDevice), modelPath_(modelPath),
      params_(std::make_shared<const Params>())
{
    qRegisterMetaType<PipelineStats>();
    thread_ = std::thread([this] { run(); });
}

Pipeline::~Pipeline()
{
    quit_ = true;
    wake();
    if (thread_.joinable())
        thread_.join();
}

void Pipeline::wake()
{
    { std::lock_guard lk(m_); }
    cv_.notify_all();
}

void Pipeline::setParams(const Params& p)
{
    std::lock_guard lk(m_);
    params_ = std::make_shared<const Params>(p);
}

void Pipeline::setBypass(bool on) { bypass_ = on; }

void Pipeline::setCamera(const QString& device)
{
    {
        std::lock_guard lk(m_);
        if (device == camera_)
            return;
        camera_ = device;
        cameraChanged_ = true;
    }
    cv_.notify_all();
}

void Pipeline::setPreviewEnabled(bool on)
{
    preview_ = on;
    wake();
}

void Pipeline::setPreviewWidth(int width)
{
    previewWidth_ = std::clamp(width, 64, int(kWidth));
}

void Pipeline::setConsumerCount(int n)
{
    consumers_ = n;
    wake();
}

void Pipeline::setForceActive(bool on)
{
    force_ = on;
    wake();
}

void Pipeline::setFrameHook(FrameHook hook)
{
    std::lock_guard lk(m_);
    frameHook_ = std::move(hook);
}

bool Pipeline::startOutput()
{
    if (!QFileInfo::exists(outputDevice_)) {
        status_ = QStringLiteral("Virtual camera %1 not found (run install.sh)").arg(outputDevice_);
        return false;
    }
    const QString desc =
        QStringLiteral("appsrc name=src is-live=true format=time do-timestamp=false block=false "
                       "max-buffers=2 leaky-type=downstream "
                       "caps=video/x-raw,format=YUY2,width=%1,height=%2,framerate=%3/1,"
                       "interlace-mode=progressive,pixel-aspect-ratio=1/1,colorimetry=bt601 "
                       "! v4l2sink name=sink device=%4 sync=false async=false")
            .arg(kWidth).arg(kHeight).arg(kFps).arg(outputDevice_);
    QString err;
    outPipe_ = parseLaunch(desc, &err);
    if (!outPipe_) {
        status_ = QStringLiteral("Output pipeline: %1").arg(err);
        return false;
    }
    appsrc_ = gst_bin_get_by_name(GST_BIN(outPipe_), "src");
    GstElement* sink = gst_bin_get_by_name(GST_BIN(outPipe_), "sink");
    GstPad* pad = gst_element_get_static_pad(sink, "sink");
    gst_pad_add_probe(
        pad, GST_PAD_PROBE_TYPE_BUFFER,
        [](GstPad*, GstPadProbeInfo* info, gpointer self) -> GstPadProbeReturn {
            GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
            static_cast<Pipeline*>(self)->onSinkBuffer(GST_BUFFER_PTS(buf));
            return GST_PAD_PROBE_OK;
        },
        this, nullptr);
    gst_object_unref(pad);
    gst_object_unref(sink);

    if (gst_element_set_state(outPipe_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        status_ = QStringLiteral("Cannot open %1: %2").arg(outputDevice_, popError(outPipe_));
        gst_object_unref(appsrc_);
        appsrc_ = nullptr;
        destroyPipeline(outPipe_);
        return false;
    }
    return true;
}

void Pipeline::stopOutput()
{
    if (appsrc_) {
        gst_object_unref(appsrc_);
        appsrc_ = nullptr;
    }
    destroyPipeline(outPipe_);
}

bool Pipeline::startInput(const QString& device)
{
    const QString sink = QStringLiteral(
        "videoconvert ! video/x-raw,format=I420 ! appsink name=sink max-buffers=1 drop=true sync=false");
    const QString candidates[] = {
        QStringLiteral("v4l2src device=%1 ! image/jpeg,width=%2,height=%3,framerate=%4/1 ! jpegdec ! %5"),
        QStringLiteral("v4l2src device=%1 ! video/x-raw,width=%2,height=%3,framerate=%4/1 ! %5"),
        QStringLiteral("v4l2src device=%1 ! video/x-raw,width=%2,height=%3 ! %5"),
        QStringLiteral("v4l2src device=%1 ! decodebin ! %5"),
    };
    for (const QString& c : candidates) {
        const QString desc = QString(c).arg(device).arg(kWidth).arg(kHeight).arg(kFps).arg(sink);
        QString err;
        inPipe_ = parseLaunch(desc, &err);
        if (!inPipe_)
            continue;
        appsink_ = gst_bin_get_by_name(GST_BIN(inPipe_), "sink");
        if (qEnvironmentVariableIntValue("CAMFX_PROFILE"))
            addProfileProbes(inPipe_);
        if (gst_element_set_state(inPipe_, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE) {
            // Negotiation errors show up only once data flows: wait for the first frame
            GstSample* s = gst_app_sink_try_pull_sample(GST_APP_SINK(appsink_), 3 * GST_SECOND);
            if (s) {
                gst_sample_unref(s);
                std::fprintf(stderr, "[pipeline] camera: %s\n", qPrintable(desc));
                return true;
            }
        }
        err = popError(inPipe_);
        std::fprintf(stderr, "[pipeline] failed: %s: %s\n", qPrintable(desc), qPrintable(err));
        stopInput();
        status_ = QStringLiteral("Cannot open camera %1: %2").arg(device, err);
    }
    return false;
}

void Pipeline::stopInput()
{
    if (appsink_) {
        gst_object_unref(appsink_);
        appsink_ = nullptr;
    }
    destroyPipeline(inPipe_);
}

bool Pipeline::inputHasError()
{
    const QString err = popError(inPipe_);
    if (err.isEmpty())
        return false;
    status_ = QStringLiteral("Camera error: %1").arg(err);
    return true;
}

void Pipeline::run()
{
    if (!startOutput())
        std::fprintf(stderr, "[pipeline] %s\n", qPrintable(status_));

    placeholder_ = cv::Mat(kHeight, kWidth, CV_8UC3, cv::Scalar(30, 26, 24));
    cv::putText(placeholder_, "Camera paused", cv::Point(kWidth / 2 - 190, kHeight / 2 + 15),
                cv::FONT_HERSHEY_SIMPLEX, 1.6, cv::Scalar(140, 140, 140), 2, cv::LINE_AA);
    effects_ = std::make_unique<Effects>();

    int64_t lastWantedNs = 0, lastPlaceholderNs = 0, retryAtNs = 0;
    QString runningCamera;

    while (!quit_) {
        const int64_t now = nowNs();
        const bool want = consumers_ > 0 || preview_ || force_;
        if (want)
            lastWantedNs = now;
        const bool shouldRun = want || (inPipe_ && now - lastWantedNs < kIdleGraceNs);

        QString camera;
        bool cameraChanged;
        {
            std::lock_guard lk(m_);
            camera = camera_;
            cameraChanged = cameraChanged_;
            cameraChanged_ = false;
        }
        if (cameraChanged && inPipe_) {
            stopInput();
            retryAtNs = 0;
        }

        if (shouldRun && !inPipe_ && now >= retryAtNs && !camera.isEmpty()) {
            if (startInput(camera)) {
                runningCamera = camera;
                status_.clear();
            } else {
                retryAtNs = now + 2000 * kMs;
            }
        } else if (!shouldRun && inPipe_) {
            stopInput();
        }

        if (inPipe_) {
            GstSample* s = gst_app_sink_try_pull_sample(GST_APP_SINK(appsink_), 100 * GST_MSECOND);
            if (s) {
                processSample(s);
                gst_sample_unref(s);
            } else if (inputHasError() || gst_app_sink_is_eos(GST_APP_SINK(appsink_))) {
                stopInput();
                retryAtNs = now + 2000 * kMs;
            }
        } else {
            if (appsrc_ && now - lastPlaceholderNs >= kPlaceholderPeriodNs) {
                pushPlaceholder();
                lastPlaceholderNs = now;
            }
            std::unique_lock lk(m_);
            cv_.wait_for(lk, std::chrono::milliseconds(100));
        }
        if (outPipe_) {
            const QString err = popError(outPipe_);
            if (!err.isEmpty()) {
                status_ = QStringLiteral("Virtual camera error: %1").arg(err);
                stopOutput();
            }
        }
        reportStats(false);
    }
    stopInput();
    stopOutput();
    segmenter_.reset();
}

void Pipeline::processSample(GstSample* sample)
{
    const int64_t recvNs = nowNs();
    GstBuffer* buf = gst_sample_get_buffer(sample);
    GstVideoInfo info;
    if (!buf || !gst_video_info_from_caps(&info, gst_sample_get_caps(sample)))
        return;

    // Capture time on the pipeline clock (system monotonic clock)
    int64_t captureNs = recvNs;
    if (GST_BUFFER_PTS_IS_VALID(buf))
        captureNs = int64_t(gst_element_get_base_time(inPipe_) + GST_BUFFER_PTS(buf));
    if (captureNs > recvNs)
        captureNs = recvNs;

    std::shared_ptr<const Params> p;
    FrameHook hook;
    {
        std::lock_guard lk(m_);
        p = params_;
        hook = frameHook_;
    }

    // I420 -> BGR (+ mirror) in one pass; other sizes are scaled afterwards
    GstVideoFrame vf;
    if (!gst_video_frame_map(&vf, &info, buf, GST_MAP_READ))
        return;
    const int w = GST_VIDEO_FRAME_WIDTH(&vf), h = GST_VIDEO_FRAME_HEIGHT(&vf);
    const bool fullRange = GST_VIDEO_INFO_COLORIMETRY(&info).range == GST_VIDEO_COLOR_RANGE_0_255;
    const bool native = w == kWidth && h == kHeight;
    cv::Mat& target = native ? frame_ : inScratch_;
    fastops::i420ToBgr(static_cast<const uchar*>(GST_VIDEO_FRAME_PLANE_DATA(&vf, 0)), GST_VIDEO_FRAME_PLANE_STRIDE(&vf, 0),
                       static_cast<const uchar*>(GST_VIDEO_FRAME_PLANE_DATA(&vf, 1)), GST_VIDEO_FRAME_PLANE_STRIDE(&vf, 1),
                       static_cast<const uchar*>(GST_VIDEO_FRAME_PLANE_DATA(&vf, 2)), GST_VIDEO_FRAME_PLANE_STRIDE(&vf, 2),
                       w, h, fullRange, target, p->mirror);
    gst_video_frame_unmap(&vf);
    if (!native)
        cv::resize(inScratch_, frame_, cv::Size(kWidth, kHeight), 0, 0, cv::INTER_AREA);

    const uint64_t seq = ++seq_;
    int64_t segWaitNs = 0;
    if (!bypass_) {
        SegResult seg;
        bool haveSeg = false;
        const bool needSeg = p->skinEnabled || p->backgroundEnabled;
        const cv::Mat& quarter = effects_->beginFrame(frame_);
        if (needSeg && !segmenter_) {
            segmenter_ = std::make_unique<Segmenter>(modelPath_.toStdString());
            if (!segmenter_->ok())
                status_ = QStringLiteral("Segmentation model: %1").arg(QString::fromStdString(segmenter_->error()));
        }
        const bool segOk = needSeg && segmenter_->ok();
        if (segOk)
            segmenter_->submit(quarter, seq);
        // Mask-independent work overlaps with the segmentation of this frame
        effects_->prepare(frame_, *p);
        if (segOk) {
            // With a fast GPU the mask of the current frame is ready within a few ms;
            // otherwise use the newest available mask without waiting.
            const auto budget = std::chrono::microseconds(segmenter_->usingGpu() ? 15000 : 0);
            const int64_t w0 = nowNs();
            haveSeg = segmenter_->latest(seg, seq, budget);
            segWaitNs = nowNs() - w0;
        }
        effects_->finish(frame_, *p, haveSeg ? &seg : nullptr);
    }
    // Everything from frame arrival to output except the mask wait: color conversion and effects
    const int64_t fxNs = nowNs() - recvNs - segWaitNs;

    if (preview_ && recvNs - lastPreviewNs_ >= kPreviewPeriodNs) {
        lastPreviewNs_ = recvNs;
        const int pw = previewWidth_;
        cv::Mat small = frame_;
        if (pw < kWidth)
            cv::resize(frame_, small, cv::Size(pw, pw * kHeight / kWidth), 0, 0, cv::INTER_AREA);
        QImage img(small.data, small.cols, small.rows, int(small.step), QImage::Format_BGR888);
        emit previewFrame(img.copy());
    }
    if (hook)
        hook(frame_, seq, captureNs);

    pushFrame(frame_, captureNs);

    std::lock_guard lk(accMutex_);
    acc_.frames++;
    acc_.capture += ms(recvNs - captureNs);
    acc_.segWait += ms(segWaitNs);
    acc_.effects += ms(fxNs);
}

void Pipeline::pushFrame(const cv::Mat& bgr, int64_t captureNs)
{
    if (!appsrc_)
        return;
    const int64_t now = nowNs();
    const gsize size = gsize(kWidth) * kHeight * 2;
    GstBuffer* buf = gst_buffer_new_allocate(nullptr, size, nullptr);
    GstMapInfo map;
    if (!gst_buffer_map(buf, &map, GST_MAP_WRITE)) {
        gst_buffer_unref(buf);
        return;
    }
    fastops::bgrToYuy2(bgr, map.data, kWidth * 2);
    gst_buffer_unmap(buf, &map);

    GstClock* clock = gst_element_get_clock(outPipe_);
    GstClockTime pts = 0;
    if (clock) {
        pts = gst_clock_get_time(clock) - gst_element_get_base_time(outPipe_);
        gst_object_unref(clock);
    }
    GST_BUFFER_PTS(buf) = pts;
    GST_BUFFER_DURATION(buf) = GST_SECOND / kFps;
    {
        std::lock_guard lk(inFlightMutex_);
        InFlight& f = inFlight_[(pts / GST_MSECOND) % 16];
        f = {pts, captureNs, now};
    }
    gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buf);
}

void Pipeline::onSinkBuffer(uint64_t pts)
{
    const int64_t now = nowNs();
    InFlight f;
    {
        std::lock_guard lk(inFlightMutex_);
        f = inFlight_[(pts / GST_MSECOND) % 16];
    }
    if (f.pts != pts || f.captureNs == 0)
        return;  // placeholder frame or entry already overwritten
    std::lock_guard lk(accMutex_);
    acc_.outFrames++;
    acc_.output += ms(now - f.pushNs);
    acc_.total += ms(now - f.captureNs);
}

void Pipeline::pushPlaceholder()
{
    pushFrame(placeholder_, 0);
}

void Pipeline::reportStats(bool force)
{
    const int64_t now = nowNs();
    if (!force && now - lastStatsNs_ < kStatsPeriodNs)
        return;
    const double dt = (now - lastStatsNs_) / 1e9;
    lastStatsNs_ = now;

    Acc a;
    {
        std::lock_guard lk(accMutex_);
        a = acc_;
        acc_ = Acc{};
    }
    PipelineStats s;
    s.active = inPipe_ != nullptr;
    s.outputOk = outPipe_ != nullptr;
    s.gpu = segmenter_ && segmenter_->usingGpu();
    s.consumers = consumers_;
    s.inferenceMs = segmenter_ ? segmenter_->inferenceMs() : 0;
    s.status = status_;
    {
        std::lock_guard lk(m_);
        s.camera = camera_;
    }
    if (a.frames > 0) {
        s.fps = a.frames / dt;
        s.captureMs = a.capture / a.frames;
        s.segWaitMs = a.segWait / a.frames;
        s.effectsMs = a.effects / a.frames;
    }
    if (a.outFrames > 0) {
        s.outputMs = a.output / a.outFrames;
        s.totalMs = a.total / a.outFrames;
    }
    emit statsUpdated(s);
}
