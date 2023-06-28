// SPDX-FileCopyrightText: 2023 Rachel Powers <508861+Ryex@users.noreply.github.com>
//
// SPDX-License-Identifier: GPL-3.0-only

/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2023 Rachel Powers <508861+Ryex@users.noreply.github.com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 3.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "PrismExternalUpdater.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QMessageBox>
#include <QProcess>
#include <QProgressDialog>
#include <QSettings>
#include <QTimer>
#include <memory>

#include "StringUtils.h"

#include "BuildConfig.h"

#include "ui/dialogs/UpdateAvailableDialog.h"

class PrismExternalUpdater::Private {
   public:
    QDir appDir;
    QDir dataDir;
    QTimer updateTimer;
    bool allowBeta;
    bool autoCheck;
    double updateInterval;
    QDateTime lastCheck;
    std::unique_ptr<QSettings> settings;
};

PrismExternalUpdater::PrismExternalUpdater(const QString& appDir, const QString& dataDir)
{
    priv = new PrismExternalUpdater::Private();
    priv->appDir = QDir(appDir);
    priv->dataDir = QDir(dataDir);
    auto settings_file = priv->dataDir.absoluteFilePath("prismlauncher_update.cfg");
    priv->settings = std::make_unique<QSettings>(settings_file, QSettings::Format::IniFormat);
    priv->allowBeta = priv->settings->value("allow_beta", false).toBool();
    priv->autoCheck = priv->settings->value("auto_check", false).toBool();
    bool interval_ok;
    priv->updateInterval = priv->settings->value("update_interval", 86400).toInt(&interval_ok);
    if (!interval_ok)
        priv->updateInterval = 86400;
    auto last_check = priv->settings->value("last_check");
    if (!last_check.isNull() && last_check.isValid()) {
        priv->lastCheck = QDateTime::fromString(last_check.toString(), Qt::ISODate);
    }
    connectTimer();
    resetAutoCheckTimer();
}

PrismExternalUpdater::~PrismExternalUpdater()
{
    if (priv->updateTimer.isActive())
        priv->updateTimer.stop();
    disconnectTimer();
    priv->settings->sync();
    delete priv;
}

void PrismExternalUpdater::checkForUpdates()
{
    QProgressDialog progress(tr("Checking for updates..."), "", 0, 0);
    progress.setCancelButton(nullptr);
    progress.show();
    QCoreApplication::processEvents();

    QProcess proc;
    auto exe_name = QStringLiteral("%1_updater").arg(BuildConfig.LAUNCHER_APP_BINARY_NAME);
#if defined Q_OS_WIN32
    exe_name.append(".exe");
#endif

    QStringList args = { "--check-only", "--dir", priv->dataDir.absolutePath(), "--debug" };
    if (priv->allowBeta)
        args.append("--pre-release");

    proc.start(priv->appDir.absoluteFilePath(exe_name), args);
    auto result_start = proc.waitForStarted(5000);
    if (!result_start) {
        auto err = proc.error();
        qDebug() << "Failed to start updater after 5 seconds."
                 << "reason:" << err << proc.errorString();
    }
    QCoreApplication::processEvents();

    auto result_finished = proc.waitForFinished(60000);
    if (!result_finished) {
        auto err = proc.error();
        qDebug() << "Updater failed to close after 60 seconds."
                 << "reason:" << err << proc.errorString();
    }

    auto exit_code = proc.exitCode();

    auto std_output = proc.readAllStandardOutput();
    auto std_error = proc.readAllStandardError();

    progress.hide();
    QCoreApplication::processEvents();

    switch (exit_code) {
        case 0:
            // no update available
            {
                qDebug() << "No update available";
                auto msgBox = QMessageBox(QMessageBox::Information, tr("No Update Available"), tr("You are running the latest version."));
                msgBox.exec();
            }
            break;
        case 1:
            // there was an error
            {
                qDebug() << "Updater subprocess error" << qPrintable(std_error);
                auto msgBox = QMessageBox(QMessageBox::Warning, tr("Update Check Error"), tr("There was an error running the update check."));
                msgBox.setDetailedText(std_error);
                msgBox.exec();
            }
            break;
        case 100:
            // update available
            {
                auto [first_line, remainder1] = StringUtils::splitFirst(std_output, '\n');
                auto [second_line, remainder2] = StringUtils::splitFirst(remainder1, '\n');
                auto [third_line, release_notes] = StringUtils::splitFirst(remainder2, '\n');
                auto version_name = StringUtils::splitFirst(first_line, ": ").second.trimmed();
                auto version_tag = StringUtils::splitFirst(second_line, ": ").second.trimmed();
                auto release_timestamp = QDateTime::fromString(StringUtils::splitFirst(third_line, ": ").second.trimmed(), Qt::ISODate);
                qDebug() << "Update available:" << version_name << version_tag << release_timestamp;
                qDebug() << "Update release notes:" << release_notes;

                offerUpdate(version_name, version_tag, release_notes);
            }
            break;
        default:
            // unknown error code
            {
                qDebug() << "Updater exited with unknown code" << exit_code;
            }
    }
    priv->lastCheck = QDateTime::currentDateTime();
    priv->settings->setValue("last_check", priv->lastCheck.toString(Qt::ISODate));
    priv->settings->sync();
}

bool PrismExternalUpdater::getAutomaticallyChecksForUpdates()
{
    return priv->autoCheck;
}

double PrismExternalUpdater::getUpdateCheckInterval()
{
    return priv->updateInterval;
}

bool PrismExternalUpdater::getBetaAllowed()
{
    return priv->allowBeta;
}

void PrismExternalUpdater::setAutomaticallyChecksForUpdates(bool check)
{
    priv->autoCheck = check;
    priv->settings->setValue("auto_check", check);
    priv->settings->sync();
    resetAutoCheckTimer();
}

void PrismExternalUpdater::setUpdateCheckInterval(double seconds)
{
    priv->updateInterval = seconds;
    priv->settings->setValue("update_interval", seconds);
    priv->settings->sync();
    resetAutoCheckTimer();
}

void PrismExternalUpdater::setBetaAllowed(bool allowed)
{
    priv->allowBeta = allowed;
    priv->settings->setValue("auto_beta", allowed);
    priv->settings->sync();
}

void PrismExternalUpdater::resetAutoCheckTimer()
{
    int timeoutDuration = 0;
    auto now = QDateTime::currentDateTime();
    if (priv->autoCheck) {
        if (priv->lastCheck.isValid()) {
            auto diff = priv->lastCheck.secsTo(now);
            auto secs_left = priv->updateInterval - diff;
            if (secs_left < 0)
                secs_left = 0;
            timeoutDuration = secs_left * 1000;  // to msec
        }
        qDebug() << "Auto update timer starting," << timeoutDuration / 1000 << "seconds left";
        priv->updateTimer.start(timeoutDuration);
    } else {
        if (priv->updateTimer.isActive())
            priv->updateTimer.stop();
    }
}

void PrismExternalUpdater::connectTimer()
{
    connect(&priv->updateTimer, &QTimer::timeout, this, &PrismExternalUpdater::autoCheckTimerFired);
}

void PrismExternalUpdater::disconnectTimer()
{
    disconnect(&priv->updateTimer, &QTimer::timeout, this, &PrismExternalUpdater::autoCheckTimerFired);
}

void PrismExternalUpdater::autoCheckTimerFired()
{
    checkForUpdates();
}

void PrismExternalUpdater::offerUpdate(const QString& version_name, const QString& version_tag, const QString& release_notes)
{
    priv->settings->beginGroup("skip");
    auto should_skip = priv->settings->value(version_tag, false).toBool();
    priv->settings->endGroup();

    if (should_skip)
        return;

    UpdateAvailableDialog dlg(BuildConfig.printableVersionString(), version_name, release_notes);

    auto result = dlg.exec();
    switch (result) {
        case UpdateAvailableDialog::Install: {
            performUpdate(version_tag);
        }
        case UpdateAvailableDialog::Skip: {
            priv->settings->beginGroup("skip");
            priv->settings->setValue(version_tag, true);
            priv->settings->endGroup();
            priv->settings->sync();
            return;
        }
        case UpdateAvailableDialog::DontInstall: {
            return;
        }
    }
}

void PrismExternalUpdater::performUpdate(const QString& version_tag)
{
    QProcess proc;
    auto exe_name = QStringLiteral("%1_updater").arg(BuildConfig.LAUNCHER_APP_BINARY_NAME);
#if defined Q_OS_WIN32
    exe_name.append(".exe");
#endif

    QStringList args = { "--dir", priv->dataDir.absolutePath(), "--install-version", version_tag };
    if (priv->allowBeta)
        args.append("--pre-release");

    auto result = proc.startDetached(priv->appDir.absoluteFilePath(exe_name), args);
    if (!result) {
        qDebug() << "Failed to start updater:" << proc.error() << proc.errorString();
    }
    QCoreApplication::exit();
}
