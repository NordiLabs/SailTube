// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <QObject>
#include <QElapsedTimer>

namespace nordtile { namespace music { namespace sailfish { namespace infrastructure {

bool pythonResolverChildRequested();
int runPythonResolverChild(int argc, char **argv);

// The QML bridge carries only completion status. Provider output stays on the
// child's bounded stdout/stderr pipes, never in the main application's engine.
class PythonResolverHost final : public QObject {
    Q_OBJECT
public:
    PythonResolverHost(const QElapsedTimer &clock, bool timingEnabled, QObject *parent = nullptr)
        : QObject(parent), clock_(clock), timingEnabled_(timingEnabled) {}
    Q_INVOKABLE void ready();
    Q_INVOKABLE void finish(int exitCode);
    Q_INVOKABLE void fail();
private:
    bool finished_ = false;
    bool ready_ = false;
    QElapsedTimer clock_;
    bool timingEnabled_ = false;
};

}}}}
