#pragma once

#include <QLabel>
#include <QResizeEvent>
#include <QPixmap>

// A QLabel with a fixed box size locked to the 1024x768 (4:3) aspect ratio. Fixed size (rather
// than a dynamic heightForWidth hint) is used deliberately: QSplitter (used by the Show Image
// tab) does not respect heightForWidth consistently, which caused the box and the splitter to
// keep re-negotiating size on every layout pass (visible as constant resizing / a portrait box).
class AspectRatioLabel : public QLabel {
public:
    explicit AspectRatioLabel(int width = 640, QWidget *parent = nullptr) : QLabel(parent) {
        setFixedSize(width, width * 768 / 1024);
    }

    void setPixmapKeepingAspect(const QPixmap &pixmap) {
        m_original = pixmap;
        rescale();
    }

private:
    void rescale() {
        if (m_original.isNull()) return;
        setPixmap(m_original.scaled(size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }

    QPixmap m_original;
};
