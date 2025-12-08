#include "gotodialog.h"
#include "ui_gotodialog.h"

#include "mapview.h"

#include <QClipboard>
#include <QDoubleValidator>
#include <QKeyEvent>
#include <QRegularExpression>

static bool g_animate;

GotoDialog::GotoDialog(MapView *map, qreal x, qreal z, qreal scale)
    : QDialog(map)
    , ui(new Ui::GotoDialog)
    , mapview(map)
{
    ui->setupUi(this);

    scalemin = 1.0 / 4096;
    scalemax = 65536;
    ui->lineX->setValidator(new QDoubleValidator(-3e7, 3e7, 1, ui->lineX));
    ui->lineZ->setValidator(new QDoubleValidator(-3e7, 3e7, 1, ui->lineZ));
    ui->lineScale->setValidator(new QDoubleValidator(scalemin, scalemax, 16, ui->lineScale));

    ui->lineX->setText(QString::asprintf("%.1f", x));
    ui->lineZ->setText(QString::asprintf("%.1f", z));
    ui->lineScale->setText(QString::asprintf("%.4f", scale));

    ui->checkAnimate->setChecked(g_animate);
}

GotoDialog::~GotoDialog()
{
    delete ui;
}

bool GotoDialog::parseCoordinates(const QString &input, qreal &x, qreal &z)
{
    QString text = input.trimmed();
    if (text.isEmpty())
        return false;

    // 移除 /tp 命令前缀（如果存在）
    if (text.startsWith("/tp", Qt::CaseInsensitive))
    {
        text = text.mid(3).trimmed();
    }

    // 使用正则表达式提取所有数字
    QRegularExpression numberRegex("-?\\d+\\.?\\d*");
    QRegularExpressionMatchIterator matches = numberRegex.globalMatch(text);

    QList<qreal> numbers;
    while (matches.hasNext())
    {
        QRegularExpressionMatch match = matches.next();
        bool ok;
        qreal num = match.captured().toDouble(&ok);
        if (ok)
        {
            numbers.append(num);
        }
    }

    // 需要至少2个数字（x和z）
    if (numbers.size() < 2)
        return false;

    // 第一个数字是x，最后一个数字是z（忽略中间的y坐标）
    x = numbers.first();
    z = numbers.last();

    return true;
}

void GotoDialog::on_buttonInterpret_clicked()
{
    QString coordInput = ui->lineCoordInput->text().trimmed();
    if (!coordInput.isEmpty())
    {
        qreal x, z;
        if (parseCoordinates(coordInput, x, z))
        {
            ui->lineX->setText(QString::asprintf("%.1f", x));
            ui->lineZ->setText(QString::asprintf("%.1f", z));
        }
    }
}

void GotoDialog::on_buttonBox_clicked(QAbstractButton *button)
{
    QDialogButtonBox::StandardButton b = ui->buttonBox->standardButton(button);

    if (b == QDialogButtonBox::Ok || b == QDialogButtonBox::Apply)
    {
        qreal x = ui->lineX->text().toDouble();
        qreal z = ui->lineZ->text().toDouble();
        qreal scale = ui->lineScale->text().toDouble();
        if (scale < scalemin) scale = scalemin;
        if (scale > scalemax) scale = scalemax;
        ui->lineScale->setText(QString::asprintf("%.4f", scale));
        g_animate = ui->checkAnimate->isChecked();
        if (g_animate)
            mapview->animateView(x, z, scale);
        else
            mapview->setView(x, z, scale);
    }
    else if (b == QDialogButtonBox::Reset)
    {
        ui->lineX->setText("0");
        ui->lineZ->setText("0");
        ui->lineScale->setText("16");
        ui->lineCoordInput->clear();
    }
}

void GotoDialog::on_lineScale_textChanged(const QString &text)
{
    qreal value = text.toDouble();
    ui->lineScale->setStyleSheet(value > 4096 ? "color: red" : "");
}

void GotoDialog::keyPressEvent(QKeyEvent *event)
{
    static QRegularExpression coord_delim = QRegularExpression("[, ]+");

    if (event->matches(QKeySequence::Paste))
    {
        QClipboard *clipboard = QGuiApplication::clipboard();
        QString s = clipboard->text().trimmed();
        QStringList xz = s.split(coord_delim);
        if (xz.count() == 2)
        {
            ui->lineX->setText(xz[0]);
            ui->lineZ->setText(xz[1]);
            return;
        }
        else if (xz.count() == 3)
        {
            ui->lineX->setText(xz[0]);
            ui->lineZ->setText(xz[2]);
            return;
        }
    }
    QWidget::keyReleaseEvent(event);
}


