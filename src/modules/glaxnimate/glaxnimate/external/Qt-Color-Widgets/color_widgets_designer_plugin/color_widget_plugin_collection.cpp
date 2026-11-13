/*
 * SPDX-FileCopyrightText: 2013-2020 Mattia Basaglia
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "color_widget_plugin_collection.hpp"
#include "color_preview_plugin.hpp"
#include "color_wheel_plugin.hpp"
#include "gradient_slider_plugin.hpp"
#include "hue_slider_plugin.hpp"
#include "color_selector_plugin.hpp"
#include "color_list_plugin.hpp"
#include "swatch_plugin.hpp"
#include "color_palette_widget_plugin.hpp"
#include "color_2d_slider_plugin.hpp"
#include "color_line_edit_plugin.hpp"
#include "gradient_editor_plugin.hpp"
// add new plugin headers above this line

ColorWidgets_PluginCollection::ColorWidgets_PluginCollection(QObject *parent) :
    QObject(parent)
{
    widgets.push_back(new ColorPreview_Plugin(this));
    widgets.push_back(new ColorWheel_Plugin(this));
    widgets.push_back(new GradientSlider_Plugin(this));
    widgets.push_back(new HueSlider_Plugin(this));
    widgets.push_back(new ColorSelector_Plugin(this));
    widgets.push_back(new ColorListWidget_Plugin(this));
    widgets.push_back(new Swatch_Plugin(this));
    widgets.push_back(new ColorPaletteWidget_Plugin(this));
    widgets.push_back(new Color2DSlider_Plugin(this));
    widgets.push_back(new ColorLineEdit_Plugin(this));
    widgets.push_back(new GradientEditor_Plugin(this));
    // add new plugins above this line
}

QList<QDesignerCustomWidgetInterface *> ColorWidgets_PluginCollection::customWidgets() const
{
    return widgets;
}
