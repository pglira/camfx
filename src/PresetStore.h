#pragma once

#include "Params.h"

#include <QString>
#include <QStringList>
#include <optional>

// Presets as JSON files in <config>/presets/<name>.json
class PresetStore {
public:
    PresetStore();

    QStringList names() const;
    std::optional<Params> load(const QString& name) const;
    bool save(const QString& name, const Params& p) const;
    bool remove(const QString& name) const;

    QString configDir() const { return dir_; }

private:
    QString path(const QString& name) const;
    void seedDefaults() const;

    QString dir_;
};
