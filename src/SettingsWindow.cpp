#include "SettingsWindow.h"

#include "Effects.h"
#include "Lut3D.h"
#include "PreviewWidget.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSplitter>
#include <QVBoxLayout>

namespace {

constexpr int kSliderSteps = 1000;

void setButtonColor(QPushButton* b, const QColor& c)
{
    b->setStyleSheet(QStringLiteral("background-color: %1; border: 1px solid palette(mid); min-height: 20px;")
                         .arg(c.name()));
}

}  // namespace

SettingsWindow::SettingsWindow(QWidget* parent) : QWidget(parent)
{
    setWindowTitle(tr("CamFX"));

    // The preview and the controls share the window height through a splitter;
    // extra window space goes to the preview
    auto* top = new QVBoxLayout(this);
    top->setContentsMargins(8, 8, 8, 8);
    auto* splitter = new QSplitter(Qt::Vertical);
    splitter->setChildrenCollapsible(false);
    top->addWidget(splitter);

    preview_ = new PreviewWidget;
    preview_->setMessage(tr("Starting camera…"));
    connect(preview_, &PreviewWidget::neededWidthChanged, this, &SettingsWindow::previewWidthChanged);
    splitter->addWidget(preview_);

    auto* controls = new QWidget;
    auto* outer = new QVBoxLayout(controls);
    outer->setContentsMargins(0, 4, 0, 0);
    splitter->addWidget(controls);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);

    stats_ = new QLabel;
    stats_->setWordWrap(true);
    QFont small = stats_->font();
    small.setPointSizeF(small.pointSizeF() * 0.85);
    stats_->setFont(small);
    stats_->setTextFormat(Qt::RichText);
    outer->addWidget(stats_);

    // Preset bar
    auto* presetRow = new QHBoxLayout;
    presetCombo_ = new QComboBox;
    presetCombo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* saveBtn = new QPushButton(tr("Save"));
    auto* saveAsBtn = new QPushButton(tr("Save as…"));
    auto* delBtn = new QPushButton(tr("Delete"));
    presetRow->addWidget(presetCombo_);
    presetRow->addWidget(saveBtn);
    presetRow->addWidget(saveAsBtn);
    presetRow->addWidget(delBtn);
    outer->addLayout(presetRow);

    connect(presetCombo_, &QComboBox::currentTextChanged, this, [this](const QString& name) {
        if (!updating_ && !name.isEmpty())
            emit presetSelected(name);
    });
    connect(saveBtn, &QPushButton::clicked, this, [this] {
        if (!presetCombo_->currentText().isEmpty())
            emit saveRequested(presetCombo_->currentText());
    });
    connect(saveAsBtn, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Save preset"), tr("Name:"), QLineEdit::Normal,
                                                   QString(), &ok).trimmed();
        if (ok && !name.isEmpty() && !name.contains('/'))
            emit saveRequested(name);
    });
    connect(delBtn, &QPushButton::clicked, this, [this] {
        const QString name = presetCombo_->currentText();
        if (!name.isEmpty() &&
            QMessageBox::question(this, tr("Delete preset"), tr("Delete preset \"%1\"?").arg(name)) ==
                QMessageBox::Yes)
            emit deleteRequested(name);
    });

    // Effect groups in a scroll area
    auto* content = new QWidget;
    auto* col = new QVBoxLayout(content);
    col->setContentsMargins(0, 0, 0, 0);

    auto makeBox = [&](const QString& title, bool Params::*field) {
        auto* box = new QGroupBox(title);
        box->setCheckable(true);
        connect(box, &QGroupBox::toggled, this, [this, field](bool on) {
            params_.*field = on;
            emitParams();
        });
        col->addWidget(box);
        return box;
    };

    toneBox_ = makeBox(tr("Tone"), &Params::toneEnabled);
    auto* toneForm = new QFormLayout(toneBox_);
    addSlider(toneForm, tr("Brightness"), -1, 1, &Params::brightness);
    addSlider(toneForm, tr("Contrast"), -1, 1, &Params::contrast);
    addSlider(toneForm, tr("Saturation"), 0, 2, &Params::saturation);
    addSlider(toneForm, tr("Warmth"), -1, 1, &Params::warmth);
    addSlider(toneForm, tr("Gamma"), 0.5, 2, &Params::gamma);
    addSlider(toneForm, tr("Sharpen"), 0, 1, &Params::sharpen);

    skinBox_ = makeBox(tr("Skin"), &Params::skinEnabled);
    auto* skinForm = new QFormLayout(skinBox_);
    addSlider(skinForm, tr("Smoothing"), 0, 1, &Params::smoothing);
    addSlider(skinForm, tr("Keep detail"), 0, 1, &Params::detail);
    addSlider(skinForm, tr("Brighten"), 0, 1, &Params::skinBrighten);

    bgBox_ = makeBox(tr("Background"), &Params::backgroundEnabled);
    auto* bgForm = new QFormLayout(bgBox_);
    bgMode_ = new QComboBox;
    bgMode_->addItem(tr("Blur"), int(BackgroundMode::Blur));
    bgMode_->addItem(tr("Image"), int(BackgroundMode::Image));
    bgMode_->addItem(tr("Color"), int(BackgroundMode::Color));
    bgForm->addRow(tr("Mode"), bgMode_);
    connect(bgMode_, &QComboBox::currentIndexChanged, this, [this] {
        params_.backgroundMode = BackgroundMode(bgMode_->currentData().toInt());
        updateBackgroundWidgets();
        emitParams();
    });
    addSlider(bgForm, tr("Blur"), 0, 1, &Params::blurStrength);
    auto* imgRow = new QHBoxLayout;
    bgImage_ = new QLineEdit;
    bgImage_->setReadOnly(true);
    bgImageButton_ = new QPushButton(tr("…"));
    bgImageButton_->setFixedWidth(32);
    imgRow->addWidget(bgImage_);
    imgRow->addWidget(bgImageButton_);
    bgForm->addRow(tr("Image"), imgRow);
    connect(bgImageButton_, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Background image"),
                                                       QFileInfo(params_.backgroundImage).absolutePath(),
                                                       tr("Images (*.png *.jpg *.jpeg *.webp *.bmp)"));
        if (f.isEmpty())
            return;
        params_.backgroundImage = f;
        bgImage_->setText(QFileInfo(f).fileName());
        emitParams();
    });
    bgColor_ = new QPushButton;
    bgForm->addRow(tr("Color"), bgColor_);
    connect(bgColor_, &QPushButton::clicked, this, [this] {
        const QColor c = QColorDialog::getColor(params_.backgroundColor, this, tr("Background color"));
        if (!c.isValid())
            return;
        params_.backgroundColor = c;
        setButtonColor(bgColor_, c);
        emitParams();
    });
    addSlider(bgForm, tr("Edge softness"), 0, 1, &Params::edgeSoftness);

    lookBox_ = makeBox(tr("Look"), &Params::lookEnabled);
    auto* lookForm = new QFormLayout(lookBox_);
    lookCombo_ = new QComboBox;
    lookForm->addRow(tr("LUT"), lookCombo_);
    connect(lookCombo_, &QComboBox::currentIndexChanged, this, [this] {
        if (lookCombo_->currentIndex() >= 0) {
            params_.look = lookCombo_->currentData().toString();
            emitParams();
        }
    });
    addSlider(lookForm, tr("Intensity"), 0, 1, &Params::lookIntensity);
    reloadLooks();

    // Camera
    auto* camRow = new QHBoxLayout;
    cameraCombo_ = new QComboBox;
    cameraCombo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    mirror_ = new QCheckBox(tr("Mirror"));
    camRow->addWidget(new QLabel(tr("Camera")));
    camRow->addWidget(cameraCombo_);
    camRow->addWidget(mirror_);
    col->addLayout(camRow);
    col->addStretch();
    connect(cameraCombo_, &QComboBox::currentIndexChanged, this, [this] {
        if (!updating_ && cameraCombo_->currentIndex() >= 0)
            emit cameraSelected(cameraCombo_->currentData().toString());
    });
    connect(mirror_, &QCheckBox::toggled, this, [this](bool on) {
        params_.mirror = on;
        emitParams();
    });

    auto* scroll = new QScrollArea;
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    outer->addWidget(scroll, 1);

    setParams(params_);
    resize(420, 900);
    splitter->setSizes({236, 650});
}

QSlider* SettingsWindow::addSlider(QFormLayout* form, const QString& label, double min, double max,
                                   double Params::*field)
{
    auto* row = new QHBoxLayout;
    auto* s = new QSlider(Qt::Horizontal);
    s->setRange(0, kSliderSteps);
    auto* v = new QLabel;
    v->setFixedWidth(36);
    v->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row->addWidget(s);
    row->addWidget(v);
    form->addRow(label, row);

    const size_t idx = sliders_.size();
    sliders_.push_back({s, v, min, max, field});
    connect(s, &QSlider::valueChanged, this, [this, idx](int pos) {
        const SliderBinding& b = sliders_[idx];
        const double val = b.min + (b.max - b.min) * pos / kSliderSteps;
        b.value->setText(QString::number(val, 'f', 2));
        if (updating_)
            return;
        params_.*(b.field) = val;
        emitParams();
    });
    return s;
}

void SettingsWindow::reloadLooks()
{
    const bool was = updating_;
    updating_ = true;
    lookCombo_->clear();
    for (const QString& n : Lut3D::builtinNames())
        lookCombo_->addItem(n, n);
    for (const QString& dir : Effects::lutDirectories()) {
        for (const QFileInfo& f : QDir(dir).entryInfoList({"*.cube", "*.CUBE"}, QDir::Files, QDir::Name))
            lookCombo_->addItem(f.completeBaseName(), f.absoluteFilePath());
    }
    updating_ = was;
}

void SettingsWindow::setParams(const Params& p)
{
    updating_ = true;
    params_ = p;
    for (const SliderBinding& b : sliders_) {
        const double t = (p.*(b.field) - b.min) / (b.max - b.min);
        b.slider->setValue(int(std::lround(std::clamp(t, 0.0, 1.0) * kSliderSteps)));
        b.value->setText(QString::number(p.*(b.field), 'f', 2));
    }
    toneBox_->setChecked(p.toneEnabled);
    skinBox_->setChecked(p.skinEnabled);
    bgBox_->setChecked(p.backgroundEnabled);
    lookBox_->setChecked(p.lookEnabled);
    bgMode_->setCurrentIndex(bgMode_->findData(int(p.backgroundMode)));
    bgImage_->setText(QFileInfo(p.backgroundImage).fileName());
    setButtonColor(bgColor_, p.backgroundColor);
    int li = lookCombo_->findData(p.look);
    if (li < 0 && !p.look.isEmpty()) {
        lookCombo_->addItem(QFileInfo(p.look).completeBaseName(), p.look);
        li = lookCombo_->count() - 1;
    }
    lookCombo_->setCurrentIndex(li);
    mirror_->setChecked(p.mirror);
    updateBackgroundWidgets();
    updating_ = false;
}

void SettingsWindow::setPresets(const QStringList& names, const QString& current)
{
    updating_ = true;
    presetCombo_->clear();
    presetCombo_->addItems(names);
    presetCombo_->setCurrentIndex(names.indexOf(current));
    updating_ = false;
}

QString SettingsWindow::currentPreset() const
{
    return presetCombo_->currentText();
}

void SettingsWindow::setCameras(const QList<CameraInfo>& cams, const QString& current)
{
    updating_ = true;
    cameraCombo_->clear();
    for (const CameraInfo& c : cams)
        cameraCombo_->addItem(QStringLiteral("%1 (%2)").arg(c.name, c.device), c.device);
    cameraCombo_->setCurrentIndex(cameraCombo_->findData(current));
    updating_ = false;
}

void SettingsWindow::updateBackgroundWidgets()
{
    const auto mode = params_.backgroundMode;
    for (const SliderBinding& b : sliders_)
        if (b.field == &Params::blurStrength)
            b.slider->setEnabled(mode == BackgroundMode::Blur);
    bgImage_->setEnabled(mode == BackgroundMode::Image);
    bgImageButton_->setEnabled(mode == BackgroundMode::Image);
    bgColor_->setEnabled(mode == BackgroundMode::Color);
}

void SettingsWindow::emitParams()
{
    if (!updating_)
        emit paramsChanged(params_);
}

void SettingsWindow::showPreview(const QImage& img)
{
    if (!isVisible())
        return;
    preview_->setFrame(img);
}

void SettingsWindow::showStats(const PipelineStats& s)
{
    QStringList lines;
    QString state;
    if (s.active)
        state = QStringLiteral("<b style='color:#3a3'>● Live</b> · %1 fps").arg(s.fps, 0, 'f', 1);
    else
        state = QStringLiteral("<b style='color:#888'>● Idle</b>");
    state += s.outputOk ? QStringLiteral(" · virtual camera: %1 app(s)").arg(s.consumers)
                        : QStringLiteral(" · <span style='color:#c33'>no virtual camera</span>");
    lines << state;
    if (s.active) {
        lines << QStringLiteral("Latency <b>%1 ms</b> = capture %2 + mask wait %3 + effects %4 + output %5")
                     .arg(s.totalMs, 0, 'f', 1)
                     .arg(s.captureMs, 0, 'f', 1)
                     .arg(s.segWaitMs, 0, 'f', 1)
                     .arg(s.effectsMs, 0, 'f', 1)
                     .arg(s.outputMs, 0, 'f', 1);
        if (s.inferenceMs > 0)
            lines << QStringLiteral("Segmentation %1 ms on %2").arg(s.inferenceMs, 0, 'f', 1).arg(s.gpu ? "GPU" : "CPU");
    }
    if (!s.status.isEmpty())
        lines << QStringLiteral("<span style='color:#c33'>%1</span>").arg(s.status.toHtmlEscaped());
    stats_->setText(lines.join("<br>"));
    if (!s.active && isVisible() && s.status.isEmpty())
        preview_->setMessage(tr("Starting camera…"));
}

void SettingsWindow::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    reloadLooks();
    setParams(params_);
    emit visibilityChanged(true);
}

void SettingsWindow::hideEvent(QHideEvent* e)
{
    QWidget::hideEvent(e);
    emit visibilityChanged(false);
}
