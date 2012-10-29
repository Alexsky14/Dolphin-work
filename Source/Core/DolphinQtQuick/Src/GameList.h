#include <QAbstractListModel>
#include "ISOFile.h"

class Game
{
public:
    Game(const std::string& fileName);
    QString type() const { return m_type; }
    QString logo() const { return m_logo; }
    QString name() const { return m_name; }
    QString desc() const  { return m_desc; }
    QString flag() const { return m_flag; }
    QString size() const { return m_size; }
    QString star() const { return m_star; }
private:
    QString m_type;
    QString m_logo;
    QString m_name;
    QString m_desc;
    QString m_flag;
    QString m_size;
    QString m_star;
};

class GameList : public QAbstractListModel
{
    Q_OBJECT
public:
    enum GameListRoles {
        TypeRole = Qt::UserRole + 1,
        LogoRole,
        NameRole,
        DescRole,
        FlagRole,
        SizeRole,
        StarRole
    };
    GameList(QObject *parent=0);

    void addGame(const Game &item);
    int rowCount(const QModelIndex & parent = QModelIndex()) const;
    QVariant data(const QModelIndex & index, int role = Qt::DisplayRole) const;
    void clear();
private:
    QList<Game> m_games;
};
