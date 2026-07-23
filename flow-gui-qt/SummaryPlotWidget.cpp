/*
  SummaryPlotWidget implementation. Part of the opm_flow_windows harness;
  GPL v3+ (see repository LICENSE).
*/
#include "SummaryPlotWidget.h"

#include <opm/io/eclipse/ESmry.hpp>
#include <opm/io/eclipse/EclFile.hpp>
#include <opm/io/eclipse/EclUtil.hpp>

#include <QCheckBox>
#include <QChart>
#include <QChartView>
#include <QComboBox>
#include <QDateTime>
#include <QDateTimeAxis>
#include <QDir>
#include <QTimeZone>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QLineSeries>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSplitter>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QValueAxis>

#include <algorithm>
#include <chrono>
#include <exception>
#include <string>
#include <utility>
#include <vector>

using Opm::EclIO::ESmry;
using Cat  = Opm::EclIO::SummaryNode::Category;
using Type = Opm::EclIO::SummaryNode::Type;

namespace {

const int RoleVecIndex = Qt::UserRole + 1;   // leaf item -> index into vecs_

QString categoryName(Cat c)
{
    switch (c) {
    case Cat::Well:          return QStringLiteral("Well");
    case Cat::Group:         return QStringLiteral("Group");
    case Cat::Field:         return QStringLiteral("Field");
    case Cat::Region:        return QStringLiteral("Region");
    case Cat::Block:         return QStringLiteral("Block");
    case Cat::Connection:    return QStringLiteral("Connection");
    case Cat::Completion:    return QStringLiteral("Completion");
    case Cat::Segment:       return QStringLiteral("Segment");
    case Cat::Aquifer:       return QStringLiteral("Aquifer");
    case Cat::Node:          return QStringLiteral("Network node");
    case Cat::Miscellaneous: return QStringLiteral("Miscellaneous");
    }
    return QStringLiteral("Other");
}

QString typeName(Type t)
{
    switch (t) {
    case Type::Rate:      return QStringLiteral("Rate");
    case Type::Total:     return QStringLiteral("Total");
    case Type::Ratio:     return QStringLiteral("Ratio");
    case Type::Pressure:  return QStringLiteral("Pressure");
    case Type::Count:     return QStringLiteral("Count");
    case Type::Mode:      return QStringLiteral("Mode");
    case Type::ProdIndex: return QStringLiteral("Prod. index");
    case Type::Undefined: return QStringLiteral("Other");
    }
    return QStringLiteral("Other");
}

// categories that carry a scope prefix letter on the keyword (WOPR -> OPR)
bool hasScopeLetter(Cat c)
{
    return c != Cat::Miscellaneous;
}

// ECLIPSE natural cell number -> 1-based I,J,K (mirrors ESmry's own
// ijk_from_global_index); falls back to the plain number without grid dims.
QString ijkLabel(int number, int nx, int ny)
{
    if (nx <= 0 || ny <= 0 || number <= 0) return QString::number(number);
    int g = number - 1;
    const int i = 1 + (g % nx);  g /= nx;
    const int j = 1 + (g % ny);
    const int k = 1 + (g / ny);
    return QStringLiteral("%1,%2,%3").arg(i).arg(j).arg(k);
}

// Split a node's item into up to two levels: `main` (well/group name, region
// number, block cell, ...) and `sub` (the cell / completion / segment within
// a well for the compound categories). Field and Miscellaneous vectors have
// no item (their number field is 0, not an identifier).
void splitItem(const Opm::EclIO::SummaryNode& n, int nx, int ny,
               QString& main, QString& sub)
{
    main.clear(); sub.clear();
    const bool haveName = !n.wgname.empty() && n.wgname != ":+:+:+:+";
    const QString name  = haveName ? QString::fromStdString(n.wgname) : QString();
    const bool haveNum  = n.number != Opm::EclIO::SummaryNode::default_number;
    const QString num   = haveNum ? QString::number(n.number) : QString();

    switch (n.category) {
    case Cat::Well: case Cat::Group: case Cat::Node:
        main = name; return;
    case Cat::Region:
        // inter-region flow vectors (R?F..., e.g. ROFR) carry a packed
        // region pair; display it as "r1-r2" like ESmry does
        if (haveNum && n.keyword.size() > 2 && n.keyword[2] == 'F') {
            const auto [r1, r2] = Opm::EclIO::splitSummaryNumber(n.number);
            main = QStringLiteral("%1-%2").arg(r1).arg(r2);
            return;
        }
        main = num; return;
    case Cat::Aquifer:
        main = num; return;
    case Cat::Block:
        if (haveNum) main = ijkLabel(n.number, nx, ny);
        return;
    case Cat::Connection:
        // per-well AND per-cell: two levels, cell shown as grid indices
        main = name;
        if (haveNum) sub = ijkLabel(n.number, nx, ny);
        if (main.isEmpty()) { main = sub; sub.clear(); }
        return;
    case Cat::Completion: case Cat::Segment:
        // per-well numbered (completion/segment id, not a cell)
        main = name;
        sub  = num;
        if (main.isEmpty()) { main = sub; sub.clear(); }
        return;
    case Cat::Field: case Cat::Miscellaneous:
    default:
        return;
    }
}

// Strict weak ordering over a mix of item styles: numeric tuples
// ("7", "12,22,7", "2-3") form one partition sorted component-wise, ahead of
// the textual partition (well names) sorted locale-aware.
void sortItems(QStringList& items)
{
    auto asTuple = [](const QString& s) -> QVector<int> {
        QVector<int> t;
        const QStringList parts = s.split(QRegularExpression(QStringLiteral("[,-]")));
        for (const QString& p : parts) {
            bool ok = false;
            const int v = p.toInt(&ok);
            if (!ok) return {};
            t.push_back(v);
        }
        return t;
    };
    std::sort(items.begin(), items.end(),
              [&asTuple](const QString& a, const QString& b) {
        const QVector<int> ta = asTuple(a), tb = asTuple(b);
        if (ta.isEmpty() != tb.isEmpty()) return !ta.isEmpty();  // tuples first
        if (!ta.isEmpty())
            return std::lexicographical_compare(ta.begin(), ta.end(),
                                                tb.begin(), tb.end());
        return a.localeAwareCompare(b) < 0;
    });
}

} // namespace

// --- friendly quantity names (body code -> name) ---------------------------
// Keyed by the scope-independent BODY code (keyword with the leading scope
// letter removed, e.g. WOPR/FOPR/GOPR -> OPR). Miscellaneous vectors are keyed
// by their full keyword. Unknown codes fall back to the raw keyword, and a
// trailing 'H' is understood as the "(History)" variant.
QString SummaryPlotWidget::friendlyName(const QString& keyword, Cat cat)
{
    static const QHash<QString, QString> table = {
        {QStringLiteral("OPR"), QStringLiteral("Oil Production Rate")},
        {QStringLiteral("WPR"), QStringLiteral("Water Production Rate")},
        {QStringLiteral("GPR"), QStringLiteral("Gas Production Rate")},
        {QStringLiteral("LPR"), QStringLiteral("Liquid Production Rate")},
        {QStringLiteral("VPR"), QStringLiteral("Reservoir Volume Production Rate")},
        {QStringLiteral("GPRF"), QStringLiteral("Free Gas Production Rate")},
        {QStringLiteral("GPRS"), QStringLiteral("Solution Gas Production Rate")},
        {QStringLiteral("OPRF"), QStringLiteral("Free Oil Production Rate")},
        {QStringLiteral("OPRS"), QStringLiteral("Solution Oil Production Rate")},
        {QStringLiteral("OPT"), QStringLiteral("Oil Production Total")},
        {QStringLiteral("WPT"), QStringLiteral("Water Production Total")},
        {QStringLiteral("GPT"), QStringLiteral("Gas Production Total")},
        {QStringLiteral("LPT"), QStringLiteral("Liquid Production Total")},
        {QStringLiteral("VPT"), QStringLiteral("Reservoir Volume Production Total")},
        {QStringLiteral("GPTF"), QStringLiteral("Free Gas Production Total")},
        {QStringLiteral("GPTS"), QStringLiteral("Solution Gas Production Total")},
        {QStringLiteral("OPTF"), QStringLiteral("Free Oil Production Total")},
        {QStringLiteral("OPTS"), QStringLiteral("Solution Oil Production Total")},
        {QStringLiteral("OIR"), QStringLiteral("Oil Injection Rate")},
        {QStringLiteral("WIR"), QStringLiteral("Water Injection Rate")},
        {QStringLiteral("GIR"), QStringLiteral("Gas Injection Rate")},
        {QStringLiteral("VIR"), QStringLiteral("Reservoir Volume Injection Rate")},
        {QStringLiteral("OIT"), QStringLiteral("Oil Injection Total")},
        {QStringLiteral("WIT"), QStringLiteral("Water Injection Total")},
        {QStringLiteral("GIT"), QStringLiteral("Gas Injection Total")},
        {QStringLiteral("VIT"), QStringLiteral("Reservoir Volume Injection Total")},
        {QStringLiteral("OPP"), QStringLiteral("Oil Production Potential")},
        {QStringLiteral("WPP"), QStringLiteral("Water Production Potential")},
        {QStringLiteral("GPP"), QStringLiteral("Gas Production Potential")},
        {QStringLiteral("LPP"), QStringLiteral("Liquid Production Potential")},
        {QStringLiteral("WCT"), QStringLiteral("Water Cut")},
        {QStringLiteral("GOR"), QStringLiteral("Gas-Oil Ratio")},
        {QStringLiteral("OGR"), QStringLiteral("Oil-Gas Ratio")},
        {QStringLiteral("WGR"), QStringLiteral("Water-Gas Ratio")},
        {QStringLiteral("GLR"), QStringLiteral("Gas-Liquid Ratio")},
        {QStringLiteral("WOR"), QStringLiteral("Water-Oil Ratio")},
        {QStringLiteral("BHP"), QStringLiteral("Bottom Hole Pressure")},
        {QStringLiteral("THP"), QStringLiteral("Tubing Head Pressure")},
        {QStringLiteral("PR"), QStringLiteral("Average Reservoir Pressure")},
        {QStringLiteral("BP"), QStringLiteral("Well Block Pressure")},
        {QStringLiteral("BP4"), QStringLiteral("Well Block Pressure (Four-Point Average)")},
        {QStringLiteral("BP5"), QStringLiteral("Well Block Pressure (Five-Point Average)")},
        {QStringLiteral("BP9"), QStringLiteral("Well Block Pressure (Nine-Point Average)")},
        {QStringLiteral("PI"), QStringLiteral("Productivity Index")},
        {QStringLiteral("PIO"), QStringLiteral("Oil Productivity Index")},
        {QStringLiteral("PIG"), QStringLiteral("Gas Productivity Index")},
        {QStringLiteral("PIW"), QStringLiteral("Water Productivity Index")},
        {QStringLiteral("PIL"), QStringLiteral("Liquid Productivity Index")},
        {QStringLiteral("PI1"), QStringLiteral("Productivity Index Based on Well Block Pressure")},
        {QStringLiteral("II"), QStringLiteral("Injectivity Index")},
        {QStringLiteral("IIO"), QStringLiteral("Oil Injectivity Index")},
        {QStringLiteral("IIW"), QStringLiteral("Water Injectivity Index")},
        {QStringLiteral("IIG"), QStringLiteral("Gas Injectivity Index")},
        {QStringLiteral("IIL"), QStringLiteral("Liquid Injectivity Index")},
        {QStringLiteral("OIP"), QStringLiteral("Oil In Place")},
        {QStringLiteral("WIP"), QStringLiteral("Water In Place")},
        {QStringLiteral("GIP"), QStringLiteral("Gas In Place")},
        {QStringLiteral("NIP"), QStringLiteral("Solvent In Place")},
        {QStringLiteral("SIP"), QStringLiteral("Salt In Place")},
        {QStringLiteral("OIPL"), QStringLiteral("Oil In Place (Liquid Phase)")},
        {QStringLiteral("OIPG"), QStringLiteral("Oil In Place (Vaporized in Gas Phase)")},
        {QStringLiteral("GIPL"), QStringLiteral("Gas In Place (Dissolved in Liquid Phase)")},
        {QStringLiteral("GIPG"), QStringLiteral("Gas In Place (Free in Gas Phase)")},
        {QStringLiteral("OE"), QStringLiteral("Oil Recovery Efficiency")},
        {QStringLiteral("OSAT"), QStringLiteral("Oil Saturation")},
        {QStringLiteral("WSAT"), QStringLiteral("Water Saturation")},
        {QStringLiteral("GSAT"), QStringLiteral("Gas Saturation")},
        {QStringLiteral("RS"), QStringLiteral("Solution Gas-Oil Ratio")},
        {QStringLiteral("RV"), QStringLiteral("Vapor Oil-Gas Ratio")},
        {QStringLiteral("PBUB"), QStringLiteral("Bubble Point Pressure")},
        {QStringLiteral("PDEW"), QStringLiteral("Dew Point Pressure")},
        {QStringLiteral("TEMP"), QStringLiteral("Temperature")},
        {QStringLiteral("MWPR"), QStringLiteral("Number of Producing Wells")},
        {QStringLiteral("MWIR"), QStringLiteral("Number of Injecting Wells")},
        {QStringLiteral("MWPT"), QStringLiteral("Total Number of Production Wells")},
        {QStringLiteral("MWIT"), QStringLiteral("Total Number of Injection Wells")},
        {QStringLiteral("MWPA"), QStringLiteral("Number of Abandoned Production Wells")},
        {QStringLiteral("MWIA"), QStringLiteral("Number of Abandoned Injection Wells")},
        {QStringLiteral("MWPP"), QStringLiteral("Number of Producers on Pressure Control")},
        {QStringLiteral("MWIP"), QStringLiteral("Number of Injectors on Pressure Control")},
        {QStringLiteral("MWPG"), QStringLiteral("Number of Producers on Group Control")},
        {QStringLiteral("MWIG"), QStringLiteral("Number of Injectors on Group Control")},
        {QStringLiteral("MWPU"), QStringLiteral("Number of Unused Production Wells")},
        {QStringLiteral("MWIU"), QStringLiteral("Number of Unused Injection Wells")},
        {QStringLiteral("STAT"), QStringLiteral("Well Status")},
        {QStringLiteral("MCTL"), QStringLiteral("Well Control Mode")},
        {QStringLiteral("AQR"), QStringLiteral("Aquifer Influx Rate")},
        {QStringLiteral("AQT"), QStringLiteral("Cumulative Aquifer Influx")},
        {QStringLiteral("AQP"), QStringLiteral("Aquifer Pressure")},
        {QStringLiteral("GSR"), QStringLiteral("Gas Sales Rate")},
        {QStringLiteral("GST"), QStringLiteral("Gas Sales Total")},
        {QStringLiteral("OFR"), QStringLiteral("Inter-Region Oil Flow Rate")},
        {QStringLiteral("WFR"), QStringLiteral("Inter-Region Water Flow Rate")},
        {QStringLiteral("GFR"), QStringLiteral("Inter-Region Gas Flow Rate")},
        {QStringLiteral("OPRH"), QStringLiteral("Oil Production Rate History")},
        {QStringLiteral("WPRH"), QStringLiteral("Water Production Rate History")},
        {QStringLiteral("GPRH"), QStringLiteral("Gas Production Rate History")},
        {QStringLiteral("LPRH"), QStringLiteral("Liquid Production Rate History")},
        {QStringLiteral("OPTH"), QStringLiteral("Oil Production Total History")},
        {QStringLiteral("WPTH"), QStringLiteral("Water Production Total History")},
        {QStringLiteral("GPTH"), QStringLiteral("Gas Production Total History")},
        {QStringLiteral("LPTH"), QStringLiteral("Liquid Production Total History")},
        {QStringLiteral("OIRH"), QStringLiteral("Oil Injection Rate History")},
        {QStringLiteral("OITH"), QStringLiteral("Oil Injection Total History")},
        {QStringLiteral("WIRH"), QStringLiteral("Water Injection Rate History")},
        {QStringLiteral("WITH"), QStringLiteral("Water Injection Total History")},
        {QStringLiteral("GIRH"), QStringLiteral("Gas Injection Rate History")},
        {QStringLiteral("GITH"), QStringLiteral("Gas Injection Total History")},
        {QStringLiteral("BHPH"), QStringLiteral("Bottom Hole Pressure History")},
        {QStringLiteral("THPH"), QStringLiteral("Tubing Head Pressure History")},
        {QStringLiteral("WCTH"), QStringLiteral("Water Cut History")},
        {QStringLiteral("GORH"), QStringLiteral("Gas-Oil Ratio History")},
        {QStringLiteral("GLRH"), QStringLiteral("Gas-Liquid Ratio History")},
        {QStringLiteral("WORH"), QStringLiteral("Water-Oil Ratio History")},
        {QStringLiteral("OGRH"), QStringLiteral("Oil-Gas Ratio History")},
        {QStringLiteral("WGRH"), QStringLiteral("Water-Gas Ratio History")},
        {QStringLiteral("TCPU"), QStringLiteral("CPU Time")},
        {QStringLiteral("TCPUTS"), QStringLiteral("CPU Time Per Timestep")},
        {QStringLiteral("TCPUDAY"), QStringLiteral("CPU Time Per Simulation Day")},
        {QStringLiteral("TIME"), QStringLiteral("Simulation Time")},
        {QStringLiteral("YEARS"), QStringLiteral("Simulation Time in Years")},
        {QStringLiteral("ELAPSED"), QStringLiteral("Elapsed Wall-Clock Time")},
        {QStringLiteral("TIMESTEP"), QStringLiteral("Timestep Length")},
        {QStringLiteral("NEWTON"), QStringLiteral("Number of Newton Iterations")},
        {QStringLiteral("NLINEARS"), QStringLiteral("Number of Linear Iterations")},
        {QStringLiteral("MLINEARS"), QStringLiteral("Average Linear Iterations Per Timestep")},
        {QStringLiteral("MSUMLINS"), QStringLiteral("Cumulative Linear Iterations")},
        {QStringLiteral("MSUMNEWT"), QStringLiteral("Cumulative Newton Iterations")},
        {QStringLiteral("STEPTYPE"), QStringLiteral("Step Type")},
    };

    QString body = keyword;
    if (hasScopeLetter(cat) && body.size() > 1)
        body = body.mid(1);

    auto it = table.constFind(body);
    if (it != table.constEnd()) return it.value();

    // history variant not listed explicitly: OPRH -> "Oil Production Rate History"
    if (body.endsWith(QLatin1Char('H')) && body.size() > 1) {
        it = table.constFind(body.left(body.size() - 1));
        if (it != table.constEnd()) return it.value() + QStringLiteral(" History");
    }
    // miscellaneous vectors keyed by full keyword
    it = table.constFind(keyword);
    if (it != table.constEnd()) return it.value();

    return QString();   // unknown -> caller shows the raw keyword
}

// ---------------------------------------------------------------------------
SummaryPlotWidget::SummaryPlotWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* top = new QVBoxLayout(this);

    // --- case row ------------------------------------------------------------
    {
        auto* row = new QHBoxLayout;
        row->addWidget(new QLabel(QStringLiteral("Case:")));
        caseBox_ = new QComboBox;
        caseBox_->setMinimumWidth(280);
        row->addWidget(caseBox_, 1);
        auto* bbrowse  = new QPushButton(QStringLiteral("Open SMSPEC..."));
        auto* brefresh = new QPushButton(QStringLiteral("Refresh"));
        autoRef_ = new QCheckBox(QStringLiteral("auto-refresh (10 s)"));
        dateAxis_ = new QCheckBox(QStringLiteral("date axis"));
        auto* bzoom = new QPushButton(QStringLiteral("Reset zoom"));
        auto* bpng  = new QPushButton(QStringLiteral("Save PNG..."));
        row->addWidget(bbrowse);
        row->addWidget(brefresh);
        row->addWidget(autoRef_);
        row->addWidget(dateAxis_);
        row->addWidget(bzoom);
        row->addWidget(bpng);
        top->addLayout(row);

        connect(bbrowse,  &QPushButton::clicked, this, [this] { browseCase(); });
        connect(brefresh, &QPushButton::clicked, this, [this] { reload(true); });
        connect(dateAxis_, &QCheckBox::toggled, this, [this](bool) { replot(); });
        connect(bzoom, &QPushButton::clicked, this, [this] { chart_->zoomReset(); });
        connect(bpng,  &QPushButton::clicked, this, [this] { savePng(); });
        connect(caseBox_, &QComboBox::currentIndexChanged, this, [this](int) { reload(false); });
        timer_ = new QTimer(this);
        timer_->setInterval(10000);
        connect(timer_, &QTimer::timeout, this, [this] { reload(true); });
        connect(autoRef_, &QCheckBox::toggled, this, [this](bool on) {
            if (on) timer_->start(); else timer_->stop();
        });
    }

    // --- filter row ----------------------------------------------------------
    {
        auto* row = new QHBoxLayout;
        row->addWidget(new QLabel(QStringLiteral("Category:")));
        catBox_ = new QComboBox;  row->addWidget(catBox_);
        row->addWidget(new QLabel(QStringLiteral("Type:")));
        typeBox_ = new QComboBox; row->addWidget(typeBox_);
        row->addWidget(new QLabel(QStringLiteral("Item:")));
        itemBox_ = new QComboBox; itemBox_->setMinimumWidth(120); row->addWidget(itemBox_);
        subLabel_ = new QLabel(QStringLiteral("Cell:"));
        subItemBox_ = new QComboBox; subItemBox_->setMinimumWidth(110);
        subLabel_->hide(); subItemBox_->hide();
        row->addWidget(subLabel_); row->addWidget(subItemBox_);
        filter_ = new QLineEdit;
        filter_->setPlaceholderText(QStringLiteral("search..."));
        row->addWidget(filter_, 1);
        top->addLayout(row);

        // Cascade: category -> item -> cell; each level repopulates the next.
        connect(catBox_,  &QComboBox::currentIndexChanged, this,
                [this](int) { populateItemBox(); rebuildTree({}); });
        connect(typeBox_, &QComboBox::currentIndexChanged, this, [this](int) { rebuildTree({}); });
        connect(itemBox_, &QComboBox::currentIndexChanged, this,
                [this](int) { populateSubItemBox(); rebuildTree({}); });
        connect(subItemBox_, &QComboBox::currentIndexChanged, this, [this](int) { rebuildTree({}); });
        connect(filter_,  &QLineEdit::textChanged,         this, [this] { rebuildTree({}); });
    }

    // --- tree + chart --------------------------------------------------------
    auto* split = new QSplitter;
    {
        tree_ = new QTreeWidget;
        tree_->setHeaderHidden(true);
        tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        tree_->setUniformRowHeights(true);
        split->addWidget(tree_);

        chart_ = new QChart;
        chart_->legend()->setVisible(true);
        chart_->legend()->setAlignment(Qt::AlignBottom);
        chartView_ = new QChartView(chart_);
        chartView_->setRenderHint(QPainter::Antialiasing);
        chartView_->setRubberBand(QChartView::RectangleRubberBand);   // drag to zoom
        split->addWidget(chartView_);
        split->setStretchFactor(0, 0);
        split->setStretchFactor(1, 1);
        split->setSizes({ 320, 680 });
        top->addWidget(split, 1);

        connect(tree_, &QTreeWidget::itemSelectionChanged, this, [this] { replot(); });
    }

    status_ = new QLabel;
    top->addWidget(status_);
    setStatus(QStringLiteral("run a job (or open an SMSPEC) to plot summary vectors"));
}

SummaryPlotWidget::~SummaryPlotWidget() = default;

void SummaryPlotWidget::setStatus(const QString& s) { status_->setText(s); }

void SummaryPlotWidget::savePng()
{
    QString suggested = caseBox_->currentText();
    if (suggested.isEmpty()) suggested = QStringLiteral("summary");
    const QString f = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save chart as PNG"), suggested + QStringLiteral(".png"),
        QStringLiteral("PNG image (*.png)"));
    if (f.isEmpty()) return;
    if (chartView_->grab().save(f))
        setStatus(QStringLiteral("chart saved to %1").arg(QDir::toNativeSeparators(f)));
    else
        setStatus(QStringLiteral("could not save %1").arg(QDir::toNativeSeparators(f)));
}

void SummaryPlotWidget::addCase(const QString& label, const QString& smspecPath)
{
    for (int i = 0; i < caseBox_->count(); ++i)
        if (caseBox_->itemData(i).toString() == smspecPath) return;
    caseBox_->addItem(label, smspecPath);
    if (caseBox_->count() == 1) caseBox_->setCurrentIndex(0);
}

void SummaryPlotWidget::browseCase()
{
    const QString f = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open summary specification"), QString(),
        QStringLiteral("Summary spec (*.SMSPEC);;All files (*)"));
    if (!f.isEmpty()) {
        addCase(QFileInfo(f).completeBaseName(), f);
        caseBox_->setCurrentIndex(caseBox_->count() - 1);
    }
}

// ---------------------------------------------------------------------------
void SummaryPlotWidget::reload(bool keepSelection)
{
    const QString path = caseBox_->currentData().toString();
    if (path.isEmpty()) return;
    if (!QFileInfo::exists(path)) {
        setStatus(QStringLiteral("waiting for %1 ...").arg(path));
        return;
    }

    QStringList reselect;
    if (keepSelection)
        for (auto* it : tree_->selectedItems())
            if (it->data(0, RoleVecIndex).isValid())
                reselect << it->data(0, RoleVecIndex).toString();   // key stored below

    // Re-open for a fresh snapshot; while flow is still writing, a read can
    // transiently fail - keep the previous data and try again next refresh.
    std::unique_ptr<ESmry> next;
    try {
        next = std::make_unique<ESmry>(path.toStdString());
    } catch (const std::exception& e) {
        setStatus(QStringLiteral("could not read summary (still being written?): %1")
                      .arg(QString::fromLocal8Bit(e.what())));
        return;
    }
    smry_ = std::move(next);

    // Grid dimensions from the SMSPEC's DIMENS array (needed to show block /
    // connection cells as I,J,K); tolerated to fail -> plain numbers.
    nx_ = ny_ = nz_ = 0;
    try {
        Opm::EclIO::EclFile spec(path.toStdString());
        if (spec.hasKey("DIMENS")) {
            const auto dims = spec.get<int>("DIMENS");
            if (dims.size() >= 4) { nx_ = dims[1]; ny_ = dims[2]; nz_ = dims[3]; }
        }
    } catch (...) {}

    // Parse every summary node into a plottable Vec.
    vecs_.clear();
    for (const auto& node : smry_->summaryNodeList()) {
        Vec v;
        v.node    = node;
        v.keyword = QString::fromStdString(node.keyword);
        splitItem(node, nx_, ny_, v.itemMain, v.itemSub);
        v.item    = v.itemSub.isEmpty() ? v.itemMain
                                        : v.itemMain + QLatin1Char(':') + v.itemSub;
        v.cat     = node.category;
        v.type    = node.type;
        v.key     = v.item.isEmpty() ? v.keyword : (v.keyword + QLatin1Char(':') + v.item);
        try { v.unit = QString::fromStdString(smry_->get_unit(node)).trimmed(); }
        catch (...) { v.unit.clear(); }
        vecs_.push_back(v);
    }

    rebuildFilters();
    rebuildTree(reselect);

    setStatus(QStringLiteral("%1: %2 vectors, %3 timesteps")
                  .arg(QFileInfo(path).completeBaseName())
                  .arg(vecs_.size())
                  .arg(int(smry_->numberOfTimeSteps())));
}

void SummaryPlotWidget::rebuildFilters()
{
    // Category and Type boxes list only the values actually present.
    const QString prevCat  = catBox_->currentText();
    const QString prevType = typeBox_->currentText();

    QSet<int> cats, types;
    for (const auto& v : vecs_) { cats.insert(int(v.cat)); types.insert(int(v.type)); }

    catBox_->blockSignals(true);
    catBox_->clear();
    catBox_->addItem(QStringLiteral("All"), -1);
    static const Cat order[] = { Cat::Field, Cat::Well, Cat::Group, Cat::Region,
        Cat::Block, Cat::Connection, Cat::Completion, Cat::Segment, Cat::Aquifer,
        Cat::Node, Cat::Miscellaneous };
    for (Cat c : order)
        if (cats.contains(int(c))) catBox_->addItem(categoryName(c), int(c));
    int ci = catBox_->findText(prevCat);
    catBox_->setCurrentIndex(ci < 0 ? 0 : ci);
    catBox_->blockSignals(false);

    typeBox_->blockSignals(true);
    typeBox_->clear();
    typeBox_->addItem(QStringLiteral("All"), -1);
    static const Type torder[] = { Type::Rate, Type::Total, Type::Ratio,
        Type::Pressure, Type::Count, Type::Mode, Type::ProdIndex, Type::Undefined };
    for (Type t : torder)
        if (types.contains(int(t))) typeBox_->addItem(typeName(t), int(t));
    int ti = typeBox_->findText(prevType);
    typeBox_->setCurrentIndex(ti < 0 ? 0 : ti);
    typeBox_->blockSignals(false);

    populateItemBox();
}

// Item box lists the FIRST-LEVEL items (well/group names, region numbers,
// block cells, ...) of the selected category, "All items" first. The second
// level (cells of a well, for Connection/Completion/Segment) lives in the
// cascading Cell box, populated by populateSubItemBox().
void SummaryPlotWidget::populateItemBox()
{
    const QString prevItem = itemBox_->currentText();
    const int selCat = catBox_->currentData().toInt();
    QSet<QString> items;
    for (const auto& v : vecs_)
        if ((selCat < 0 || int(v.cat) == selCat) && !v.itemMain.isEmpty())
            items.insert(v.itemMain);
    QStringList sorted(items.begin(), items.end());
    sortItems(sorted);
    itemBox_->blockSignals(true);
    itemBox_->clear();
    itemBox_->addItem(QStringLiteral("All items"), QString());
    for (const QString& s : sorted) itemBox_->addItem(s, s);
    int ii = itemBox_->findText(prevItem);
    itemBox_->setCurrentIndex(ii < 0 ? 0 : ii);
    itemBox_->blockSignals(false);

    populateSubItemBox();
}

// Cell box: visible only when the current category selection carries
// two-level items (connections/completions/segments); enabled once a
// specific first-level item (well) is chosen, listing that well's cells.
void SummaryPlotWidget::populateSubItemBox()
{
    const int     selCat  = catBox_->currentData().toInt();
    const QString selMain = itemBox_->currentData().toString();
    const QString prevSub = subItemBox_->currentText();

    bool anySub = false;
    QSet<QString> subs;
    for (const auto& v : vecs_) {
        if (selCat >= 0 && int(v.cat) != selCat) continue;
        if (v.itemSub.isEmpty()) continue;
        anySub = true;
        if (!selMain.isEmpty() && v.itemMain == selMain) subs.insert(v.itemSub);
    }

    subItemBox_->blockSignals(true);
    subItemBox_->clear();
    if (!anySub) {
        subLabel_->hide(); subItemBox_->hide();
    } else {
        subLabel_->show(); subItemBox_->show();
        subItemBox_->addItem(QStringLiteral("All"), QString());
        if (selMain.isEmpty()) {
            subItemBox_->setEnabled(false);   // pick a well first
        } else {
            subItemBox_->setEnabled(true);
            QStringList sorted(subs.begin(), subs.end());
            sortItems(sorted);
            for (const QString& s : sorted) subItemBox_->addItem(s, s);
            int si = subItemBox_->findText(prevSub);
            subItemBox_->setCurrentIndex(si < 0 ? 0 : si);
        }
    }
    subItemBox_->blockSignals(false);
}

void SummaryPlotWidget::rebuildTree(const QStringList& reselect)
{
    const int     selCat  = catBox_->currentData().toInt();
    const int     selType = typeBox_->currentData().toInt();
    const QString selItem = itemBox_->currentData().toString();
    const QString selSub  = (subItemBox_->isVisible() && subItemBox_->isEnabled())
                                ? subItemBox_->currentData().toString() : QString();
    const QString search  = filter_->text().trimmed();

    // Preserve the selection across any rebuild (filter change or refresh):
    // seed the keep-set from the passed-in list AND the current selection.
    // Vectors that survive the new filter stay selected; hidden ones drop out.
    QSet<QString> keep(reselect.begin(), reselect.end());
    for (auto* it : tree_->selectedItems()) {
        const QVariant k = it->data(0, RoleVecIndex);
        if (k.isValid()) keep.insert(k.toString());
    }

    tree_->blockSignals(true);
    tree_->clear();

    // Group the filtered vectors by keyword; each group's children are the
    // matching items. A group with a single member is shown as a leaf.
    QHash<QString, QList<int>> byKeyword;   // keyword -> indices into vecs_
    QStringList keywordOrder;
    for (int i = 0; i < vecs_.size(); ++i) {
        const Vec& v = vecs_[i];
        if (selCat  >= 0 && int(v.cat)  != selCat)  continue;
        if (selType >= 0 && int(v.type) != selType) continue;
        if (!selItem.isEmpty() && v.itemMain != selItem) continue;
        if (!selSub.isEmpty()  && v.itemSub  != selSub)  continue;
        if (!search.isEmpty()) {
            const QString fn = friendlyName(v.keyword, v.cat);
            if (!v.key.contains(search, Qt::CaseInsensitive) &&
                !fn.contains(search, Qt::CaseInsensitive))
                continue;
        }
        if (!byKeyword.contains(v.keyword)) keywordOrder << v.keyword;
        byKeyword[v.keyword] << i;
    }
    keywordOrder.sort();

    for (const QString& kw : std::as_const(keywordOrder)) {
        const QList<int>& idxs = byKeyword[kw];
        const Vec& first = vecs_[idxs.first()];
        const QString fn = friendlyName(kw, first.cat);
        const QString kwLabel = fn.isEmpty() ? kw : (kw + QStringLiteral("  -  ") + fn);

        if (idxs.size() == 1) {
            const Vec& v = vecs_[idxs.first()];
            auto* leaf = new QTreeWidgetItem(tree_);
            leaf->setText(0, v.item.isEmpty() ? kwLabel
                                              : (kwLabel + QStringLiteral("  [") + v.item + QLatin1Char(']')));
            leaf->setData(0, RoleVecIndex, v.key);
            leaf->setData(0, RoleVecIndex + 1, idxs.first());
            if (keep.contains(v.key)) leaf->setSelected(true);
        } else {
            auto* grp = new QTreeWidgetItem(tree_);
            grp->setText(0, kwLabel);
            grp->setFlags(grp->flags() & ~Qt::ItemIsSelectable);   // select children, not the group
            for (int idx : idxs) {
                const Vec& v = vecs_[idx];
                auto* leaf = new QTreeWidgetItem(grp);
                leaf->setText(0, v.item.isEmpty() ? kw : v.item);
                leaf->setData(0, RoleVecIndex, v.key);
                leaf->setData(0, RoleVecIndex + 1, idx);
                if (keep.contains(v.key)) leaf->setSelected(true);
            }
        }
    }
    tree_->blockSignals(false);

    // Selection was set with signals blocked (or cleared by clear()); sync the
    // chart to whatever is now selected.
    replot();
}

// ---------------------------------------------------------------------------
void SummaryPlotWidget::replot()
{
    chart_->removeAllSeries();     // series are deleted by Qt
    const auto oldAxes = chart_->axes();
    for (auto* a : oldAxes) {
        chart_->removeAxis(a);     // removeAxis returns ownership to us: delete
        delete a;
    }
    if (!smry_) return;

    std::vector<float> time;
    try {
        if (smry_->hasKey("TIME")) time = smry_->get(std::string("TIME"));
    } catch (...) {}
    if (time.empty()) { chart_->setTitle(QString()); return; }

    // Collect the selected leaves (indices into vecs_).
    QList<int> sel;
    for (auto* it : tree_->selectedItems()) {
        const QVariant idx = it->data(0, RoleVecIndex + 1);
        if (idx.isValid()) sel << idx.toInt();
    }
    if (sel.isEmpty()) { chart_->setTitle(caseBox_->currentText()); return; }

    // Two Y axes at most, keyed by unit. The left axis carries the first
    // distinct unit (or is a generic "value" axis when the selection has no
    // units); a second distinct unit gets the right axis. Series whose unit is
    // a third distinct one would distort a mismatched axis, so they are not
    // plotted and are reported in the status line instead.
    QString unitL, unitR;
    bool haveL = false, haveR = false;
    for (int i : sel) {
        const QString u = vecs_[i].unit;
        if (u.isEmpty()) continue;
        if      (!haveL)            { unitL = u; haveL = true; }
        else if (u != unitL && !haveR) { unitR = u; haveR = true; }
    }
    // If nothing had a unit, the left axis is a generic value axis that takes
    // every series.
    const bool genericLeft = !haveL;

    auto axisFor = [&](const QString& u) -> int {   // 0 left, 1 right, -1 skip
        if (genericLeft) return 0;
        if (u == unitL)  return 0;
        if (haveR && u == unitR) return 1;
        return -1;
    };

    double lmin = 0, lmax = 0, rmin = 0, rmax = 0; bool lset = false, rset = false;

    // X axis: simulated days, or calendar dates (UTC) when toggled on. Series
    // x-values are stored to match the axis type (days vs ms-since-epoch).
    const bool useDates = dateAxis_ && dateAxis_->isChecked();
    double startMs = 0.0;
    if (useDates) {
        const auto tp = smry_->startdate();
        startMs = double(std::chrono::duration_cast<std::chrono::milliseconds>(
                             tp.time_since_epoch()).count());
    }
    auto xval = [&](float days) {
        return useDates ? startMs + double(days) * 86400.0e3 : double(days);
    };

    QAbstractAxis* ax = nullptr;
    if (useDates) {
        auto* a = new QDateTimeAxis;
        a->setFormat(QStringLiteral("yyyy-MM-dd"));
        a->setTitleText(QStringLiteral("date"));
        ax = a;
    } else {
        auto* a = new QValueAxis;
        a->setTitleText(QStringLiteral("time [days]"));
        ax = a;
    }
    chart_->addAxis(ax, Qt::AlignBottom);
    QValueAxis* ayL = new QValueAxis;
    ayL->setTitleText(genericLeft ? QStringLiteral("value") : unitL);
    chart_->addAxis(ayL, Qt::AlignLeft);
    QValueAxis* ayR = nullptr;
    if (haveR) {
        ayR = new QValueAxis; ayR->setTitleText(unitR);
        chart_->addAxis(ayR, Qt::AlignRight);
    }

    int skipped = 0;
    for (int i : sel) {
        const Vec& v = vecs_[i];
        const int side = axisFor(v.unit);
        if (side < 0) { ++skipped; continue; }
        std::vector<float> data;
        try { data = smry_->get(v.node); } catch (...) { continue; }
        auto* s = new QLineSeries;
        s->setName(v.key);
        const size_t n = std::min(time.size(), data.size());
        for (size_t k = 0; k < n; ++k) {
            s->append(xval(time[k]), data[k]);
            if (side == 1) {
                rmin = rset ? std::min<double>(rmin, data[k]) : data[k];
                rmax = rset ? std::max<double>(rmax, data[k]) : data[k];
                rset = true;
            } else {
                lmin = lset ? std::min<double>(lmin, data[k]) : data[k];
                lmax = lset ? std::max<double>(lmax, data[k]) : data[k];
                lset = true;
            }
        }
        chart_->addSeries(s);
        s->attachAxis(ax);
        s->attachAxis(side == 1 ? ayR : ayL);
    }

    if (!time.empty()) {
        if (useDates) {
            static_cast<QDateTimeAxis*>(ax)->setRange(
                QDateTime::fromMSecsSinceEpoch(qint64(xval(time.front())), QTimeZone::utc()),
                QDateTime::fromMSecsSinceEpoch(qint64(xval(time.back())),  QTimeZone::utc()));
        } else {
            static_cast<QValueAxis*>(ax)->setRange(time.front(), time.back());
        }
    }
    auto pad = [](QValueAxis* a, double lo, double hi) {
        if (hi > lo) a->setRange(lo - 0.05 * (hi - lo), hi + 0.05 * (hi - lo));
        else         a->setRange(lo - 1.0, hi + 1.0);
    };
    if (lset) pad(ayL, lmin, lmax);
    if (ayR && rset) pad(ayR, rmin, rmax);
    chart_->setTitle(caseBox_->currentText());
    if (skipped > 0)
        setStatus(QStringLiteral("%1 selected vector(s) not shown - a plot mixes at "
                                 "most two units; deselect to change the pair").arg(skipped));
}
