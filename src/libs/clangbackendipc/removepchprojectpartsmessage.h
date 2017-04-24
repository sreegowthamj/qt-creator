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

#pragma once

#include "projectpartcontainerv2.h"

namespace ClangBackEnd {

class RemovePchProjectPartsMessage
{
public:
    RemovePchProjectPartsMessage() = default;
    RemovePchProjectPartsMessage(Utils::SmallStringVector &&projectsPartIds)
        : projectsPartIds_(std::move(projectsPartIds))
    {}

    const Utils::SmallStringVector &projectsPartIds() const
    {
        return projectsPartIds_;
    }

    Utils::SmallStringVector takeProjectsPartIds()
    {
        return std::move(projectsPartIds_);
    }

    friend QDataStream &operator<<(QDataStream &out, const RemovePchProjectPartsMessage &message)
    {
        out << message.projectsPartIds_;

        return out;
    }

    friend QDataStream &operator>>(QDataStream &in, RemovePchProjectPartsMessage &message)
    {
        in >> message.projectsPartIds_;

        return in;
    }

    friend bool operator==(const RemovePchProjectPartsMessage &first,
                           const RemovePchProjectPartsMessage &second)
    {
        return first.projectsPartIds_ == second.projectsPartIds_;
    }

    RemovePchProjectPartsMessage clone() const
    {
        return RemovePchProjectPartsMessage(projectsPartIds_.clone());
    }

private:
    Utils::SmallStringVector projectsPartIds_;
};

CMBIPC_EXPORT QDebug operator<<(QDebug debug, const RemovePchProjectPartsMessage &message);
std::ostream &operator<<(std::ostream &out, const RemovePchProjectPartsMessage &message);

DECLARE_MESSAGE(RemovePchProjectPartsMessage)

} // namespace ClangBackEnd