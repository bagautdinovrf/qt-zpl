#include <QtZpl/qtzpl.hpp>

#include <QtCore/QByteArrayView>
#include <algorithm>
#include <limits>
#include <zlib.h>

namespace QtZpl {
namespace {
using namespace Qt::StringLiterals;

constexpr qsizetype hardByteLimit = 64 * 1024 * 1024;
constexpr qsizetype hardInputLimit = 128 * 1024 * 1024;
using BytesResult = std::expected<QByteArray, GraphicError>;

std::unexpected<GraphicError> failure(QString code, QString message) {
  return std::unexpected(GraphicError{std::move(code), std::move(message)});
}

std::unexpected<GraphicError> cancelled() {
  return failure(u"operation-cancelled"_s, u"Graphic decoding was cancelled."_s);
}

std::optional<quint16> z64Crc(QByteArrayView encoded, std::stop_token stopToken) {
  quint16 crc = 0;
  qsizetype offset = 0;
  for (const unsigned char byte : encoded) {
    if ((offset++ & 4095) == 0 && stopToken.stop_requested()) return std::nullopt;
    crc ^= static_cast<quint16>(byte) << 8;
    for (int bit = 0; bit < 8; ++bit)
      crc = crc & 0x8000U ? static_cast<quint16>((crc << 1) ^ 0x1021U)
                         : static_cast<quint16>(crc << 1);
  }
  return crc;
}

BytesResult decodeZ64(const GraphicField& graphic, const GraphicDecodeOptions& options) {
  const auto data = QByteArrayView{graphic.data}.trimmed();
  constexpr QByteArrayView prefix{":Z64:"};
  const auto separator = data.lastIndexOf(':');
  if (separator <= prefix.size() || data.size() - separator - 1 != 4)
    return failure(u"graphic-field-z64-format"_s,
                   u"The Z64 graphic must use :Z64:<Base64>:<CRC16>."_s);
  const auto encoded = data.sliced(prefix.size(), separator - prefix.size());
  bool checksumOk = false;
  const auto expectedCrc = QString::fromLatin1(data.sliced(separator + 1)).toUShort(&checksumOk, 16);
  if (!checksumOk)
    return failure(u"graphic-field-z64-format"_s,
                   u"The Z64 graphic CRC must contain four hexadecimal digits."_s);
  const auto actualCrc = z64Crc(encoded, options.stopToken);
  if (!actualCrc) return cancelled();
  if (*actualCrc != expectedCrc)
    return failure(u"graphic-field-z64-crc"_s,
                   u"The Z64 graphic CRC does not match its Base64-encoded data."_s);
  if (options.stopToken.stop_requested()) return cancelled();
  const auto decoded = QByteArray::fromBase64Encoding(encoded.toByteArray(),
    QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
  if (!decoded)
    return failure(u"graphic-field-z64-base64"_s, u"The Z64 graphic contains invalid Base64 data."_s);
  if (decoded.decoded.size() > std::numeric_limits<uInt>::max())
    return failure(u"graphic-field-z64-size"_s, u"The compressed Z64 input is too large."_s);

  // qUncompress grows its output when the declared size is too small. A fixed
  // destination and chunked inflate enforce the budget even for forged headers.
  QByteArray result(graphic.totalBytes, Qt::Uninitialized);
  z_stream stream{};
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(decoded.decoded.constData()));
  stream.avail_in = static_cast<uInt>(decoded.decoded.size());
  if (inflateInit(&stream) != Z_OK)
    return failure(u"graphic-field-z64-zlib"_s, u"The Z64 decompressor could not be initialized."_s);
  struct EndInflate {
    z_stream& stream;
    ~EndInflate() { inflateEnd(&stream); }
  } cleanup{stream};

  int status = Z_OK;
  // One extra stack byte lets inflate read the trailer when the output exactly
  // fills its declared buffer. Producing that byte is a size mismatch.
  unsigned char extra = 0;
  while (status == Z_OK) {
    if (options.stopToken.stop_requested()) return cancelled();
    const auto remaining = result.size() - static_cast<qsizetype>(stream.total_out);
    if (remaining < 0) break;
    stream.next_out = remaining > 0
      ? reinterpret_cast<Bytef*>(result.data() + stream.total_out) : &extra;
    stream.avail_out = remaining > 0 ? static_cast<uInt>(std::min<qsizetype>(remaining, 65536)) : 1;
    status = inflate(&stream, Z_NO_FLUSH);
    if (stream.total_out > static_cast<uLong>(result.size())) break;
  }
  if (status != Z_STREAM_END || stream.total_out != static_cast<uLong>(result.size())
      || stream.avail_in != 0)
    return failure(u"graphic-field-z64-zlib"_s,
                   u"The Z64 graphic could not be decompressed to its declared size."_s);
  return result;
}

int repeatCount(char code) {
  if (code >= 'G' && code <= 'Y') return code - 'G' + 1;
  if (code >= 'g' && code <= 'z') return (code - 'g' + 1) * 20;
  return 0;
}

bool isHex(char value) {
  return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'F')
      || (value >= 'a' && value <= 'f');
}

BytesResult decodeAscii(const GraphicField& graphic, const GraphicDecodeOptions& options) {
  const auto fail = [](QString message) -> BytesResult {
    return failure(u"graphic-field-ascii-compression"_s, std::move(message));
  };
  if (graphic.totalBytes % graphic.bytesPerRow != 0)
    return fail(u"The graphic byte count is not an exact number of rows."_s);
  const qsizetype rowNibbles = static_cast<qsizetype>(graphic.bytesPerRow) * 2;
  QByteArray result;
  result.reserve(graphic.totalBytes);
  QByteArray row;
  row.reserve(rowNibbles);
  QByteArray previousRow;
  const auto finishRow = [&]() {
    if (row.size() != rowNibbles) return false;
    const auto bytes = QByteArray::fromHex(row);
    if (bytes.size() != graphic.bytesPerRow || result.size() + bytes.size() > graphic.totalBytes)
      return false;
    result.append(bytes);
    previousRow = bytes;
    row.clear();
    return true;
  };
  const auto append = [&](char nibble, qsizetype count) {
    if (count <= 0 || count > rowNibbles - row.size()) return false;
    row.append(count, nibble);
    return row.size() != rowNibbles || finishRow();
  };
  for (qsizetype i = 0; i < graphic.data.size();) {
    if (options.stopToken.stop_requested()) return cancelled();
    const char code = graphic.data[i];
    if (code == ' ' || code == '\t' || code == '\r' || code == '\n') { ++i; continue; }
    if (code == ',' || code == '!') {
      ++i;
      if (!append(code == ',' ? '0' : 'F', rowNibbles - row.size()))
        return fail(u"A fill marker overflowed its row."_s);
      continue;
    }
    if (code == ':') {
      ++i;
      if (!row.isEmpty() || previousRow.size() != graphic.bytesPerRow
          || result.size() + previousRow.size() > graphic.totalBytes)
        return fail(u"A repeat-row marker has no complete preceding row or overflows the graphic."_s);
      result.append(previousRow);
      continue;
    }
    if (repeatCount(code) > 0) {
      qsizetype count = 0;
      while (i < graphic.data.size() && repeatCount(graphic.data[i]) > 0) {
        count += repeatCount(graphic.data[i++]);
        if (count > rowNibbles - row.size()) return fail(u"Repeated hexadecimal data overflowed its row."_s);
      }
      if (i >= graphic.data.size()) return fail(u"A repeat count has no following hexadecimal nibble."_s);
      const char nibble = graphic.data[i++];
      if (!isHex(nibble)) return fail(u"A repeat count is not followed by a hexadecimal nibble."_s);
      if (!append(nibble, count)) return fail(u"Repeated hexadecimal data overflowed its row."_s);
      continue;
    }
    if (!isHex(code)) return fail(u"The compressed graphic contains an invalid character."_s);
    ++i;
    if (!append(code, 1)) return fail(u"Hexadecimal data overflowed its row."_s);
  }
  if (!row.isEmpty()) return fail(u"The final graphic row contains fewer bytes than declared."_s);
  // Existing Labelary-compatible semantics: omitted complete trailing rows are blank.
  if (result.size() < graphic.totalBytes) result.append(graphic.totalBytes - result.size(), char{0});
  return result;
}
} // namespace

std::expected<DecodedGraphic, GraphicError> decodeGraphic(
    const GraphicField& graphic, const GraphicDecodeOptions& options) {
  if (options.stopToken.stop_requested()) return cancelled();
  if (options.maxDecodedBytes < 0 || options.maxEncodedBytes < 0)
    return failure(u"invalid-graphic-options"_s, u"Graphic byte budgets must not be negative."_s);
  if (graphic.data.size() > std::min(options.maxEncodedBytes, hardInputLimit))
    return failure(u"graphic-input-limit"_s, u"The encoded graphic exceeds the input byte budget."_s);
  const bool z64 = graphic.compression == u'A'
    && QByteArrayView{graphic.data}.trimmed().startsWith(":Z64:");
  if (graphic.compression != u'A' && graphic.compression != u'B')
    return failure(u"graphic-field-compression"_s, u"This graphic compression mode is not supported."_s);
  if (graphic.bytesPerRow <= 0 || graphic.bytesPerRow > std::numeric_limits<int>::max() / 8
      || graphic.totalBytes <= 0)
    return failure(z64 ? u"graphic-field-z64-size"_s : u"graphic-field-ascii-compression"_s,
                   u"The graphic byte count and bytes-per-row must be positive and representable."_s);
  const qint64 rows = (qint64(graphic.totalBytes) + graphic.bytesPerRow - 1) / graphic.bytesPerRow;
  const qint64 paddedBytes = rows * graphic.bytesPerRow;
  if (paddedBytes > hardByteLimit || paddedBytes > options.maxDecodedBytes)
    return failure(u"graphic-byte-limit"_s, u"The decoded graphic exceeds the byte budget."_s);

  BytesResult bytes;
  if (z64) bytes = decodeZ64(graphic, options);
  else if (graphic.compression == u'A') bytes = decodeAscii(graphic, options);
  else {
    if (graphic.data.size() != graphic.totalBytes)
      return failure(u"graphic-field-binary-size"_s, u"The binary graphic data does not match its declared size."_s);
    bytes = graphic.data;
  }
  if (!bytes) return std::unexpected(bytes.error());
  if (options.stopToken.stop_requested()) return cancelled();
  if (bytes->size() < paddedBytes) bytes->append(paddedBytes - bytes->size(), char{0});
  return DecodedGraphic{QSize(graphic.bytesPerRow * 8, static_cast<int>(rows)),
                        graphic.bytesPerRow, std::move(*bytes)};
}
} // namespace QtZpl
