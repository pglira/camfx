#include "PreviewWidget.h"

#include <QPainter>
#include <QResizeEvent>
#include <cmath>

PreviewWidget::PreviewWidget(QWidget* parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void PreviewWidget::setFrame(const QImage& img)
{
    frame_ = img;
    message_.clear();
    update();
}

void PreviewWidget::setMessage(const QString& text)
{
    frame_ = QImage();
    message_ = text;
    update();
}

void PreviewWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(24, 24, 24));
    if (frame_.isNull()) {
        p.setPen(QColor(136, 136, 136));
        p.drawText(rect(), Qt::AlignCenter, message_);
        return;
    }
    const QSize s = frame_.size().scaled(size(), Qt::KeepAspectRatio);
    const QRect target(QPoint((width() - s.width()) / 2, (height() - s.height()) / 2), s);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(target, frame_);
}

void PreviewWidget::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    // Width of the drawn image: limited by the height for wide widgets
    const int w = std::min(width(), height() * 16 / 9);
    emit neededWidthChanged(int(std::lround(w * devicePixelRatioF())));
}
