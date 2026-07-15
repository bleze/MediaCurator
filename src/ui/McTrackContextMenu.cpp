#include "ui/McTrackContextMenu.h"
#include "ui/McCardDelegate.h"
#include "ui/McFlagRowWidget.h"
#include "ui/McLanguageFlags.h"

#include <QIcon>
#include <QMenu>
#include <QPixmap>
#include <QWidgetAction>

namespace Mc {

void buildTrackFlagMenu(QMenu& menu, const StreamRecord& stream, const QString& fileOriginalLanguage,
                        qreal devicePixelRatio, bool showFlagRowsForExternal,
                        const std::function<void(const QString& flag, bool value)>& onFlagToggled,
                        const std::function<void(const QString& langCode)>& onLanguageChosen)
{
	// Sidecar, Library view: nothing to toggle that would take visible effect yet —
	// skip straight to the language submenu below.
	if (!stream.isExternal || showFlagRowsForExternal) {
		const bool isAudio = stream.codecType == QLatin1String("audio");

		// A track can look "original" either because the container explicitly flags
		// it (isOriginal) or, for files scanned before that flag existed, because its
		// language happens to match the file's detected original language.
		const bool isOrigLang = isAudio && !fileOriginalLanguage.isEmpty()
		    && stream.language.compare(fileOriginalLanguage, Qt::CaseInsensitive) == 0;

		const QColor trackColor = [&] {
			if (stream.codecType == QLatin1String("audio"))    return QColor(0x10, 0x6a, 0xc0);
			if (stream.codecType == QLatin1String("subtitle")) return QColor(0x1a, 0x86, 0x4a);
			return QColor(0x60, 0x60, 0x60);
		}();

		struct FlagItem { QString flag; const char* badgeChar; bool current; QString label; };
		const QList<FlagItem> flagItems = {
			{ QStringLiteral("default"),  "\xe2\x98\x85", stream.isDefault,            QObject::tr("Default") },
			{ QStringLiteral("forced"),   "\xe2\x97\x8f", stream.isForced,             QObject::tr("Forced") },
			{ QStringLiteral("original"), "\xe2\x97\x8e", stream.isOriginal || isOrigLang, QObject::tr("Original") },
		};
		// "Original" is only meaningful for audio tracks — skip it for subtitles/video.
		for (const FlagItem& fi : flagItems) {
			if (fi.flag == QLatin1String("original") && !isAudio) continue;
			auto* wa  = new QWidgetAction(&menu);
			auto* row = new McFlagRowWidget(
				QString::fromUtf8(fi.badgeChar), trackColor, fi.label, fi.current);
			const QString flagName = fi.flag;
			row->onToggled = [&menu, flagName, onFlagToggled](bool newVal) {
				onFlagToggled(flagName, newVal);
				menu.close();
			};
			wa->setDefaultWidget(row);
			menu.addAction(wa);
		}
	}

	const bool unlabeledSubtitle = stream.codecType == QLatin1String("subtitle")
		&& (stream.language.isEmpty() || stream.language == QLatin1String("und"));
	if (unlabeledSubtitle) {
		menu.addSeparator();
		QMenu* langMenu = menu.addMenu(QObject::tr("Set &Language"));
		for (const auto& [code, name] : McLanguageFlags::commonLanguages()) {
			QAction* act = langMenu->addAction(name);
			const QPixmap flag = McLanguageFlags::flag(code, McCardDelegate::kFlagH, devicePixelRatio);
			if (!flag.isNull()) act->setIcon(QIcon(flag));
			QObject::connect(act, &QAction::triggered, &menu, [onLanguageChosen, code] {
				onLanguageChosen(code);
			});
		}
	}
}

} // namespace Mc
