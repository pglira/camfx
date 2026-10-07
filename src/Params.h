#pragma once

#include <QColor>
#include <QJsonObject>
#include <QString>

enum class BackgroundMode { Blur, Image, Color };

// All user-adjustable effect parameters. One instance is one preset.
struct Params {
    bool mirror = true;

    bool toneEnabled = true;
    double brightness = 0.0;  // -1..1
    double contrast = 0.0;    // -1..1
    double saturation = 1.0;  // 0..2
    double warmth = 0.0;      // -1..1, negative is cooler
    double gamma = 1.0;       // 0.5..2
    double sharpen = 0.0;     // 0..1

    bool skinEnabled = true;
    double smoothing = 0.5;     // 0..1
    double detail = 0.3;        // 0..1, fraction of skin texture kept
    double skinBrighten = 0.0;  // 0..1

    bool backgroundEnabled = false;
    BackgroundMode backgroundMode = BackgroundMode::Blur;
    double blurStrength = 0.6;  // 0..1
    QString backgroundImage;
    QColor backgroundColor = QColor(40, 44, 52);
    double edgeSoftness = 0.3;  // 0..1

    bool lookEnabled = false;
    QString look = QStringLiteral("Warm");
    double lookIntensity = 0.7;  // 0..1

    QJsonObject toJson() const;
    static Params fromJson(const QJsonObject& o);
};
