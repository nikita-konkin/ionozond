#include "lfpfile.h"

#include <QByteArray>
#include <QDataStream>
#include <QFile>
#include <QFileInfo>
#include <QtEndian>

namespace {

const quint16 LFP_VERSION_MAJOR = 1;
const quint16 LFP_VERSION_MINOR = 0;
const quint32 LFP_HEADER_SIZE   = 512;
const int     SECTION_ENTRY     = 32;

const quint16 DTYPE_FLOAT32 = 1;
const quint16 COMP_NONE     = 0;
const quint16 COMP_ZLIB     = 1;

/* ---- little-endian scalar helpers ---------------------------------- */

template <typename T>
void put(QByteArray &buf, int offset, T value)
{
    qToLittleEndian<T>(value, reinterpret_cast<uchar *>(buf.data() + offset));
}

template <typename T>
T get(const QByteArray &buf, int offset)
{
    return qFromLittleEndian<T>(reinterpret_cast<const uchar *>(buf.constData() + offset));
}

void putFloat(QByteArray &buf, int offset, float value)
{
    quint32 bits;
    memcpy(&bits, &value, 4);
    put<quint32>(buf, offset, bits);
}

float getFloat(const QByteArray &buf, int offset)
{
    const quint32 bits = get<quint32>(buf, offset);
    float value;
    memcpy(&value, &bits, 4);
    return value;
}

void putString(QByteArray &buf, int offset, const QString &s, int size)
{
    const QByteArray utf8 = s.toUtf8().left(size);
    memset(buf.data() + offset, 0, size);
    memcpy(buf.data() + offset, utf8.constData(), utf8.size());
}

QString getString(const QByteArray &buf, int offset, int size)
{
    const char *p = buf.constData() + offset;
    int len = 0;
    while (len < size && p[len] != '\0')
        ++len;
    return QString::fromUtf8(p, len);
}

/*
 * Qt's qCompress prepends a 4-byte big-endian uncompressed size before the
 * zlib stream. The format stores a plain RFC 1950 stream so Python can use
 * zlib.decompress() directly, so that prefix is stripped on write and
 * reconstructed on read.
 */
QByteArray deflateRaw(const QByteArray &plain)
{
    const QByteArray withPrefix = qCompress(plain, 9);
    return withPrefix.mid(4);
}

QByteArray inflateRaw(const QByteArray &raw, int expectedBytes)
{
    QByteArray withPrefix;
    withPrefix.resize(4);
    qToBigEndian<quint32>((quint32)expectedBytes,
                          reinterpret_cast<uchar *>(withPrefix.data()));
    withPrefix.append(raw);
    return qUncompress(withPrefix);
}

QByteArray floatsToBytes(const QVector<float> &v)
{
    QByteArray out;
    out.resize(v.size() * 4);
    for (int i = 0; i < v.size(); ++i)
        putFloat(out, i * 4, v.at(i));
    return out;
}

QVector<float> bytesToFloats(const QByteArray &b)
{
    QVector<float> out;
    out.resize(b.size() / 4);
    for (int i = 0; i < out.size(); ++i)
        out[i] = getFloat(b, i * 4);
    return out;
}

struct Section {
    QByteArray type;      /* 4 chars */
    quint32 rows;
    quint32 cols;
    QByteArray payload;   /* already compressed */
    quint16 compression;
    quint32 plainLength;
};

} // namespace

QString lfpPathFor(const QString &lfsPath)
{
    QFileInfo info(lfsPath);
    return info.absolutePath() + QLatin1Char('/') +
           info.completeBaseName() + QLatin1String(".lfp");
}

bool writeLfp(const QString &path, const LfpProducts &p,
              const QString &producer, const QString &producerVersion)
{
    QVector<Section> sections;

    struct { const char *type; const QVector<float> *data; quint32 rows, cols; }
    payloads[] = {
        { "IONO", &p.ionogram, p.specCount,  p.specPointCount },
        { "SNR ", &p.snr,      1u,           p.specCount      },
        { "PDP ", &p.pdp,      1u,           p.specPointCount },
    };

    for (int i = 0; i < 3; ++i) {
        if (payloads[i].data->isEmpty())
            continue;
        Section s;
        s.type = QByteArray(payloads[i].type, 4);
        s.rows = payloads[i].rows;
        s.cols = payloads[i].cols;

        const QByteArray plain = floatsToBytes(*payloads[i].data);
        s.plainLength = (quint32)plain.size();

        /* Gating zeroes most of the ionogram, so it deflates very well. */
        const QByteArray packed = deflateRaw(plain);
        if (packed.size() < plain.size()) {
            s.payload = packed;
            s.compression = COMP_ZLIB;
        } else {
            s.payload = plain;
            s.compression = COMP_NONE;
        }
        sections.append(s);
    }

    QByteArray header(LFP_HEADER_SIZE, '\0');
    memcpy(header.data(), "LFPR", 4);
    put<quint16>(header, 0x004, LFP_VERSION_MAJOR);
    put<quint16>(header, 0x006, LFP_VERSION_MINOR);
    put<quint32>(header, 0x008, LFP_HEADER_SIZE);
    put<quint32>(header, 0x00C, (quint32)sections.size());
    put<quint32>(header, 0x010, LFP_HEADER_SIZE);          /* table follows */
    put<quint32>(header, 0x014, p.gated ? 1u : 0u);
    putString(header, 0x018, producer, 8);
    putString(header, 0x020, producerVersion, 16);

    putString(header, 0x030, p.txName, 64);
    putFloat (header, 0x070, p.txLat);
    putFloat (header, 0x074, p.txLon);
    putString(header, 0x078, p.rxName, 64);
    putFloat (header, 0x0B8, p.rxLat);
    putFloat (header, 0x0BC, p.rxLon);
    put<quint64>(header, 0x0C0, (quint64)p.startEpochMs);
    put<quint32>(header, 0x0C8, p.cfHz);
    put<quint32>(header, 0x0CC, p.rateHzS);
    put<quint32>(header, 0x0D0, p.sampleRateHz);
    put<quint32>(header, 0x0D4, p.dec);
    put<quint16>(header, 0x0D8, p.durS);
    put<quint16>(header, 0x0DA, p.whiten);
    put<quint32>(header, 0x0DC, p.whitenLen);
    put<quint32>(header, 0x0E0, p.whitenN);

    put<quint32>(header, 0x0E4, p.fftCount);
    put<quint32>(header, 0x0E8, p.specCount);
    put<quint32>(header, 0x0EC, p.specPointCount);
    putFloat (header, 0x0F0, p.freqMinMHz);
    putFloat (header, 0x0F4, p.freqMaxMHz);
    putFloat (header, 0x0F8, p.delayMinMs);
    putFloat (header, 0x0FC, p.delayMaxMs);
    putFloat (header, 0x100, p.noiseGateDb);
    putFloat (header, 0x104, p.maxValueDb);
    putFloat (header, 0x108, p.lufMHz);
    putFloat (header, 0x10C, p.mufMHz);
    put<qint32>(header, 0x110, p.lufIndex);
    put<qint32>(header, 0x114, p.mufIndex);
    put<quint32>(header, 0x118, p.tb);
    put<quint32>(header, 0x11C, p.lfsrPolynomeDegree);

    /* Section table, then payloads. */
    QByteArray table(sections.size() * SECTION_ENTRY, '\0');
    quint64 offset = LFP_HEADER_SIZE + (quint64)table.size();
    for (int i = 0; i < sections.size(); ++i) {
        const Section &s = sections.at(i);
        const int e = i * SECTION_ENTRY;
        memcpy(table.data() + e, s.type.constData(), 4);
        put<quint16>(table, e + 0x04, DTYPE_FLOAT32);
        put<quint16>(table, e + 0x06, s.compression);
        put<quint32>(table, e + 0x08, s.rows);
        put<quint32>(table, e + 0x0C, s.cols);
        put<quint64>(table, e + 0x10, offset);
        put<quint64>(table, e + 0x18, (quint64)s.payload.size());
        offset += (quint64)s.payload.size();
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;

    if (file.write(header) != header.size()) return false;
    if (file.write(table) != table.size())   return false;
    for (int i = 0; i < sections.size(); ++i) {
        const QByteArray &payload = sections.at(i).payload;
        if (file.write(payload) != payload.size())
            return false;
    }
    file.close();
    return true;
}

bool readLfp(const QString &path, LfpProducts &p)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    const QByteArray header = file.read(LFP_HEADER_SIZE);
    if (header.size() != (int)LFP_HEADER_SIZE)
        return false;
    if (memcmp(header.constData(), "LFPR", 4) != 0)
        return false;

    /* Compatibility rule: refuse a higher major, tolerate any minor. */
    if (get<quint16>(header, 0x004) != LFP_VERSION_MAJOR)
        return false;

    const quint32 sectionCount = get<quint32>(header, 0x00C);
    const quint32 tableOffset  = get<quint32>(header, 0x010);
    p.gated = (get<quint32>(header, 0x014) & 1u) != 0;

    p.txName = getString(header, 0x030, 64);
    p.txLat  = getFloat(header, 0x070);
    p.txLon  = getFloat(header, 0x074);
    p.rxName = getString(header, 0x078, 64);
    p.rxLat  = getFloat(header, 0x0B8);
    p.rxLon  = getFloat(header, 0x0BC);
    p.startEpochMs = (qint64)get<quint64>(header, 0x0C0);
    p.cfHz         = get<quint32>(header, 0x0C8);
    p.rateHzS      = get<quint32>(header, 0x0CC);
    p.sampleRateHz = get<quint32>(header, 0x0D0);
    p.dec          = get<quint32>(header, 0x0D4);
    p.durS         = get<quint16>(header, 0x0D8);
    p.whiten       = get<quint16>(header, 0x0DA);
    p.whitenLen    = get<quint32>(header, 0x0DC);
    p.whitenN      = get<quint32>(header, 0x0E0);

    p.fftCount       = get<quint32>(header, 0x0E4);
    p.specCount      = get<quint32>(header, 0x0E8);
    p.specPointCount = get<quint32>(header, 0x0EC);
    p.freqMinMHz  = getFloat(header, 0x0F0);
    p.freqMaxMHz  = getFloat(header, 0x0F4);
    p.delayMinMs  = getFloat(header, 0x0F8);
    p.delayMaxMs  = getFloat(header, 0x0FC);
    p.noiseGateDb = getFloat(header, 0x100);
    p.maxValueDb  = getFloat(header, 0x104);
    p.lufMHz      = getFloat(header, 0x108);
    p.mufMHz      = getFloat(header, 0x10C);
    p.lufIndex    = get<qint32>(header, 0x110);
    p.mufIndex    = get<qint32>(header, 0x114);
    p.tb          = get<quint32>(header, 0x118);
    p.lfsrPolynomeDegree = get<quint32>(header, 0x11C);

    if (!file.seek(tableOffset))
        return false;
    const QByteArray table = file.read((int)sectionCount * SECTION_ENTRY);
    if (table.size() != (int)sectionCount * SECTION_ENTRY)
        return false;

    for (quint32 i = 0; i < sectionCount; ++i) {
        const int e = (int)i * SECTION_ENTRY;
        const QByteArray type(table.constData() + e, 4);
        const quint16 dtype = get<quint16>(table, e + 0x04);
        const quint16 comp  = get<quint16>(table, e + 0x06);
        const quint32 rows  = get<quint32>(table, e + 0x08);
        const quint32 cols  = get<quint32>(table, e + 0x0C);
        const quint64 off   = get<quint64>(table, e + 0x10);
        const quint64 len   = get<quint64>(table, e + 0x18);

        /* Skip anything this build does not understand. */
        if (dtype != DTYPE_FLOAT32)
            continue;
        if (type != "IONO" && type != "SNR " && type != "PDP ")
            continue;

        if (!file.seek((qint64)off))
            return false;
        QByteArray raw = file.read((qint64)len);
        if ((quint64)raw.size() != len)
            return false;

        const int plainBytes = (int)(rows * cols * 4);
        if (comp == COMP_ZLIB)
            raw = inflateRaw(raw, plainBytes);
        if (raw.size() != plainBytes)
            return false;

        const QVector<float> values = bytesToFloats(raw);
        if (type == "IONO")      p.ionogram = values;
        else if (type == "SNR ") p.snr = values;
        else if (type == "PDP ") p.pdp = values;
    }

    file.close();
    return true;
}
