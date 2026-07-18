#include "core/AppSettings.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageAuthenticationCode>
#include <QStandardPaths>

#include <limits>

namespace Mc {

namespace {
// Not a real secret — this is an open-source repo — so this only raises the
// bar past hand-editing settings.json in a text editor, not a determined
// tamperer willing to read this file and recompute a matching HMAC.
constexpr char kReclaimedHmacKey[] = "MediaCurator-reclaimed-v1";

QString reclaimedHmac(qint64 total)
{
	QMessageAuthenticationCode mac(QCryptographicHash::Sha256);
	mac.setKey(QByteArray(kReclaimedHmacKey));
	mac.addData(QByteArray::number(total));
	return QString::fromLatin1(mac.result().toHex());
}
} // namespace

// ── QVariant ↔ QJsonValue helpers ─────────────────────────────────────────────

static QJsonValue variantToJson(const QVariant& v)
{
	switch (v.typeId()) {
	case QMetaType::Bool:
		return v.toBool();
	case QMetaType::Int:
	case QMetaType::LongLong:
	case QMetaType::UInt:
	case QMetaType::ULongLong:
		return v.toLongLong();
	case QMetaType::Double:
	case QMetaType::Float:
		return v.toDouble();
	case QMetaType::QStringList: {
		QJsonArray a;
		for (const QString& s : v.toStringList()) a.append(s);
		return a;
	}
	case QMetaType::QJsonObject:
		return QJsonValue(v.toJsonObject());
	case QMetaType::QJsonArray:
		return QJsonValue(v.toJsonArray());
	default:
		return v.toString();
	}
}

static QVariant jsonToVariant(const QJsonValue& jv, const QVariant& def)
{
	switch (jv.type()) {
	case QJsonValue::Bool:
		return jv.toBool();
	case QJsonValue::Double:
		// Preserve int/string semantics from the default value's type.
		if (def.typeId() == QMetaType::LongLong || def.typeId() == QMetaType::ULongLong)
			return static_cast<qint64>(jv.toDouble());
		if (def.typeId() == QMetaType::Int || def.typeId() == QMetaType::UInt) {
			// A stored value outside int range (e.g. an ms-epoch written as qint64
			// but read back with an int default) must not go through the narrowing
			// cast — that's UB. Fall back to qint64 so the caller's toLongLong()
			// still sees the real value.
			const double d = jv.toDouble();
			if (d < static_cast<double>(std::numeric_limits<int>::min())
			        || d > static_cast<double>(std::numeric_limits<int>::max()))
				return static_cast<qint64>(d);
			return static_cast<int>(d);
		}
		return jv.toDouble();
	case QJsonValue::String:
		return jv.toString();
	case QJsonValue::Array: {
		QStringList list;
		for (const QJsonValue& item : jv.toArray())
			list << item.toString();
		return list;
	}
	case QJsonValue::Object:
		return QVariant(jv.toObject());
	default:
		return def;
	}
}

// ── Singleton ──────────────────────────────────────────────────────────────────

AppSettings& AppSettings::instance()
{
	static AppSettings s;
	return s;
}

// static
QString AppSettings::filePath()
{
	return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
	       + QStringLiteral("/settings.json");
}

// static
QString AppSettings::geometryFilePath()
{
	return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
	       + QStringLiteral("/mediacurator.ini");
}

void AppSettings::load()
{
	QFile f(filePath());
	if (!f.exists() || !f.open(QIODevice::ReadOnly | QIODevice::Text))
		return;
	const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
	if (!doc.isNull() && doc.isObject())
		m_root = doc.object();
}

void AppSettings::save()
{
	const QString path = filePath();
	QDir().mkpath(QFileInfo(path).absolutePath());
	QFile f(path);
	if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
		f.write(QJsonDocument(m_root).toJson(QJsonDocument::Indented));
}

// ── "app" section ──────────────────────────────────────────────────────────────

QVariant AppSettings::value(const QString& key, const QVariant& defaultValue) const
{
	const QJsonObject app = m_root[QStringLiteral("app")].toObject();
	if (!app.contains(key)) return defaultValue;
	return jsonToVariant(app[key], defaultValue);
}

void AppSettings::setValue(const QString& key, const QVariant& value)
{
	QJsonObject app = m_root[QStringLiteral("app")].toObject();
	app[key] = variantToJson(value);
	m_root[QStringLiteral("app")] = app;
	save();
}

qint64 AppSettings::reclaimedBytes()
{
	const qint64 stored = value(QStringLiteral("stats/totalReclaimedBytes"), 0LL).toLongLong();
	if (stored == 0)
		return 0;   // nothing to verify yet — avoids migrating/writing an HMAC before any value exists

	// One-time adoption, guarded by its own flag (not just "is the hash
	// missing") — otherwise tampering by deleting the hash key alongside
	// editing the value would look identical to legitimate pre-existing data
	// from before this check existed, every single time. Once this flag is
	// set, a missing/wrong hash is always treated as tampering, never adopted.
	const bool migrated = value(QStringLiteral("stats/reclaimedHmacMigrated"), false).toBool();
	const QString storedHmac = value(QStringLiteral("stats/totalReclaimedBytesHmac")).toString();

	if (!migrated) {
		setValue(QStringLiteral("stats/totalReclaimedBytesHmac"), reclaimedHmac(stored));
		setValue(QStringLiteral("stats/reclaimedHmacMigrated"), true);
		return stored;
	}

	if (storedHmac != reclaimedHmac(stored)) {
		qWarning() << "AppSettings: reclaimed-bytes counter failed integrity check — resetting to 0";
		setValue(QStringLiteral("stats/totalReclaimedBytes"), 0LL);
		setValue(QStringLiteral("stats/totalReclaimedBytesHmac"), reclaimedHmac(0));
		return 0;
	}
	return stored;
}

void AppSettings::addReclaimedBytes(qint64 deltaBytes)
{
	const qint64 total = reclaimedBytes() + deltaBytes;
	setValue(QStringLiteral("stats/totalReclaimedBytes"), total);
	setValue(QStringLiteral("stats/totalReclaimedBytesHmac"), reclaimedHmac(total));
}

void AppSettings::restoreReclaimedBytes(qint64 bytesValue)
{
	setValue(QStringLiteral("stats/totalReclaimedBytes"), bytesValue);
	setValue(QStringLiteral("stats/totalReclaimedBytesHmac"), reclaimedHmac(bytesValue));
	setValue(QStringLiteral("stats/reclaimedHmacMigrated"), true);
}

// ── "profile" section ──────────────────────────────────────────────────────────

QJsonObject AppSettings::profileSection() const
{
	return m_root[QStringLiteral("profile")].toObject();
}

void AppSettings::setProfileSection(const QJsonObject& obj)
{
	m_root[QStringLiteral("profile")] = obj;
	save();
}

} // namespace Mc
