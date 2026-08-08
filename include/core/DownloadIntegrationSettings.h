#pragma once

#include <QString>

namespace Mc {

struct NzbGetConfig {
	bool    enabled          = false;
	QString host;
	int     port             = 6789;
	QString username;
	QString password;
	bool    autoQuickScan    = true;
	bool    autoQuickAnalyze = true;
};

/**
 * DownloadIntegrationSettings — download-client integration config, stored
 * in AppSettings under the "downloads" key as one JSON object keyed by
 * provider id ("nzbget" today). A second provider is a sibling key here,
 * not a schema change. Mirrors StorageGroupSettings' thin-static-helper
 * pattern over AppSettings.
 */
class DownloadIntegrationSettings
{
public:
	[[nodiscard]] static NzbGetConfig nzbgetConfig();
	static void setNzbgetConfig(const NzbGetConfig& config);
};

} // namespace Mc
