// Offline donor-font research. Labelary images are comparison input only;
// this tool never copies their pixels into a generated font.
#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMap>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>
#include <QtCore/QTextStream>
#include <QtGui/QImage>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include <algorithm>
#include <array>
#include <cmath>
#include <expected>
#include <limits>
#include <memory>

using namespace Qt::StringLiterals;

namespace {
constexpr qint64 maximumPixels = 64 * 1024 * 1024;
// Verified independently for each axis by the committed minimum-size-clamp
// Labelary fixture. Requested sizes are retained in the report and ZPL.
constexpr int minimumFont0Dimension = 10;

struct LibraryDeleter { void operator()(FT_Library library) const { FT_Done_FreeType(library); } };
struct FaceDeleter { void operator()(FT_Face face) const { FT_Done_Face(face); } };
using Library = std::unique_ptr<FT_LibraryRec_, LibraryDeleter>;
using Face = std::unique_ptr<FT_FaceRec_, FaceDeleter>;

struct Adjustment {
    double scaleX = 1;
    double scaleY = 1;
    double offsetX = 0;
    double offsetY = 0; // Font coordinates: positive Y points upward.
};

struct PixelComparison {
    qint64 different = 0;
    qint64 intersection = 0;
    qint64 unionPixels = 0;
    QRect actualInk;
    QRect expectedInk;
    [[nodiscard]] double iou() const { return unionPixels ? double(intersection) / unionPixels : 1; }
};

QJsonArray rectangle(const QRect& value) {
    return {value.x(), value.y(), value.width(), value.height()};
}

QRect comparisonArea(const QJsonObject& cell, const QRect& imageBounds) {
    const QRect bounds(cell[u"x"_s].toInt(), cell[u"y"_s].toInt(),
                       cell[u"width"_s].toInt(), cell[u"height"_s].toInt());
    // Explicit anchors are placed inside padded, independently checked cells.
    // Overscanning those cells can collect ink from the previous glyph.
    // The legacy format instead put the implicit anchor on the cell boundary.
    const int padding = cell.contains(u"anchorX"_s) && cell.contains(u"anchorY"_s) ? 0 : 10;
    return bounds.adjusted(-padding, -padding, 0, 0).intersected(imageBounds);
}

std::expected<QByteArray, QString> readFile(const QString& path, qint64 maximumBytes) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return std::unexpected(path + u": "_s + file.errorString());
    if (file.size() < 0 || file.size() > maximumBytes)
        return std::unexpected(u"Input exceeds the size limit: "_s + path);
    return file.readAll();
}

std::expected<QJsonObject, QString> readObject(const QString& path) {
    const auto bytes = readFile(path, 16 * 1024 * 1024);
    if (!bytes) return std::unexpected(bytes.error());
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(*bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return std::unexpected(u"Expected a JSON object: "_s + path + u": "_s + error.errorString());
    return document.object();
}

bool validDimension(int value) { return value > 0 && value <= 32000; }

bool validAdjustment(const Adjustment& value) {
    return std::isfinite(value.scaleX) && value.scaleX > 0 && value.scaleX <= 16
        && std::isfinite(value.scaleY) && value.scaleY > 0 && value.scaleY <= 16
        && std::isfinite(value.offsetX) && std::abs(value.offsetX) <= 16
        && std::isfinite(value.offsetY) && std::abs(value.offsetY) <= 16;
}

QJsonObject adjustmentJson(const Adjustment& value) {
    return {{u"scaleX"_s, value.scaleX}, {u"scaleY"_s, value.scaleY},
            {u"offsetX"_s, value.offsetX}, {u"offsetY"_s, value.offsetY}};
}

std::expected<Adjustment, QString> adjustment(const QJsonObject& object) {
    Adjustment value;
    const auto number = [&](const QString& key, double fallback) {
        return object.contains(key) ? object[key].toDouble(std::numeric_limits<double>::quiet_NaN()) : fallback;
    };
    value.scaleX = number(u"scaleX"_s, 1);
    value.scaleY = number(u"scaleY"_s, 1);
    value.offsetX = number(u"offsetX"_s, 0);
    value.offsetY = number(u"offsetY"_s, 0);
    if (!validAdjustment(value)) return std::unexpected(u"Invalid glyph adjustment"_s);
    return value;
}

PixelComparison compare(const QImage& actual, const QImage& expected, const QRect& area) {
    PixelComparison result;
    for (int y = area.top(); y <= area.bottom(); ++y) {
        const auto* a = actual.constScanLine(y);
        const auto* e = expected.constScanLine(y);
        for (int x = area.left(); x <= area.right(); ++x) {
            const bool actualBlack = a[x] < 128;
            const bool expectedBlack = e[x] < 128;
            result.different += actualBlack != expectedBlack;
            result.intersection += actualBlack && expectedBlack;
            result.unionPixels += actualBlack || expectedBlack;
            if (actualBlack) result.actualInk |= QRect(x, y, 1, 1);
            if (expectedBlack) result.expectedInk |= QRect(x, y, 1, 1);
        }
    }
    return result;
}

void addComparison(QJsonObject& object, const PixelComparison& result) {
    object[u"differentPixels"_s] = result.different;
    object[u"intersectionPixels"_s] = result.intersection;
    object[u"unionPixels"_s] = result.unionPixels;
    object[u"iou"_s] = result.iou();
    object[u"actualInk"_s] = rectangle(result.actualInk);
    object[u"expectedInk"_s] = rectangle(result.expectedInk);
}

std::expected<QJsonObject, QString> renderGlyph(
    FT_Face face, QImage& image, const QJsonObject& cell, int width, int height,
    double baseline, const Adjustment& global, const QJsonObject& localObject, bool noHinting,
    const QJsonObject& profile = {}) {
    const auto codepoint = cell[u"codepoint"_s].toString();
    bool parsed = false;
    const auto character = codepoint.mid(2).toUInt(&parsed, 16);
    if (!codepoint.startsWith(u"U+") || !parsed || character > 0x10ffff
        || (character >= 0xd800 && character <= 0xdfff))
        return std::unexpected(u"Invalid Unicode scalar: "_s + codepoint);
    const auto local = adjustment(localObject);
    if (!local) return std::unexpected(local.error() + u": "_s + codepoint);
    const int requestedHeight = height;
    width = std::max(minimumFont0Dimension, width);
    height = std::max(minimumFont0Dimension, height);
    const double pixelHeight = height * global.scaleY * local->scaleY;
    const double pixelWidth = width * global.scaleX * local->scaleX;
    if (pixelHeight < 1.0 / 64 || pixelHeight > 32000 || pixelWidth < 1.0 / 64 || pixelWidth > 32000
        || pixelWidth * pixelHeight > maximumPixels || pixelWidth / pixelHeight > 32000)
        return std::unexpected(u"Transformed glyph dimensions exceed the limit: "_s + codepoint);
    const auto size = static_cast<FT_F26Dot6>(std::llround(pixelHeight * 64));
    if (FT_Set_Char_Size(face, size, size, 72, 72))
        return std::unexpected(u"FreeType rejected the glyph size: "_s + codepoint);
    // Match the production printer raster: fit hints at Y ppem, then stretch X.
    FT_Matrix matrix{static_cast<FT_Fixed>(std::llround(pixelWidth / pixelHeight * 65536)), 0, 0, 65536};
    FT_Vector delta{
        static_cast<FT_Pos>(std::llround(width * (global.offsetX + local->offsetX) * 64)),
        static_cast<FT_Pos>(std::llround(height * (global.offsetY + local->offsetY) * 64))};
    const auto orientation = profile[u"orientation"_s].toString(u"N"_s);
    const auto anchor = profile[u"anchor"_s].toString(u"FT"_s);
    if (anchor != u"FO" && anchor != u"FT") return std::unexpected(u"Unknown atlas anchor: "_s + anchor);
    const auto stretch = matrix.xx;
    if (orientation == u"R") { matrix = {0, 65536, -stretch, 0}; delta = {delta.y, -delta.x}; }
    else if (orientation == u"I") { matrix = {-stretch, 0, 0, -65536}; delta = {-delta.x, -delta.y}; }
    else if (orientation == u"B") { matrix = {0, -65536, stretch, 0}; delta = {-delta.y, delta.x}; }
    else if (orientation != u"N") return std::unexpected(u"Unknown atlas orientation: "_s + orientation);
    FT_Set_Transform(face, &matrix, &delta);
    const auto index = FT_Get_Char_Index(face, character);
    QJsonObject result{{u"codepoint"_s, codepoint}, {u"glyphIndex"_s, qint64(index)},
                       {u"missingGlyph"_s, index == 0}};
    if (!index) return result;
    if (localObject.contains(u"pointEdits"_s)) {
        if (!noHinting) return std::unexpected(u"Point edits require --no-hinting"_s);
        if (!localObject[u"pointEdits"_s].isArray()) return std::unexpected(u"pointEdits must be an array: "_s + codepoint);
        // Edit original integer design coordinates, then scale exactly once.
        // Applying rounded deltas after raster scaling would measure a different
        // outline from the final baked TTF, especially at small printer sizes.
        FT_Set_Transform(face, nullptr, nullptr);
        if (const auto error = FT_Load_Glyph(face, index,
                FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP | FT_LOAD_IGNORE_TRANSFORM); error)
            return std::unexpected(u"FreeType outline error %1: %2"_s.arg(error).arg(codepoint));
        if (face->glyph->format != FT_GLYPH_FORMAT_OUTLINE)
            return std::unexpected(u"Point edits require an outline glyph: "_s + codepoint);
        auto& outline = face->glyph->outline;
        QSet<int> editedPoints;
        for (const auto editValue : localObject[u"pointEdits"_s].toArray()) {
            const auto edit = editValue.toObject();
            const int point = edit[u"point"_s].toInt(-1);
            const double dx = edit[u"dx"_s].toDouble(std::numeric_limits<double>::quiet_NaN());
            const double dy = edit[u"dy"_s].toDouble(std::numeric_limits<double>::quiet_NaN());
            if (point < 0 || point >= outline.n_points || editedPoints.contains(point)
                || !std::isfinite(dx) || !std::isfinite(dy) || dx != std::round(dx) || dy != std::round(dy)
                || std::abs(dx) > face->units_per_EM * 16 || std::abs(dy) > face->units_per_EM * 16)
                return std::unexpected(u"Invalid design-coordinate point edit: "_s + codepoint);
            outline.points[point].x += static_cast<FT_Pos>(dx);
            outline.points[point].y += static_cast<FT_Pos>(dy);
            editedPoints.insert(point);
        }
        const FT_Matrix sizeMatrix{face->size->metrics.x_scale, 0, 0, face->size->metrics.y_scale};
        face->glyph->advance = {FT_MulFix(face->glyph->metrics.horiAdvance, sizeMatrix.xx), 0};
        FT_Vector_Transform(&face->glyph->advance, &matrix);
        FT_Outline_Transform(&outline, &sizeMatrix);
        FT_Outline_Transform(&outline, &matrix);
        FT_Outline_Translate(&outline, delta.x, delta.y);
        if (const auto error = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_MONO); error)
            return std::unexpected(u"FreeType edited raster error %1: %2"_s.arg(error).arg(codepoint));
    } else {
        const FT_Int32 flags = FT_LOAD_RENDER | FT_LOAD_TARGET_MONO | FT_LOAD_NO_BITMAP
            | (noHinting ? FT_LOAD_NO_HINTING : 0);
        if (const auto error = FT_Load_Glyph(face, index, flags); error)
            return std::unexpected(u"FreeType glyph error %1: %2"_s.arg(error).arg(codepoint));
    }
    const auto& bitmap = face->glyph->bitmap;
    if (qint64(bitmap.width) * bitmap.rows > maximumPixels
        || bitmap.width > 32000 || bitmap.rows > 32000)
        return std::unexpected(u"Glyph bitmap exceeds the limit: "_s + codepoint);
    if (bitmap.width && bitmap.rows && bitmap.pixel_mode != FT_PIXEL_MODE_MONO)
        return std::unexpected(u"FreeType returned a non-bitonal glyph: "_s + codepoint);
    const double unadjustedAdvance = double(orientation == u"R" ? -face->glyph->advance.y
        : orientation == u"I" ? -face->glyph->advance.x
        : orientation == u"B" ? face->glyph->advance.y : face->glyph->advance.x) / 64;
    double advance = unadjustedAdvance;
    if (localObject.contains(u"advance"_s)) {
        const double requested = localObject[u"advance"_s].toDouble(-1);
        if (!std::isfinite(requested) || requested < 0 || requested > 16)
            return std::unexpected(u"Invalid advance override: "_s + codepoint);
        advance = requested * width;
    }
    result[u"advance"_s] = advance;
    result[u"unadjustedAdvance"_s] = unadjustedAdvance;
    result[u"bitmap"_s] = QJsonObject{{u"left"_s, face->glyph->bitmap_left},
        {u"top"_s, face->glyph->bitmap_top}, {u"width"_s, qint64(bitmap.width)},
        {u"height"_s, qint64(bitmap.rows)}};
    int originX = cell[u"anchorX"_s].toInt(cell[u"x"_s].toInt());
    int originY = cell[u"anchorY"_s].toInt(cell[u"y"_s].toInt() + (anchor == u"FT" ? int(std::floor(requestedHeight * baseline)) : 0));
    if (anchor == u"FO") {
        // ^FO rotates the nominal em cell, including side bearings/descent.
        // The normalized research fonts use the Font 0 cap baseline, 0.75 em.
        const int ascent = int(std::floor(height * 0.75));
        const int span = int(std::floor(advance));
        if (orientation == u"N") originY += ascent;
        else if (orientation == u"R") originX += height - ascent;
        else if (orientation == u"I") { originX += span; originY += height - ascent; }
        else { originX += ascent; originY += span; }
    }
    result[u"origin"_s] = QJsonArray{originX, originY};
    for (unsigned int y = 0; y < bitmap.rows; ++y) {
        const auto* source = bitmap.buffer + (bitmap.pitch >= 0 ? y : bitmap.rows - y - 1) * std::abs(bitmap.pitch);
        const qint64 targetY = qint64(originY) - face->glyph->bitmap_top + y;
        if (targetY < 0 || targetY >= image.height()) continue;
        auto* target = image.scanLine(int(targetY));
        for (unsigned int x = 0; x < bitmap.width; ++x) {
            const qint64 targetX = qint64(originX) + face->glyph->bitmap_left + x;
            if (targetX >= 0 && targetX < image.width() && (source[x / 8] & (128 >> (x % 8))))
                target[int(targetX)] = 0;
        }
    }
    return result;
}

struct FitSample {
    QImage expected;
    QJsonObject cell;
    int width = 0;
    int height = 0;
    QRect expectedInk;
};

struct FitLoss {
    qint64 different = 0;
    qint64 unionPixels = 0;
    [[nodiscard]] double value() const { return unionPixels ? double(different) / unionPixels : 0; }
};

std::expected<FitLoss, QString> measure(
    FT_Face face, const QList<FitSample>& samples, const Adjustment& value,
    double baseline, const Adjustment& global, bool noHinting, const QJsonArray* pointEdits = nullptr) {
    FitLoss loss;
    auto localObject = adjustmentJson(value);
    if (pointEdits) localObject[u"pointEdits"_s] = *pointEdits;
    for (const auto& sample : samples) {
        QImage actual(sample.expected.size(), QImage::Format_Grayscale8);
        if (actual.isNull()) return std::unexpected(u"Cannot allocate fit image"_s);
        actual.fill(255);
        const auto rendered = renderGlyph(face, actual, sample.cell, sample.width, sample.height,
                                           baseline, global, localObject, noHinting);
        if (!rendered) return std::unexpected(rendered.error());
        const auto bitmap = (*rendered)[u"bitmap"_s].toObject();
        const auto origin = (*rendered)[u"origin"_s].toArray();
        QRect actualBounds;
        if (origin.size() == 2 && bitmap[u"width"_s].toInt() > 0 && bitmap[u"height"_s].toInt() > 0)
            actualBounds = QRect(origin[0].toInt() + bitmap[u"left"_s].toInt(),
                                 origin[1].toInt() - bitmap[u"top"_s].toInt(),
                                 bitmap[u"width"_s].toInt(), bitmap[u"height"_s].toInt());
        // Outside this union both images are white. Skipping the atlas padding
        // preserves the exact objective while speeding large donor sweeps.
        const QRect relevant = sample.expectedInk.united(actualBounds).intersected(actual.rect());
        for (int y = relevant.top(); y <= relevant.bottom(); ++y) {
            const auto* a = actual.constScanLine(y);
            const auto* e = sample.expected.constScanLine(y);
            for (int x = relevant.left(); x <= relevant.right(); ++x) {
                const bool actualBlack = a[x] < 128;
                const bool expectedBlack = e[x] < 128;
                loss.different += actualBlack != expectedBlack;
                loss.unionPixels += actualBlack || expectedBlack;
            }
        }
    }
    return loss;
}

struct OutlineDesign {
    QList<FT_Vector> points;
    QList<int> contourEnds;
    FT_BBox bounds{};
    int unitsPerEm = 0;
};

struct CoordinateWarp {
    // Fixed endpoints; three internal control positions for each axis.
    std::array<double, 5> x{};
    std::array<double, 5> y{};
};

bool orderedWarp(const std::array<double, 5>& shifts, FT_Pos minimum, FT_Pos maximum) {
    if (maximum <= minimum) return std::ranges::all_of(shifts, [](double shift) { return shift == 0; });
    double previous = double(minimum) + shifts[0];
    for (int i = 1; i < 5; ++i) {
        const double current = minimum + (maximum - minimum) * (i / 4.0) + shifts[i];
        if (current <= previous) return false;
        previous = current;
    }
    return true;
}

FT_Pos warpedCoordinate(FT_Pos coordinate, FT_Pos minimum, FT_Pos maximum, const std::array<double, 5>& shifts) {
    if (maximum <= minimum) return coordinate;
    const double position = std::clamp(double(coordinate - minimum) / (maximum - minimum) * 4, 0.0, 4.0);
    const int index = std::min(3, int(position));
    return static_cast<FT_Pos>(std::llround(coordinate + std::lerp(shifts[index], shifts[index + 1], position - index)));
}

std::expected<QJsonArray, QString> outlinePointEdits(const OutlineDesign& design, const CoordinateWarp& warp) {
    if (!orderedWarp(warp.x, design.bounds.xMin, design.bounds.xMax)
        || !orderedWarp(warp.y, design.bounds.yMin, design.bounds.yMax))
        return std::unexpected(u"Warp control positions must remain ordered"_s);
    QJsonArray edits;
    QList<FT_Vector> moved;
    moved.reserve(design.points.size());
    for (qsizetype i = 0; i < design.points.size(); ++i) {
        const auto& before = design.points[i];
        const FT_Vector after{
            warpedCoordinate(before.x, design.bounds.xMin, design.bounds.xMax, warp.x),
            warpedCoordinate(before.y, design.bounds.yMin, design.bounds.yMax, warp.y)};
        moved.append(after);
        if (after.x != before.x || after.y != before.y)
            edits.append(QJsonObject{{u"point"_s, qint64(i)}, {u"dx"_s, qint64(after.x - before.x)},
                                     {u"dy"_s, qint64(after.y - before.y)}});
    }
    // Keep point tags, contour boundaries and winding. Reject degenerate or
    // reversed contour control polygons in addition to non-monotone warps.
    int first = 0;
    for (const int last : design.contourEnds) {
        qint64 originalArea = 0, movedArea = 0;
        for (int i = first; i <= last; ++i) {
            const int next = i == last ? first : i + 1;
            originalArea += qint64(design.points[i].x) * design.points[next].y
                - qint64(design.points[next].x) * design.points[i].y;
            movedArea += qint64(moved[i].x) * moved[next].y - qint64(moved[next].x) * moved[i].y;
        }
        if (originalArea && ((originalArea > 0) != (movedArea > 0) || !movedArea))
            return std::unexpected(u"Warp would reverse or collapse a contour"_s);
        first = last + 1;
    }
    return edits;
}

double warpPenalty(const CoordinateWarp& warp, double em) {
    double total = 0;
    for (int i = 1; i < 4; ++i) total += warp.x[i] * warp.x[i] + warp.y[i] * warp.y[i];
    return 0.05 * total / (6 * em * em);
}

std::expected<QJsonObject, QString> fitOutlineGlyph(
    FT_Face face, const QString& codepoint, const QList<FitSample>& samples,
    double baseline, const Adjustment& global) {
    const auto glyphIndex = FT_Get_Char_Index(face, codepoint.mid(2).toUInt(nullptr, 16));
    QJsonArray bestEdits;
    auto bestLoss = measure(face, samples, {}, baseline, global, true, &bestEdits);
    if (!bestLoss) return std::unexpected(bestLoss.error());
    const auto initialLoss = *bestLoss;
    OutlineDesign design;
    design.unitsPerEm = face->units_per_EM;
    if (glyphIndex) {
        FT_Set_Transform(face, nullptr, nullptr);
        if (FT_Load_Glyph(face, glyphIndex, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP))
            return std::unexpected(u"Cannot load candidate contours for "_s + codepoint);
        if (face->glyph->format != FT_GLYPH_FORMAT_OUTLINE)
            return std::unexpected(u"Outline fitting requires vector contours: "_s + codepoint);
        auto& outline = face->glyph->outline;
        if (FT_Outline_Check(&outline)) return std::unexpected(u"Invalid candidate outline: "_s + codepoint);
        for (int i = 0; i < outline.n_points; ++i) design.points.append(outline.points[i]);
        for (int i = 0; i < outline.n_contours; ++i) design.contourEnds.append(outline.contours[i]);
        FT_Outline_Get_CBox(&outline, &design.bounds);
    }
    CoordinateWarp best;
    double bestObjective = bestLoss->value();
    int evaluations = 1;
    constexpr double steps[]{0.012, 0.006, 0.003};
    if (!design.points.isEmpty() && design.unitsPerEm > 0 && bestLoss->different) {
        const double limit = std::floor(design.unitsPerEm * 0.03);
        for (const double emStep : steps) {
            const double step = std::max(1.0, std::round(design.unitsPerEm * emStep));
            for (int pass = 0; pass < 2; ++pass) {
                bool changed = false;
                for (int parameter = 0; parameter < 6; ++parameter) {
                    const auto current = best;
                    for (const double direction : {-1.0, 1.0}) {
                        auto candidate = current;
                        auto& axis = parameter < 3 ? candidate.x : candidate.y;
                        const int knot = parameter % 3 + 1;
                        axis[knot] += direction * step;
                        if (std::abs(axis[knot]) > limit) continue;
                        const auto edits = outlinePointEdits(design, candidate);
                        if (!edits) continue; // The candidate violates geometric constraints.
                        const auto candidateLoss = measure(face, samples, {}, baseline, global, true, &*edits);
                        if (!candidateLoss) return std::unexpected(candidateLoss.error());
                        ++evaluations;
                        const double objective = candidateLoss->value() + warpPenalty(candidate, design.unitsPerEm);
                        if (objective + 1e-12 < bestObjective) {
                            best = candidate; bestEdits = *edits; bestLoss = candidateLoss;
                            bestObjective = objective; changed = true;
                        }
                    }
                }
                if (!changed) break;
            }
        }
    }
    QJsonArray xKnots, yKnots;
    for (int i = 0; i < 5; ++i) { xKnots.append(best.x[i]); yKnots.append(best.y[i]); }
    auto result = adjustmentJson({});
    result[u"pointEdits"_s] = bestEdits;
    result[u"sourcePointCount"_s] = design.points.size();
    result[u"unitsPerEm"_s] = design.unitsPerEm;
    result[u"warp"_s] = QJsonObject{{u"xKnotOffsets"_s, xKnots}, {u"yKnotOffsets"_s, yKnots},
        {u"bounds"_s, QJsonArray{qint64(design.bounds.xMin), qint64(design.bounds.yMin),
                                 qint64(design.bounds.xMax), qint64(design.bounds.yMax)}},
        {u"maximumOffsetEm"_s, 0.03}, {u"regularization"_s, warpPenalty(best, std::max(1, design.unitsPerEm))}};
    result[u"missingGlyph"_s] = glyphIndex == 0;
    result[u"samples"_s] = samples.size();
    result[u"evaluations"_s] = evaluations;
    result[u"initialLoss"_s] = initialLoss.value();
    result[u"loss"_s] = bestLoss->value();
    result[u"differentPixels"_s] = bestLoss->different;
    result[u"unionPixels"_s] = bestLoss->unionPixels;
    return result;
}

std::expected<QJsonObject, QString> fitGlyphs(
    FT_Face face, const QJsonObject& manifest, const QDir& inputs, const QString& caseFilter,
    double baseline, const Adjustment& global, const QJsonObject& startingAdjustments,
    bool noHinting, bool fitOutline) {
    // Validation pages never enter the fit, even if --role selects them for
    // the subsequent report. Each codepoint gets one transform for all sizes.
    QMap<QString, QList<FitSample>> samplesByCharacter;
    QSet<QString> unsupported;
    for (const auto value : manifest[u"unsupportedCharacters"_s].toArray()) unsupported.insert(value.toString());
    QJsonArray trainingCases;
    const QRegularExpression safeName(u"^[A-Za-z0-9_-]+$"_s);
    for (const auto value : manifest[u"cases"_s].toArray()) {
        const auto profile = value.toObject();
        const auto name = profile[u"name"_s].toString();
        if (profile[u"role"_s].toString(u"train"_s) != u"train"
            || profile[u"orientation"_s].toString(u"N"_s) != u"N"
            || profile[u"anchor"_s].toString(u"FT"_s) != u"FT"
            || !name.contains(caseFilter)) continue;
        if (!safeName.match(name).hasMatch()) return std::unexpected(u"Invalid fit case name"_s);
        const int height = profile[u"fontHeight"_s].toInt();
        const int width = profile[u"fontWidth"_s].toInt(height);
        const QSize atlasSize(profile[u"width"_s].toInt(manifest[u"width"_s].toInt()),
                              profile[u"height"_s].toInt(manifest[u"height"_s].toInt()));
        if (!validDimension(atlasSize.width()) || !validDimension(atlasSize.height())
            || qint64(atlasSize.width()) * atlasSize.height() > maximumPixels)
            return std::unexpected(u"Invalid fit atlas dimensions: "_s + name);
        if (!validDimension(width) || !validDimension(height)) return std::unexpected(u"Invalid fit dimensions: "_s + name);
        const QImage golden = QImage(inputs.filePath(name + u"-labelary-bitonal.png"_s)).convertToFormat(QImage::Format_Grayscale8);
        if (golden.isNull() || golden.size() != atlasSize) return std::unexpected(u"Cannot read fit golden: "_s + name);
        trainingCases.append(name);
        for (const auto cellValue : profile[u"cells"_s].toArray()) {
            auto cell = cellValue.toObject();
            const auto codepoint = cell[u"codepoint"_s].toString();
            if (unsupported.contains(codepoint)) continue;
            const QRect bounds(cell[u"x"_s].toInt(-1), cell[u"y"_s].toInt(-1),
                               cell[u"width"_s].toInt(), cell[u"height"_s].toInt());
            if (!bounds.isValid() || !golden.rect().contains(bounds))
                return std::unexpected(u"Invalid fit cell: "_s + name);
            const QRect area = comparisonArea(cell, golden.rect());
            cell[u"x"_s] = bounds.x() - area.x();
            cell[u"y"_s] = bounds.y() - area.y();
            if (cell.contains(u"anchorX"_s)) cell[u"anchorX"_s] = cell[u"anchorX"_s].toInt() - area.x();
            if (cell.contains(u"anchorY"_s)) cell[u"anchorY"_s] = cell[u"anchorY"_s].toInt() - area.y();
            const auto expected = golden.copy(area);
            const auto ink = compare(expected, expected, expected.rect()).expectedInk;
            samplesByCharacter[codepoint].append({expected, cell, width, height, ink});
        }
    }
    if (samplesByCharacter.isEmpty()) return std::unexpected(u"No normal FT training glyphs matched --fit"_s);
    QJsonObject fitted;
    int fittedCount = 0;
    for (auto entry = samplesByCharacter.cbegin(); entry != samplesByCharacter.cend(); ++entry) {
        const auto& codepoint = entry.key();
        const auto& samples = entry.value();
        if (fitOutline) {
            const auto glyph = fitOutlineGlyph(face, codepoint, samples, baseline, global);
            if (!glyph) return std::unexpected(glyph.error());
            fitted[codepoint] = *glyph;
            ++fittedCount;
            if (fittedCount % 25 == 0)
                QTextStream(stderr) << "Refined " << fittedCount << '/' << samplesByCharacter.size() << " glyph contours" << Qt::endl;
            continue;
        }
        const auto original = adjustment(startingAdjustments[codepoint].toObject());
        if (!original) return std::unexpected(original.error() + u": "_s + codepoint);
        Adjustment best = *original;
        auto bestLoss = measure(face, samples, best, baseline, global, noHinting);
        if (!bestLoss) return std::unexpected(bestLoss.error());
        const FitLoss initialLoss = *bestLoss;
        int evaluations = 1;
        bool validCodepoint = false;
        const auto character = codepoint.mid(2).toUInt(&validCodepoint, 16);
        const auto glyphIndex = validCodepoint ? FT_Get_Char_Index(face, character) : 0;
        if (glyphIndex) {
            // Start from contour bounds against the largest training raster.
            // This measures the golden; it does not derive contours from pixels.
            const auto largest = std::ranges::max_element(samples, {}, [](const FitSample& s) { return s.width * s.height; });
            const auto expectedBounds = largest->expectedInk;
            FT_Set_Transform(face, nullptr, nullptr);
            if (!expectedBounds.isEmpty()
                && FT_Load_Glyph(face, glyphIndex, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) == 0
                && face->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
                FT_BBox bounds{};
                FT_Outline_Get_CBox(&face->glyph->outline, &bounds);
                const double em = face->units_per_EM;
                if (em > 0 && bounds.xMax > bounds.xMin && bounds.yMax > bounds.yMin) {
                    const int effectiveWidth = std::max(minimumFont0Dimension, largest->width);
                    const int effectiveHeight = std::max(minimumFont0Dimension, largest->height);
                    Adjustment initial;
                    initial.scaleX = expectedBounds.width() * em / ((bounds.xMax - bounds.xMin) * effectiveWidth * global.scaleX);
                    initial.scaleY = expectedBounds.height() * em / ((bounds.yMax - bounds.yMin) * effectiveHeight * global.scaleY);
                    const double originX = largest->cell[u"anchorX"_s].toInt(largest->cell[u"x"_s].toInt());
                    const double originY = largest->cell[u"anchorY"_s].toDouble(largest->cell[u"y"_s].toInt() + std::floor(largest->height * baseline));
                    initial.offsetX = (expectedBounds.x() + expectedBounds.width() / 2.0 - originX) / effectiveWidth
                        - (bounds.xMin + bounds.xMax) / (2 * em) * initial.scaleX * global.scaleX - global.offsetX;
                    initial.offsetY = (originY - expectedBounds.y() - expectedBounds.height() / 2.0) / effectiveHeight
                        - (bounds.yMin + bounds.yMax) / (2 * em) * initial.scaleY * global.scaleY - global.offsetY;
                    if (validAdjustment(initial)) {
                        const auto candidateLoss = measure(face, samples, initial, baseline, global, noHinting);
                        if (!candidateLoss) return std::unexpected(candidateLoss.error());
                        ++evaluations;
                        if (candidateLoss->value() < bestLoss->value()) { best = initial; bestLoss = candidateLoss; }
                    }
                }
            }
            // Absolute em offsets keep one size-independent correction per glyph.
            // A fixed iteration budget and tie handling make fitting reproducible.
            constexpr double scaleSteps[]{0.1, 0.04, 0.015, 0.005};
            constexpr double offsetSteps[]{0.03, 0.01, 0.004, 0.001};
            for (int level = 0; level < 4 && bestLoss->different; ++level) {
                for (int pass = 0; pass < 3; ++pass) {
                    bool changed = false;
                    for (int parameter = 0; parameter < 4; ++parameter) {
                        const Adjustment current = best;
                        for (const double direction : {-1.0, 1.0}) {
                            Adjustment candidate = current;
                            const double step = direction * (parameter < 2 ? scaleSteps[level] : offsetSteps[level]);
                            if (parameter == 0) candidate.scaleX += step;
                            else if (parameter == 1) candidate.scaleY += step;
                            else if (parameter == 2) candidate.offsetX += step;
                            else candidate.offsetY += step;
                            if (!validAdjustment(candidate)) continue;
                            const auto candidateLoss = measure(face, samples, candidate, baseline, global, noHinting);
                            if (!candidateLoss) return std::unexpected(candidateLoss.error());
                            ++evaluations;
                            if (candidateLoss->value() + 1e-12 < bestLoss->value()) {
                                best = candidate; bestLoss = candidateLoss; changed = true;
                            }
                        }
                    }
                    if (!changed) break;
                }
            }
        }
        auto result = adjustmentJson(best);
        result[u"missingGlyph"_s] = glyphIndex == 0;
        result[u"samples"_s] = samples.size();
        result[u"evaluations"_s] = evaluations;
        result[u"initialLoss"_s] = initialLoss.value();
        result[u"loss"_s] = bestLoss->value();
        result[u"differentPixels"_s] = bestLoss->different;
        result[u"unionPixels"_s] = bestLoss->unionPixels;
        fitted[codepoint] = result;
        ++fittedCount;
        if (fittedCount % 25 == 0)
            QTextStream(stderr) << "Fitted " << fittedCount << '/' << samplesByCharacter.size() << " glyphs" << Qt::endl;
    }
    return QJsonObject{{u"schemaVersion"_s, 1},
        {u"method"_s, fitOutline ? u"bounded monotone coordinate-warp refinement"_s : u"size-independent outline affine coordinate descent"_s},
        {u"loss"_s, u"sum(differentPixels)/sum(unionPixels) over training samples"_s},
        {u"excludesOracleUnsupported"_s, true}, {u"trainingCases"_s, trainingCases}, {u"glyphs"_s, fitted}};
}

std::expected<QJsonObject, QString> run(const QCommandLineParser& parser) {
    const bool fitOutline = parser.isSet(u"fit-outline"_s);
    if (fitOutline && (parser.isSet(u"fit"_s) || parser.isSet(u"adjustments"_s)))
        return std::unexpected(u"--fit-outline operates on an already composed font; omit --fit and --adjustments"_s);
    if (fitOutline && !parser.isSet(u"no-hinting"_s))
        return std::unexpected(u"--fit-outline requires --no-hinting"_s);
    const auto manifest = readObject(parser.value(u"manifest"_s));
    if (!manifest) return std::unexpected(manifest.error());
    const auto fontBytes = readFile(parser.value(u"font"_s), 128 * 1024 * 1024);
    if (!fontBytes) return std::unexpected(fontBytes.error());
    QJsonObject adjustments;
    if (parser.isSet(u"adjustments"_s)) {
        const auto object = readObject(parser.value(u"adjustments"_s));
        if (!object) return std::unexpected(object.error());
        adjustments = object->contains(u"glyphs"_s) ? (*object)[u"glyphs"_s].toObject() : *object;
    }
    const int imageWidth = (*manifest)[u"width"_s].toInt();
    const int imageHeight = (*manifest)[u"height"_s].toInt();
    if (!validDimension(imageWidth) || !validDimension(imageHeight)
        || qint64(imageWidth) * imageHeight > maximumPixels)
        return std::unexpected(u"Invalid atlas dimensions"_s);
    Adjustment global;
    const auto number = [&](const QString& option) {
        bool ok = false;
        const auto value = parser.value(option).toDouble(&ok);
        return ok ? value : std::numeric_limits<double>::quiet_NaN();
    };
    global.scaleX = number(u"scale-x"_s); global.scaleY = number(u"scale-y"_s);
    global.offsetX = number(u"offset-x"_s); global.offsetY = number(u"offset-y"_s);
    const double baseline = number(u"baseline"_s);
    if (!validAdjustment(global) || !std::isfinite(baseline) || baseline < -4 || baseline > 4)
        return std::unexpected(u"Invalid transform or baseline option"_s);
    if (parser.value(u"role"_s) != u"all" && parser.value(u"role"_s) != u"train" && parser.value(u"role"_s) != u"validation")
        return std::unexpected(u"Role must be train, validation or all"_s);
    const QDir inputs(QFileInfo(parser.value(u"manifest"_s)).absolutePath());
    const QDir outputs(QFileInfo(parser.value(u"output-dir"_s)).absoluteFilePath());
    if (!QDir().mkpath(outputs.absolutePath())) return std::unexpected(u"Cannot create output directory"_s);
    FT_Library rawLibrary = nullptr;
    if (FT_Init_FreeType(&rawLibrary)) return std::unexpected(u"Cannot initialize FreeType"_s);
    const Library library(rawLibrary);
    FT_Face rawFace = nullptr;
    if (FT_New_Memory_Face(library.get(), reinterpret_cast<const FT_Byte*>(fontBytes->constData()),
                           static_cast<FT_Long>(fontBytes->size()), 0, &rawFace))
        return std::unexpected(u"Cannot load the donor font"_s);
    const Face face(rawFace);
    if (FT_Select_Charmap(face.get(), FT_ENCODING_UNICODE))
        return std::unexpected(u"Donor font has no Unicode character map"_s);
    if (parser.isSet(u"fit"_s) || fitOutline) {
        auto fitted = fitGlyphs(face.get(), *manifest, inputs, parser.value(u"case"_s), baseline, global,
                                adjustments, parser.isSet(u"no-hinting"_s), fitOutline);
        if (!fitted) return std::unexpected(fitted.error());
        (*fitted)[u"fontSha256"_s] = QString::fromLatin1(QCryptographicHash::hash(*fontBytes, QCryptographicHash::Sha256).toHex());
        (*fitted)[u"noHinting"_s] = parser.isSet(u"no-hinting"_s);
        (*fitted)[u"global"_s] = adjustmentJson(global);
        adjustments = (*fitted)[u"glyphs"_s].toObject();
        QSaveFile fittedFile(outputs.filePath(u"fitted-adjustments.json"_s));
        const auto fittedBytes = QJsonDocument(*fitted).toJson(QJsonDocument::Indented);
        if (!fittedFile.open(QIODevice::WriteOnly) || fittedFile.write(fittedBytes) != fittedBytes.size() || !fittedFile.commit())
            return std::unexpected(u"Cannot save fitted-adjustments.json: "_s + fittedFile.errorString());
    }
    QSet<QString> unsupported;
    for (const auto value : (*manifest)[u"unsupportedCharacters"_s].toArray()) unsupported.insert(value.toString());
    QJsonArray cases;
    qint64 totalDifferent = 0, totalIntersection = 0, totalUnion = 0;
    int totalGlyphs = 0, missingGlyphs = 0, exactGlyphs = 0;
    const QRegularExpression safeName(u"^[A-Za-z0-9_-]+$"_s);
    for (const auto value : (*manifest)[u"cases"_s].toArray()) {
        const auto profile = value.toObject();
        const auto name = profile[u"name"_s].toString();
        if (!name.contains(parser.value(u"case"_s))) continue;
        const auto role = profile[u"role"_s].toString(u"train"_s);
        if (parser.value(u"role"_s) != u"all" && role != parser.value(u"role"_s)) continue;
        if (!safeName.match(name).hasMatch()) return std::unexpected(u"Invalid atlas case name"_s);
        const int height = profile[u"fontHeight"_s].toInt();
        const int width = profile[u"fontWidth"_s].toInt(height);
        if (!validDimension(width) || !validDimension(height)) return std::unexpected(u"Invalid font dimensions: "_s + name);
        const int caseWidth = profile[u"width"_s].toInt(imageWidth);
        const int caseHeight = profile[u"height"_s].toInt(imageHeight);
        if (!validDimension(caseWidth) || !validDimension(caseHeight)
            || qint64(caseWidth) * caseHeight > maximumPixels)
            return std::unexpected(u"Invalid case atlas dimensions: "_s + name);
        const QImage golden = QImage(inputs.filePath(name + u"-labelary-bitonal.png"_s)).convertToFormat(QImage::Format_Grayscale8);
        if (golden.isNull() || golden.size() != QSize(caseWidth, caseHeight))
            return std::unexpected(u"Cannot load matching golden image: "_s + name);
        QImage actual(caseWidth, caseHeight, QImage::Format_Grayscale8);
        if (actual.isNull()) return std::unexpected(u"Cannot allocate atlas image"_s);
        actual.fill(255);
        const auto cells = profile[u"cells"_s].toArray();
        QJsonArray glyphs;
        for (const auto cellValue : cells) {
            const auto cell = cellValue.toObject();
            const QRect cellRectangle(cell[u"x"_s].toInt(-1), cell[u"y"_s].toInt(-1),
                                      cell[u"width"_s].toInt(), cell[u"height"_s].toInt());
            if (!cellRectangle.isValid() || !actual.rect().contains(cellRectangle))
                return std::unexpected(u"Cell outside the atlas: "_s + name);
            const auto codepoint = cell[u"codepoint"_s].toString();
            const auto rendered = renderGlyph(face.get(), actual, cell, width, height, baseline, global,
                                               adjustments[codepoint].toObject(), parser.isSet(u"no-hinting"_s), profile);
            if (!rendered) return std::unexpected(rendered.error());
            glyphs.append(*rendered);
        }
        if (parser.isSet(u"glyph-images"_s) && !QDir().mkpath(outputs.filePath(name)))
            return std::unexpected(u"Cannot create glyph-image directory"_s);
        for (qsizetype i = 0; i < cells.size(); ++i) {
            const auto cell = cells[i].toObject();
            auto glyph = glyphs[i].toObject();
            const auto codepoint = glyph[u"codepoint"_s].toString();
            const QRect area = comparisonArea(cell, actual.rect());
            const auto comparison = compare(actual, golden, area);
            addComparison(glyph, comparison);
            glyph[u"area"_s] = rectangle(area);
            const auto originArray = glyph[u"origin"_s].toArray();
            const QPoint origin(originArray.size() == 2 ? originArray[0].toInt() : cell[u"x"_s].toInt(),
                                originArray.size() == 2 ? originArray[1].toInt() : cell[u"y"_s].toInt() + int(std::floor(height * baseline)));
            glyph[u"actualInkFromBaseline"_s] = rectangle(comparison.actualInk.translated(-origin));
            glyph[u"expectedInkFromBaseline"_s] = rectangle(comparison.expectedInk.translated(-origin));
            glyph[u"oracleUnsupported"_s] = unsupported.contains(codepoint);
            if (!unsupported.contains(codepoint)) {
                totalDifferent += comparison.different; totalIntersection += comparison.intersection;
                totalUnion += comparison.unionPixels; ++totalGlyphs;
                missingGlyphs += glyph[u"missingGlyph"_s].toBool();
                exactGlyphs += comparison.different == 0 && !glyph[u"missingGlyph"_s].toBool();
            }
            if (parser.isSet(u"glyph-images"_s)) {
                const auto base = outputs.filePath(name + u'/' + codepoint);
                if (!actual.copy(area).save(base + u"-actual.png"_s)
                    || !golden.copy(area).save(base + u"-expected.png"_s))
                    return std::unexpected(u"Cannot save glyph image: "_s + base);
            }
            glyphs[i] = glyph;
        }
        if (!parser.isSet(u"no-atlas-images"_s) && !actual.save(outputs.filePath(name + u"-actual.png"_s)))
            return std::unexpected(u"Cannot save atlas: "_s + name);
        QJsonObject caseResult{{u"case"_s, name}, {u"fontHeight"_s, height},
            {u"fontWidth"_s, width}, {u"effectiveFontHeight"_s, std::max(minimumFont0Dimension, height)},
            {u"effectiveFontWidth"_s, std::max(minimumFont0Dimension, width)},
            {u"role"_s, role}, {u"glyphs"_s, glyphs}};
        addComparison(caseResult, compare(actual, golden, actual.rect()));
        cases.append(caseResult);
    }
    if (cases.isEmpty()) return std::unexpected(u"No atlas cases matched"_s);
    int major = 0, minor = 0, patch = 0;
    FT_Library_Version(library.get(), &major, &minor, &patch);
    QJsonObject report{{u"schemaVersion"_s, 1}, {u"oracle"_s, u"Labelary Bitonal"_s},
        {u"font"_s, QFileInfo(parser.value(u"font"_s)).absoluteFilePath()},
        {u"fontSha256"_s, QString::fromLatin1(QCryptographicHash::hash(*fontBytes, QCryptographicHash::Sha256).toHex())},
        {u"freeTypeVersion"_s, u"%1.%2.%3"_s.arg(major).arg(minor).arg(patch)},
        {u"rasterizer"_s, parser.isSet(u"no-hinting"_s) ? u"FreeType mono unhinted"_s : u"FreeType mono hinted"_s},
        {u"parameters"_s, QJsonObject{{u"scaleX"_s, global.scaleX}, {u"scaleY"_s, global.scaleY},
            {u"minimumFont0Dimension"_s, minimumFont0Dimension},
            {u"offsetX"_s, global.offsetX}, {u"offsetY"_s, global.offsetY}, {u"baseline"_s, baseline}}},
        {u"adjustments"_s, adjustments}, {u"cases"_s, cases},
        {u"summary"_s, QJsonObject{{u"glyphComparisons"_s, totalGlyphs}, {u"missingGlyphs"_s, missingGlyphs},
            {u"exactGlyphs"_s, exactGlyphs}, {u"differentPixels"_s, totalDifferent},
            {u"intersectionPixels"_s, totalIntersection}, {u"unionPixels"_s, totalUnion},
            {u"iou"_s, totalUnion ? double(totalIntersection) / totalUnion : 1},
            {u"excludesOracleUnsupported"_s, true}}}};
    QSaveFile file(outputs.filePath(u"report.json"_s));
    if (!file.open(QIODevice::WriteOnly)) return std::unexpected(file.errorString());
    const auto output = QJsonDocument(report).toJson(QJsonDocument::Indented);
    if (file.write(output) != output.size() || !file.commit()) return std::unexpected(file.errorString());
    return report;
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription(u"Offline bitonal FreeType comparison of an explicitly supplied donor font against glyph atlases."_s);
    parser.addHelpOption();
    parser.addOptions({
        {u"font"_s, u"Donor TTF/OTF file. The tool does not infer licensing."_s, u"path"_s},
        {u"manifest"_s, u"Glyph atlas manifest; goldens are resolved relative to it."_s, u"path"_s},
        {u"output-dir"_s, u"Directory for report.json and rendered atlases."_s, u"path"_s},
        {u"adjustments"_s, u"JSON glyph map with scaleX/scaleY/offsetX/offsetY/advance and optional pointEdits. Offsets/advance are em fractions, pointEdits are integer design units; Y points upward."_s, u"path"_s},
        {u"scale-x"_s, u"Global horizontal scale."_s, u"factor"_s, u"1"_s},
        {u"scale-y"_s, u"Global vertical scale."_s, u"factor"_s, u"1"_s},
        {u"offset-x"_s, u"Global X translation as a fraction of requested width."_s, u"fraction"_s, u"0"_s},
        {u"offset-y"_s, u"Global upward Y translation as a fraction of requested height."_s, u"fraction"_s, u"0"_s},
        {u"baseline"_s, u"Baseline from cell top as a fraction of font height; committed ^FT atlases use 1."_s, u"fraction"_s, u"1"_s},
        {u"case"_s, u"Only process case names containing this text."_s, u"text"_s, QString{}},
        {u"role"_s, u"Report cases with role train, validation or all; legacy cases default to train."_s, u"role"_s, u"all"_s},
        {u"fit"_s, u"Fit one affine correction per glyph to normal FT training cases; write fitted-adjustments.json. Validation pages are excluded from fitting."_s},
        {u"fit-outline"_s, u"Refine contours of an already composed font with bounded monotone coordinate warps shared by all training sizes. Requires --no-hinting; writes design-unit pointEdits in fitted-adjustments.json."_s},
        {u"no-hinting"_s, u"Disable donor hints when researching outline geometry."_s},
        {u"no-atlas-images"_s, u"Write only the report (and optional glyph images), for faster parameter sweeps."_s},
        {u"glyph-images"_s, u"Also save individual actual and golden cell PNGs."_s}
    });
    parser.process(application);
    for (const auto option : {u"font"_s, u"manifest"_s, u"output-dir"_s}) {
        if (!parser.isSet(option)) {
            QTextStream(stderr) << "Missing required --" << option << '\n' << parser.helpText();
            return 2;
        }
    }
    const auto result = run(parser);
    if (!result) {
        QTextStream(stderr) << result.error() << Qt::endl;
        return 2;
    }
    QTextStream(stdout) << QJsonDocument((*result)[u"summary"_s].toObject()).toJson(QJsonDocument::Compact) << Qt::endl;
    // This is a research measurement; nonzero pixel differences are reported,
    // not treated as execution failures. Exact production tests remain strict.
    return 0;
}
