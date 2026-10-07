#include "Bench.h"
#include "TrayApp.h"

#include <gst/gst.h>

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMessageBox>
#include <QSystemTrayIcon>
#include <cstdio>
#include <unistd.h>

namespace {

constexpr const char* kModelFile = "selfie_multiclass_256x256.onnx";

QString findModel(const QString& explicitPath)
{
    if (!explicitPath.isEmpty())
        return explicitPath;
    const QStringList candidates = {
        QCoreApplication::applicationDirPath() + "/../share/camfx/models/" + kModelFile,
        QDir::homePath() + "/.local/share/camfx/models/" + kModelFile,
        QStringLiteral(CAMFX_SOURCE_MODEL_DIR) + "/" + kModelFile,
    };
    for (const QString& c : candidates)
        if (QFileInfo::exists(c))
            return QFileInfo(c).canonicalFilePath();
    return candidates.last();
}

}  // namespace

int main(int argc, char** argv)
{
    gst_init(&argc, &argv);

    // Bench mode needs no display; detect it before creating the application object
    bool bench = false;
    for (int i = 1; i < argc; ++i)
        bench |= QByteArray(argv[i]) == "--bench";

    std::unique_ptr<QCoreApplication> app;
    if (bench)
        app = std::make_unique<QCoreApplication>(argc, argv);
    else
        app = std::make_unique<QApplication>(argc, argv);
    QCoreApplication::setApplicationName("camfx");
    QCoreApplication::setApplicationVersion("0.1.0");

    QCommandLineParser parser;
    parser.setApplicationDescription("Webcam effects to a v4l2loopback virtual camera");
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption deviceOpt("device", "Virtual camera device.", "path", "/dev/video10");
    QCommandLineOption cameraOpt("camera", "Input camera device.", "path");
    QCommandLineOption modelOpt("model", "Segmentation model (.onnx).", "path");
    QCommandLineOption showOpt("show", "Open the settings window at start.");
    QCommandLineOption benchOpt("bench", "Measure latency headless, then exit.");
    QCommandLineOption secondsOpt("seconds", "Benchmark duration.", "n", "10");
    QCommandLineOption snapshotOpt("snapshot", "Benchmark: save the last virtual camera frame.", "file");
    QCommandLineOption presetOpt("preset", "Benchmark preset (default: all effects).", "name");
    parser.addOptions({deviceOpt, cameraOpt, showOpt, modelOpt, benchOpt, secondsOpt, presetOpt, snapshotOpt});
    parser.process(*app);

    const QString model = findModel(parser.value(modelOpt));
    if (!QFileInfo::exists(model))
        std::fprintf(stderr, "warning: model not found at %s\n", qPrintable(model));

    if (bench) {
        BenchOptions o;
        o.outputDevice = parser.value(deviceOpt);
        o.camera = parser.value(cameraOpt);
        o.modelPath = model;
        o.preset = parser.value(presetOpt);
        o.snapshot = parser.value(snapshotOpt);
        o.seconds = std::max(1, parser.value(secondsOpt).toInt());
        return runBench(o);
    }

    // Single instance: a second start asks the running instance to show its window
    const QString serverName = QStringLiteral("camfx-%1").arg(getuid());
    {
        QLocalSocket probe;
        probe.connectToServer(serverName);
        if (probe.waitForConnected(500)) {
            probe.write("show\n");
            probe.waitForBytesWritten(500);
            return 0;
        }
    }
    QLocalServer::removeServer(serverName);
    QLocalServer server;
    if (!server.listen(serverName))
        std::fprintf(stderr, "warning: single-instance server: %s\n", qPrintable(server.errorString()));

    QGuiApplication::setDesktopFileName("camfx");
    QApplication::setWindowIcon(QIcon::fromTheme("camfx", QIcon::fromTheme("camera-web")));
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        std::fprintf(stderr, "warning: no system tray available\n");
    QApplication::setQuitOnLastWindowClosed(false);

    AppOptions o;
    o.outputDevice = parser.value(deviceOpt);
    o.camera = parser.value(cameraOpt);
    o.modelPath = model;
    o.showSettings = parser.isSet(showOpt);
    TrayApp tray(o);
    QObject::connect(&server, &QLocalServer::newConnection, &tray, [&server, &tray] {
        while (QLocalSocket* s = server.nextPendingConnection()) {
            QObject::connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
            tray.showSettings();
        }
    });
    return app->exec();
}
