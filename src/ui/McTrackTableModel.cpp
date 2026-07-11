#include "ui/McTrackTableModel.h"

#include <QColor>
#include <QFont>

namespace Mc {

McTrackTableModel::McTrackTableModel(QObject* parent)
	: QAbstractTableModel(parent)
{}

void McTrackTableModel::reload()
{
	beginResetModel();
	m_rows.clear();

	auto& db = DatabaseManager::instance();
	const QList<FileRecord> files = db.allFiles();

	for (const FileRecord& f : files) {
		const QList<StreamRecord> streams = db.streamsForFile(f.id);
		for (const StreamRecord& s : streams) {
			m_rows.append({f, s});
		}
	}

	endResetModel();
}

int McTrackTableModel::rowCount(const QModelIndex& parent) const
{
	if (parent.isValid()) return 0;
	return m_rows.size();
}

int McTrackTableModel::columnCount(const QModelIndex& parent) const
{
	if (parent.isValid()) return 0;
	return ColumnCount;
}

QVariant McTrackTableModel::data(const QModelIndex& index, int role) const
{
	if (!index.isValid() || index.row() >= m_rows.size())
		return {};

	const Row& row     = m_rows.at(index.row());
	const FileRecord& f   = row.file;
	const StreamRecord& s = row.stream;

	if (role == Qt::DisplayRole || role == Qt::ToolTipRole) {
		switch (index.column()) {
		case Col_Filename:   return f.filename;
		case Col_Container:  return f.container.toUpper();
		case Col_TrackType:  return s.codecType.toUpper();
		case Col_Language:   return s.language.isEmpty() ? tr("und") : s.language;
		case Col_Codec:      return s.codecName;
		case Col_Channels:
			if (s.codecType == "audio")
				return s.channels > 0 ? QString::number(s.channels) : QString();
			return {};
		case Col_Resolution:
			if (s.codecType == "video" && s.width > 0)
				return QStringLiteral("%1×%2").arg(s.width).arg(s.height);
			return {};
		case Col_Bitrate:
			if (s.bitRate > 0) {
				if (s.bitRate >= 1'000'000)
					return QStringLiteral("%1 Mb/s").arg(s.bitRate / 1'000'000.0, 0, 'f', 1);
				return QStringLiteral("%1 kb/s").arg(s.bitRate / 1000);
			}
			return {};
		case Col_Title:      return s.title;
		case Col_Duration:
			if (f.durationSec > 0) {
				const int h = static_cast<int>(f.durationSec) / 3600;
				const int m = (static_cast<int>(f.durationSec) % 3600) / 60;
				const int sec = static_cast<int>(f.durationSec) % 60;
				return h > 0 ? QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(sec, 2, 10, QChar('0'))
							 : QStringLiteral("%1:%2").arg(m).arg(sec, 2, 10, QChar('0'));
			}
			return {};
		case Col_FileSize: {
			const double gb = f.sizeBytes / 1073741824.0;
			if (gb >= 1.0) return QStringLiteral("%1 GB").arg(gb, 0, 'f', 2);
			const double mb = f.sizeBytes / 1048576.0;
			return QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
		}
		case Col_HDR:
		if (s.hdrFormat.isEmpty()) return {};
		if (s.maxCll > 0 || s.maxFall > 0)
			return QString("%1 (%2/%3)").arg(s.hdrFormat).arg(s.maxCll).arg(s.maxFall);
		return s.hdrFormat;
		case Col_Default:    return s.isDefault ? tr("✓") : QString();
		case Col_Forced:     return s.isForced  ? tr("✓") : QString();
		default: return {};
		}
	}

	if (role == Qt::ForegroundRole) {
		if (s.codecType == "audio")    return QColor(0x22, 0x7a, 0xd4); // blue
		if (s.codecType == "subtitle") return QColor(0x27, 0x9c, 0x5a); // green
		if (s.codecType == "video")    return QColor(0xc0, 0x60, 0x0a); // orange
		return {};
	}

	if (role == Qt::TextAlignmentRole) {
		switch (index.column()) {
		case Col_Channels:
		case Col_Resolution:
		case Col_Bitrate:
		case Col_Duration:
		case Col_FileSize:
			return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
		case Col_Default:
		case Col_Forced:
			return static_cast<int>(Qt::AlignCenter);
		default:
			return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
		}
	}

	return {};
}

QVariant McTrackTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
	if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
		return {};

	switch (section) {
	case Col_Filename:   return tr("File");
	case Col_Container:  return tr("Format");
	case Col_TrackType:  return tr("Type");
	case Col_Language:   return tr("Language");
	case Col_Codec:      return tr("Codec");
	case Col_Channels:   return tr("Ch");
	case Col_Resolution: return tr("Resolution");
	case Col_Bitrate:    return tr("Bit Rate");
	case Col_Title:      return tr("Title");
	case Col_Duration:   return tr("Duration");
	case Col_FileSize:   return tr("File Size");
	case Col_HDR:        return tr("HDR");
	case Col_Default:    return tr("Def");
	case Col_Forced:     return tr("Frc");
	default: return {};
	}
}

} // namespace Mc
