#pragma once

#include <QString>

struct BenchOptions {
    QString outputDevice;
    QString camera;
    QString modelPath;
    QString preset;  // empty: all effects enabled
    int seconds = 10;
    QString snapshot;  // if set, save the last frame the consumer received (PNG/JPEG)
};

// Run the pipeline headless, read the virtual camera back as a consumer, and print
// per-stage and end-to-end latency. Returns a process exit code.
int runBench(const BenchOptions& opts);
