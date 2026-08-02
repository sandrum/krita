/*
 * This file is part of Krita
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_WDG_DEPTH_BLUR_H
#define KIS_WDG_DEPTH_BLUR_H

#include <kis_config_widget.h>
#include <kis_types.h>

#include <QByteArray>
#include <QMap>
#include <QVector>

class Ui_WdgDepthBlur;

class KisWdgDepthBlur : public KisConfigWidget
{
    Q_OBJECT
public:
    KisWdgDepthBlur(QWidget *parent, KisPaintDeviceSP filteredDevice);
    ~KisWdgDepthBlur() override;

    void setConfiguration(const KisPropertiesConfigurationSP config) override;
    KisPropertiesConfigurationSP configuration() const override;
    void setView(KisViewManager *view) override;

private:
    void updateDepthSourceList();
    void addLayersRecursively(KisNodeSP node);
    // (Re-)renders the currently picked depth layer into m_bakedGreyData, but
    // only if the combo selection changed since the last bake - full-image
    // conversion is too slow to redo on every debounced slider tweak.
    void ensureDepthDataBaked() const;

    Ui_WdgDepthBlur *m_widget;
    KisPaintDeviceSP m_filteredDevice;
    KisViewManager *m_view;
    QVector<KisNodeSP> m_depthSources;
    QMap<QString, QString> m_shapeTranslations;

    mutable int m_bakedComboIndex;
    mutable QByteArray m_bakedGreyData;
    mutable int m_bakedWidth;
    mutable int m_bakedHeight;
    mutable int m_bakedOriginX;
    mutable int m_bakedOriginY;
};

#endif // KIS_WDG_DEPTH_BLUR_H
