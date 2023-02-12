#include "updater.h"

#include "aboutdialog.h"

#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>
#include <QMessageBox>
#include <QDesktopServices>


void replyFinished(QNetworkReply *reply, bool quiet)
{
    if (reply->error() != QNetworkReply::NoError)
    {
        if (!quiet)
        {
            QString msg = QObject::tr("检查更新失败:\n");
            switch (reply->error())
            {
            case QNetworkReply::ConnectionRefusedError:
                msg += QObject::tr("拒绝连接");
                break;
            case QNetworkReply::HostNotFoundError:
                msg += QObject::tr("未找到主机");
                break;
            case QNetworkReply::TimeoutError:
                msg += QObject::tr("连接超时");
                break;
            default:
                msg += QObject::tr("网络错误(%1).").arg(reply->error());
                break;
            }
            QMessageBox::warning(NULL, QObject::tr("连接错误"), msg, QMessageBox::Ok);
        }
        return;
    }

    QJsonParseError jerr;
    QJsonDocument document = QJsonDocument::fromJson(reply->readAll(), &jerr);

    if (jerr.error != QJsonParseError::NoError)
    {
        QMessageBox::warning(
            NULL, QObject::tr("Json错误"),
            QObject::tr("解析Json失败:\n%1").arg(jerr.errorString()),
            QMessageBox::Ok);
        return;
    }

    QJsonArray jarr = document.array();
    QString newest;

    if (getVersStr().contains("dev"))
    {
        newest = jarr[0].toObject().take("tag_name").toString();
    }
    else
    {
        for (auto jval : jarr)
        {
            if (!jval.toObject().take("prerelease").toBool())
            {
                newest = jval.toObject().take("tag_name").toString();
                break;
            }
        }
    }

    if (newest == getVersStr())
    {
        if (!quiet)
        {
            QMessageBox::information(
                NULL, QObject::tr("没有更新"),
                QObject::tr("你使用的Cubiomes Viewer是最新版"));
        }
        return;
    }

    QMessageBox::StandardButton answer = QMessageBox::question(
        NULL, QObject::tr("发现新版本"),
        QObject::tr("<p>发现新版本: <b>%1</b></p><p>是否在浏览器中打开下载链接？</p>").arg(newest),
        QMessageBox::Yes|QMessageBox::No);

    if (answer == QMessageBox::Yes)
    {
        QString url = QString("https://github.com/Cubitect/cubiomes-viewer/releases/tag/%1").arg(newest);
        QDesktopServices::openUrl(QUrl(url));
    }
}

void searchForUpdates(bool quiet)
{
    QNetworkAccessManager *manager = new QNetworkAccessManager();
    QObject::connect(manager, &QNetworkAccessManager::finished,
        [=](QNetworkReply *reply) {
            replyFinished(reply, quiet);
            manager->deleteLater();
        });

    QUrl qrl("https://api.github.com/repos/Cubitect/cubiomes-viewer/releases");
    manager->get(QNetworkRequest(qrl));
}

