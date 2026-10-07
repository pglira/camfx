#include "TrayApp.h"

#include "Cameras.h"
#include "LoopbackMonitor.h"
#include "SettingsWindow.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QPainter>
#include <QSaveFile>
#include <QStandardPaths>

namespace {

enum IconState { Idle, Live, Bypass };

QIcon makeIcon(int state)
{
    const QColor ring = state == Live ? QColor(230, 90, 150) : state == Bypass ? QColor(230, 150, 40)
                                                                               : QColor(140, 140, 140);
    QPixmap pm(64, 64);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(ring);
    p.drawEllipse(QRectF(4, 4, 56, 56));
    p.setBrush(QColor(30, 30, 30));
    p.drawEllipse(QRectF(16, 16, 32, 32));
    p.setBrush(state == Idle ? QColor(90, 90, 90) : QColor(255, 255, 255));
    p.drawEllipse(QRectF(24, 22, 9, 9));
    return QIcon(pm);
}

}  // namespace

TrayApp::TrayApp(const AppOptions& opts, QObject* parent) : QObject(parent), opts_(opts)
{
    loadState();
    if (!opts_.camera.isEmpty())
        camera_ = opts_.camera;
    const QList<CameraInfo> cams = listCameras();
    bool found = false;
    for (const CameraInfo& c : cams)
        found |= c.device == camera_;
    if (!found && !cams.isEmpty())
        camera_ = cams.first().device;

    pipeline_ = std::make_unique<Pipeline>(opts_.outputDevice, opts_.modelPath);
    pipeline_->setParams(params_);
    pipeline_->setBypass(bypass_);
    pipeline_->setCamera(camera_);

    Pipeline* pl = pipeline_.get();
    monitor_ = std::make_unique<LoopbackMonitor>(opts_.outputDevice, [pl](int n) { pl->setConsumerCount(n); });

    window_ = std::make_unique<SettingsWindow>();
    window_->setParams(params_);
    window_->setPresets(presets_.names(), preset_);
    window_->setCameras(cams, camera_);

    connect(pl, &Pipeline::previewFrame, window_.get(), &SettingsWindow::showPreview);
    connect(pl, &Pipeline::statsUpdated, window_.get(), &SettingsWindow::showStats);
    connect(pl, &Pipeline::statsUpdated, this, [this](const PipelineStats& s) {
        lastStats_ = s;
        updateTrayIcon();
    });
    connect(window_.get(), &SettingsWindow::visibilityChanged, this, [this](bool v) {
        pipeline_->setPreviewEnabled(v);
        if (v)
            window_->setCameras(listCameras(), camera_);
    });
    connect(window_.get(), &SettingsWindow::previewWidthChanged, pl, &Pipeline::setPreviewWidth);
    connect(window_.get(), &SettingsWindow::paramsChanged, this, &TrayApp::applyParams);
    connect(window_.get(), &SettingsWindow::presetSelected, this, &TrayApp::applyPreset);
    connect(window_.get(), &SettingsWindow::saveRequested, this, [this](const QString& name) {
        presets_.save(name, params_);
        preset_ = name;
        window_->setPresets(presets_.names(), preset_);
        rebuildPresetMenu();
        saveState();
    });
    connect(window_.get(), &SettingsWindow::deleteRequested, this, [this](const QString& name) {
        presets_.remove(name);
        if (preset_ == name)
            preset_.clear();
        window_->setPresets(presets_.names(), preset_);
        rebuildPresetMenu();
        saveState();
    });
    connect(window_.get(), &SettingsWindow::cameraSelected, this, [this](const QString& dev) {
        camera_ = dev;
        pipeline_->setCamera(dev);
        saveState();
    });

    // Tray menu
    menu_ = std::make_unique<QMenu>();
    presetMenu_ = menu_->addMenu(tr("Preset"));
    rebuildPresetMenu();
    bypassAction_ = menu_->addAction(tr("Bypass effects"));
    bypassAction_->setCheckable(true);
    bypassAction_->setChecked(bypass_);
    connect(bypassAction_, &QAction::toggled, this, &TrayApp::setBypass);
    menu_->addSeparator();
    connect(menu_->addAction(tr("Settings…")), &QAction::triggered, this, &TrayApp::showSettings);
    connect(menu_->addAction(tr("Quit")), &QAction::triggered, qApp, &QApplication::quit);

    tray_.setContextMenu(menu_.get());
    connect(&tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
        if (r != QSystemTrayIcon::Trigger)
            return;
        if (window_->isVisible())
            window_->hide();
        else
            showSettings();
    });
    updateTrayIcon();
    tray_.show();

    saveTimer_.setSingleShot(true);
    saveTimer_.setInterval(500);
    connect(&saveTimer_, &QTimer::timeout, this, &TrayApp::saveState);

    if (opts_.showSettings)
        showSettings();
}

void TrayApp::showSettings()
{
    window_->showNormal();
    window_->raise();
    window_->activateWindow();
}

TrayApp::~TrayApp()
{
    saveState();
    monitor_.reset();
    pipeline_.reset();
}

void TrayApp::applyPreset(const QString& name)
{
    const auto p = presets_.load(name);
    if (!p)
        return;
    preset_ = name;
    params_ = *p;
    pipeline_->setParams(params_);
    window_->setParams(params_);
    window_->setPresets(presets_.names(), preset_);
    for (QAction* a : presetGroup_->actions())
        a->setChecked(a->text() == name);
    saveTimer_.start();
}

void TrayApp::applyParams(const Params& p)
{
    params_ = p;
    pipeline_->setParams(params_);
    saveTimer_.start();
}

void TrayApp::setBypass(bool on)
{
    bypass_ = on;
    pipeline_->setBypass(on);
    updateTrayIcon();
    saveTimer_.start();
}

void TrayApp::rebuildPresetMenu()
{
    presetMenu_->clear();
    delete presetGroup_;
    presetGroup_ = new QActionGroup(this);
    presetGroup_->setExclusive(true);
    for (const QString& n : presets_.names()) {
        QAction* a = presetMenu_->addAction(n);
        a->setCheckable(true);
        a->setChecked(n == preset_);
        presetGroup_->addAction(a);
        connect(a, &QAction::triggered, this, [this, n] { applyPreset(n); });
    }
}

void TrayApp::updateTrayIcon()
{
    const int state = !lastStats_.active ? Idle : bypass_ ? Bypass : Live;
    if (state != iconState_) {
        iconState_ = state;
        tray_.setIcon(makeIcon(state));
    }
    QString tip = QStringLiteral("CamFX – %1").arg(preset_.isEmpty() ? tr("custom") : preset_);
    if (lastStats_.active)
        tip += QStringLiteral("\nLive, %1 ms latency").arg(lastStats_.totalMs, 0, 'f', 0);
    else
        tip += tr("\nIdle");
    if (!lastStats_.status.isEmpty())
        tip += "\n" + lastStats_.status;
    tray_.setToolTip(tip);
}

QString TrayApp::statePath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/state.json";
}

void TrayApp::loadState()
{
    QFile f(statePath());
    QJsonObject o;
    if (f.open(QIODevice::ReadOnly))
        o = QJsonDocument::fromJson(f.readAll()).object();
    preset_ = o.value("preset").toString();
    camera_ = o.value("camera").toString();
    bypass_ = o.value("bypass").toBool(false);
    if (o.contains("params")) {
        params_ = Params::fromJson(o.value("params").toObject());
    } else {
        if (preset_.isEmpty())
            preset_ = QStringLiteral("Natural");
        if (const auto p = presets_.load(preset_))
            params_ = *p;
    }
}

void TrayApp::saveState()
{
    QSaveFile f(statePath());
    if (!f.open(QIODevice::WriteOnly))
        return;
    const QJsonObject o{{"preset", preset_}, {"camera", camera_}, {"bypass", bypass_}, {"params", params_.toJson()}};
    f.write(QJsonDocument(o).toJson());
    f.commit();
}
