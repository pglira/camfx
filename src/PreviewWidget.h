#pragma once

#include <QImage>
#include <QString>
#include <QWidget>

// Shows a video frame scaled to the widget size with the aspect ratio kept,
// or a text message when no frame is available.
class PreviewWidget : public QWidget {
    Q_OBJECT
public:
    explicit PreviewWidget(QWidget* parent = nullptr);

    void setFrame(const QImage& img);
    void setMessage(const QString& text);

    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return w * 9 / 16; }
    QSize sizeHint() const override { return {400, 225}; }
    QSize minimumSizeHint() const override { return {240, 135}; }

signals:
    // Width in device pixels at which frames should be delivered
    void neededWidthChanged(int width);

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    QImage frame_;
    QString message_;
};
