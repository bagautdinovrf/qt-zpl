#include "benchmark.hpp"

#include <QtCore/QCommandLineParser>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QSysInfo>
#include <QtCore/QTextStream>
#include <QtCore/QThread>
#include <QtGui/QGuiApplication>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif

namespace {
using namespace Qt::StringLiterals;
using Clock = std::chrono::steady_clock;
volatile quint64 resultSink = 0;

struct Batch { qint64 iterations; double elapsedNs; quint64 checksum; quint64 threadCycles; };

quint64 threadCycles() {
#ifdef Q_OS_WIN
  ULONG64 cycles = 0;
  if (QueryThreadCycleTime(GetCurrentThread(), &cycles)) return cycles;
#endif
  return 0;
}

Batch batch(const Bench::Case& test, qint64 count) {
  quint64 checksum = 0;
  const auto cyclesStart = threadCycles();
  const auto start = Clock::now();
  for (qint64 i = 0; i < count; ++i)
    checksum += test.run() ^ static_cast<quint64>(i);
  const auto end = Clock::now();
  const auto cyclesEnd = threadCycles();
  resultSink = checksum;
  return {count, std::chrono::duration<double, std::nano>(end - start).count(), checksum,
          cyclesStart && cyclesEnd >= cyclesStart ? cyclesEnd - cyclesStart : 0};
}

qint64 calibrate(const Bench::Case& test, double targetNs) {
  qint64 iterations = 1;
  for (;;) {
    const auto measured = batch(test, iterations);
    if (measured.elapsedNs >= targetNs || iterations >= 10'000'000)
      return iterations;
    const double factor = std::clamp(targetNs / std::max(1.0, measured.elapsedNs), 2.0, 10.0);
    iterations = std::min<qint64>(10'000'000, static_cast<qint64>(iterations * factor));
  }
}

double quantile(std::vector<double> values, double fraction) {
  std::sort(values.begin(), values.end());
  const double index = fraction * static_cast<double>(values.size() - 1);
  const auto lower = static_cast<size_t>(index);
  const auto upper = std::min(lower + 1, values.size() - 1);
  return values[lower] + (values[upper] - values[lower]) * (index - lower);
}

int boundedInt(const QCommandLineParser& parser, const QString& option, int low, int high) {
  bool ok = false;
  const int value = parser.value(option).toInt(&ok);
  if (!ok || value < low || value > high)
    throw std::runtime_error((u"Invalid --"_s + option + u" value"_s).toStdString());
  return value;
}
} // namespace

int main(int argc, char* argv[]) {
  QGuiApplication app(argc, argv);
  QCoreApplication::setApplicationName(u"qtzpl_benchmarks"_s);
  QCommandLineParser parser;
  parser.setApplicationDescription(u"Offline, warmed, single-thread QtZpl benchmarks; no PNG I/O in timings."_s);
  parser.addHelpOption();
  parser.addOptions({
    {u"filter"_s, u"Only case names containing this text; repeat to match any filter."_s, u"text"_s},
    {u"list"_s, u"Validate workloads and list cases without timing."_s},
    {u"samples"_s, u"Number of measured batches (3..101)."_s, u"count"_s, u"9"_s},
    {u"min-ms"_s, u"Calibration target per measured batch (1..10000 ms)."_s, u"ms"_s, u"50"_s},
    {u"warmup-ms"_s, u"Warmup per case (1..10000 ms)."_s, u"ms"_s, u"20"_s},
    {u"output"_s, u"Write raw batch timings and environment to JSON."_s, u"file"_s},
    {u"corpus"_s, u"Directory containing offline corpus manifest.json."_s, u"directory"_s,
      QString::fromUtf8(QTZPL_BENCH_CORPUS_DIR)}
  });
  parser.process(app);
  QTextStream out(stdout), err(stderr);
  try {
    const int samples = boundedInt(parser, u"samples"_s, 3, 101);
    const int minMs = boundedInt(parser, u"min-ms"_s, 1, 10000);
    const int warmupMs = boundedInt(parser, u"warmup-ms"_s, 1, 10000);
    Bench::Cases cases;
    Bench::addPipelineCases(cases, parser.value(u"corpus"_s));
    Bench::addAlgorithmCases(cases);
    Bench::addExtensionCases(cases);
    Bench::addDesignerCases(cases);
    Bench::addComponentCases(cases, QString::fromUtf8(QTZPL_BENCH_FONT_FILE));
    cases.push_back({u"component/harness-dispatch"_s, u"components"_s,
      u"std::function dispatch and checksum baseline, no production work"_s,
      0, 1, u"call"_s, 0, [] { return quint64{43}; }});
    QJsonArray results;
    out << "Qt " << qVersion() << "; " << QTZPL_BENCH_COMPILER << "; "
        << QTZPL_BENCH_BUILD_TYPE << "; platform " << app.platformName() << Qt::endl;
    if (QString::fromUtf8(QTZPL_BENCH_BUILD_TYPE) == u"Debug")
      err << "Debug timings are for harness validation only; use Release for performance conclusions.\n";
    qsizetype selected = 0;
    const auto filters = parser.values(u"filter"_s);
    for (const auto& test : cases) {
      if (!filters.isEmpty() && std::none_of(filters.cbegin(), filters.cend(),
          [&test](const QString& filter) { return test.name.contains(filter); })) continue;
      ++selected;
      // Validation and warmup are never part of a measured batch.
      resultSink = test.run();
      if (parser.isSet(u"list"_s)) {
        out << test.name << " | " << test.description << Qt::endl;
        continue;
      }
      calibrate(test, warmupMs * 1'000'000.0);
      const qint64 iterations = calibrate(test, minMs * 1'000'000.0);
      std::vector<double> perOperation;
      QJsonArray batches;
      for (int sample = 0; sample < samples; ++sample) {
        const auto measured = batch(test, iterations);
        perOperation.push_back(measured.elapsedNs / iterations);
        QJsonObject sampleResult{
          {u"iterations"_s, iterations}, {u"elapsed_ns"_s, measured.elapsedNs},
          {u"ns_per_operation"_s, perOperation.back()},
          {u"checksum"_s, QString::number(measured.checksum)}
        };
        // Optional Windows diagnostic, not elapsed time. Compare the same
        // binary workload on the same processor; zero means unavailable.
        if (measured.threadCycles)
          sampleResult[u"thread_cycles_per_operation"_s] = static_cast<double>(measured.threadCycles) / iterations;
        batches.append(sampleResult);
      }
      const double median = quantile(perOperation, 0.5);
      std::vector<double> deviations;
      for (double value : perOperation) deviations.push_back(std::abs(value - median));
      QJsonObject row{
        {u"name"_s, test.name}, {u"category"_s, test.category},
        {u"description"_s, test.description}, {u"input_bytes"_s, test.inputBytes},
        {u"output_fingerprint"_s, test.outputFingerprint},
        {u"units_per_operation"_s, test.units}, {u"unit"_s, test.unit},
        {u"diagnostics"_s, test.diagnostics}, {u"median_ns"_s, median},
        {u"min_ns"_s, quantile(perOperation, 0)}, {u"max_ns"_s, quantile(perOperation, 1)},
        {u"p95_batch_mean_ns"_s, quantile(perOperation, 0.95)},
        {u"mad_ns"_s, quantile(deviations, 0.5)},
        {u"operations_per_second"_s, 1'000'000'000.0 / median},
        {u"units_per_second"_s, test.units * 1'000'000'000.0 / median},
        {u"input_mib_per_second"_s, test.inputBytes * 1'000'000'000.0 / median / (1024 * 1024)},
        {u"samples"_s, batches}
      };
      results.append(row);
      out << test.name << ": " << QString::number(median / 1000, 'f', 3)
          << " us/op; MAD " << QString::number(100 * quantile(deviations, 0.5) / median, 'f', 1)
          << "%; diagnostics " << test.diagnostics << Qt::endl;
    }
    if (selected == 0) throw std::runtime_error("No benchmark case matches --filter");
    if (!parser.isSet(u"list"_s) && parser.isSet(u"output"_s)) {
      const QString path = parser.value(u"output"_s);
      if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        throw std::runtime_error("Cannot create output directory");
      QSaveFile file(path);
      const QJsonObject document{
        {u"schema_version"_s, 1},
        {u"metadata"_s, QJsonObject{
          {u"timestamp_utc"_s, QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
          {u"qt"_s, QString::fromUtf8(qVersion())},
          {u"compiler"_s, QString::fromUtf8(QTZPL_BENCH_COMPILER)},
          {u"build_type"_s, QString::fromUtf8(QTZPL_BENCH_BUILD_TYPE)},
          {u"shared_library"_s, static_cast<bool>(QTZPL_BENCH_SHARED)},
          {u"os"_s, QSysInfo::prettyProductName()}, {u"architecture"_s, QSysInfo::currentCpuArchitecture()},
          {u"logical_cpu_count"_s, QThread::idealThreadCount()}, {u"qt_platform"_s, app.platformName()},
          {u"cpu"_s, qEnvironmentVariable("QTZPL_BENCH_CPU")},
          {u"revision"_s, qEnvironmentVariable("QTZPL_BENCH_REVISION")},
          {u"samples"_s, samples}, {u"target_batch_ms"_s, minMs}, {u"warmup_ms"_s, warmupMs},
          {u"method"_s, u"Warmed single-thread wall time, median of batch means; allocations and destruction included; file I/O excluded. p95 is NOT request latency."_s}
        }},
        {u"results"_s, results}
      };
      const QByteArray bytes = QJsonDocument(document).toJson(QJsonDocument::Indented);
      if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        throw std::runtime_error("Cannot write benchmark JSON");
      out << "Saved " << path << Qt::endl;
    }
  } catch (const std::exception& exception) {
    err << "Benchmark failed: " << exception.what() << Qt::endl;
    return 1;
  }
  return 0;
}
