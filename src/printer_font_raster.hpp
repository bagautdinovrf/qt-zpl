#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtCore/QPoint>
#include <QtCore/QString>
#include <QtGui/QImage>
#include <QtGui/QTransform>
#include <expected>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <utility>
#include <ft2build.h>
#include FT_FREETYPE_H

namespace QtZpl {

// A glyph's origin is its baseline. All geometry is in integer printer dots.
struct PrinterGlyph {
    QImage alpha;
    QPoint bearing;
};

// Each render owns its face and library: FreeType sizes and glyph slots are
// mutable, and must never be shared between concurrent document renders.
// This uses the native FreeType bundled with Qt, without a new runtime DLL.
class PrinterFontRaster final {
public:
    explicit PrinterFontRaster(QByteArray bytes) : bytes_(std::move(bytes)) {
        if (FT_Init_FreeType(&library_) != 0) return;
        if (bytes_.isEmpty() || bytes_.size() > std::numeric_limits<FT_Long>::max()) return;
        FT_New_Memory_Face(library_, reinterpret_cast<const FT_Byte*>(bytes_.constData()),
                           static_cast<FT_Long>(bytes_.size()), 0, &face_);
    }
    ~PrinterFontRaster() {
        if (face_) FT_Done_Face(face_);
        if (library_) FT_Done_FreeType(library_);
    }
    PrinterFontRaster(const PrinterFontRaster&) = delete;
    PrinterFontRaster& operator=(const PrinterFontRaster&) = delete;
    PrinterFontRaster(PrinterFontRaster&&) = delete;
    PrinterFontRaster& operator=(PrinterFontRaster&&) = delete;

    [[nodiscard]] bool isValid() const noexcept { return face_ != nullptr; }

    [[nodiscard]] std::expected<PrinterGlyph, QString> glyph(
        char32_t character, int pixelWidth, int pixelHeight, int quarterTurns = 0) {
        if (!face_) return std::unexpected(QStringLiteral("The embedded printer font could not be opened"));
        if (pixelWidth < 1 || pixelWidth > 32000 || pixelHeight < 1 || pixelHeight > 32000
            || qint64(pixelWidth) * pixelHeight > 64 * 1024 * 1024)
            return std::unexpected(QStringLiteral("Printer font dimensions must be 1 through 32000 dots"));
        quarterTurns &= 3;
        if (pixelWidth != width_ || pixelHeight != height_ || quarterTurns != quarterTurns_) {
            if (FT_Set_Char_Size(face_, pixelHeight * 64, pixelHeight * 64, 72, 72) != 0)
                return std::unexpected(QStringLiteral("The printer font size is unsupported"));
            // Fit the TrueType hints at the requested height, then stretch
            // horizontally. Independent X/Y ppem hinting changes stem pixels.
            FT_Matrix stretch{static_cast<FT_Fixed>(std::round(double(pixelWidth) / pixelHeight * 65536)), 0, 0, 65536};
            // FreeType uses an upward Y axis. Rasterizing the rotated outline
            // preserves directional dropout handling at one-dot diagonals.
            if (quarterTurns == 1) stretch = {0,65536,-stretch.xx,0};
            else if (quarterTurns == 2) stretch = {-stretch.xx,0,0,-65536};
            else if (quarterTurns == 3) stretch = {0,-65536,stretch.xx,0};
            FT_Set_Transform(face_, &stretch, nullptr);
            width_ = pixelWidth;
            height_ = pixelHeight;
            quarterTurns_ = quarterTurns;
            cache_.clear();
            cacheBytes_ = 0;
        }
        const auto index = FT_Get_Char_Index(face_, static_cast<FT_ULong>(character));
        if (!index) return std::unexpected(QStringLiteral("The printer font has no glyph for U+%1")
                                           .arg(static_cast<quint32>(character), 4, 16, QLatin1Char('0')));
        if (const auto found = cache_.constFind(index); found != cache_.cend()) return found.value();
        if (FT_Load_Glyph(face_, index, FT_LOAD_RENDER | FT_LOAD_TARGET_MONO) != 0)
            return std::unexpected(QStringLiteral("The printer glyph could not be rasterized"));
        const auto& bitmap = face_->glyph->bitmap;
        if (qint64(bitmap.width) * bitmap.rows > 64 * 1024 * 1024)
            return std::unexpected(QStringLiteral("Printer glyph raster exceeds the allocation limit"));
        PrinterGlyph result{QImage(static_cast<int>(bitmap.width), static_cast<int>(bitmap.rows), QImage::Format_Alpha8),
                            QPoint(face_->glyph->bitmap_left, -face_->glyph->bitmap_top)};
        if (bitmap.width == 0 || bitmap.rows == 0) return result;
        if (result.alpha.isNull()) return std::unexpected(QStringLiteral("Could not allocate the printer glyph raster"));
        result.alpha.fill(0);
        for (unsigned int y = 0; y < bitmap.rows; ++y) {
            const auto* input = bitmap.buffer + (bitmap.pitch >= 0 ? y : bitmap.rows - y - 1) * std::abs(bitmap.pitch);
            auto* output = result.alpha.scanLine(static_cast<int>(y));
            for (unsigned int x = 0; x < bitmap.width; ++x)
                output[x] = input[x / 8] & (128 >> (x % 8)) ? 255 : 0;
        }
        if (quarterTurns) {
            // Return the same baseline contract as the normal glyph; the
            // field compositor applies its existing rotation exactly once.
            const auto p=result.bearing;
            const auto size=result.alpha.size();
            if(quarterTurns==1)result.bearing={p.y(),-p.x()-size.width()};
            else if(quarterTurns==2)result.bearing={-p.x()-size.width(),-p.y()-size.height()};
            else result.bearing={-p.y()-size.height(),p.x()};
            QTransform inverse;inverse.rotate(-90*quarterTurns);
            result.alpha=result.alpha.transformed(inverse,Qt::FastTransformation);
        }
        // Caches hold only the current size, and never retain large glyphs.
        // This bounds memory even for labels that cycle through font sizes.
        if (cache_.size() < 128 && cacheBytes_ + result.alpha.sizeInBytes() <= 4 * 1024 * 1024) {
            cacheBytes_ += result.alpha.sizeInBytes();
            cache_.insert(index, result);
        }
        return result;
    }

private:
    QByteArray bytes_;
    FT_Library library_ = nullptr;
    FT_Face face_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int quarterTurns_ = 0;
    QHash<FT_UInt, PrinterGlyph> cache_;
    qsizetype cacheBytes_ = 0;
};

} // namespace QtZpl
