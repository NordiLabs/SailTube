// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// DIA-03: off by default writes nothing; on writes one timestamped line per phase, 0600, tokens only.
#include "../../Core/Diagnostics/Infrastructure/TimingLog.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <cstdio>

using nordtile::diagnostics::TimingLog;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main() {
    QTemporaryDir tmp;
    const QString dir = tmp.path() + QStringLiteral("/cache/timing");
    {
        TimingLog log(dir);
        CHECK(!log.enabled());
        for (int i = 0; i < 5; ++i) CHECK(!log.record(QStringLiteral("p1-1"), QStringLiteral("first_frame"), 900));
        CHECK(!QFile::exists(log.filePath()));
        CHECK(!QDir(dir).exists());   // not even the directory
    }
    {
        TimingLog log(dir);
        log.setEnabled(true);
        const char *phases[] = {"queue", "post", "probe", "preroll", "first_frame"};
        for (int run = 1; run <= 3; ++run)
            for (const char *p : phases) CHECK(log.record(QStringLiteral("P%1-%2").arg(run).arg(run), QString::fromLatin1(p), run * 10));
        CHECK(log.record(QStringLiteral("https://x.googlevideo.com/videoplayback?sig=1"), QStringLiteral("post"), 5));
        QFile f(log.filePath());
        CHECK(f.open(QIODevice::ReadOnly));
        const QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'), QString::SkipEmptyParts);
        CHECK(lines.size() == 16);
        const QRegularExpression shape(QStringLiteral("^\\d{4}-\\d\\d-\\d\\dT\\d\\d:\\d\\d:\\d\\d\\.\\d{3}Z run=[A-Za-z0-9_.-]+ phase=[A-Za-z0-9_.-]+ ms=-?\\d+$"));
        for (const QString &l : lines) CHECK(shape.match(l).hasMatch());
        CHECK(!lines.last().contains(QStringLiteral("googlevideo.com/")));
        CHECK((f.permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::WriteOther)) == 0);
        log.clear();
        CHECK(!QFile::exists(log.filePath()));
    }
    CHECK(TimingLog::sanitizeToken(QStringLiteral("P1-3.first_frame")) == QStringLiteral("P1-3.first_frame"));
    CHECK(TimingLog::sanitizeToken(QStringLiteral("a b/c?d")) == QStringLiteral("a_b_c_d"));
    CHECK(TimingLog::sanitizeToken(QString()) == QStringLiteral("_"));
    CHECK(TimingLog::sanitizeToken(QString(100, QLatin1Char('x'))).size() == 48);
    {
        TimingLog log(dir, 2048);
        log.setEnabled(true);
        for (int i = 0; i < 200; ++i) log.record(QStringLiteral("run"), QStringLiteral("phase"), i);
        CHECK(QFile(log.filePath()).size() <= 2048 + 128);
    }
    if (failures) { std::printf("timing log: %d failure(s)\n", failures); return 1; }
    std::printf("diagnostics TimingLogTest: all checks passed\n");
    return 0;
}
