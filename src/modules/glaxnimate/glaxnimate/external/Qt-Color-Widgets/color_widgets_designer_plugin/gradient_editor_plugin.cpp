/*
 * SPDX-FileCopyrightText: 2013-2020 Mattia Basaglia
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "gradient_editor_plugin.hpp"
#include "QtColorWidgets/gradient_editor.hpp"

QWidget* GradientEditor_Plugin::createWidget(QWidget *parent)
{
    color_widgets::GradientEditor *widget = new color_widgets::GradientEditor(parent);
    return widget;
}

QIcon GradientEditor_Plugin::icon() const
{
    color_widgets::GradientEditor w;
    w.resize(64,16);
    QGradientStops cols;
    cols.push_back({0.2, Qt::green});
    cols.push_back({0.5, Qt::yellow});
    cols.push_back({0.8, Qt::red});
    w.setStops(cols);
    QPixmap pix(64,64);
    pix.fill(Qt::transparent);
    w.render(&pix, QPoint(0,16));
    return QIcon(pix);
}

QString GradientEditor_Plugin::domXml() const
{
    return QStringLiteral("<ui language=\"c++\">\n"
                          " <widget class=\"color_widgets::GradientEditor\" name=\"gradient_editor\">\n"
                          " </widget>\n"
                          "</ui>\n");
}

bool GradientEditor_Plugin::isContainer() const
{
    return false;
}

GradientEditor_Plugin::GradientEditor_Plugin(QObject *parent) :
    QObject(parent), initialized(false)
{
}

void GradientEditor_Plugin::initialize(QDesignerFormEditorInterface *)
{
    initialized = true;
}

bool GradientEditor_Plugin::isInitialized() const
{
    return initialized;
}

QString GradientEditor_Plugin::name() const
{
    return QStringLiteral("color_widgets::GradientEditor");
}

QString GradientEditor_Plugin::group() const
{
    return QStringLiteral("Color Widgets");
}

QString GradientEditor_Plugin::toolTip() const
{
    return QStringLiteral("Widget to edit gradient stops");
}

QString GradientEditor_Plugin::whatsThis() const
{
    return toolTip();
}

QString GradientEditor_Plugin::includeFile() const
{
    return QStringLiteral("QtColorWidgets/gradient_editor.hpp");
}

