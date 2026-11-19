#ifndef IMAGEVIEW_H
#define IMAGEVIEW_H

#include "litewindow.h"

#include <QImage>
#include <QPointF>
#include <QVector>
#include <QString>
#include <QWidget>

class QVBoxLayout;

/**
 * @brief The image canvas: zoom (wheel), pan (drag), click to advance.
 *
 * Moved out of tag_quick_editor unchanged; it knows nothing about the dataset,
 * the caption list or the program it runs in.
 */
class ImageViewCanvas : public QWidget {
    Q_OBJECT
public:
    explicit ImageViewCanvas(QWidget *parent = nullptr);

    void setImageList(const QVector<QString> &list);
    void setCurrentIndex(int idx);

    QString currentImagePath() const;
    void next();
    void prev();

signals:
    void nextImageRequested();
    void prevImageRequested();
    void requestDeleteCurrent();

protected:
    void paintEvent(QPaintEvent *) override;
    void wheelEvent(QWheelEvent *ev) override;
    void mousePressEvent(QMouseEvent *ev) override;
    void mouseMoveEvent(QMouseEvent *ev) override;
    void mouseReleaseEvent(QMouseEvent *ev) override;
    void resizeEvent(QResizeEvent *ev) override;
    void keyPressEvent(QKeyEvent *ev) override;

private:
    void loadImageAt(int idx);
    void adjustToFit();
    void ensureBounds();

private:
    QVector<QString> m_images;
    int m_curIndex = -1;

    QImage m_image;

    double m_scale = 1.0;
    QPointF m_offset {0.0, 0.0};   // 视口左上角对应的图像坐标
    QPoint  m_lastMouse;
    QPoint  m_mousePressPos;
    bool    m_dragging = false;
    int     m_clickThreshold = 4;
};

/**
 * @brief The preview window: a LiteWindow holding one ImageViewCanvas.
 *
 * The window look is the host's business (tqe applies its sheet in
 * teImageWidget), so this class stays usable from gallery_info as well.
 */
class ImageView : public LiteWindow {
    Q_OBJECT
public:
    explicit ImageView(QWidget *parent = nullptr);

    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void setImage(const QString &path);
signals:
    void nextImage();
    void prevImage();

private:
    QVBoxLayout *content_layout = nullptr;
    ImageViewCanvas *imgView = nullptr;
};

#endif // IMAGEVIEW_H
