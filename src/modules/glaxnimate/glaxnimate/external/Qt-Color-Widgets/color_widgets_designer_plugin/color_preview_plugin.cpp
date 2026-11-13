/*
 * SPDX-FileCopyrightText: 2013-2020 Mattia Basaglia
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "color_preview_plugin.hpp"
#include "QtColorWidgets/color_preview.hpp"
#include <QtPlugin>


ColorPreview_Plugin::ColorPreview_Plugin(QObject *parent)
    : QObject(parent), initialized(false)
{
}


void ColorPreview_Plugin::initialize(QDesignerFormEditorInterface *)
{
    if (initialized)
        return;

    initialized = true;
}

bool ColorPreview_Plugin::isInitialized() const
{
    return initialized;
}

QWidget *ColorPreview_Plugin::createWidget(QWidget *parent)
{
    return new color_widgets::ColorPreview(parent);
}

QString ColorPreview_Plugin::name() const
{
    return QStringLiteral("color_widgets::ColorPreview");
}

QString ColorPreview_Plugin::group() const
{
    return QStringLiteral("Color Widgets");
}

QIcon ColorPreview_Plugin::icon() const
{
    return QIcon();
}

QString ColorPreview_Plugin::toolTip() const
{
    return QStringLiteral("Display a color");
}

QString ColorPreview_Plugin::whatsThis() const
{
    return toolTip();
}

bool ColorPreview_Plugin::isContainer() const
{
    return false;
}

QString ColorPreview_Plugin::domXml() const
{

    return QStringLiteral("<ui language=\"c++\">\n"
                          " <widget class=\"color_widgets::ColorPreview\" name=\"colorPreview\">\n"
                          " </widget>\n"
                          "</ui>\n");
}

QString ColorPreview_Plugin::includeFile() const
{
    return QStringLiteral("QtColorWidgets/color_preview.hpp");
}

//Q_EXPORT_PLUGIN2(color_widgets, ColorPreview_Plugin);
