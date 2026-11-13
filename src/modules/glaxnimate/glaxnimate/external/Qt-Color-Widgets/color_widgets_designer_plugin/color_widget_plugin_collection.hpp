/*
 * SPDX-FileCopyrightText: 2013-2020 Mattia Basaglia
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef COLOR_WIDGET_PLUGIN_COLLECTION_HPP
#define COLOR_WIDGET_PLUGIN_COLLECTION_HPP

#include <QtUiPlugin/QDesignerCustomWidgetCollectionInterface>

class ColorWidgets_PluginCollection : public QObject, public QDesignerCustomWidgetCollectionInterface
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "mattia.basaglia.ColorWidgetsPlugin")
    Q_INTERFACES(QDesignerCustomWidgetCollectionInterface)

public:
    explicit ColorWidgets_PluginCollection(QObject *parent = nullptr);

    QList<QDesignerCustomWidgetInterface*> customWidgets() const;

   private:
       QList<QDesignerCustomWidgetInterface*> widgets;
    
};

#endif // COLOR_WIDGET_PLUGIN_COLLECTION_HPP
