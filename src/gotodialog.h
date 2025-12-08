#ifndef GOTODIALOG_H
#define GOTODIALOG_H

#include <QDialog>
#include <QAbstractButton>

namespace Ui {
class GotoDialog;
}
class MapView;

class GotoDialog : public QDialog
{
    Q_OBJECT

public:
    explicit GotoDialog(MapView *map, qreal x, qreal z, qreal scale);
    ~GotoDialog();

    void keyPressEvent(QKeyEvent *event) override;

private slots:
    void on_lineScale_textChanged(const QString &text);
    void on_buttonBox_clicked(QAbstractButton *button);
    void on_buttonInterpret_clicked();

private:
    bool parseCoordinates(const QString &input, qreal &x, qreal &z);

private:
    Ui::GotoDialog *ui;
    MapView *mapview;
    qreal scalemin, scalemax;
};

#endif // GOTODIALOG_H
