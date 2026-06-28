#include "mainwindow.h"

#include <QApplication>
#include <QCommandLineParser>

// main.cpp: entry point for the Qt desktop UI. Mirrors solakon-one-ui's own
// "--host skips the connection dialog" convention, but targets this
// project's REST API instead of Modbus TCP directly. The API key itself is
// never a CLI option -- it is only ever read from the platform secret store
// (see SecretStore), keyed by the resolved host/port.
int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName("flunaras");
    QApplication::setApplicationName("solakon-one-fritz-powerregulator-ui");

    QCommandLineParser parser;
    parser.setApplicationDescription(
        "Qt desktop UI for solakon-one-fritz-powerregulator's REST API");
    parser.addHelpOption();

    QCommandLineOption hostOpt({"H", "host"}, "REST API host or IP", "host");
    QCommandLineOption portOpt({"p", "port"}, "REST API port (default: 8080)", "port", "8080");
    QCommandLineOption schemeOpt("scheme", "http or https (default: http)", "scheme", "http");
    QCommandLineOption ignoreSslOpt("ignore-ssl-errors",
        "Ignore TLS certificate errors (needed for the tool's self-signed certificate)");
    QCommandLineOption intervalOpt({"i", "interval"},
        "Poll interval in seconds (default: 10)", "seconds", "10");
    parser.addOption(hostOpt);
    parser.addOption(portOpt);
    parser.addOption(schemeOpt);
    parser.addOption(ignoreSslOpt);
    parser.addOption(intervalOpt);
    parser.process(app);

    MainWindow window;
    // Window size/position is restored (or defaulted) by MainWindow itself
    // (see restoreLayout(), called from its constructor) -- resizing here
    // unconditionally would clobber whatever was just restored.

    if (parser.isSet(hostOpt)) {
        ConnectionSettings settings;
        settings.host = parser.value(hostOpt);
        settings.port = parser.value(portOpt).toInt();
        settings.scheme = parser.value(schemeOpt);
        settings.ignoreSslErrors = parser.isSet(ignoreSslOpt);
        settings.pollIntervalS = parser.value(intervalOpt).toInt();
        window.connectWithCliArgs(settings);
    } else {
        // No explicit --host: fall back to the saved connection if the user
        // previously enabled "Automatically connect..." in the connection
        // dialog. No-op otherwise (dialog shown on demand via Connect...).
        window.autoConnectIfEnabled();
    }

    window.show();
    return QApplication::exec();
}
