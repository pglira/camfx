#pragma once

#include "Params.h"
#include "Pipeline.h"
#include "PresetStore.h"

#include <QObject>
#include <QSystemTrayIcon>
#include <QTimer>
#include <memory>

class LoopbackMonitor;
class QAction;
class QActionGroup;
class QMenu;
class SettingsWindow;

struct AppOptions {
    QString outputDevice = QStringLiteral("/dev/video10");
    QString camera;     // empty: last used or first available
    QString modelPath;
    bool showSettings = false;
};

// Owns the pipeline, the tray icon, the settings window and the persisted state.
class TrayApp : public QObject {
    Q_OBJECT
public:
    explicit TrayApp(const AppOptions& opts, QObject* parent = nullptr);
    ~TrayApp() override;

    void showSettings();

private:
    void applyPreset(const QString& name);
    void applyParams(const Params& p);
    void setBypass(bool on);
    void rebuildPresetMenu();
    void updateTrayIcon();
    void loadState();
    void saveState();
    QString statePath() const;

    AppOptions opts_;
    PresetStore presets_;
    std::unique_ptr<Pipeline> pipeline_;
    std::unique_ptr<LoopbackMonitor> monitor_;
    std::unique_ptr<SettingsWindow> window_;
    std::unique_ptr<QMenu> menu_;
    QSystemTrayIcon tray_;
    QActionGroup* presetGroup_ = nullptr;
    QMenu* presetMenu_ = nullptr;
    QAction* bypassAction_ = nullptr;
    QTimer saveTimer_;

    Params params_;
    QString preset_;
    QString camera_;
    bool bypass_ = false;
    PipelineStats lastStats_;
    int iconState_ = -1;
};
