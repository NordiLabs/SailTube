// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// A bounded worker pool for discovery parsing (RES-02: background lanes separate from the
// playback component). JSON answers of 0.5 MB are parsed here, not on the UI thread. The result is
// delivered back on the thread of `context` (a QObject that must live in a thread with an event
// loop), and dropped when `context` is gone. Qt 5.6 compatible (no functor invokeMethod).

#include <QObject>
#include <QPointer>
#include <QRunnable>
#include <QString>
#include <QThreadPool>
#include <functional>
#include <memory>

namespace nordtile { namespace video { namespace catalog {

class LaneDelivery final : public QObject {
    Q_OBJECT
public:
    explicit LaneDelivery(std::function<void()> deliver) : deliver_(std::move(deliver)) {}
    void fire() { emit ready(); }
signals:
    void ready();
public:
    void run() { if (deliver_) deliver_(); }
private:
    std::function<void()> deliver_;
};

class WorkLane {
public:
    explicit WorkLane(const QString &name, int threads = 1) : name_(name) {
        pool_.setMaxThreadCount(threads);
        pool_.setExpiryTimeout(30000);
    }
    ~WorkLane() { pool_.waitForDone(); }

    QString name() const { return name_; }
    int activeThreads() const { return pool_.activeThreadCount(); }

    // Runs `work` on the lane and `done(result)` on `context`'s thread.
    template <typename T>
    void run(QObject *context, std::function<T()> work, std::function<void(T)> done) {
        QPointer<QObject> guard(context);
        auto result = std::make_shared<T>();
        LaneDelivery *delivery = new LaneDelivery([guard, result, done] {
            if (guard && done) done(std::move(*result));
        });
        delivery->moveToThread(context->thread());
        QObject::connect(delivery, &LaneDelivery::ready, delivery, [delivery] {
            delivery->run();
            delivery->deleteLater();
        }, Qt::QueuedConnection);
        pool_.start(new Job([work, result, delivery] {
            *result = work();
            delivery->fire();
        }));
    }

    // Blocks until all queued work finished (tests and shutdown).
    void drain() { pool_.waitForDone(); }

private:
    class Job final : public QRunnable {
    public:
        explicit Job(std::function<void()> f) : f_(std::move(f)) { setAutoDelete(true); }
        void run() override { f_(); }
    private:
        std::function<void()> f_;
    };
    QString name_;
    QThreadPool pool_;
};

}}}
