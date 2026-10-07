#pragma once

#include <QtCore/QString>
#include <functional>
#include <vector>

namespace Bench {

struct Case {
  QString name;
  QString category;
  QString description;
  qint64 inputBytes = 0;
  qint64 units = 1;
  QString unit = QStringLiteral("operation");
  int diagnostics = 0;
  std::function<quint64()> run;
  QString outputFingerprint;
};

using Cases = std::vector<Case>;
void addAlgorithmCases(Cases& cases);
void addPipelineCases(Cases& cases, const QString& corpusDir);
void addComponentCases(Cases& cases, const QString& fontFile);

} // namespace Bench
