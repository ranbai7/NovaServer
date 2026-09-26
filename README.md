

NovaServer
===============
Linux 下 C++ 轻量级 Web 服务器，在 **线程池 + Epoll (ET) + Reactor/Proactor** 高并发架构的基础上，新增文件上传功能，并修复了静态资源 MIME 类型等实用性细节。

> **项目来源**：本项目的架构设计参考自开源项目 [qinguoyi/TinyWebServer](https://github.com/qinguoyi/TinyWebServer)（MIT 协议），在其基础上进行二次开发与重构。原项目版权归原作者所有，详见 [LICENSE](LICENSE)。

## 新增功能 / 改进
- **文件上传**  
  支持 `multipart/form-data` 格式的文件上传，自动解析 boundary，提取文件名与内容，保存至 `./upload/` 目录，并返回用户友好的成功页面。
- **全站界面美化**  
  重新设计所有 HTML 页面（登录、注册、主面板、上传、成功提示等），使用统一的现代卡片式布局，配色简洁，交互流畅。
- **MIME 类型支持**  
  增加了基于文件扩展名的 MIME 类型映射（`get_mime_type`），为静态资源响应添加正确的 `Content-Type` 头，解决了部分浏览器直接显示 HTML 源码或乱码的问题。
- **上传路径安全**  
  上传文件名自动去除路径前缀，防止目录穿越攻击。
- **路由扩展**  
  新增 `/8`（上传页面）和 `/upload`（上传接口）路由，与原有登录/注册/图片视频等功能无缝集成。

---
## 原项目描述
* 使用 **线程池 + 非阻塞socket + epoll(ET和LT均实现) + 事件处理(Reactor和模拟Proactor均实现)** 的并发模型
* 使用**状态机**解析HTTP请求报文，支持解析**GET和POST**请求
* 访问服务器数据库实现web端用户**注册、登录**功能，可以请求服务器**图片和视频文件**
* 实现**同步/异步日志系统**，记录服务器运行状态
* 经Webbench压力测试可以实现**上万的并发连接**数据交换


目录
-----

| [概述](#概述) | [框架](#框架) | [界面展示](#界面展示) | [压力测试](#压力测试) | [快速运行](#快速运行) | [个性化运行](#个性化运行) | [致谢](#致谢) |
|:--------:|:--------:|:--------:|:--------:|:--------:|:--------:|:--------:|


概述
----------

> * C/C++
> * B/S模型
> * [线程同步机制包装类](https://github.com/qinguoyi/TinyWebServer/tree/master/lock)
> * [http连接请求处理类](https://github.com/qinguoyi/TinyWebServer/tree/master/http)
> * [半同步/半反应堆线程池](https://github.com/qinguoyi/TinyWebServer/tree/master/threadpool)
> * [定时器处理非活动连接](https://github.com/qinguoyi/TinyWebServer/tree/master/timer)
> * [同步/异步日志系统 ](https://github.com/qinguoyi/TinyWebServer/tree/master/log)  
> * [数据库连接池](https://github.com/qinguoyi/TinyWebServer/tree/master/CGImysql) 
> * [同步线程注册和登录校验](https://github.com/qinguoyi/TinyWebServer/tree/master/CGImysql) 
> * [简易服务器压力测试](https://github.com/qinguoyi/TinyWebServer/tree/master/test_presure)


框架
-------------
<div align=center><img src="http://ww1.sinaimg.cn/large/005TJ2c7ly1ge0j1atq5hj30g60lm0w4.jpg" height="765"/> </div>

界面展示
----------
> * 首界面

<div align=center><img src="root/judge.png" height="429"/> </div>

> * 注册界面

<div align=center><img src="root/register.png" height="429"/> </div>

> * 登录界面

<div align=center><img src="root/log.png" height="429"/> </div>

> * 欢迎界面

<div align=center><img src="root/welcome.png" height="429"/> </div>

> * 图片展示界面

<div align=center><img src="root/pictrue.png" height="429"/> </div>

> * 视频播放界面

<div align=center><img src="root/video.png" height="429"/> </div>

> * 文件上传界面

<div align=center><img src="root/upload.png" height="429"/> </div>

> * 文件上传成功界面

<div align=center><img src="root/uploadSuccess.png" height="429"/> </div>

压力测试
-------------
在关闭日志后，使用Webbench对服务器进行压力测试，对listenfd和connfd分别采用ET和LT模式，均可实现上万的并发连接，下面列出的是两者组合后的测试结果. 

> * Proactor，LT + LT，93251 QPS

<div align=center><img src="http://ww1.sinaimg.cn/large/005TJ2c7ly1gfjqu2hptkj30gz07474n.jpg" height="201"/> </div>

> * Proactor，LT + ET，97459 QPS

<div align=center><img src="http://ww1.sinaimg.cn/large/005TJ2c7ly1gfjr1xppdgj30h206zdg6.jpg" height="201"/> </div>

> * Proactor，ET + LT，80498 QPS

<div align=center><img src="http://ww1.sinaimg.cn/large/005TJ2c7ly1gfjr24vmjtj30gz0720t3.jpg" height="201"/> </div>

> * Proactor，ET + ET，92167 QPS

<div align=center><img src="http://ww1.sinaimg.cn/large/005TJ2c7ly1gfjrflrebdj30gz06z0t3.jpg" height="201"/> </div>

> * Reactor，LT + ET，69175 QPS

<div align=center><img src="http://ww1.sinaimg.cn/large/005TJ2c7ly1gfjr1humcbj30h207474n.jpg" height="201"/> </div>

> * 并发连接总数：10500
> * 访问服务器时间：5s
> * 所有访问均成功

**注意：** 使用本项目的webbench进行压测时，若报错显示webbench命令找不到，将可执行文件webbench删除后，重新编译即可。


快速运行
------------
* 服务器测试环境
	* Ubuntu版本16.04
	* MySQL版本5.7.29
* 浏览器测试环境
	* Windows、Linux均可
	* Chrome
	* FireFox
	* 其他浏览器暂无测试

* 测试前确认已安装MySQL数据库

    ```C++
    // 建立yourdb库
    create database yourdb;

    // 创建user表
    USE yourdb;
    CREATE TABLE user(
        username char(50) NULL,
        passwd char(50) NULL
    )ENGINE=InnoDB;

    // 添加数据
    INSERT INTO user(username, passwd) VALUES('name', 'passwd');
    ```

* 修改main.cpp中的数据库初始化信息

    ```C++
    //数据库登录名,密码,库名
    string user = "root";
    string passwd = "root";
    string databasename = "yourdb";
    ```

* build

    ```C++
    sh ./build.sh
    ```

* 启动server

    ```C++
    ./build/server
    ```

* 浏览器端

    ```C++
    ip:9006
    ```

运行测试
------
单元测试依赖 GoogleTest，首次使用需先安装：

```bash
sudo apt install libgtest-dev
```

执行全部测试：

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

个性化运行
------

```C++
./build/server [-p port] [-l LOGWrite] [-m TRIGMode] [-o OPT_LINGER] [-s sql_num] [-t thread_num] [-c close_log] [-a actor_model]
```

温馨提示:以上参数不是非必须，不用全部使用，根据个人情况搭配选用即可.

* -p，自定义端口号
	* 默认9006
* -l，选择日志写入方式，默认同步写入
	* 0，同步写入
	* 1，异步写入
* -m，listenfd和connfd的模式组合，默认使用LT + LT
	* 0，表示使用LT + LT
	* 1，表示使用LT + ET
    * 2，表示使用ET + LT
    * 3，表示使用ET + ET
* -o，优雅关闭连接，默认不使用
	* 0，不使用
	* 1，使用
* -s，数据库连接数量
	* 默认为8
* -t，线程数量
	* 默认为8
* -c，关闭日志，默认打开
	* 0，打开日志
	* 1，关闭日志
* -a，选择反应堆模型，默认Proactor
	* 0，Proactor模型
	* 1，Reactor模型

测试示例命令与含义

```C++
./build/server -p 9007 -l 1 -m 0 -o 1 -s 10 -t 10 -c 1 -a 1
```

- [x] 端口9007
- [x] 异步写入日志
- [x] 使用LT + LT组合
- [x] 使用优雅关闭连接
- [x] 数据库连接池内有10条连接
- [x] 线程池内有10条线程
- [x] 关闭日志
- [x] Reactor反应堆模型


致谢
------------
本项目基于 qinguoyi/TinyWebServer 进行二次开发，只用于自主学习，遵守原项目许可证。
十分感谢原作者的开源，如果你觉得有帮助，欢迎给原项目点个 Star⭐️。



