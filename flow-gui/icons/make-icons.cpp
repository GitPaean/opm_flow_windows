/*
  Regenerates the flow-gui application icon: an anticline, the trap geometry
  a reservoir simulator exists to model - arched strata under a seal, with a
  well into the crest.

  The icon is drawn rather than kept as a painting, because it has to work at
  16px as well as 256. Every size is rendered for the size it is used at, with
  the geometry in units of the tile, so the bands stay a whole pixel or two
  thick at the small end instead of dissolving the way a downscaled 256px
  image does.

  Colours: the frame is SINTEF's brand blue, and the strata are sampled from
  the department's own geomechanics figure - see icons/README.md.

  Not part of the build; the PNGs it writes are committed. To change the icon,
  edit here, rebuild and re-run, then remake the .ico:

      c++ -O1 -std=gnu++20 -fPIC make-icons.cpp -o make-icons \
          $(pkg-config --cflags --libs Qt6Widgets)
      QT_QPA_PLATFORM=offscreen ./make-icons .
      convert flow-gui-{16,24,32,48,64,128,256}.png flow-gui.ico

  Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics
  Part of the opm_flow_windows harness; GPL v3+ (see repository LICENSE).
*/
#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <cmath>

static const QColor BLUE (0x00, 0x3c, 0x65);   // SINTEF brand blue, the frame
static const QColor GREEN(0x14, 0xb9, 0x78);   // SINTEF green, the well
static const QColor SHALE(0x8c, 0x3f, 0x3b);   // overburden
static const QColor CAP  (0xf2, 0xd0, 0x8a);   // seal above the reservoir
static const QColor SAND (0xE6, 0x9F, 0x00);   // the reservoir itself
static const QColor DEEP (0x5a, 0x2b, 0x2a);   // basement

// One stratum, between two depths, arched into a dome by `arch`. A Gaussian
// rather than a circular arc: the layers stay parallel over the crest and
// flatten towards the flanks, which is what an anticline actually looks like
// and what keeps the bands from crossing at the edges of the tile.
static QPainterPath band(double u, double yTop, double yBot, double arch)
{
    QPainterPath p;
    const auto edge = [&](double y, bool forward) {
        for (int i = 0; i <= 40; ++i) {
            const double t = (forward ? i : 40 - i) / 40.0;
            const double x = t * 256.0 * u;
            const double yy = (y - arch * std::exp(-std::pow((t - 0.5) * 3.1, 2))) * u;
            if (i == 0 && forward) p.moveTo(x, yy); else p.lineTo(x, yy);
        }
    };
    edge(yTop, true);
    edge(yBot, false);
    p.closeSubpath();
    return p;
}

static QImage draw(int s)
{
    QImage img(s, s, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    const double u = s / 256.0;

    // Rounded tile, and everything below is clipped to it.
    QPainterPath tile;
    tile.addRoundedRect(QRectF(0, 0, s, s), 46 * u, 46 * u);
    p.setClipPath(tile);
    p.fillRect(QRectF(0, 0, s, s), BLUE);

    p.setPen(Qt::NoPen);
    p.setBrush(SHALE); p.drawPath(band(u,  84, 150, 46));
    p.setBrush(CAP);   p.drawPath(band(u, 150, 172, 46));
    p.setBrush(SAND);  p.drawPath(band(u, 172, 214, 46));
    p.setBrush(DEEP);  p.drawPath(band(u, 214, 270, 46));

    // The well, stopping in the crest where the reservoir is.
    p.setPen(QPen(GREEN, 15 * u, Qt::SolidLine, Qt::FlatCap));
    p.drawLine(QPointF(128 * u, 20 * u), QPointF(128 * u, 150 * u));
    p.end();
    return img;
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    const QString dir = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                 : QStringLiteral(".");
    for (int s : { 16, 24, 32, 48, 64, 128, 256 })
        draw(s).save(QStringLiteral("%1/flow-gui-%2.png").arg(dir).arg(s));
    return 0;
}
