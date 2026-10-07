#pragma once

#include "Cameras.h"
#include "Params.h"
#include "Pipeline.h"

#include <QWidget>
#include <functional>
#include <vector>

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class PreviewWidget;

class SettingsWindow : public QWidget {
    Q_OBJECT
public:
    explicit SettingsWindow(QWidget* parent = nullptr);

    // Update the widgets without emitting paramsChanged
    void setParams(const Params& p);
    void setPresets(const QStringList& names, const QString& current);
    void setCameras(const QList<CameraInfo>& cams, const QString& current);
    QString currentPreset() const;

public slots:
    void showPreview(const QImage& img);
    void showStats(const PipelineStats& s);

signals:
    void paramsChanged(const Params& p);
    void presetSelected(const QString& name);
    void saveRequested(const QString& name);
    void deleteRequested(const QString& name);
    void cameraSelected(const QString& device);
    void visibilityChanged(bool visible);
    void previewWidthChanged(int width);

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;

private:
    struct SliderBinding {
        QSlider* slider;
        QLabel* value;
        double min, max;
        double Params::*field;
    };

    QSlider* addSlider(class QFormLayout* form, const QString& label, double min, double max,
                       double Params::*field);
    void reloadLooks();
    void emitParams();
    void updateBackgroundWidgets();

    Params params_;
    bool updating_ = false;
    std::vector<SliderBinding> sliders_;

    PreviewWidget* preview_;
    QLabel* stats_;
    QComboBox* presetCombo_;
    QGroupBox* toneBox_;
    QGroupBox* skinBox_;
    QGroupBox* bgBox_;
    QGroupBox* lookBox_;
    QComboBox* bgMode_;
    QLineEdit* bgImage_;
    QPushButton* bgImageButton_;
    QPushButton* bgColor_;
    QComboBox* lookCombo_;
    QComboBox* cameraCombo_;
    QCheckBox* mirror_;
};
