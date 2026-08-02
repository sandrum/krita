/*
 * This file is part of Krita
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_wdg_depth_blur.h"
#include "ui_wdg_depth_blur.h"

#include <QColor>
#include <QImage>
#include <QLayout>
#include <QUuid>

#include <KisGlobalResourcesInterface.h>
#include <KisViewManager.h>

#include <filter/kis_filter_configuration.h>
#include <kis_image.h>
#include <kis_layer.h>
#include <kis_node.h>
#include <kis_paint_device.h>

KisWdgDepthBlur::KisWdgDepthBlur(QWidget *parent, KisPaintDeviceSP filteredDevice)
    : KisConfigWidget(parent)
    , m_filteredDevice(filteredDevice)
    , m_view(0)
    , m_bakedComboIndex(-1)
    , m_bakedWidth(0)
    , m_bakedHeight(0)
    , m_bakedOriginX(0)
    , m_bakedOriginY(0)
{
    m_widget = new Ui_WdgDepthBlur();
    m_widget->setupUi(this);

    m_widget->irisRotationSelector->setDecimals(0);
    m_widget->irisRotationSelector->setIncreasingDirection(KisAngleGauge::IncreasingDirection_Clockwise);

    m_widget->focalDistanceSlider->setRange(0, 255);
    m_widget->irisRadiusSlider->setRange(0, 200);

    m_shapeTranslations[i18n("Triangle")] = "Triangle";
    m_shapeTranslations[i18n("Quadrilateral (4)")] = "Quadrilateral (4)";
    m_shapeTranslations[i18n("Pentagon (5)")] = "Pentagon (5)";
    m_shapeTranslations[i18n("Hexagon (6)")] = "Hexagon (6)";
    m_shapeTranslations[i18n("Heptagon (7)")] = "Heptagon (7)";
    m_shapeTranslations[i18n("Octagon (8)")] = "Octagon (8)";

    connect(m_widget->depthSourceCombo, SIGNAL(currentIndexChanged(int)), SIGNAL(sigConfigurationItemChanged()));
    connect(m_widget->invertCheckBox, SIGNAL(toggled(bool)), SIGNAL(sigConfigurationItemChanged()));
    connect(m_widget->focalDistanceSlider, SIGNAL(valueChanged(int)), SIGNAL(sigConfigurationItemChanged()));
    connect(m_widget->irisShapeCombo, SIGNAL(currentIndexChanged(int)), SIGNAL(sigConfigurationItemChanged()));
    connect(m_widget->irisRadiusSlider, SIGNAL(valueChanged(int)), SIGNAL(sigConfigurationItemChanged()));
    connect(m_widget->irisRotationSelector, SIGNAL(angleChanged(qreal)), SIGNAL(sigConfigurationItemChanged()));
}

KisWdgDepthBlur::~KisWdgDepthBlur()
{
    delete m_widget;
}

void KisWdgDepthBlur::setView(KisViewManager *view)
{
    KisConfigWidget::setView(view);
    m_view = view;
    updateDepthSourceList();
}

void KisWdgDepthBlur::addLayersRecursively(KisNodeSP node)
{
    if (!node) return;

    KisLayerSP layer(qobject_cast<KisLayer *>(node.data()));
    if (layer && layer->paintDevice() != m_filteredDevice) {
        m_depthSources << node;
    }

    for (KisNodeSP child = node->firstChild(); child; child = child->nextSibling()) {
        addLayersRecursively(child);
    }
}

void KisWdgDepthBlur::updateDepthSourceList()
{
    m_depthSources.clear();
    m_widget->depthSourceCombo->clear();
    m_bakedComboIndex = -1;

    if (!m_view || !m_view->image()) return;

    addLayersRecursively(m_view->image()->root());

    Q_FOREACH (KisNodeSP node, m_depthSources) {
        m_widget->depthSourceCombo->addItem(node->name());
    }
}

void KisWdgDepthBlur::ensureDepthDataBaked() const
{
    const int index = m_widget->depthSourceCombo->currentIndex();

    if (index == m_bakedComboIndex) return;

    m_bakedComboIndex = index;
    m_bakedGreyData.clear();
    m_bakedWidth = 0;
    m_bakedHeight = 0;
    m_bakedOriginX = 0;
    m_bakedOriginY = 0;

    if (index < 0 || index >= m_depthSources.size() || !m_view || !m_view->image()) return;

    KisLayerSP layer(qobject_cast<KisLayer *>(m_depthSources.at(index).data()));
    if (!layer) return;

    KisPaintDeviceSP projection = layer->projection();
    if (!projection) return;

    const QRect bounds = m_view->image()->bounds();
    if (bounds.isEmpty()) return;

    QImage image = projection->convertToQImage(0, bounds);
    if (image.isNull()) return;

    image = image.convertToFormat(QImage::Format_ARGB32);

    m_bakedWidth = image.width();
    m_bakedHeight = image.height();
    m_bakedOriginX = bounds.x();
    m_bakedOriginY = bounds.y();
    m_bakedGreyData.resize(m_bakedWidth * m_bakedHeight);

    char *dst = m_bakedGreyData.data();
    for (int y = 0; y < m_bakedHeight; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < m_bakedWidth; ++x) {
            dst[y * m_bakedWidth + x] = char(qGray(line[x]));
        }
    }
}

KisPropertiesConfigurationSP KisWdgDepthBlur::configuration() const
{
    KisFilterConfigurationSP config = new KisFilterConfiguration("depth blur", 1, KisGlobalResourcesInterface::instance());

    config->setProperty("irisShape", m_shapeTranslations[m_widget->irisShapeCombo->currentText()]);
    config->setProperty("irisRadius", m_widget->irisRadiusSlider->value());
    config->setProperty("irisRotation", static_cast<int>(m_widget->irisRotationSelector->angle()));
    config->setProperty("focalDistance", m_widget->focalDistanceSlider->value());
    config->setProperty("invert", m_widget->invertCheckBox->isChecked());

    ensureDepthDataBaked();

    const int index = m_widget->depthSourceCombo->currentIndex();
    QString uuidStr, nameStr;
    if (index >= 0 && index < m_depthSources.size()) {
        KisNodeSP node = m_depthSources.at(index);
        uuidStr = node->uuid().toString();
        nameStr = node->name();
    }
    config->setProperty("depthMapUuid", uuidStr);
    config->setProperty("depthMapName", nameStr);
    config->setProperty("depthMapGreyData", m_bakedGreyData);
    config->setProperty("depthMapWidth", m_bakedWidth);
    config->setProperty("depthMapHeight", m_bakedHeight);
    config->setProperty("depthMapOriginX", m_bakedOriginX);
    config->setProperty("depthMapOriginY", m_bakedOriginY);

    return config;
}

void KisWdgDepthBlur::setConfiguration(const KisPropertiesConfigurationSP config)
{
    QVariant value;

    if (config->getProperty("irisShape", value)) {
        for (int i = 0; i < m_widget->irisShapeCombo->count(); ++i) {
            if (m_shapeTranslations[value.toString()] == m_widget->irisShapeCombo->itemText(i)) {
                m_widget->irisShapeCombo->setCurrentIndex(i);
            }
        }
    }
    if (config->getProperty("irisRadius", value)) {
        m_widget->irisRadiusSlider->setValue(value.toInt());
    }
    if (config->getProperty("irisRotation", value)) {
        m_widget->irisRotationSelector->setAngle(static_cast<qreal>(value.toInt()));
    }
    if (config->getProperty("focalDistance", value)) {
        m_widget->focalDistanceSlider->setValue(value.toInt());
    }
    if (config->getProperty("invert", value)) {
        m_widget->invertCheckBox->setChecked(value.toBool());
    }

    // Depth source is matched by UUID, since the combo's index ordering isn't
    // stable across dialog sessions (layers can be added/removed/reordered).
    if (config->getProperty("depthMapUuid", value)) {
        const QUuid uuid(value.toString());
        if (!uuid.isNull()) {
            for (int i = 0; i < m_depthSources.size(); ++i) {
                if (m_depthSources.at(i)->uuid() == uuid) {
                    m_widget->depthSourceCombo->setCurrentIndex(i);
                    break;
                }
            }
        }
    }
}
