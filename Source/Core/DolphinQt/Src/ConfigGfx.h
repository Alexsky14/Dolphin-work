#include <QTabWidget>

class DConfigGfx : public QTabWidget
{
	Q_OBJECT

public:
	DConfigGfx(QWidget* parent = NULL);
	virtual ~DConfigGfx();

public slots:
	void Reset();
	void Apply();
};
