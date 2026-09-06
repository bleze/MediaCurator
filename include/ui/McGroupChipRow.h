#pragma once
#include <QWidget>
#include <functional>

namespace Mc {

// A row of colored disk-icon chips, one per storage group, exactly one checked
// at a time. Hand-painted (hover/checked state via bookkeeping, no QButtonGroup
// precedent exists in this codebase) — same idea as McMainWindow's McQueueToggle,
// generalized from a single bool toggle to an N-way exclusive pick, and sharing
// the card badge's color+icon language (StorageGroupSettings::colorForGroup,
// storage_group.svg) so "assign a group" and "this file is in group X" read as
// one design system. Used for library-folder group assignment
// (McManageFoldersDialog) and any other one-of-many storage-group pick (e.g.
// the Downloads settings tab's "which group do downloads land in").
class McGroupChipRow final : public QWidget
{
public:
	std::function<void(int)> onGroupChanged;

	// Renders chips for groups MinGroup..max(maxGroup, initialGroup) — a caller
	// already holding a group beyond the current uiMaxGroup() setting still
	// shows its real assignment instead of silently losing it.
	//
	// allowNone lets initialGroup be 0 (no chip selected) and lets clicking the
	// currently-selected chip again clear the selection back to 0 — e.g. the
	// Downloads settings tab, where "not tracked" (a cache/temp drive outside
	// every storage group) is a valid, common choice. McManageFoldersDialog's
	// folder-assignment picker leaves this false: a folder always belongs to
	// some group, so exactly one chip is always checked and clicks only switch
	// which one.
	explicit McGroupChipRow(int maxGroup, int initialGroup, bool allowNone = false,
	                         QWidget* parent = nullptr);

	int selected() const { return m_selected; }

protected:
	void paintEvent(QPaintEvent*) override;
	void mousePressEvent(QMouseEvent* event) override;
	void mouseMoveEvent(QMouseEvent* event) override;
	void leaveEvent(QEvent* event) override;
	QSize sizeHint() const override;

private:
	static constexpr int kChipH   = 18; // matches McCardDelegate::kBadgeH
	static constexpr int kChipGap = 4;

	QRect chipRect(int group) const;

	int    m_maxGroup;
	int    m_selected;
	bool   m_allowNone;
	QPoint m_hoverPos { -1, -1 };
};

} // namespace Mc
