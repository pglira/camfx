#include "Params.h"

namespace {

QString modeToString(BackgroundMode m)
{
    switch (m) {
    case BackgroundMode::Image: return QStringLiteral("image");
    case BackgroundMode::Color: return QStringLiteral("color");
    case BackgroundMode::Blur: break;
    }
    return QStringLiteral("blur");
}

BackgroundMode modeFromString(const QString& s)
{
    if (s == QLatin1String("image"))
        return BackgroundMode::Image;
    if (s == QLatin1String("color"))
        return BackgroundMode::Color;
    return BackgroundMode::Blur;
}

}  // namespace

QJsonObject Params::toJson() const
{
    return QJsonObject{
        {"mirror", mirror},
        {"tone", QJsonObject{{"enabled", toneEnabled},
                             {"brightness", brightness},
                             {"contrast", contrast},
                             {"saturation", saturation},
                             {"warmth", warmth},
                             {"gamma", gamma},
                             {"sharpen", sharpen}}},
        {"skin", QJsonObject{{"enabled", skinEnabled},
                             {"smoothing", smoothing},
                             {"detail", detail},
                             {"brighten", skinBrighten}}},
        {"background", QJsonObject{{"enabled", backgroundEnabled},
                                   {"mode", modeToString(backgroundMode)},
                                   {"blur", blurStrength},
                                   {"image", backgroundImage},
                                   {"color", backgroundColor.name()},
                                   {"edgeSoftness", edgeSoftness}}},
        {"look", QJsonObject{{"enabled", lookEnabled},
                             {"name", look},
                             {"intensity", lookIntensity}}},
    };
}

Params Params::fromJson(const QJsonObject& o)
{
    // Missing keys keep the default values
    Params p;
    p.mirror = o.value("mirror").toBool(p.mirror);

    const QJsonObject t = o.value("tone").toObject();
    p.toneEnabled = t.value("enabled").toBool(p.toneEnabled);
    p.brightness = t.value("brightness").toDouble(p.brightness);
    p.contrast = t.value("contrast").toDouble(p.contrast);
    p.saturation = t.value("saturation").toDouble(p.saturation);
    p.warmth = t.value("warmth").toDouble(p.warmth);
    p.gamma = t.value("gamma").toDouble(p.gamma);
    p.sharpen = t.value("sharpen").toDouble(p.sharpen);

    const QJsonObject s = o.value("skin").toObject();
    p.skinEnabled = s.value("enabled").toBool(p.skinEnabled);
    p.smoothing = s.value("smoothing").toDouble(p.smoothing);
    p.detail = s.value("detail").toDouble(p.detail);
    p.skinBrighten = s.value("brighten").toDouble(p.skinBrighten);

    const QJsonObject b = o.value("background").toObject();
    p.backgroundEnabled = b.value("enabled").toBool(p.backgroundEnabled);
    p.backgroundMode = modeFromString(b.value("mode").toString());
    p.blurStrength = b.value("blur").toDouble(p.blurStrength);
    p.backgroundImage = b.value("image").toString(p.backgroundImage);
    const QColor c(b.value("color").toString());
    if (c.isValid())
        p.backgroundColor = c;
    p.edgeSoftness = b.value("edgeSoftness").toDouble(p.edgeSoftness);

    const QJsonObject l = o.value("look").toObject();
    p.lookEnabled = l.value("enabled").toBool(p.lookEnabled);
    p.look = l.value("name").toString(p.look);
    p.lookIntensity = l.value("intensity").toDouble(p.lookIntensity);
    return p;
}
