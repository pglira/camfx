#pragma once

#include <QList>
#include <QString>

struct CameraInfo {
    QString device;  // e.g. /dev/video0
    QString name;    // card name reported by the driver
};

// Video capture devices, excluding v4l2loopback devices and metadata nodes
QList<CameraInfo> listCameras();
