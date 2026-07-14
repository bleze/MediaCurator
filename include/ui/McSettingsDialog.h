#pragma once
#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSlider;
class QSpinBox;

namespace Mc {

class UserProfile;

class McSettingsDialog : public QDialog
{
	Q_OBJECT
public:
	explicit McSettingsDialog(UserProfile* profile, QWidget* parent = nullptr);

signals:
	// Emitted live as the slider is dragged (0.0-1.0) so the caller can preview
	// the effect on the actual cards behind the dialog. AppSettings itself isn't
	// touched until Save (see accept()) — on Cancel the caller must re-apply the
	// previously-saved value itself to undo the preview.
	void fanartOpacityChanged(double opacity);

protected:
	void done(int result) override;

private slots:
	void onAddLanguage();
	void onRemoveLanguage();
	void onAudioFormatUp();
	void onAudioFormatDown();
	void onSubFmtUp();
	void onSubFmtDown();
	void onBrowseStagingDir();
	void onAddEditionToken();
	void onRemoveEditionToken();
	void onResetEditionTokens();

private:
	void accept() override;

	UserProfile* m_profile;

	QListWidget* m_langList;
	QComboBox*   m_langCombo;
	QCheckBox*   m_chkKeepOriginalAudio;
	QCheckBox*   m_chkKeepCommentary;
	QCheckBox*   m_chkStereoCommentary;
	QListWidget* m_audioFormatList;
	QPushButton* m_btnFormatUp;
	QPushButton* m_btnFormatDown;
	QListWidget* m_subFormatList;
	QPushButton* m_btnSubFmtUp;
	QPushButton* m_btnSubFmtDown;
	QCheckBox*   m_chkSkipSubOnly;
	QCheckBox*   m_chkRemoveMjpeg;
	QCheckBox*   m_chkKeepForced;
	QComboBox*   m_cmbSdhMode;
	QCheckBox*   m_chkKeepOriginalSub;
	QCheckBox*   m_chkMergeSidecarSubs;
	QCheckBox*   m_chkDetectSubLanguage;
	QCheckBox*   m_chkWriteLog;
	QCheckBox*   m_chkUseLocalStaging;
	QLineEdit*   m_editStagingDir;
	QPushButton* m_btnBrowseStagingDir;
	QLineEdit*   m_editTmdbKey;
	QLineEdit*   m_editOsApiKey;
	QLineEdit*   m_editOsUsername;
	QLineEdit*   m_editOsPassword;
	QCheckBox*   m_chkAutoDownloadSubs;
	QCheckBox*   m_chkComputeMovieHash;
	QListWidget* m_editionTokenList;
	QLineEdit*   m_editEditionToken;
	QCheckBox*   m_chkAutoTrack;
	QSpinBox*    m_spinScanGroups;
	QSpinBox*    m_spinPosterWorkers;
	QSlider*     m_sliderFanartOpacity;
	QLabel*      m_lblFanartOpacity;
};

} // namespace Mc
