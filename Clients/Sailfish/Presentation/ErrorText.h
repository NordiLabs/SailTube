// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include "Core/Resolver/Infrastructure/YtDlpJson.h"
#include <QCoreApplication>
#include <QString>

namespace nordtile { namespace video { namespace sailfish {

// Fixed, translatable texts for resolver failures; provider text is never shown.
inline QString errorText(resolver::ErrorCode code) {
    using resolver::ErrorCode;
    switch (code) {
    case ErrorCode::None: return QString();
    case ErrorCode::InvalidRequest: return QCoreApplication::translate("SailTubeErrors", "The request is not valid.");
    case ErrorCode::RuntimeUnavailable: return QCoreApplication::translate("SailTubeErrors", "The packaged YouTube resolver is not available. Reinstall SailTube or check that PyOtherSide and Python 3.11 are installed.");
    case ErrorCode::Busy: return QCoreApplication::translate("SailTubeErrors", "The resolver is busy. Try again.");
    case ErrorCode::Cancelled: return QString();
    case ErrorCode::Timeout: return QCoreApplication::translate("SailTubeErrors", "YouTube took too long to answer. Try again.");
    case ErrorCode::Network: return QCoreApplication::translate("SailTubeErrors", "YouTube could not be reached. Check the connection.");
    case ErrorCode::BotCheck: return QCoreApplication::translate("SailTubeErrors", "YouTube asked to confirm that this is not a bot. Try again later or from another network.");
    case ErrorCode::AgeRestricted: return QCoreApplication::translate("SailTubeErrors", "This video is age-restricted and needs a signed-in account.");
    case ErrorCode::PrivateVideo: return QCoreApplication::translate("SailTubeErrors", "This video is private.");
    case ErrorCode::VideoUnavailable: return QCoreApplication::translate("SailTubeErrors", "This video is not available.");
    case ErrorCode::Upcoming: return QCoreApplication::translate("SailTubeErrors", "This live event has not started yet.");
    case ErrorCode::InvalidResponse: return QCoreApplication::translate("SailTubeErrors", "The resolver returned an unexpected answer.");
    case ErrorCode::NoPlayableStream: return QCoreApplication::translate("SailTubeErrors", "None of the offered streams can be played on this device.");
    case ErrorCode::Failed: return QCoreApplication::translate("SailTubeErrors", "The resolver failed. Try again.");
    }
    return QString();
}

}}}
