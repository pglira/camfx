#pragma once

#include <QString>
#include <atomic>
#include <functional>
#include <thread>

// Watches a v4l2loopback device and reports whether another application streams from it.
// Uses the v4l2loopback client-usage event; falls back to scanning /proc for open
// file descriptors when the module does not support the event.
class LoopbackMonitor {
public:
    using Callback = std::function<void(int consumers)>;

    LoopbackMonitor(const QString& device, Callback cb);
    ~LoopbackMonitor();

private:
    void run();
    bool runEvents();
    void runProcScan();

    QString device_;
    Callback cb_;
    std::atomic<bool> quit_{false};
    std::thread thread_;
};
