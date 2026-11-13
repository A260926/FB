/*
 * SPDX-FileCopyrightText: 2013-2020 Mattia Basaglia
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef COLOR_WIDGETS_GRADIENT_EDITOR_PLUGIN_HPP
#define COLOR_WIDGETS_GRADIENT_EDITOR_PLUGIN_HPP

#include <QObject>
#include <QtUiPlugin/QDesignerCustomWidgetInterface>

class GradientEditor_Plugin : public QObject, public QDesignerCustomWidgetInterface
{
    Q_OBJECT
    Q_INTERFACES(QDesignerCustomWidgetInterface)

public:
    explicit GradientEditor_Plugin(QObject *parent = nullptr);

    void initialize(QDesignerFormEditorInterface *core) Q_DECL_OVERRIDE;
    bool isInitialized() const Q_DECL_OVERRIDE;

    QWidget *createWidget(QWidget *parent) Q_DECL_OVERRIDE;

    QString name() const Q_DECL_OVERRIDE;
    QString group() const Q_DECL_OVERRIDE;
    QIcon icon() const Q_DECL_OVERRIDE;
    QString toolTip() const Q_DECL_OVERRIDE;
    QString whatsThis() const Q_DECL_OVERRIDE;
    bool isContainer() const Q_DECL_OVERRIDE;

    QString domXml() const Q_DECL_OVERRIDE;

    QString includeFile() const Q_DECL_OVERRIDE;

private:
    bool initialized;
};


#endif // COLOR_WIDGETS_GRADIENT_EDITOR_PLUGIN_HPP

