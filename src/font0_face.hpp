#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtCore/QString>
#include <QtCore/QStringView>
#include <QtGui/QPainterPath>
#include <expected>
#include <limits>
#include <memory>
#include <utility>
#include <ft2build.h>
#include FT_ADVANCES_H
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_TRUETYPE_TABLES_H

namespace QtZpl {

// Unhinted Font 0 design data in a 1000-unit em, independent of the platform's
// Qt font backend. Each render owns its FreeType face and bounded glyph caches.
class Font0Face final {
public:
    struct Glyph {
        QPainterPath path;
        qreal advance = 0;
    };

    explicit Font0Face(QByteArray bytes) : bytes_(std::move(bytes)) {
        if (bytes_.isEmpty() || bytes_.size() > std::numeric_limits<FT_Long>::max()) {
            error_ = QStringLiteral("The embedded Font 0 data is empty or exceeds the supported size");
            return;
        }
        FT_Library library = nullptr;
        if (FT_Init_FreeType(&library) != 0) {
            error_ = QStringLiteral("The Font 0 outline library could not be initialized");
            return;
        }
        library_.reset(library);
        FT_Face face = nullptr;
        if (FT_New_Memory_Face(library_.get(), reinterpret_cast<const FT_Byte*>(bytes_.constData()),
                               static_cast<FT_Long>(bytes_.size()), 0, &face) != 0) {
            error_ = QStringLiteral("The embedded Font 0 face could not be opened");
            return;
        }
        face_.reset(face);
        if (!FT_IS_SCALABLE(face_.get()) || face_->units_per_EM == 0
            || face_->num_glyphs <= 0 || FT_Select_Charmap(face_.get(), FT_ENCODING_UNICODE) != 0) {
            error_ = QStringLiteral("The embedded Font 0 face has no scalable Unicode outlines");
            face_.reset();
            return;
        }
        scale_ = qreal(1000) / face_->units_per_EM;
        const auto* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(face_.get(), ft_sfnt_os2));
        if (!os2 || os2->version < 2 || os2->sCapHeight <= 0) {
            error_ = QStringLiteral("The embedded Font 0 face has no valid cap-height metric");
            face_.reset();
            return;
        }
        capHeight_ = os2->sCapHeight * scale_;
    }

    Font0Face(const Font0Face&) = delete;
    Font0Face& operator=(const Font0Face&) = delete;
    Font0Face(Font0Face&&) = delete;
    Font0Face& operator=(Font0Face&&) = delete;

    [[nodiscard]] bool isValid() const noexcept { return face_ != nullptr; }
    [[nodiscard]] const QString& errorString() const noexcept { return error_; }
    [[nodiscard]] qreal capHeight() const noexcept { return capHeight_; }

    [[nodiscard]] QList<quint32> glyphIndexesForString(QStringView text) const {
        QList<quint32> result;
        result.reserve(text.size());
        for (qsizetype i = 0; i < text.size(); ++i) {
            char32_t character = text[i].unicode();
            if (text[i].isHighSurrogate() && i + 1 < text.size() && text[i + 1].isLowSurrogate()) {
                character = QChar::surrogateToUcs4(text[i], text[i + 1]);
                ++i;
            } else if (text[i].isSurrogate()) {
                character = QChar::ReplacementCharacter;
            }
            result.append(face_ ? FT_Get_Char_Index(face_.get(), static_cast<FT_ULong>(character)) : 0);
        }
        return result;
    }

    // Metrics-only callers need neither a QPainterPath nor outline conversion.
    [[nodiscard]] std::expected<qreal, QString> advance(quint32 index) {
        if (!isValid()) return std::unexpected(error_);
        if (!validIndex(index)) return std::unexpected(QStringLiteral("Font 0 glyph index is outside the face"));
        if (const auto found = advances_.constFind(index); found != advances_.cend()) return found.value();
        FT_Fixed value = 0;
        if (FT_Get_Advance(face_.get(), index, loadFlags, &value) != 0)
            return std::unexpected(QStringLiteral("The Font 0 glyph advance could not be read"));
        // FT_LOAD_NO_SCALE requests integer font units, not 16.16 pixels.
        const qreal result = value * scale_;
        advances_.insert(index, result);
        return result;
    }

    [[nodiscard]] std::expected<Glyph, QString> glyph(quint32 index) {
        if (!isValid()) return std::unexpected(error_);
        if (!validIndex(index)) return std::unexpected(QStringLiteral("Font 0 glyph index is outside the face"));
        if (const auto found = glyphs_.constFind(index); found != glyphs_.cend()) return found.value();
        if (FT_Load_Glyph(face_.get(), index, loadFlags) != 0)
            return std::unexpected(QStringLiteral("The Font 0 glyph outline could not be loaded"));
        const auto slot = face_->glyph;
        if (slot->format != FT_GLYPH_FORMAT_OUTLINE)
            return std::unexpected(QStringLiteral("The Font 0 glyph is not a scalable outline"));

        Glyph result;
        result.advance = slot->metrics.horiAdvance * scale_;
        OutlineContext context{&result.path, scale_};
        const FT_Outline_Funcs callbacks{moveTo, lineTo, conicTo, cubicTo, 0, 0};
        if (FT_Outline_Decompose(&slot->outline, &callbacks, &context) != 0)
            return std::unexpected(QStringLiteral("The Font 0 glyph outline could not be decomposed"));
        if (context.open) result.path.closeSubpath();
        advances_.insert(index, result.advance);
        // Validated indices bound both caches by this embedded face's glyph
        // count. QPainterPath shares its data when a cached value is returned.
        glyphs_.insert(index, result);
        return result;
    }

private:
    struct LibraryDeleter {
        void operator()(FT_Library library) const noexcept { FT_Done_FreeType(library); }
    };
    struct FaceDeleter {
        void operator()(FT_Face face) const noexcept { FT_Done_Face(face); }
    };
    struct OutlineContext {
        QPainterPath* path;
        qreal scale;
        bool open = false;
        [[nodiscard]] QPointF point(const FT_Vector& value) const {
            return {value.x * scale, -value.y * scale};
        }
    };
    static int moveTo(const FT_Vector* point, void* opaque) {
        auto& context = *static_cast<OutlineContext*>(opaque);
        if (context.open) context.path->closeSubpath();
        context.path->moveTo(context.point(*point));
        context.open = true;
        return 0;
    }
    static int lineTo(const FT_Vector* point, void* opaque) {
        auto& context = *static_cast<OutlineContext*>(opaque);
        context.path->lineTo(context.point(*point));
        return 0;
    }
    static int conicTo(const FT_Vector* control, const FT_Vector* point, void* opaque) {
        auto& context = *static_cast<OutlineContext*>(opaque);
        context.path->quadTo(context.point(*control), context.point(*point));
        return 0;
    }
    static int cubicTo(const FT_Vector* first, const FT_Vector* second, const FT_Vector* point, void* opaque) {
        auto& context = *static_cast<OutlineContext*>(opaque);
        context.path->cubicTo(context.point(*first), context.point(*second), context.point(*point));
        return 0;
    }
    [[nodiscard]] bool validIndex(quint32 index) const noexcept {
        return quint64(index) < static_cast<quint64>(face_->num_glyphs);
    }
    static constexpr FT_Int32 loadFlags = FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING
        | FT_LOAD_NO_BITMAP | FT_LOAD_IGNORE_TRANSFORM;

    QByteArray bytes_;
    std::unique_ptr<FT_LibraryRec_, LibraryDeleter> library_;
    std::unique_ptr<FT_FaceRec_, FaceDeleter> face_;
    QString error_;
    qreal scale_ = 1;
    qreal capHeight_ = 0;
    QHash<quint32, qreal> advances_;
    QHash<quint32, Glyph> glyphs_;
};

} // namespace QtZpl
