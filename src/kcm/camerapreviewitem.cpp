// SPDX-License-Identifier: GPL-3.0-or-later

#include "camerapreviewitem.h"

#include <QQuickWindow>
#include <QSGSimpleTextureNode>

CameraPreviewItem::CameraPreviewItem(QQuickItem *parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
}

CameraPreviewSession *CameraPreviewItem::session() const
{
    return m_session;
}

void CameraPreviewItem::setSession(CameraPreviewSession *session)
{
    if (m_session == session)
        return;
    if (m_session)
        disconnect(m_session, nullptr, this, nullptr);
    m_session = session;
    if (m_session)
    {
        connect(m_session, &CameraPreviewSession::frameChanged, this, [this]() { update(); });
        connect(m_session, &QObject::destroyed, this,
                [this]()
                {
                    m_session = nullptr;
                    update();
                    Q_EMIT sessionChanged();
                });
    }
    update();
    Q_EMIT sessionChanged();
}

bool CameraPreviewItem::mirrored() const
{
    return m_mirrored;
}

void CameraPreviewItem::setMirrored(bool mirrored)
{
    if (m_mirrored == mirrored)
        return;
    m_mirrored = mirrored;
    update();
    Q_EMIT mirroredChanged();
}

QSGNode *CameraPreviewItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    if (width() <= 0.0 || height() <= 0.0 || !m_session || !window())
    {
        delete oldNode;
        return nullptr;
    }
    const QImage frame = m_session->frame();
    if (frame.isNull())
    {
        delete oldNode;
        return nullptr;
    }

    auto *node = static_cast<QSGSimpleTextureNode *>(oldNode);
    if (!node)
    {
        node = new QSGSimpleTextureNode();
        node->setOwnsTexture(true);
    }

    const QSizeF scaled = frame.size().scaled(boundingRect().size().toSize(), Qt::KeepAspectRatio);
    const QRectF target((width() - scaled.width()) / 2.0, (height() - scaled.height()) / 2.0, scaled.width(),
                        scaled.height());

    QSGTexture *texture = window()->createTextureFromImage(frame);
    node->setTexture(texture);
    node->setRect(target);
    node->setFiltering(QSGTexture::Linear);
    node->setTextureCoordinatesTransform(m_mirrored ? QSGSimpleTextureNode::MirrorHorizontally
                                                    : QSGSimpleTextureNode::NoTransform);
    return node;
}
