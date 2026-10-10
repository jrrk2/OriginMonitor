#include "Image16.hpp"

#include <QByteArray>
#include <QFile>

namespace {

quint32 crc32(const char* p, qsizetype n, quint32 crc = 0xFFFFFFFFu)
{
    static quint32 table[256]; static bool init = false;
    if (!init) { for (quint32 i = 0; i < 256; ++i) { quint32 c = i; for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; table[i] = c; } init = true; }
    for (qsizetype i = 0; i < n; ++i) crc = table[(crc ^ quint8(p[i])) & 0xFF] ^ (crc >> 8);
    return crc;
}

void be32(QByteArray& b, quint32 v) { for (int s = 24; s >= 0; s -= 8) b.append(char((v >> s) & 0xFF)); }

void chunk(QByteArray& out, const char* type, const QByteArray& data)
{
    be32(out, quint32(data.size()));
    QByteArray td(type, 4); td += data;
    out += td;
    be32(out, ~crc32(td.constData(), td.size()));
}

}   // namespace

bool Image16::savePng(const QString& path) const
{
    if (isNull()) return false;
    QByteArray raw; raw.reserve(qsizetype(height) * (1 + width * 6));
    for (int y = 0; y < height; ++y) {
        raw.append(char(0));                                            // filter type 0 (none)
        const quint16* r = row(y);
        for (int i = 0; i < width * 3; ++i) { raw.append(char(r[i] >> 8)); raw.append(char(r[i] & 0xFF)); }
    }
    const QByteArray z = qCompress(raw, 6);                             // 4-byte length prefix followed by a plain zlib stream, which is what PNG wants
    QByteArray png("\x89PNG\r\n\x1a\n", 8), ihdr;
    be32(ihdr, quint32(width)); be32(ihdr, quint32(height)); ihdr.append(char(16)); ihdr.append(char(2)); ihdr.append(char(0)); ihdr.append(char(0)); ihdr.append(char(0));
    chunk(png, "IHDR", ihdr); chunk(png, "IDAT", z.mid(4)); chunk(png, "IEND", QByteArray());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(png) == png.size();
}
