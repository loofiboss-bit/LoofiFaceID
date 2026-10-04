// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QList>

namespace KFaceAuth
{
inline bool hasManagedPamAuthBlock(const QByteArray &contents)
{
    static const QByteArray beginMarker = "# BEGIN kfaceauth experimental authentication";
    static const QByteArray endMarker = "# END kfaceauth experimental authentication";
    static const QByteArray rule = "auth        sufficient    pam_kfaceauth.so";

    int beginCount = 0;
    int endCount = 0;
    int ruleCount = 0;
    bool inside = false;
    bool invalid = false;
    bool passwordFallbackAfterBlock = false;

    const QList<QByteArray> lines = contents.split('\n');
    for (const QByteArray &rawLine : lines)
    {
        const QByteArray line = rawLine.endsWith('\r') ? rawLine.chopped(1) : rawLine;
        if (line == beginMarker)
        {
            ++beginCount;
            if (inside || beginCount > 1)
                invalid = true;
            inside = true;
            continue;
        }
        if (line == endMarker)
        {
            ++endCount;
            if (!inside || endCount > 1)
                invalid = true;
            inside = false;
            continue;
        }

        if (inside)
        {
            if (line == rule)
                ++ruleCount;
            else
                invalid = true;
            continue;
        }

        const QList<QByteArray> fields = line.simplified().split(' ');
        if (!fields.isEmpty() && fields.front() == "auth" && line.contains("pam_kfaceauth.so"))
            invalid = true;
        if (endCount == 1 && fields.size() >= 3 && fields[0] == "auth" &&
            (fields[1] == "substack" || fields[1] == "include") && fields[2] == "password-auth")
            passwordFallbackAfterBlock = true;
    }

    return beginCount == 1 && endCount == 1 && !inside && !invalid && ruleCount == 1 && passwordFallbackAfterBlock;
}
} // namespace KFaceAuth
