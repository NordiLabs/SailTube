// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// DIA-03: opt-in timestamped timing log for the §4 performance measurements (NFR-PERF-02/03).
// Off by default: while disabled nothing is opened or created. When enabled, one line per phase:
//   2026-11-08T13:05:01.123Z run=<id> phase=<label> ms=<duration>
// Labels and run ids are fixed tokens chosen by the caller ([A-Za-z0-9_.-], max 48 chars); anything
// else is replaced, so no address, title or identifier can be written. The file lives in the
// private cache directory given by the caller, mode 0600, and is capped (oldest half dropped).

#include <QString>

namespace nordtile { namespace diagnostics {

class TimingLog {
public:
    explicit TimingLog(const QString &directory, qint64 maxBytes = 512 * 1024);
    bool enabled() const { return enabled_; }
    void setEnabled(bool enabled) { enabled_ = enabled; }
    QString filePath() const;
    // Appends one line; returns false when disabled or the write failed.
    bool record(const QString &runId, const QString &phase, qint64 milliseconds);
    // Removes the file (the user turned the log off and asked to clear it).
    void clear();
    static QString sanitizeToken(const QString &token);

private:
    void trimIfNeeded();
    QString directory_;
    qint64 maxBytes_;
    bool enabled_ = false;
};

}}
