/*
 * SPDX-FileCopyrightText: 2013-2020 Mattia Basaglia
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "color_list_plugin.hpp"
#include "QtColorWidgets/color_list_widget.hpp"

ColorListWidget_Plugin::ColorListWidget_Plugin(QObject *parent) :
    QObject(parent)
{
}


void ColorListWidget_Plugin::initialize(QDesignerFormEditorInterface *)
{
    initialized = true;
}

bool ColorListWidget_Plugin::isInitialized() const
{
    return initialized;
}

QWidget *ColorListWidget_Plugin::createWidget(QWidget *parent)
{
    return new color_widgets::ColorListWidget(parent);
}

QString ColorListWidget_Plugin::name() const
{
    return QStringLiteral("color_widgets::ColorListWidget");
}

QString ColorListWidget_Plugin::group() const
{
    return QStringLiteral("Color Widgets");
}

QIcon ColorListWidget_Plugin::icon() const
{
    return QIcon::fromTheme(QStringLiteral("format-stroke-color"));
}

QString ColorListWidget_Plugin::toolTip() const
{
    return QStringLiteral("An editable list of colors");
}

QString ColorListWidget_Plugin::whatsThis() const
{
    return toolTip();
}

bool ColorListWidget_Plugin::isContainer() const
{
    return false;
}

QString ColorListWidget_Plugin::domXml() const
{

    return QStringLiteral("<ui language=\"c++\">\n"
                          " <widget class=\"color_widgets::ColorListWidget\" name=\"ColorListWidget\">\n"
                          " </widget>\n"
                          "</ui>\n");
}

QString ColorListWidget_Plugin::includeFile() const
{
    return QStringLiteral("QtColorWidgets/color_list_widget.hpp");
}
