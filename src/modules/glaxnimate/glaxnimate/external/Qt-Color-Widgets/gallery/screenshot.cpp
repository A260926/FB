/*
 * SPDX-FileCopyrightText: 2013-2025 Mattia Basaglia
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#include <cstring>
#include <algorithm>

#include <QApplication>
#include <QCommandLineParser>
#include <QComboBox>
#include <QListView>
#include <QTableWidget>

#include "QtColorWidgets/color_2d_slider.hpp"
#include "QtColorWidgets/color_delegate.hpp" /// \todo show it
#include "QtColorWidgets/color_dialog.hpp"
#include "QtColorWidgets/color_line_edit.hpp"
#include "QtColorWidgets/color_list_widget.hpp"
#include "QtColorWidgets/color_palette_widget.hpp"
#include "QtColorWidgets/color_preview.hpp"
#include "QtColorWidgets/color_wheel.hpp"
#include "QtColorWidgets/harmony_color_wheel.hpp"
#include "QtColorWidgets/hue_slider.hpp"
#include "QtColorWidgets/gradient_editor.hpp"
#include "QtColorWidgets/gradient_list_model.hpp"
#include "QtColorWidgets/gradient_delegate.hpp"

bool run = false;
QStringList just_these;

void screenshot(QWidget& widget, QString name = QString())
{
    if ( name.isEmpty() )
    {
        name = QString::fromLatin1(widget.metaObject()->className());
        name.remove(QStringLiteral("color_widgets::"));
    }
    if ( !just_these.isEmpty() && !just_these.contains(name) )
        return;

    widget.setWindowTitle(name);
    QPixmap pic(widget.size());
    widget.render(&pic);
    name += QStringLiteral(".png");
    pic.save(name);
    if ( run )
        widget.show();
}

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("just_these"), QStringLiteral("Only these widgets"));
    QCommandLineOption run_option(QStringLiteral("run"), QStringLiteral("Show widgets instead of saving to file"));
    parser.addOption(run_option);

    parser.process(a);
    run = parser.isSet(run_option);
    just_these = parser.positionalArguments();

    QColor demo_color(64,172,143,128);

    color_widgets::ColorPalette palette1;
    color_widgets::ColorPalette palette2;
    color_widgets::ColorPalette palette3;
    int palette_columns = 12;
    palette1.setName(QStringLiteral("Palette 1"));
    palette2.setName(QStringLiteral("Palette 2"));
    palette3.setName(QStringLiteral("Palette 3"));
    palette1.setColumns(palette_columns);
    palette2.setColumns(palette_columns);
    for ( int i = 0; i < 6; i++ )
    {
        for ( int j = 0; j < palette_columns; j++ )
        {
            float f = float(j)/palette_columns;
            palette1.appendColor(QColor::fromHsvF(i/8.0,1-f,0.5+f/2));
            palette2.appendColor(QColor::fromHsvF(i/8.0,1-f,1-f));
        }

        palette3.appendColor(QColor::fromHsvF(i/8.0, 0.8, 1));
    }
    color_widgets::ColorPaletteModel palette_model;
    palette_model.addPalette(palette1, false);
    palette_model.addPalette(palette2, false);
    palette_model.addPalette(palette3, false);



    color_widgets::ColorPreview preview;
    preview.setColor(demo_color);
    preview.setDisplayMode(color_widgets::ColorPreview::SplitAlpha);
    preview.resize(128,32);
    screenshot(preview);

    color_widgets::ColorDialog dialog;
    dialog.setColorSpace(color_widgets::ColorWheel::ColorLCH);
    dialog.setColor(demo_color);
    screenshot(dialog);

    color_widgets::Color2DSlider slider2d;
    slider2d.setColor(demo_color);
    slider2d.resize(128,192);
    screenshot(slider2d);

    color_widgets::ColorLineEdit line_edit;
    line_edit.setColor(demo_color);
    line_edit.resize(line_edit.sizeHint());
    screenshot(line_edit);
    line_edit.setPreviewColor(true);
    screenshot(line_edit, QStringLiteral("ColorLineEdit_with_color"));

    color_widgets::ColorWheel wheel;
    wheel.resize(256, 256);
    wheel.setColor(demo_color);
    screenshot(wheel);

    color_widgets::HarmonyColorWheel harwheel;
    harwheel.resize(256, 256);
    harwheel.setColor(demo_color);
    harwheel.addHarmony(.333, true);
    harwheel.addHarmony(.667, true);
    screenshot(harwheel);

    color_widgets::Swatch swatch;
    swatch.setPalette(palette1);
    swatch.resize(swatch.sizeHint());
    screenshot(swatch);

    color_widgets::ColorPaletteWidget palette_widget;
    palette_widget.setModel(&palette_model);
    screenshot(palette_widget);

    color_widgets::ColorPaletteWidget palette_widget1;
    palette_widget1.setModel(&palette_model);
    palette_widget1.setReadOnly(true);
    screenshot(palette_widget1, QStringLiteral("ColorPaletteWidget_readonly"));

    color_widgets::HueSlider hue_slider;
    hue_slider.setColor(demo_color);
    hue_slider.resize(192, hue_slider.sizeHint().height());
//     hue_slider.setInvertedAppearance(true);
//     hue_slider.setOrientation(Qt::Vertical);
    screenshot(hue_slider);

    color_widgets::ColorListWidget list_widget;
    list_widget.setColors({
        demo_color,
        palette1.colorAt(palette_columns*0),
        palette1.colorAt(palette_columns*1),
        palette1.colorAt(palette_columns*3),
        palette1.colorAt(palette_columns*5),
    });
    list_widget.resize(list_widget.sizeHint());
    screenshot(list_widget);

    color_widgets::GradientEditor editor;
    QGradientStops gradient_colors;
    float n_colors = 4;
    for ( int i = 0; i <= n_colors; ++i )
        gradient_colors.append(QGradientStop(i/n_colors, QColor::fromHsvF(i/n_colors, 0.5, 1)));
    editor.setStops(gradient_colors);
    screenshot(editor);

    QComboBox gradient_list;
    color_widgets::GradientListModel gradient_model;
    gradient_model.setGradient(QStringLiteral("Rainbow"), gradient_colors);
    gradient_model.setGradient(QStringLiteral("Black to Transparent"), QGradientStops{{0, Qt::black}, {1, QColor(0, 0, 0, 0)}});
    gradient_list.setModel(&gradient_model);
    gradient_model.setIconSize(QSize(128, 24));
    gradient_list.setIconSize(gradient_model.iconSize());
    QObject::connect(&editor, &color_widgets::GradientEditor::stopsChanged, &gradient_model,
            [&gradient_model](const QGradientStops& stops){ gradient_model.setGradient(QStringLiteral("Rainbow"), stops); });
    gradient_list.resize(gradient_list.sizeHint());
    screenshot(gradient_list, QStringLiteral("GradientListModel_combo"));

    QListView gradient_view;
    color_widgets::GradientDelegate gradient_delegate;
    gradient_view.setItemDelegate(&gradient_delegate);
    gradient_view.setModel(&gradient_model);
//     gradient_model.setEditMode(color_widgets::GradientListModel::EditName);
    gradient_model.setEditMode(color_widgets::GradientListModel::EditGradient);
    gradient_view.resize(QSize(gradient_view.sizeHintForColumn(0) + 4, gradient_view.sizeHint().height()));
    screenshot(gradient_view, QStringLiteral("GradientListModel_view"));

    QTableWidget gradient_table;
    gradient_table.setItemDelegate(&gradient_delegate);
    gradient_table.setRowCount(2);
    gradient_table.setColumnCount(2);
    gradient_table.setItem(0, 0, new QTableWidgetItem());
    gradient_table.item(0, 0)->setData(Qt::EditRole, QVariant::fromValue(gradient_model.gradientBrush(0)));
    gradient_table.setItem(0, 1, new QTableWidgetItem(gradient_model.nameFromIndex(0)));
    gradient_table.setItem(1, 0, new QTableWidgetItem());
    gradient_table.item(1, 0)->setData(Qt::EditRole, QVariant::fromValue(gradient_model.gradientBrush(1)));
    gradient_table.setItem(1, 1, new QTableWidgetItem(gradient_model.nameFromIndex(1)));
    screenshot(gradient_table, QStringLiteral("GradientDelegate_table"));

    if ( run )
        return a.exec();

    return 0;
}
