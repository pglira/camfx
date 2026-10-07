#include "Cameras.h"

#include <QDir>
#include <algorithm>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

QList<CameraInfo> listCameras()
{
    QList<CameraInfo> cams;
    QStringList nodes = QDir("/dev").entryList({"video*"}, QDir::System);
    std::sort(nodes.begin(), nodes.end(), [](const QString& a, const QString& b) {
        return a.mid(5).toInt() < b.mid(5).toInt();
    });
    for (const QString& n : nodes) {
        const QString dev = "/dev/" + n;
        const int fd = ::open(dev.toLocal8Bit().constData(), O_RDWR | O_NONBLOCK);
        if (fd < 0)
            continue;
        v4l2_capability cap{};
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
            const uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
            const QString driver = QString::fromLatin1(reinterpret_cast<const char*>(cap.driver));
            if ((caps & V4L2_CAP_VIDEO_CAPTURE) && !driver.contains(QLatin1String("loopback")))
                cams.append({dev, QString::fromLatin1(reinterpret_cast<const char*>(cap.card)).trimmed()});
        }
        ::close(fd);
    }
    return cams;
}
