/*
 * SPDX-FileCopyrightText: 2013-2020 Mattia Basaglia
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "color_selector_plugin.hpp"
#include "QtColorWidgets/color_selector.hpp"
#include <QtPlugin>

ColorSelector_Plugin::ColorSelector_Plugin(QObject *parent)
    : QObject(parent), initialized(false)
{
}


void ColorSelector_Plugin::initialize(QDesignerFormEditorInterface *)
{
    if (initialized)
        return;

    initialized = true;
}

bool ColorSelector_Plugin::isInitialized() const
{
    return initialized;
}

QWidget *ColorSelector_Plugin::createWidget(QWidget *parent)
{
    return new color_widgets::ColorSelector(parent);
}

QString ColorSelector_Plugin::name() const
{
    return QStringLiteral("color_widgets::ColorSelector");
}

QString ColorSelector_Plugin::group() const
{
    return QStringLiteral("Color Widgets");
}

QIcon ColorSelector_Plugin::icon() const
{
    return QIcon();
}

QString ColorSelector_Plugin::toolTip() const
{
    return QStringLiteral("Display a color and opens a color dialog on click");
}

QString ColorSelector_Plugin::whatsThis() const
{
    return toolTip();
}

bool ColorSelector_Plugin::isContainer() const
{
    return false;
}

QString ColorSelector_Plugin::domXml() const
{

    return QStringLiteral("<ui language=\"c++\">\n"
                          " <widget class=\"color_widgets::ColorSelector\" name=\"ColorSelector\">\n"
                          " </widget>\n"
                          "</ui>\n");
}

QString ColorSelector_Plugin::includeFile() const
{
    return QStringLiteral("QtColorWidgets/color_selector.hpp");
}
