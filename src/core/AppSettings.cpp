#include "core/AppSettings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>

namespace Mc {

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
		if (def.typeId() == QMetaType::Int || def.typeId() == QMetaType::UInt)
			return static_cast<int>(jv.toDouble());
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
