/*
 * This file is part of Krita
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_depth_blur_filter.h"
#include "kis_wdg_depth_blur.h"

#include <KoColorSpace.h>
#include <KoMixColorsOp.h>

#include <kis_convolution_kernel.h>
#include <kis_convolution_painter.h>
#include <kis_lod_transform.h>
#include <kis_painter.h>
#include <kis_paint_device.h>
#include <kis_processing_information.h>
#include <kis_selection.h>
#include <kis_sequential_iterator.h>

#include <filter/kis_filter_category_ids.h>
#include <filter/kis_filter_configuration.h>

#include <QColor>
#include <QImage>
#include <QPainter>
#include <QTransform>
#include <QVector>

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace
{
// How many discrete blur radii we pre-compute and interpolate between. Higher
// values track the depth map more faithfully at the cost of one extra full-image
// convolution pass each. See the "Blur algorithm" section of the design notes.
const int BucketCount = 8;
}

KisDepthBlurFilter::KisDepthBlurFilter() : KisFilter(id(), FiltersCategoryBlurId, i18n("&Depth Blur..."))
{
    setSupportsPainting(false);
    setSupportsAdjustmentLayers(false);
    setSupportsLevelOfDetail(true);
    setColorSpaceIndependence(FULLY_INDEPENDENT);
}

KisConfigWidget * KisDepthBlurFilter::createConfigurationWidget(QWidget* parent, const KisPaintDeviceSP dev, bool) const
{
    return new KisWdgDepthBlur(parent, dev);
}

KisFilterConfigurationSP KisDepthBlurFilter::defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const
{
    KisFilterConfigurationSP config = factoryConfiguration(resourcesInterface);
    config->setProperty("irisShape", "Hexagon (6)");
    config->setProperty("irisRadius", 10);
    config->setProperty("irisRotation", 0);
    config->setProperty("focalDistance", 128);
    config->setProperty("invert", false);
    config->setProperty("depthMapUuid", QString());
    config->setProperty("depthMapName", QString());
    config->setProperty("depthMapGreyData", QByteArray());
    config->setProperty("depthMapWidth", 0);
    config->setProperty("depthMapHeight", 0);
    config->setProperty("depthMapOriginX", 0);
    config->setProperty("depthMapOriginY", 0);
    return config;
}

QPolygonF KisDepthBlurFilter::irisPolygon(const KisFilterConfigurationSP config, qreal radius)
{
    KIS_ASSERT_RECOVER(config) { return QPolygonF(); }

    if (radius < 1.0) return QPolygonF();

    QVariant value;
    config->getProperty("irisShape", value);
    const QString irisShape = value.toString();
    config->getProperty("irisRotation", value);
    const qreal irisRotation = value.toReal();

    int sides = 0;
    if (irisShape == "Triangle") sides = 3;
    else if (irisShape == "Quadrilateral (4)") sides = 4;
    else if (irisShape == "Pentagon (5)") sides = 5;
    else if (irisShape == "Hexagon (6)") sides = 6;
    else if (irisShape == "Heptagon (7)") sides = 7;
    else if (irisShape == "Octagon (8)") sides = 8;
    else return QPolygonF();

    QPolygonF irisShapePoly;
    qreal angle = 0;
    for (int i = 0; i < sides; ++i) {
        irisShapePoly << QPointF(0.5 * cos(angle), 0.5 * sin(angle));
        angle += 2 * M_PI / sides;
    }

    QTransform transform;
    transform.rotate(irisRotation);
    transform.scale(radius * 2, radius * 2);

    return transform.map(irisShapePoly);
}

QSize KisDepthBlurFilter::kernelHalfSize(const KisFilterConfigurationSP config, int lod)
{
    KIS_ASSERT_RECOVER(config) { return QSize(0, 0); }

    QVariant value;
    config->getProperty("irisRadius", value);

    KisLodTransformScalar t(lod);
    const qreal radius = t.scale(qreal(value.toInt()));

    QPolygonF iris = irisPolygon(config, radius);
    if (iris.isEmpty()) return QSize(0, 0);

    QRect rect = iris.boundingRect().toAlignedRect();
    int w = std::ceil(qreal(rect.width()) / 2.0);
    int h = std::ceil(qreal(rect.height()) / 2.0);

    return QSize(w, h);
}

QRect KisDepthBlurFilter::neededRect(const QRect & rect, const KisFilterConfigurationSP config, int lod) const
{
    QSize halfSize = kernelHalfSize(config, lod);
    return rect.adjusted(-halfSize.width() * 2, -halfSize.height() * 2, halfSize.width() * 2, halfSize.height() * 2);
}

QRect KisDepthBlurFilter::changedRect(const QRect & rect, const KisFilterConfigurationSP config, int lod) const
{
    QSize halfSize = kernelHalfSize(config, lod);
    return rect.adjusted(-halfSize.width(), -halfSize.height(), halfSize.width(), halfSize.height());
}

void KisDepthBlurFilter::processImpl(KisPaintDeviceSP device,
                                     const QRect& applyRect,
                                     const KisFilterConfigurationSP config,
                                     KoUpdater* progressUpdater
                                     ) const
{
    Q_ASSERT(device);
    KIS_SAFE_ASSERT_RECOVER_RETURN(config);

    if (applyRect.isEmpty()) return;

    QVariant value;

    config->getProperty("irisRadius", value);
    const int irisRadius = qMax(0, value.toInt());
    if (irisRadius < 1) {
        // No blur requested: leave the layer untouched.
        return;
    }

    config->getProperty("depthMapWidth", value);
    const int depthWidth = value.toInt();
    config->getProperty("depthMapHeight", value);
    const int depthHeight = value.toInt();
    config->getProperty("depthMapGreyData", value);
    const QByteArray depthData = value.toByteArray();

    if (depthWidth <= 0 || depthHeight <= 0 || depthData.size() < depthWidth * depthHeight) {
        // No depth map has been picked (or it hasn't been baked yet): nothing to do.
        return;
    }

    config->getProperty("depthMapOriginX", value);
    const int depthOriginX = value.toInt();
    config->getProperty("depthMapOriginY", value);
    const int depthOriginY = value.toInt();

    config->getProperty("focalDistance", value);
    const int focalDistance = qBound(0, value.toInt(), 255);

    config->getProperty("invert", value);
    const bool invert = value.toBool();

    const int lod = device->defaultBounds()->currentLevelOfDetail();
    KisLodTransformScalar t(lod);

    QBitArray channelFlags = config->channelFlags();
    if (channelFlags.isEmpty()) {
        channelFlags = QBitArray(device->colorSpace()->channelCount(), true);
    }

    // Bucket 0 always means "no blur" (the pristine source), so we only need to
    // convolve for buckets 1..BucketCount-1. `device` itself is never written to
    // until the very last step, so it is safe to keep reading from it here.
    QVector<KisPaintDeviceSP> buckets(BucketCount);
    buckets[0] = device;

    for (int i = 1; i < BucketCount; ++i) {
        const qreal bucketRadius = t.scale(qreal(irisRadius) * qreal(i) / qreal(BucketCount - 1));
        QPolygonF polygon = irisPolygon(config, bucketRadius);

        QRect kernelBoundingRect = polygon.boundingRect().toAlignedRect();
        if (polygon.isEmpty() || kernelBoundingRect.width() < 1 || kernelBoundingRect.height() < 1) {
            buckets[i] = device;
            continue;
        }

        const int kernelWidth = kernelBoundingRect.width();
        const int kernelHeight = kernelBoundingRect.height();

        QImage kernelRepresentation(kernelWidth, kernelHeight, QImage::Format_RGB32);
        kernelRepresentation.fill(0);

        QPainter imagePainter(&kernelRepresentation);
        imagePainter.setRenderHint(QPainter::Antialiasing);
        imagePainter.setBrush(QColor::fromRgb(255, 255, 255));

        QTransform offsetTransform;
        offsetTransform.translate(-kernelBoundingRect.x(), -kernelBoundingRect.y());
        imagePainter.setTransform(offsetTransform);
        imagePainter.drawPolygon(polygon, Qt::WindingFill);
        imagePainter.end();

        Eigen::Matrix<qreal, Eigen::Dynamic, Eigen::Dynamic> irisKernel(kernelHeight, kernelWidth);
        for (int row = 0; row < kernelHeight; ++row) {
            for (int col = 0; col < kernelWidth; ++col) {
                irisKernel(row, col) = qRed(kernelRepresentation.pixel(col, row));
            }
        }

        KisPaintDeviceSP bucketDevice = new KisPaintDevice(device->colorSpace());
        KisConvolutionPainter convPainter(bucketDevice);
        convPainter.setChannelFlags(channelFlags);

        KisConvolutionKernelSP kernel = KisConvolutionKernel::fromMatrix(irisKernel, 0, irisKernel.sum());
        convPainter.applyMatrix(kernel, device, applyRect.topLeft(), applyRect.topLeft(), applyRect.size(), BORDER_REPEAT);

        buckets[i] = bucketDevice;
    }

    const KoColorSpace *cs = device->colorSpace();
    KoMixColorsOp *mixOp = cs->mixColorsOp();
    const int pixelSize = cs->pixelSize();

    KisPaintDeviceSP result = new KisPaintDevice(cs);

    std::vector<std::unique_ptr<KisSequentialConstIterator>> bucketIterators;
    bucketIterators.reserve(BucketCount);
    for (int i = 0; i < BucketCount; ++i) {
        bucketIterators.push_back(std::make_unique<KisSequentialConstIterator>(buckets[i], applyRect));
    }

    KisSequentialIterator dstIt(result, applyRect);

    while (dstIt.nextPixel()) {
        for (int i = 0; i < BucketCount; ++i) {
            bucketIterators[i]->nextPixel();
        }

        const int sampleX = dstIt.x() - depthOriginX;
        const int sampleY = dstIt.y() - depthOriginY;

        int depthValue = 0;
        if (sampleX >= 0 && sampleX < depthWidth && sampleY >= 0 && sampleY < depthHeight) {
            depthValue = quint8(depthData.at(sampleY * depthWidth + sampleX));
        }
        if (invert) depthValue = 255 - depthValue;

        const int distance = qAbs(depthValue - focalDistance);
        const int maxDistance = qMax(focalDistance, 255 - focalDistance);
        const qreal normalized = maxDistance > 0
            ? qBound(qreal(0.0), qreal(distance) / qreal(maxDistance), qreal(1.0))
            : qreal(0.0);
        const qreal position = normalized * (BucketCount - 1);

        const int lowerBucket = qBound(0, int(position), BucketCount - 1);
        const int upperBucket = qMin(lowerBucket + 1, BucketCount - 1);
        const qreal frac = position - lowerBucket;

        if (lowerBucket == upperBucket || frac <= 0.0) {
            memcpy(dstIt.rawData(), bucketIterators[lowerBucket]->rawDataConst(), pixelSize);
        } else {
            const quint8 *colors[2] = { bucketIterators[lowerBucket]->rawDataConst(), bucketIterators[upperBucket]->rawDataConst() };
            const qint16 weightB = qint16(qRound(frac * 255));
            const qint16 weightA = qint16(255 - weightB);
            const qint16 weights[2] = { weightA, weightB };
            mixOp->mixColors(colors, weights, 2, dstIt.rawData());
        }
    }

    KisPainter finalPainter(device);
    finalPainter.setChannelFlags(channelFlags);
    finalPainter.bitBlt(applyRect.topLeft(), result, applyRect);

    if (progressUpdater) {
        progressUpdater->setProgress(100);
    }
}
