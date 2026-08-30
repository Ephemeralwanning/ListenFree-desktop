#include "sourcehost/source_protocol.h"

#include <QCoreApplication>
#include <QTextStream>

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QTextStream input(stdin);
    QTextStream output(stdout);
    output << "listenfree-sourcehost-ready\n" << Qt::flush;
    while (!input.atEnd()) {
        const QString line = input.readLine();
        if (line == QStringLiteral("shutdown")) break;
    }
    return 0;
}
