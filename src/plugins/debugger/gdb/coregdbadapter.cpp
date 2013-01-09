/****************************************************************************
**
** Copyright (C) 2012 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
****************************************************************************/

#include "coregdbadapter.h"

#include "debuggerstartparameters.h"
#include "debuggercore.h"
#include "debuggeractions.h"
#include "debuggerstringutils.h"
#include "gdbmi.h"
#include "gdbengine.h"

#include <utils/consoleprocess.h>
#include <utils/elfreader.h>
#include <utils/qtcassert.h>
#include <utils/qtcprocess.h>

#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QTemporaryFile>

using namespace Utils;

namespace Debugger {
namespace Internal {

#define CB(callback) \
    static_cast<GdbEngine::GdbCommandCallback>(&GdbCoreEngine::callback), \
    STRINGIFY(callback)

///////////////////////////////////////////////////////////////////////
//
// CoreGdbAdapter
//
///////////////////////////////////////////////////////////////////////

GdbCoreEngine::GdbCoreEngine(const DebuggerStartParameters &startParameters)
    : GdbEngine(startParameters)
{}

GdbCoreEngine::~GdbCoreEngine()
{
    if (false && !m_tempCoreName.isEmpty()) {
        QFile tmpFile(m_tempCoreName);
        tmpFile.remove();
    }
}

void GdbCoreEngine::setupEngine()
{
    QTC_ASSERT(state() == EngineSetupRequested, qDebug() << state());
    showMessage(_("TRYING TO START ADAPTER"));

    const DebuggerStartParameters &sp = startParameters();
    m_executable = sp.executable;
    QFileInfo fi(sp.coreFile);
    m_coreName = fi.absoluteFilePath();

    unpackCoreIfNeeded();
}

QString GdbCoreEngine::readExecutableNameFromCore(bool *isCore)
{
#if 0
    ElfReader reader(coreFileName());
    return QString::fromLocal8Bit(reader.readCoreName(isCore));
#else
    const DebuggerStartParameters &sp = startParameters();
    QStringList args;
    args.append(QLatin1String("-nx"));
    args.append(QLatin1String("-batch"));
    args.append(QLatin1String("-c"));
    args.append(coreFileName());
    QProcess proc;
    proc.start(sp.debuggerCommand, args);
    if (proc.waitForFinished()) {
        QByteArray ba = proc.readAllStandardOutput();
        // Core was generated by `/data/dev/creator-2.6/bin/qtcreator'.
        // Program terminated with signal 11, Segmentation fault.
        int pos1 = ba.indexOf("Core was generated by");
        if (pos1 != -1) {
            pos1 += 23;
            int pos2 = ba.indexOf('\'', pos1);
            if (pos2 != -1) {
                *isCore = true;
                return QString::fromLocal8Bit(ba.mid(pos1, pos2 - pos1));
            }
        }
    }
    return QString();
#endif
}

void GdbCoreEngine::continueSetupEngine()
{
    if (m_executable.isEmpty()) {
        // Read executable from core.
        bool isCore = false;
        m_executable = readExecutableNameFromCore(&isCore);

        if (!isCore) {
            showMessageBox(QMessageBox::Warning,
                tr("Error Loading Core File"),
                tr("The specified file does not appear to be a core file."));
            notifyEngineSetupFailed();
            return;
        }

        // Strip off command line arguments. FIXME: make robust.
        int idx = m_executable.indexOf(QLatin1Char(' '));
        if (idx >= 0)
            m_executable.truncate(idx);
        if (m_executable.isEmpty()) {
            showMessageBox(QMessageBox::Warning,
                tr("Error Loading Symbols"),
                tr("No executable to load symbols from specified core."));
            notifyEngineSetupFailed();
            return;
        }
    }
    startGdb();
}

void GdbCoreEngine::setupInferior()
{
    QTC_ASSERT(state() == InferiorSetupRequested, qDebug() << state());
    // Do that first, otherwise no symbols are loaded.
    QFileInfo fi(m_executable);
    QByteArray path = fi.absoluteFilePath().toLocal8Bit();
    postCommand("-file-exec-and-symbols \"" + path + '"',
         CB(handleFileExecAndSymbols));
}

void GdbCoreEngine::handleFileExecAndSymbols(const GdbResponse &response)
{
    QTC_ASSERT(state() == InferiorSetupRequested, qDebug() << state());
    QString core = coreFileName();
    if (response.resultClass == GdbResultDone) {
        showMessage(tr("Symbols found."), StatusBar);
        postCommand("target core " + core.toLocal8Bit(),
            CB(handleTargetCore));
        return;
    }
    QString msg = tr("No symbols found in core file <i>%1</i>.")
        .arg(core);
    msg += _(" ");
    msg += tr("This can be caused by a path length limitation in the "
        "core file.");
    msg += _(" ");
    msg += tr("Try to specify the binary using the "
        "<i>Debug->Start Debugging->Attach to Core</i> dialog.");
    notifyInferiorSetupFailed(msg);
}

void GdbCoreEngine::handleTargetCore(const GdbResponse &response)
{
    QTC_ASSERT(state() == InferiorSetupRequested, qDebug() << state());
    if (response.resultClass == GdbResultDone) {
        // HACK: The namespace is not accessible in the initial run.
        tryLoadPythonDumpers();
        showMessage(tr("Attached to core."), StatusBar);
        handleInferiorPrepared();
        // Due to the auto-solib-add off setting, we don't have any
        // symbols yet. Load them in order of importance.
        reloadStack(true);
        reloadModulesInternal();
        postCommand("p 5", CB(handleRoundTrip));
        return;
    }
    QString msg = tr("Attach to core \"%1\" failed:\n")
        .arg(startParameters().coreFile)
        + QString::fromLocal8Bit(response.data.findChild("msg").data());
    notifyInferiorSetupFailed(msg);
}

void GdbCoreEngine::handleRoundTrip(const GdbResponse &response)
{
    Q_UNUSED(response);
    loadSymbolsForStack();
    QTimer::singleShot(1000, this, SLOT(loadAllSymbols()));
}

void GdbCoreEngine::runEngine()
{
    QTC_ASSERT(state() == EngineRunRequested, qDebug() << state());
    notifyInferiorUnrunnable();
    updateAll();
}

void GdbCoreEngine::interruptInferior()
{
    // A core never runs, so this cannot be called.
    QTC_CHECK(false);
}

void GdbCoreEngine::shutdownEngine()
{
    notifyAdapterShutdownOk();
}

void GdbCoreEngine::unpackCoreIfNeeded()
{
    if (!m_coreName.endsWith(QLatin1String(".lzo"))) {
        continueSetupEngine();
        return;
    }

    {
        QString pattern = QDir::tempPath() + QLatin1String("/tmpcore-XXXXXX");
        QTemporaryFile tmp(pattern, this);
        tmp.open();
        m_tempCoreName = tmp.fileName();
    }

    QProcess *process = new QProcess(this);
    process->setWorkingDirectory(QDir::tempPath());
    QStringList arguments;
    arguments << QLatin1String("-o") << m_tempCoreName << QLatin1String("-x") << m_coreName;
    process->start(QLatin1String("/usr/bin/lzop"), arguments);
    connect(process, SIGNAL(finished(int)), SLOT(continueSetupEngine()));
}

QString GdbCoreEngine::coreFileName() const
{
    return m_tempCoreName.isEmpty() ? m_coreName : m_tempCoreName;
}

} // namespace Internal
} // namespace Debugger
