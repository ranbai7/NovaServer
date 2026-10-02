#include "sql_connection_pool.h"
#include <iostream>
#include <list>
#include <mysql/mysql.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

using namespace std;

connection_pool::connection_pool()
{
    m_CurConn = 0;
    m_FreeConn = 0;
}

connection_pool *connection_pool::GetInstance()
{
    static connection_pool connPool;
    return &connPool;
}

void connection_pool::init(const string &url, const string &User, const string &PassWord, const string &DBName,
                           int Port, int MaxConn, int close_log)
{
    m_url = url;
    m_Port = Port;
    m_User = User;
    m_PassWord = PassWord;
    m_DatabaseName = DBName;
    m_close_log = close_log;

    for (int i = 0; i < MaxConn; i++)
    {
        MYSQL *con = mysql_init(NULL);
        if (con == NULL)
        {
            LOG_ERROR("mysql_init failed");
            exit(EXIT_FAILURE);
        }

        //保留句柄本身：mysql_real_connect 失败时返回空指针，
        //但错误信息仍挂在传入的句柄上，直接覆盖会丢掉失败原因
        if (mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, NULL, 0) == NULL)
        {
            LOG_ERROR("connect to MySQL %s:%d failed: %s", url.c_str(), Port, mysql_error(con));
            exit(EXIT_FAILURE);
        }

        connList.push_back(con);
        ++m_FreeConn;
    }

    reserve = sem(m_FreeConn);

    m_MaxConn = m_FreeConn;
}

MYSQL *connection_pool::GetConnection()
{
    reserve.wait();

    MYSQL *con = NULL;

    //对 connList 的读取必须与 ReleaseConnection 的写入同锁：此前在锁外读 size()，
    //与另一线程的 push_back 构成未保护的并发访问
    lock.lock();
    if (!connList.empty())
    {
        con = connList.front();
        connList.pop_front();

        --m_FreeConn;
        ++m_CurConn;
    }
    lock.unlock();

    //没取到就把信号量还回去，否则它的计数与实际空闲连接数不再对应
    if (NULL == con)
        reserve.post();

    return con;
}

bool connection_pool::ReleaseConnection(MYSQL *con)
{
    if (NULL == con)
        return false;

    lock.lock();

    connList.push_back(con);
    ++m_FreeConn;
    --m_CurConn;

    lock.unlock();

    reserve.post();
    return true;
}

void connection_pool::DestroyPool()
{
    lock.lock();
    if (connList.size() > 0)
    {
        list<MYSQL *>::iterator it;
        for (it = connList.begin(); it != connList.end(); ++it)
        {
            MYSQL *con = *it;
            mysql_close(con);
        }
        m_CurConn = 0;
        m_FreeConn = 0;
        connList.clear();
    }

    lock.unlock();
}

int connection_pool::GetFreeConn() const
{
    return this->m_FreeConn;
}

connection_pool::~connection_pool()
{
    DestroyPool();
}

connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool)
{
    *SQL = connPool->GetConnection();

    conRAII = *SQL;
    poolRAII = connPool;
}

connectionRAII::~connectionRAII()
{
    poolRAII->ReleaseConnection(conRAII);
}