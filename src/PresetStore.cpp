#include "PresetStore.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

PresetStore::PresetStore()
    : dir_(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/presets")
{
    QDir().mkpath(dir_);
    if (names().isEmpty())
        seedDefaults();
}

QString PresetStore::path(const QString& name) const
{
    return dir_ + "/" + name + ".json";
}

QStringList PresetStore::names() const
{
    QStringList out;
    for (const QString& f : QDir(dir_).entryList({"*.json"}, QDir::Files, QDir::Name))
        out << f.chopped(5);
    return out;
}

std::optional<Params> PresetStore::load(const QString& name) const
{
    QFile f(path(name));
    if (!f.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject())
        return std::nullopt;
    return Params::fromJson(doc.object());
}

bool PresetStore::save(const QString& name, const Params& p) const
{
    QSaveFile f(path(name));
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(QJsonDocument(p.toJson()).toJson());
    return f.commit();
}

bool PresetStore::remove(const QString& name) const
{
    return QFile::remove(path(name));
}

void PresetStore::seedDefaults() const
{
    Params natural;
    natural.smoothing = 0.35;
    natural.detail = 0.35;
    natural.sharpen = 0.15;
    save("Natural", natural);

    Params soft;
    soft.smoothing = 0.7;
    soft.detail = 0.2;
    soft.skinBrighten = 0.25;
    soft.warmth = 0.15;
    soft.lookEnabled = true;
    soft.look = "Warm";
    soft.lookIntensity = 0.4;
    save("Soft", soft);

    Params meeting;
    meeting.smoothing = 0.4;
    meeting.backgroundEnabled = true;
    meeting.backgroundMode = BackgroundMode::Blur;
    meeting.blurStrength = 0.6;
    save("Meeting", meeting);

    Params cinematic;
    cinematic.smoothing = 0.4;
    cinematic.contrast = 0.1;
    cinematic.backgroundEnabled = true;
    cinematic.blurStrength = 0.8;
    cinematic.lookEnabled = true;
    cinematic.look = "Teal & Orange";
    cinematic.lookIntensity = 0.8;
    save("Cinematic", cinematic);

    Params off;
    off.toneEnabled = false;
    off.skinEnabled = false;
    save("Off", off);
}
