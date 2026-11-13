/*
 * SPDX-FileCopyrightText: 2013-2020 Mattia Basaglia
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "swatch_plugin.hpp"
#include "QtColorWidgets/swatch.hpp"

Swatch_Plugin::Swatch_Plugin(QObject *parent) :
    QObject(parent), initialized(false)
{
}

void Swatch_Plugin::initialize(QDesignerFormEditorInterface *)
{
    initialized = true;
}

bool Swatch_Plugin::isInitialized() const
{
    return initialized;
}

QWidget* Swatch_Plugin::createWidget(QWidget *parent)
{
    color_widgets::Swatch *wid = new color_widgets::Swatch(parent);
    wid->palette().setColumns(12);
    for ( int i = 0; i < 6; i++ )
    {
        for ( int j = 0; j < wid->palette().columns(); j++ )
        {
            float f = float(j)/wid->palette().columns();
            wid->palette().appendColor(QColor::fromHsvF(i/8.0,1-f,0.5+f/2));
        }
    }
    return wid;
}

QString Swatch_Plugin::name() const
{
    return QStringLiteral("color_widgets::Swatch");
}

QString Swatch_Plugin::group() const
{
    return QStringLiteral("Color Widgets");
}

QIcon Swatch_Plugin::icon() const
{
    color_widgets::ColorPalette w;
    w.setColumns(6);
    for ( int i = 0; i < 4; i++ )
    {
        for ( int j = 0; j < w.columns(); j++ )
        {
            float f = float(j)/w.columns();
            w.appendColor(QColor::fromHsvF(i/5.0,1-f,0.5+f/2));
        }
    }
    return QIcon(w.preview(QSize(64,64)));
}

QString Swatch_Plugin::toolTip() const
{
    return QStringLiteral("A widget that displays a color palette");
}

QString Swatch_Plugin::whatsThis() const
{
    return toolTip();
}

bool Swatch_Plugin::isContainer() const
{
    return false;
}

QString Swatch_Plugin::domXml() const
{
    return QStringLiteral("<ui language=\"c++\">\n"
                          " <widget class=\"color_widgets::Swatch\" name=\"swatch\">\n"
                          " </widget>\n"
                          "</ui>\n");
}

QString Swatch_Plugin::includeFile() const
{
    return QStringLiteral("QtColorWidgets/swatch.hpp");
}
