#pragma once

#include <QString>

namespace Mc {

struct NzbGetConfig {
	bool    enabled = false;
	QString host;
	int     port    = 6789;
	QString username;
	QString password;
};

struct SabnzbdConfig {
	bool    enabled = false;
	QString host;
	int     port    = 8080;
	QString apiKey;
};

/**
 * DownloadIntegrationSettings — download-client integration config, stored
 * in AppSettings under the "downloads" key as one JSON object keyed by
 * provider id ("nzbget", "sabnzbd"). Each additional provider is a sibling
 * key here, not a schema change. Mirrors StorageGroupSettings' thin-static-
 * helper pattern over AppSettings.
 *
 * autoQuickScan/autoQuickAnalyze are shared across every provider — a
 * completed download should trigger the same behavior regardless of which
 * client reported it, so they live at the top level rather than duplicated
 * per provider.
 */
class DownloadIntegrationSettings
{
public:
	[[nodiscard]] static NzbGetConfig nzbgetConfig();
	static void setNzbgetConfig(const NzbGetConfig& config);

	[[nodiscard]] static SabnzbdConfig sabnzbdConfig();
	static void setSabnzbdConfig(const SabnzbdConfig& config);

	[[nodiscard]] static bool autoQuickScanOnComplete();
	static void setAutoQuickScanOnComplete(bool enabled);

	[[nodiscard]] static bool autoQuickAnalyzeOnComplete();
	static void setAutoQuickAnalyzeOnComplete(bool enabled);

	// Which storage group (StorageGroupSettings::MinGroup..MaxGroup) the
	// status-bar drive-activity indicator should light up for. Shared across
	// every provider, same reasoning as autoQuickScan/autoQuickAnalyze above.
	// 0 means "not tracked" — the default, since a download client commonly
	// writes to a cache/temp drive that isn't part of any storage group.
	//
	// downloadingStorageGroup is touched while a download is actively in
	// progress; downloadFinishedStorageGroup is touched once a download's own
	// post-processing (sort/rename into the library) has completed. They're
	// independent: a temp drive outside every storage group leaves the first
	// at 0 while the second still points at wherever the file lands.
	[[nodiscard]] static int downloadingStorageGroup();
	static void setDownloadingStorageGroup(int group);

	[[nodiscard]] static int downloadFinishedStorageGroup();
	static void setDownloadFinishedStorageGroup(int group);
};

} // namespace Mc
