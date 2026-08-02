/*
 * This file is part of Krita
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_DEPTH_BLUR_FILTER_H
#define KIS_DEPTH_BLUR_FILTER_H

#include "filter/kis_filter.h"

#include <QPolygonF>

class KisDepthBlurFilter : public KisFilter
{
public:
    KisDepthBlurFilter();

    void processImpl(KisPaintDeviceSP device,
                     const QRect& applyRect,
                     const KisFilterConfigurationSP config,
                     KoUpdater* progressUpdater
                     ) const override;

    static inline KoID id() {
        return KoID("depth blur", i18n("Depth Blur"));
    }

    KisFilterConfigurationSP defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const override;

    QRect neededRect(const QRect & rect, const KisFilterConfigurationSP config, int lod) const override;
    QRect changedRect(const QRect & rect, const KisFilterConfigurationSP config, int lod) const override;

    KisConfigWidget * createConfigurationWidget(QWidget* parent, const KisPaintDeviceSP dev, bool useForMasks) const override;

private:
    // radius is expected to already be LoD-scaled by the caller
    static QPolygonF irisPolygon(const KisFilterConfigurationSP config, qreal radius);
    static QSize kernelHalfSize(const KisFilterConfigurationSP config, int lod);
};

#endif // KIS_DEPTH_BLUR_FILTER_H
