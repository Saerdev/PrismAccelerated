// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2022 Sefa Eyeoglu <contact@scrumplex.net>
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
 * This file incorporates work covered by the following copyright and
 * permission notice:
 *
 *      Copyright 2013-2021 MultiMC Contributors
 *
 *      Authors: Orochimarufan <orochimarufan.x3@gmail.com>
 *
 *      Licensed under the Apache License, Version 2.0 (the "License");
 *      you may not use this file except in compliance with the License.
 *      You may obtain a copy of the License at
 *
 *          http://www.apache.org/licenses/LICENSE-2.0
 *
 *      Unless required by applicable law or agreed to in writing, software
 *      distributed under the License is distributed on an "AS IS" BASIS,
 *      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *      See the License for the specific language governing permissions and
 *      limitations under the License.
 */

#include "launch/LaunchTask.h"
#include <assert.h>
#include <QAnyStringView>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <variant>
#include "Commandline.h"
#include "MessageLevel.h"
#include "tasks/Task.h"

void LaunchTask::init()
{
    m_instance->setRunning(true);
}

std::unique_ptr<LaunchTask> LaunchTask::create(MinecraftInstance* inst)
{
    auto task = std::unique_ptr<LaunchTask>(new LaunchTask(inst));
    task->init();
    return task;
}

LaunchTask::LaunchTask(MinecraftInstance* instance) : m_instance(instance) {}

void LaunchTask::appendStep(shared_qobject_ptr<LaunchStep> step)
{
    m_steps.append(step);
}

void LaunchTask::prependStep(shared_qobject_ptr<LaunchStep> step)
{
    m_steps.prepend(step);
}

void LaunchTask::executeTask()
{
    m_instance->setCrashed(false);
    m_launchProfileTimer.start();
    if (!m_steps.size()) {
        state = LaunchTask::Finished;
        emitSucceeded();
        return;
    }
    state = LaunchTask::Running;
    onStepFinished();
}

void LaunchTask::onReadyForLaunch()
{
    state = LaunchTask::Waiting;
    emit readyForLaunch();
}

void LaunchTask::onStepFinished()
{
    // initial -> just start the first step
    if (currentStep == -1) {
        currentStep++;
        startProfiledStep();
        return;
    }

    auto step = m_steps[currentStep];
    const bool stepSuccessful = step->wasSuccessful();
    finishProfiledStep(stepSuccessful);
    if (stepSuccessful) {
        // end?
        if (currentStep == m_steps.size() - 1) {
            finalizeSteps(true, QString());
        } else {
            currentStep++;
            startProfiledStep();
        }
    } else {
        finalizeSteps(false, step->failReason());
    }
}

void LaunchTask::startProfiledStep()
{
    auto step = m_steps[currentStep];
    m_profileStepName = QString::fromLatin1(step->metaObject()->className());
    m_profileStepStartOffsetMs = m_launchProfileTimer.elapsed();
    m_stepProfileTimer.start();
    step->start();
}

void LaunchTask::finishProfiledStep(bool successful)
{
    // The game-launch step is measured until the child JVM starts, not while the player
    // spends time in the game. Other preparation steps are measured to completion.
    if (m_profileStepName.isEmpty()) {
        return;
    }

    QJsonObject record;
    record.insert("index", currentStep);
    record.insert("step", m_profileStepName);
    record.insert("started_at_ms", m_profileStepStartOffsetMs);
    record.insert("duration_ms", m_stepProfileTimer.isValid() ? m_stepProfileTimer.elapsed() : 0);
    record.insert("success", successful);
    m_profileSteps.append(record);

    m_profileStepName.clear();
    m_profileStepStartOffsetMs = -1;
}

void LaunchTask::beginGameProcessProfile()
{
    if (!m_launchProfileTimer.isValid()) {
        m_launchProfileTimer.start();
    }
    if (m_gameProcessTimer.isValid()) {
        return;
    }
    m_gameProcessStartOffsetMs = m_launchProfileTimer.elapsed();
    m_gameProcessTimer.start();
}

void LaunchTask::gameProcessStarted()
{
    if (m_gameProcessTimer.isValid() && m_gameProcessRunningOffsetMs < 0) {
        m_gameProcessRunningOffsetMs = m_gameProcessTimer.elapsed();
    }
    finishProfiledStep(true);
}

void LaunchTask::gameProcessFinished(int exitCode)
{
    if (m_gameProcessTimer.isValid() && m_gameProcessEndOffsetMs < 0) {
        m_gameProcessEndOffsetMs = m_gameProcessTimer.elapsed();
        m_gameProcessExitCode = exitCode;
    }
}

void LaunchTask::recordGameLogMilestones(const QString& line)
{
    if (!m_gameProcessTimer.isValid()) {
        return;
    }

    const qint64 elapsedMs = m_gameProcessTimer.elapsed();
    if (m_gameProcessFirstLogOffsetMs < 0) {
        m_gameProcessFirstLogOffsetMs = elapsedMs;
    }

    const auto capture = [this, elapsedMs, &line](const QString& name, const QString& marker) {
        if (m_seenProfileMilestones.contains(name) || !line.contains(marker, Qt::CaseInsensitive)) {
            return;
        }
        m_seenProfileMilestones.insert(name);
        QJsonObject milestone;
        milestone.insert("name", name);
        milestone.insert("elapsed_ms", elapsedMs);
        m_profileMilestones.append(milestone);
    };

    // These are observable hints, not guaranteed lifecycle boundaries. They vary by
    // Minecraft version, loader, and modpack.
    capture("modlauncher_running", "ModLauncher running");
    capture("resource_reload_started", "Reloading ResourceManager");
    capture("texture_atlas_created", "textures-atlas");
    capture("audio_initialized", "OpenAL initialized");
    capture("world_spawn_preparation", "Preparing spawn area");
    capture("integrated_server_ready_hint", "For help, type");
}

void LaunchTask::writeLaunchProfile()
{
    if (m_profileWritten || !m_launchProfileTimer.isValid()) {
        return;
    }
    m_profileWritten = true;

    const QString logsPath = QDir(m_instance->instanceRoot()).filePath("logs");
    if (!QDir().mkpath(logsPath)) {
        qWarning() << "Unable to create launch profile directory:" << logsPath;
        return;
    }

    QJsonObject profile;
    profile.insert("schema_version", 1);
    profile.insert("captured_at_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    profile.insert("launcher_preparation_ms",
                   m_gameProcessStartOffsetMs >= 0 ? m_gameProcessStartOffsetMs : m_launchProfileTimer.elapsed());
    profile.insert("process_start_to_running_ms", m_gameProcessRunningOffsetMs);
    profile.insert("first_game_log_ms", m_gameProcessFirstLogOffsetMs);
    profile.insert("game_process_lifetime_ms", m_gameProcessEndOffsetMs);
    profile.insert("game_process_exit_code", m_gameProcessExitCode);
    profile.insert("steps", m_profileSteps);
    profile.insert("milestones", m_profileMilestones);

    QFile file(QDir(logsPath).filePath("launch-profiles.jsonl"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        qWarning() << "Unable to write launch profile:" << file.fileName() << file.errorString();
        return;
    }

    file.write(QJsonDocument(profile).toJson(QJsonDocument::Compact));
    file.write("\n");
}

void LaunchTask::finalizeSteps(bool successful, const QString& error)
{
    for (auto step = currentStep; step >= 0; step--) {
        m_steps[step]->finalize();
    }
    if (successful) {
        emitSucceeded();
    } else {
        emitFailed(error);
    }
}

void LaunchTask::onProgressReportingRequested()
{
    state = LaunchTask::Waiting;
    emit requestProgress(m_steps[currentStep].get());
}

void LaunchTask::setCensorFilter(QMap<QString, QString> filter)
{
    m_censorFilter = filter;
}

QString LaunchTask::censorPrivateInfo(QString in)
{
    auto iter = m_censorFilter.begin();
    while (iter != m_censorFilter.end()) {
        in.replace(iter.key(), iter.value());
        iter++;
    }
    return in;
}

void LaunchTask::proceed()
{
    if (state != LaunchTask::Waiting) {
        return;
    }
    m_steps[currentStep]->proceed();
}

bool LaunchTask::canAbort() const
{
    switch (state) {
        case LaunchTask::Aborted:
        case LaunchTask::Failed:
        case LaunchTask::Finished:
            return false;
        case LaunchTask::NotStarted:
            return true;
        case LaunchTask::Running:
        case LaunchTask::Waiting: {
            auto step = m_steps[currentStep];
            return step->canAbort();
        }
    }
    return false;
}

bool LaunchTask::abort()
{
    switch (state) {
        case LaunchTask::Aborted:
        case LaunchTask::Failed:
        case LaunchTask::Finished:
            return true;
        case LaunchTask::NotStarted: {
            state = LaunchTask::Aborted;
            emitAborted();
            return true;
        }
        case LaunchTask::Running:
        case LaunchTask::Waiting: {
            auto step = m_steps[currentStep];
            if (!step->canAbort()) {
                return false;
            }
            if (step->abort()) {
                state = LaunchTask::Aborted;
                return true;
            }
        }
        default:
            break;
    }
    return false;
}

shared_qobject_ptr<LogModel> LaunchTask::getLogModel()
{
    if (!m_logModel) {
        m_logModel.reset(new LogModel());
        m_logModel->setMaxLines(getConsoleMaxLines(m_instance->settings()));
        m_logModel->setStopOnOverflow(shouldStopOnConsoleOverflow(m_instance->settings()));
        // FIXME: should this really be here?
        m_logModel->setOverflowMessage(tr("Stopped watching the game log because the log length surpassed %1 lines.\n"
                                          "You may have to fix your mods because the game is still logging to files and"
                                          " likely wasting harddrive space at an alarming rate!")
                                           .arg(m_logModel->getMaxLines()));
    }
    return m_logModel;
}

bool LaunchTask::parseXmlLogs(const QString& line, MessageLevel level)
{
    LogParser* parser;
    switch (static_cast<MessageLevel::Enum>(level)) {
        case MessageLevel::StdErr:
            parser = &m_stderrParser;
            break;
        case MessageLevel::StdOut:
            parser = &m_stdoutParser;
            break;
        default:
            return false;
    }

    parser->appendLine(line);
    auto items = parser->parseAvailable();
    if (auto err = parser->getError(); err.has_value()) {
        auto& model = *getLogModel();
        model.append(MessageLevel::Error, tr("[Log4j Parse Error] Failed to parse log4j log event: %1").arg(err.value().errMessage));
        return false;
    }

    if (items.isEmpty())
        return true;

    auto model = getLogModel();
    for (const auto& item : items) {
        if (std::holds_alternative<LogParser::LogEntry>(item)) {
            auto entry = std::get<LogParser::LogEntry>(item);
            auto msg = QString("[%1] [%2/%3] [%4]: %5")
                           .arg(entry.timestamp.toString("HH:mm:ss"))
                           .arg(entry.thread)
                           .arg(entry.levelText)
                           .arg(entry.logger)
                           .arg(entry.message);
            msg = censorPrivateInfo(msg);
            model->append(entry.level, msg);
        } else if (std::holds_alternative<LogParser::PlainText>(item)) {
            auto msg = std::get<LogParser::PlainText>(item).message;

            MessageLevel newLevel = MessageLevel::takeFromLine(msg);

            if (newLevel == MessageLevel::Unknown)
                newLevel = LogParser::guessLevel(line, model->previousLevel());

            msg = censorPrivateInfo(msg);

            model->append(newLevel, msg);
        }
    }

    return true;
}

void LaunchTask::onLogLines(const QStringList& lines, MessageLevel defaultLevel)
{
    for (auto& line : lines) {
        onLogLine(line, defaultLevel);
    }
}

void LaunchTask::onLogLine(QString line, MessageLevel level)
{
    if (level == MessageLevel::StdOut || level == MessageLevel::StdErr) {
        recordGameLogMilestones(line);
    }

    if (parseXmlLogs(line, level)) {
        return;
    }

    // censor private user info
    line = censorPrivateInfo(line);

    getLogModel()->append(level, line);
}

void LaunchTask::emitSucceeded()
{
    writeLaunchProfile();
    m_instance->setRunning(false);
    Task::emitSucceeded();
}

void LaunchTask::emitFailed(QString reason)
{
    writeLaunchProfile();
    m_instance->setRunning(false);
    m_instance->setCrashed(true);
    Task::emitFailed(reason);
}

void LaunchTask::emitAborted()
{
    writeLaunchProfile();
    m_instance->setRunning(false);
    Task::emitAborted();
}

QStringList LaunchTask::substituteVariables(QString& cmd, bool isLaunch) const
{
    return Commandline::process(cmd, isLaunch ? m_instance->createLaunchEnvironment() : m_instance->createEnvironment());
}
