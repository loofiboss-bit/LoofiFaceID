// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace KFaceAuth
{
inline bool authComponentsAvailable(bool syncHelperExists, bool pamModule64Exists, bool pamModuleExists)
{
    return syncHelperExists && (pamModule64Exists || pamModuleExists);
}
} // namespace KFaceAuth
