/*
 * SPDX-FileCopyrightText: 2013-2020 Mattia Basaglia
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "color_line_edit_plugin.hpp"
#include "QtColorWidgets/color_line_edit.hpp"

QWidget* ColorLineEdit_Plugin::createWidget(QWidget *parent)
{
    color_widgets::ColorLineEdit *widget = new color_widgets::ColorLineEdit(parent);
    return widget;
}

QIcon ColorLineEdit_Plugin::icon() const
{
    return QIcon::fromTheme(QStringLiteral("edit-rename"));
}

QString ColorLineEdit_Plugin::domXml() const
{
    return QStringLiteral("<ui language=\"c++\">\n"
                          " <widget class=\"color_widgets::ColorLineEdit\" name=\"color_line_edit\">\n"
                          " </widget>\n"
                          "</ui>\n");
}

bool ColorLineEdit_Plugin::isContainer() const
{
    return false;
}

ColorLineEdit_Plugin::ColorLineEdit_Plugin(QObject *parent) :
    QObject(parent), initialized(false)
{
}

void ColorLineEdit_Plugin::initialize(QDesignerFormEditorInterface *)
{
    initialized = true;
}

bool ColorLineEdit_Plugin::isInitialized() const
{
    return initialized;
}

QString ColorLineEdit_Plugin::name() const
{
    return QStringLiteral("color_widgets::ColorLineEdit");
}

QString ColorLineEdit_Plugin::group() const
{
    return QStringLiteral("Color Widgets");
}

QString ColorLineEdit_Plugin::toolTip() const
{
    return QStringLiteral("A widget to manipulate a string representing a color");
}

QString ColorLineEdit_Plugin::whatsThis() const
{
    return toolTip();
}

QString ColorLineEdit_Plugin::includeFile() const
{
    return QStringLiteral("QtColorWidgets/color_line_edit.hpp");
}
