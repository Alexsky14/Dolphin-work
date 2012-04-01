#include <QTabWidget>
#include <QPushButton>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QListWidget;
class DControlStateManager;

class DConfigMainGeneralTab : public QTabWidget
{
	Q_OBJECT

public:
    DConfigMainGeneralTab(QWidget* parent = NULL);

signals:
	void IsoPathsChanged();

public slots:
	void Reset();
	void Apply();

	void OnAddIsoPath();
	void OnRemoveIsoPath();
	void OnClearIsoPathList();

	void OnPathListChanged();
	void OnSelectionChanged();

private:
	QWidget* CreateCoreTabWidget(QWidget* parent);
	QWidget* CreatePathsTabWidget(QWidget* parent);

	// Core
	QCheckBox* cbDualCore;
	QCheckBox* cbIdleSkipping;
	QCheckBox* cbCheats;

	QComboBox* chFramelimit;
	QButtonGroup* rbCPUEngine;

	QCheckBox* cbConfirmOnStop;
	QCheckBox* cbRenderToMain;

	// Paths
	QListWidget* pathList;
	QPushButton* clearPathList,* removePath;
	bool paths_changed;

	DControlStateManager* ctrlManager;
};
