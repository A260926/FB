/*
 * SPDX-FileCopyrightText: 2013-2020 Mattia Basaglia
 * SPDX-FileCopyrightText: 2014 Calle Laakkonen
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "hue_slider_plugin.hpp"
#include "QtColorWidgets/hue_slider.hpp"
#include <QtPlugin>

HueSlider_Plugin::HueSlider_Plugin(QObject *parent)
    : QObject(parent), initialized(false)
{
}


void HueSlider_Plugin::initialize(QDesignerFormEditorInterface *)
{
    if (initialized)
        return;

    initialized = true;
}

bool HueSlider_Plugin::isInitialized() const
{
    return initialized;
}

QWidget *HueSlider_Plugin::createWidget(QWidget *parent)
{
    return new color_widgets::HueSlider(parent);
}

QString HueSlider_Plugin::name() const
{
    return QStringLiteral("color_widgets::HueSlider");
}

QString HueSlider_Plugin::group() const
{
    return QStringLiteral("Color Widgets");
}

QIcon HueSlider_Plugin::icon() const
{
    color_widgets::HueSlider w;
    w.resize(64,16);
    QPixmap pix(64,64);
    pix.fill(Qt::transparent);
    w.render(&pix,QPoint(0,16));
    return QIcon(pix);
}

QString HueSlider_Plugin::toolTip() const
{
    return QStringLiteral("Slider over a hue gradient");
}

QString HueSlider_Plugin::whatsThis() const
{
    return toolTip();
}

bool HueSlider_Plugin::isContainer() const
{
    return false;
}

QString HueSlider_Plugin::domXml() const
{

    return QStringLiteral("<ui language=\"c++\">\n"
                          " <widget class=\"color_widgets::HueSlider\" name=\"HueSlider\">\n"
                          " </widget>\n"
                          "</ui>\n");
}

QString HueSlider_Plugin::includeFile() const
{
    return QStringLiteral("QtColorWidgets/hue_slider.hpp");
}
