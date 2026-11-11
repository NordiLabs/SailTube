// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// The app binary in warm-worker mode (NORDTILE_RESOLVER_WORKER=1): a Qt Core + QML process whose only job
// is PyOtherSide running <runtime>/worker.py (Core/Worker/Runtime). Same environment discipline as the
// one-shot resolver child: Python and Qt overrides removed, no user site, no bytecode, working directory
// the packaged runtime. Started by WarmResolver through HelperProcess.

#include <QObject>

namespace nordtile { namespace video { namespace worker {

bool pythonWorkerRequested();
// runtimeDirectory: where worker.py, resolver.py and Worker.qml live.
int runPythonWorkerChild(int argc, char **argv, const char *runtimeDirectory);

class PythonWorkerHost : public QObject {
    Q_OBJECT
public:
    Q_INVOKABLE void finish(int exitCode);
    Q_INVOKABLE void fail();
private:
    bool finished_ = false;
};

}}}
