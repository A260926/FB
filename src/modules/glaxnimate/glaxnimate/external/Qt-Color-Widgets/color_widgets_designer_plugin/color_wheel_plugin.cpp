/*
 * SPDX-FileCopyrightText: 2013-2020 Mattia Basaglia
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "color_wheel_plugin.hpp"
#include "QtColorWidgets/color_wheel.hpp"

ColorWheel_Plugin::ColorWheel_Plugin(QObject *parent) :
    QObject(parent), initialized(false)
{
}

void ColorWheel_Plugin::initialize(QDesignerFormEditorInterface *)
{
    initialized = true;
}

bool ColorWheel_Plugin::isInitialized() const
{
    return initialized;
}

QWidget *ColorWheel_Plugin::createWidget(QWidget *parent)
{
    return new color_widgets::ColorWheel(parent);
}

QString ColorWheel_Plugin::name() const
{
    return QStringLiteral("color_widgets::ColorWheel");
}

QString ColorWheel_Plugin::group() const
{
    return QStringLiteral("Color Widgets");
}

QIcon ColorWheel_Plugin::icon() const
{
    color_widgets::ColorWheel w;
    w.resize(64,64);
    w.setWheelWidth(8);
    QPixmap pix(64,64);
    w.render(&pix);
    return QIcon(pix);
}

QString ColorWheel_Plugin::toolTip() const
{
    return QStringLiteral("A widget that allows an intuitive selection of HSL parameters for a QColor");
}

QString ColorWheel_Plugin::whatsThis() const
{
    return toolTip();
}

bool ColorWheel_Plugin::isContainer() const
{
    return false;
}

QString ColorWheel_Plugin::domXml() const
{
    return QStringLiteral("<ui language=\"c++\">\n"
                          " <widget class=\"color_widgets::ColorWheel\" name=\"colorWheel\">\n"
                          "  <property name=\"sizePolicy\">\n"
                          "   <sizepolicy hsizetype=\"Minimum\" vsizetype=\"Minimum\">\n"
                          "    <horstretch>0</horstretch>\n"
                          "    <verstretch>0</verstretch>\n"
                          "   </sizepolicy>\n"
                          "  </property>\n"
                          " </widget>\n"
                          "</ui>\n");
}

QString ColorWheel_Plugin::includeFile() const
{
    return QStringLiteral("QtColorWidgets/color_wheel.hpp");
}
