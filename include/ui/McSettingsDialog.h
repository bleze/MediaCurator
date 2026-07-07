#pragma once
#include <QDialog>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace Mc {

class UserProfile;

class McSettingsDialog : public QDialog
{
	Q_OBJECT
public:
	explicit McSettingsDialog(UserProfile* profile, QWidget* parent = nullptr);

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
	QCheckBox*   m_chkWriteLog;
	QCheckBox*   m_chkUseLocalStaging;
	QLineEdit*   m_editStagingDir;
	QPushButton* m_btnBrowseStagingDir;
	QLineEdit*   m_editTmdbKey;
	QLineEdit*   m_editOsApiKey;
	QLineEdit*   m_editOsUsername;
	QLineEdit*   m_editOsPassword;
	QCheckBox*   m_chkAutoTrack;
};

} // namespace Mc
