#pragma once
#include <QDialog>
#include <QString>

namespace Mc {

class McNfoView;

// Read-only viewer for a scene-release NFO's cached text (see
// DatabaseManager::sceneNfoText) — shared by the library and Job Queue cards.
// A thin dialog around the shared McNfoView (libs/nfoview), which does the
// rendering and inline-image loading.
class McSceneNfoDialog : public QDialog
{
	Q_OBJECT
public:
	explicit McSceneNfoDialog(const QString& videoPath, const QString& nfoText, QWidget* parent = nullptr);

private:
	McNfoView* m_view = nullptr;
};

} // namespace Mc
