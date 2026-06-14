#pragma once

#include "core/DatabaseManager.h"
#include <QObject>
#include <QString>
#include <QList>
#include <optional>

namespace Mc {

/**
 * FfprobeScanner — invokes ffprobe on a file and parses its JSON output
 * into FileRecord + StreamRecord structs ready for insertion into the DB.
 *
 * This class is stateless and thread-safe. Each call to scanFile() is independent.
 */
class FfprobeScanner : public QObject
{
	Q_OBJECT
public:
	explicit FfprobeScanner(const QString& ffprobePath, QObject* parent = nullptr);

	struct ScanResult {
		FileRecord              file;
		QList<StreamRecord>     streams;
		QString                 errorMessage;
		bool                    success = false;
	};

	/**
	 * Synchronously scan a single file.
	 * Invokes ffprobe as a subprocess and parses its JSON output.
	 * Returns a ScanResult; check result.success before using the data.
	 */
	[[nodiscard]] ScanResult scanFile(const QString& filePath, qint64 scanRunId = -1) const;

	/**
	 * Validate that the configured ffprobe binary exists and is functional.
	 * Returns the version string on success, empty string on failure.
	 */
	[[nodiscard]] QString validateFfprobe() const;

	void setFfprobePath(const QString& path) { m_ffprobePath = path; }
	QString ffprobePath() const { return m_ffprobePath; }

private:
	[[nodiscard]] QByteArray invokeFfprobe(const QString& filePath) const;
	[[nodiscard]] ScanResult parseJsonOutput(const QByteArray& json,
											  const QString& filePath,
											  qint64 scanRunId) const;

	static StreamRecord parseStreamObject(const QJsonObject& obj);
	static QString      detectHdrFormat(const QJsonObject& tags, const QJsonObject& sideData);
	static QString      normalizeLanguage(const QString& lang);
	static QString      languageFromTitle(const QString& title);

	QString m_ffprobePath;
};

} // namespace Mc
