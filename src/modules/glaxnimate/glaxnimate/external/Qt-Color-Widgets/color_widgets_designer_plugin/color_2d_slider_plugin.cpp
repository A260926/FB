/*
 * SPDX-FileCopyrightText: 2013-2020 Mattia Basaglia
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "color_2d_slider_plugin.hpp"
#include "QtColorWidgets/color_2d_slider.hpp"

Color2DSlider_Plugin::Color2DSlider_Plugin(QObject *parent) :
    QObject(parent), initialized(false)
{
}

void Color2DSlider_Plugin::initialize(QDesignerFormEditorInterface *)
{
    initialized = true;
}

bool Color2DSlider_Plugin::isInitialized() const
{
    return initialized;
}

QWidget *Color2DSlider_Plugin::createWidget(QWidget *parent)
{
    return new color_widgets::Color2DSlider(parent);
}

QString Color2DSlider_Plugin::name() const
{
    return QStringLiteral("color_widgets::Color2DSlider");
}

QString Color2DSlider_Plugin::group() const
{
    return QStringLiteral("Color Widgets");
}

QIcon Color2DSlider_Plugin::icon() const
{
    color_widgets::Color2DSlider w;
    w.resize(64,64);
    QPixmap pix(64,64);
    w.render(&pix);
    return QIcon(pix);
}

QString Color2DSlider_Plugin::toolTip() const
{
    return QStringLiteral("An analog widget to select 2 color components at the same time");
}

QString Color2DSlider_Plugin::whatsThis() const
{
    return toolTip();
}

bool Color2DSlider_Plugin::isContainer() const
{
    return false;
}

QString Color2DSlider_Plugin::domXml() const
{
    return QStringLiteral("<ui language=\"c++\">\n"
                          " <widget class=\"color_widgets::Color2DSlider\" name=\"color2DSlider\">\n"
                          " </widget>\n"
                          "</ui>\n");
}

QString Color2DSlider_Plugin::includeFile() const
{
    return QStringLiteral("QtColorWidgets/color_2d_slider.hpp");
}
