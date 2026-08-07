/*
  flow-gui - a cross-platform (Windows / Linux / macOS) Qt 6 GUI front
  end for running OPM Flow simulations: job queue with live progress,
  MPI/OpenMP run options, streamed simulator log, and (when built with
  summary support) plots of summary vectors via opm-common's ESmry.

  Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics

  This file is part of the opm_flow_windows build harness and is licensed
  under the GNU General Public License v3+ like the OPM project itself.
*/

#include "FlowGuiWindow.h"

#include <QApplication>
#include <QIcon>
#include <QPalette>
#include <QStyleHints>

#include <cstdio>
#include <cstring>

int main(int argc, char** argv)
{
    // --version: print and exit (headless smoke test, no QApplication needed)
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("flow-gui " FLOWGUI_VERSION "\n");
            return 0;
        }
    }

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("OPM"));
    QApplication::setApplicationName(QStringLiteral("flow-gui"));

    // The window icon, from the compiled-in resource so it does not depend on
    // anything being installed. Every size is added rather than one large PNG
    // left to be scaled: Qt picks the nearest and downscaling 256 to 16 loses
    // the curve to a smudge.
    QIcon icon;
    for (int s : { 16, 24, 32, 48, 64, 128, 256 })
        icon.addFile(QStringLiteral(":/icons/flow-gui-%1.png").arg(s), QSize(s, s));
    QApplication::setWindowIcon(icon);
    // ... and the name of the desktop entry this application is launched from.
    // setWindowIcon is enough for the title bar and for X11, but a Wayland
    // compositor ignores it: there, the shell takes the icon from the .desktop
    // file it can match the window's app id to, and this is what makes that
    // match. It also gives GNOME the same handle on X11, so a running flow-gui
    // shares its dock entry with the launcher instead of appearing beside it.
    QGuiApplication::setDesktopFileName(QStringLiteral("flow-gui"));

    // Bright, platform-independent appearance: do not follow a dark system
    // color scheme; use the Fusion style with an explicit light palette.
    app.setStyle(QStringLiteral("Fusion"));
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    app.styleHints()->setColorScheme(Qt::ColorScheme::Light);
#endif
    QPalette pal;
    pal.setColor(QPalette::Window,          QColor(0xf4, 0xf6, 0xf8));
    pal.setColor(QPalette::WindowText,      Qt::black);
    pal.setColor(QPalette::Base,            Qt::white);
    pal.setColor(QPalette::AlternateBase,   QColor(0xec, 0xf0, 0xf4));
    pal.setColor(QPalette::Text,            Qt::black);
    pal.setColor(QPalette::Button,          QColor(0xe8, 0xec, 0xf0));
    pal.setColor(QPalette::ButtonText,      Qt::black);
    pal.setColor(QPalette::ToolTipBase,     QColor(0xff, 0xff, 0xe1));
    pal.setColor(QPalette::ToolTipText,     Qt::black);
    pal.setColor(QPalette::Highlight,       QColor(0x2f, 0x6f, 0xd0));
    pal.setColor(QPalette::HighlightedText, Qt::white);
    pal.setColor(QPalette::PlaceholderText, QColor(0x80, 0x88, 0x90));
    pal.setColor(QPalette::Disabled, QPalette::Text,       QColor(0x9a, 0xa0, 0xa6));
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x9a, 0xa0, 0xa6));
    pal.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x9a, 0xa0, 0xa6));
    app.setPalette(pal);

    FlowGuiWindow win;
    win.show();
    return app.exec();
}
