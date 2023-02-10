#include "protobasedialog.h"
#include "ui_protobasedialog.h"

ProtoBaseDialog::ProtoBaseDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::ProtoBaseDialog)
{
    ui->setupUi(this);
}

ProtoBaseDialog::~ProtoBaseDialog()
{
    delete ui;
}

bool ProtoBaseDialog::closeOnDone()
{
    return ui->checkBox->isChecked();
}

void ProtoBaseDialog::setPath(QString path)
{
    QString msg = tr(
            "这可能会花一些时间\n"
            "结果会被保存到 \"%1\" "
            "所以接下来的搜索会更快 ").arg(path);
    ui->label->setText(msg);
}
