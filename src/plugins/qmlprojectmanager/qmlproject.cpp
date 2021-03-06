/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#include "qmlproject.h"
#include "fileformat/qmlprojectfileformat.h"
#include "fileformat/qmlprojectitem.h"
#include "qmlprojectrunconfiguration.h"
#include "qmlprojectconstants.h"
#include "qmlprojectnodes.h"

#include <coreplugin/icontext.h>
#include <coreplugin/icore.h>
#include <coreplugin/messagemanager.h>
#include <coreplugin/documentmanager.h>

#include <projectexplorer/kitinformation.h>
#include <projectexplorer/kitmanager.h>
#include <projectexplorer/target.h>

#include <qtsupport/baseqtversion.h>
#include <qtsupport/qtkitinformation.h>
#include <qtsupport/qtsupportconstants.h>
#include <qmljs/qmljsmodelmanagerinterface.h>

#include <utils/algorithm.h>

#include <QDebug>

using namespace Core;
using namespace ProjectExplorer;

namespace QmlProjectManager {

QmlProject::QmlProject(const Utils::FileName &fileName) :
    Project(QString::fromLatin1(Constants::QMLPROJECT_MIMETYPE), fileName,
            [this]() { refreshProjectFile(); })
{
    setId("QmlProjectManager.QmlProject");
    setProjectContext(Context(QmlProjectManager::Constants::PROJECTCONTEXT));
    setProjectLanguages(Context(ProjectExplorer::Constants::QMLJS_LANGUAGE_ID));
    setDisplayName(fileName.toFileInfo().completeBaseName());
}

QmlProject::~QmlProject()
{
    delete m_projectItem.data();
}

void QmlProject::addedTarget(Target *target)
{
    connect(target, &Target::addedRunConfiguration,
            this, &QmlProject::addedRunConfiguration);
    foreach (RunConfiguration *rc, target->runConfigurations())
        addedRunConfiguration(rc);
}

void QmlProject::onActiveTargetChanged(Target *target)
{
    if (m_activeTarget)
        disconnect(m_activeTarget, &Target::kitChanged, this, &QmlProject::onKitChanged);
    m_activeTarget = target;
    if (m_activeTarget)
        connect(target, &Target::kitChanged, this, &QmlProject::onKitChanged);

    // make sure e.g. the default qml imports are adapted
    refresh(Configuration);
}

void QmlProject::onKitChanged()
{
    // make sure e.g. the default qml imports are adapted
    refresh(Configuration);
}

void QmlProject::addedRunConfiguration(RunConfiguration *rc)
{
    // The enabled state of qml runconfigurations can only be decided after
    // they have been added to a project
    QmlProjectRunConfiguration *qmlrc = qobject_cast<QmlProjectRunConfiguration *>(rc);
    if (qmlrc)
        qmlrc->updateEnabledState();
}

QDir QmlProject::projectDir() const
{
    return projectFilePath().toFileInfo().dir();
}

void QmlProject::parseProject(RefreshOptions options)
{
    if (options & Files) {
        if (options & ProjectFile)
            delete m_projectItem.data();
        if (!m_projectItem) {
              QString errorMessage;
              m_projectItem = QmlProjectFileFormat::parseProjectFile(projectFilePath(), &errorMessage);
              if (m_projectItem) {
                  connect(m_projectItem.data(), &QmlProjectItem::qmlFilesChanged,
                          this, &QmlProject::refreshFiles);

              } else {
                  MessageManager::write(tr("Error while loading project file %1.")
                                        .arg(projectFilePath().toUserOutput()),
                                        MessageManager::NoModeSwitch);
                  MessageManager::write(errorMessage);
              }
        }
        if (m_projectItem) {
            m_projectItem.data()->setSourceDirectory(projectDir().path());
            if (auto modelManager = QmlJS::ModelManagerInterface::instance())
                modelManager->updateSourceFiles(m_projectItem.data()->files(), true);

            QString mainFilePath = m_projectItem.data()->mainFile();
            if (!mainFilePath.isEmpty()) {
                mainFilePath = projectDir().absoluteFilePath(mainFilePath);
                Utils::FileReader reader;
                QString errorMessage;
                if (!reader.fetch(mainFilePath, &errorMessage)) {
                    MessageManager::write(tr("Warning while loading project file %1.")
                                          .arg(projectFilePath().toUserOutput()));
                    MessageManager::write(errorMessage);
                }
            }
        }
        generateProjectTree();
    }

    if (options & Configuration) {
        // update configuration
    }
}

void QmlProject::refresh(RefreshOptions options)
{
    emitParsingStarted();
    parseProject(options);

    if (options & Files)
        generateProjectTree();

    auto modelManager = QmlJS::ModelManagerInterface::instance();
    if (!modelManager)
        return;

    QmlJS::ModelManagerInterface::ProjectInfo projectInfo =
            modelManager->defaultProjectInfoForProject(this);
    foreach (const QString &searchPath, customImportPaths())
        projectInfo.importPaths.maybeInsert(Utils::FileName::fromString(searchPath),
                                            QmlJS::Dialect::Qml);

    modelManager->updateProjectInfo(projectInfo, this);

    emitParsingFinished(true);
}

QString QmlProject::mainFile() const
{
    if (m_projectItem)
        return m_projectItem.data()->mainFile();
    return QString();
}

bool QmlProject::validProjectFile() const
{
    return !m_projectItem.isNull();
}

QStringList QmlProject::customImportPaths() const
{
    QStringList importPaths;
    if (m_projectItem)
        importPaths = m_projectItem.data()->importPaths();

    return importPaths;
}

bool QmlProject::addFiles(const QStringList &filePaths)
{
    QStringList toAdd;
    foreach (const QString &filePath, filePaths) {
        if (!m_projectItem.data()->matchesFile(filePath))
            toAdd << filePaths;
    }
    return toAdd.isEmpty();
}

void QmlProject::refreshProjectFile()
{
    refresh(QmlProject::ProjectFile | Files);
}

void QmlProject::refreshFiles(const QSet<QString> &/*added*/, const QSet<QString> &removed)
{
    refresh(Files);
    if (!removed.isEmpty()) {
        if (auto modelManager = QmlJS::ModelManagerInterface::instance())
            modelManager->removeFiles(removed.toList());
    }
}

bool QmlProject::supportsKit(Kit *k, QString *errorMessage) const
{
    Id deviceType = DeviceTypeKitInformation::deviceTypeId(k);
    if (deviceType != ProjectExplorer::Constants::DESKTOP_DEVICE_TYPE) {
        if (errorMessage)
            *errorMessage = tr("Device type is not desktop.");
        return false;
    }

    QtSupport::BaseQtVersion *version = QtSupport::QtKitInformation::qtVersion(k);
    if (!version) {
        if (errorMessage)
            *errorMessage = tr("No Qt version set in kit.");
        return false;
    }

    if (version->qtVersion() < QtSupport::QtVersionNumber(5, 0, 0)) {
        if (errorMessage)
            *errorMessage = tr("Qt version is too old.");
        return false;
    }
    return true;
}

Internal::QmlProjectNode *QmlProject::rootProjectNode() const
{
    return static_cast<Internal::QmlProjectNode *>(Project::rootProjectNode());
}

Project::RestoreResult QmlProject::fromMap(const QVariantMap &map, QString *errorMessage)
{
    RestoreResult result = Project::fromMap(map, errorMessage);
    if (result != RestoreResult::Ok)
        return result;

    // refresh first - project information is used e.g. to decide the default RC's
    refresh(Everything);

    if (!activeTarget()) {
        // find a kit that matches prerequisites (prefer default one)
        QList<Kit*> kits = KitManager::kits(
            std::function<bool(const Kit *)>([](const Kit *k) -> bool {
                if (!k->isValid())
                    return false;

                IDevice::ConstPtr dev = DeviceKitInformation::device(k);
                if (dev.isNull() || dev->type() != ProjectExplorer::Constants::DESKTOP_DEVICE_TYPE)
                    return false;
                QtSupport::BaseQtVersion *version = QtSupport::QtKitInformation::qtVersion(k);
                if (!version || version->type() != QLatin1String(QtSupport::Constants::DESKTOPQT))
                    return false;

                return version->qtVersion() >= QtSupport::QtVersionNumber(5, 0, 0)
                        && !version->qmlsceneCommand().isEmpty();
            })
        );

        if (!kits.isEmpty()) {
            Kit *kit = 0;
            if (kits.contains(KitManager::defaultKit()))
                kit = KitManager::defaultKit();
            else
                kit = kits.first();
            addTarget(createTarget(kit));
        }
    }

    // addedTarget calls updateEnabled on the runconfigurations
    // which needs to happen after refresh
    foreach (Target *t, targets())
        addedTarget(t);

    connect(this, &ProjectExplorer::Project::addedTarget, this, &QmlProject::addedTarget);

    connect(this, &ProjectExplorer::Project::activeTargetChanged,
            this, &QmlProject::onActiveTargetChanged);

    onActiveTargetChanged(activeTarget());

    return RestoreResult::Ok;
}

void QmlProject::generateProjectTree()
{
    if (!m_projectItem)
        return;

    auto newRoot = new Internal::QmlProjectNode(this);

    for (const QString &f : m_projectItem.data()->files()) {
        const Utils::FileName fileName = Utils::FileName::fromString(f);
        const FileType fileType = (fileName == projectFilePath())
                ? FileType::Project : FileNode::fileTypeForFileName(fileName);
        newRoot->addNestedNode(new FileNode(fileName, fileType, false));
    }
    newRoot->addNestedNode(new FileNode(projectFilePath(), FileType::Project, false));

    setRootProjectNode(newRoot);
}

} // namespace QmlProjectManager

