#include "LoopbackMonitor.h"

#include <QDir>
#include <QFileInfo>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

// From v4l2loopback.h (v0.13 and later)
constexpr uint32_t kEventClientUsage = V4L2_EVENT_PRIVATE_START + 0x08E00000 + 1;

}  // namespace

LoopbackMonitor::LoopbackMonitor(const QString& device, Callback cb) : device_(device), cb_(std::move(cb))
{
    thread_ = std::thread([this] { run(); });
}

LoopbackMonitor::~LoopbackMonitor()
{
    quit_ = true;
    if (thread_.joinable())
        thread_.join();
}

void LoopbackMonitor::run()
{
    while (!quit_) {
        if (!QFileInfo::exists(device_)) {
            usleep(1000 * 1000);
            continue;
        }
        if (!runEvents())
            runProcScan();
    }
}

bool LoopbackMonitor::runEvents()
{
    const int fd = ::open(device_.toLocal8Bit().constData(), O_RDWR | O_NONBLOCK);
    if (fd < 0)
        return false;

    v4l2_event_subscription sub{};
    sub.type = kEventClientUsage;
    sub.flags = V4L2_EVENT_SUB_FL_SEND_INITIAL;
    if (ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0) {
        std::fprintf(stderr, "[monitor] client-usage event not supported, scanning /proc\n");
        ::close(fd);
        return false;
    }

    int last = -1;
    while (!quit_) {
        pollfd pfd{fd, POLLPRI, 0};
        const int r = poll(&pfd, 1, 300);
        if (r < 0 && errno != EINTR)
            break;
        if (r <= 0 || !(pfd.revents & POLLPRI))
            continue;
        v4l2_event ev{};
        while (ioctl(fd, VIDIOC_DQEVENT, &ev) == 0) {
            if (ev.type != kEventClientUsage)
                continue;
            uint32_t count;
            std::memcpy(&count, ev.u.data, sizeof(count));
            if (int(count) != last) {
                last = int(count);
                cb_(last);
            }
        }
    }
    ::close(fd);
    return true;
}

void LoopbackMonitor::runProcScan()
{
    // Count other processes that hold the device open
    const QString target = QFileInfo(device_).canonicalFilePath();
    const QString self = QString::number(getpid());
    int last = -1;
    while (!quit_) {
        int count = 0;
        const QStringList pids = QDir("/proc").entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString& pid : pids) {
            if (pid == self || !pid[0].isDigit())
                continue;
            const QDir fdDir("/proc/" + pid + "/fd");
            const QFileInfoList fds = fdDir.entryInfoList(QDir::System | QDir::NoDotAndDotDot);
            for (const QFileInfo& f : fds) {
                if (f.symLinkTarget() == target) {
                    ++count;
                    break;
                }
            }
        }
        if (count != last) {
            last = count;
            cb_(count);
        }
        for (int i = 0; i < 10 && !quit_; ++i)
            usleep(100 * 1000);
    }
}
