Color Widgets
=============

Here is a color dialog that is more user-friendly than the default QColorDialog
and several other color-related widgets

The provided widgets are:

* ColorWheel,         An analog widget used to select a color
* ColorPreview,       A simple widget that displays a color
* GradientSlider,     A slider that has a gradient background
* HueSlider,          A variant of GradientSlider that has a rainbow background
* ColorSelector,      A ColorPreview that shows a ColorDialog when clicked
* ColorDialog,        A dialog that uses the above widgets to provide a better user experience than QColorDialog
* ColorListWidget,    A widget to edit a list of colors
* Swatch,             A widget to display a color palette
* ColorPaletteWidget, A widget to use and manage a list of palettes
* Color2DSlider,      An analog widget used to select 2 color components
* ColorLineEdit,      A widget to manipulate a string representing a color
* HarmonyColorWheel,  A ColorWheel which allows defining multiple colors, separated by hue
* GradientListModel,  A QAbstractListModel used to list gradients (useful for combo boxes, list views and the like)

they are all in the color_widgets namespace.

See [the gallery](gallery/README.md) for more information and screenshots.


Using it in a project
---------------------

All the required files are in ./src and ./include.

### QMake
For QMake-based projects, include color_widgets.pri in the QMake project file.

### CMake
For CMake-based projects, add this as subdirectory:
```
add_subdirectory(externals/Qt-Color-Widgets)
```
It will be compiled as a library and you can link your dependent projects to a target called `QtColorWidgets` in any suitable way. For example:
```
target_link_libraries(MyProject
	PRIVATE
	...

	PUBLIC
	QtColorWidgets
	...
)
```

#### Shared or static linking

To choose between static and shared library to build, there is an option `QTCOLORWIDGETS_BUILD_STATIC_LIBS` that you can set before `add_subdirectory()` to specify whether you want to build static library or not:
```
set(QTCOLORWIDGETS_BUILD_STATIC_LIBS OFF CACHE BOOL "Build static libraries for QtColorWidgets" FORCE)
add_subdirectory(externals/Qt-Color-Widgets)
```

> If you intend to use this library in a product with closed-source code, please ensure that you are linking to the library dynamically. According to the LGPL v3 license, static linking may impose additional requirements, such as the obligation to provide source code for the entire application or allowing users to relink the library.
>
> To comply with the LGPL v3 license and maintain the closed-source nature of your product, dynamic linking is required. This ensures that users can replace or modify the LGPL-licensed library independently without affecting your closed-source application.

Documentation
-------------

See https://mattbas.gitlab.io/Qt-Color-Widgets/


Installing as a Qt Designer/Creator Plugin
------------------------------------------

The sources for the designer plugin are in ./color_widgets_designer_plugin

Compile the library and install in
(Qt SDK)/Tools/QtCreator/bin/designer/
(Qt SDK)/(Qt Version)/(Toolchain)/plugins/designer

    mkdir build && cd build && cmake .. && make QtColorWidgetsPlugin && make install


Latest Version
--------------

The latest version of the sources can be found at the following locations:

* https://gitlab.com/mattbas/Qt-Color-Widgets
* git@gitlab.com:mattbas/Qt-Color-Widgets.git


License
-------

LGPLv3+, See COPYING.
As a special exception, this library can be included in any project under the
terms of any of the GNU liceses, distributing the whole project under a
different GNU license, see LICENSE-EXCEPTION for details.

Copyright (C) 2013-2020 Mattia Basaglia <dev@dragon.best>
