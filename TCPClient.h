#include <tr1/functional>
#include "uv.h"
#include <string>
#include <list>
#ifndef BUFFER_SIZE
#define BUFFER_SIZE (1024*10)
#endif

//TCPClient接收到服务器数据回调给用户
typedef   std::tr1::function<void (const char* s, int size)>   userRecvCallback;
//TCPClient断线重连函数,心跳函数类型
typedef   std::tr1::function<void (void)>   voidParamCallback;

/*****************************************************/
//写请求结构体
typedef struct {
  uv_write_t req;  	//请求 
  uv_buf_t buf;		//保存发送的数据
} WriteReq_t;
//创建一个写请求
WriteReq_t * allocWriteParam(int packageSize);
//销毁一个写请求
void freeWriteParam(WriteReq_t* param);
/*****************************************************/


/*****************************************************/
//保存client上下文的结构体
typedef struct tcp_client_ctx {
	uv_tcp_t tcpHandle;		//tcp handle
	uv_buf_t recvBuf;		//接受消息的buf
	int client;				//client_id没用, 为TCPServer统一结构体
	void* parent_server;	//保存this指针
}TcpClientContext;
//创建一个client上下文	
TcpClientContext* allocTcpClientCtx(int packageSize, void* parentserver);
//释放一个client上下文
void freeTcpClientCtx(TcpClientContext* ctx);
/****************************************************/



class TCPClient
{
public:
	/*****************************************************
     * @param maxReceivePackageSize: 	接受包数据最大值,默认10k
     * @param maxSendPackageSize:		发送包数据最大值，默认10k
     * @param reconnectTimeout:	断开重连时间间隔
     ****************************************************/
	TCPClient(int reconnectTimeout,int maxReceivePackageSize=BUFFER_SIZE,int maxSendPackageSize=BUFFER_SIZE); 

	~TCPClient();
	/*****************************************************
     * @brief 设置接受消息事件回调函数
     * @param callback: 	回调函数名称
     ****************************************************/
	void setReceiveCallback(userRecvCallback callback);

	/*****************************************************
     * @brief 设置重连事件回调函数
     * @param callback: 	回调函数名称
     ****************************************************/
	void setReconnectCallback(voidParamCallback callback);	

	/*****************************************************
     * @brief 设置心跳事件回调函数
     * @param callback: 	回调函数名称
     * @param enable:		是否开启定时检测心跳
     * @param time:			心跳检测时间间隔
     ****************************************************/
	void setHeartbeatCallback(voidParamCallback callback, bool enable, int time);	

	/*****************************************************
     * @brief 是否启用 Nagle’s 算法
     * @param enable: 		
     *  true 	启用
     *  false 	禁用
     * @return 返回设置结果:
     *  true: 	设置成功
     *  false: 	失败
     ****************************************************/
    bool setNoDelay(bool enable);

	/*****************************************************
     * @brief 是否启用TCP keep-alive
     * @param enable: 		
     *  true 	启用
     *  false 	禁用
     * @param delay:	延时时间,以秒为单位,当enable为0时吗,值将被忽略
     * @return 返回设置结果:
     *  true: 	设置成功
     *  false: 	失败
     ****************************************************/
    bool setKeepAlive(int enable, unsigned int delay);	

    /*****************************************************
     * @brief 连接服务器
     * @param ip: 		服务器ip地址
     * @param port:		连接端口
     * @return 返回连接结果:
     *  true: 	成功
     *  false: 	失败
     ****************************************************/
	bool connect(const char* ip, int port); 

	/*****************************************************
     * @brief 用户主动重连服务器，适用场景:心跳超时
     * @return 返回重连尝试结果:
     *  true: 	成功
     *  false: 	失败
     ****************************************************/
	bool reconnect(); 

	/*****************************************************
     * @brief 发送数据,只检测数据有效性
     * @param data：	发送的数据
     * @param len:		数据长度
     * @return 返回连接结果:
     *  -1: 	数据有误
     *   0: 	数据无误
     ****************************************************/
	int  send(char* data, int len);

	/*****************************************************
     * @brief	关闭client,程序退出时必须调用
     ****************************************************/
	void close();   

	/*****************************************************
     * @brief   等待loop线程退出
     ****************************************************/
    void join();   
					
protected:
	bool init();
	bool run();
	int  sendToServer();
	//成员函数做回调函数
	static void onConnectCallback(uv_connect_t* req, int status);
	static void onReadCallback(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);
	static void onWriteCallback(uv_write_t* req, int status);
	static void onClientCloseCallback(uv_handle_t* handle);
	static void allocBufForRecvCallback(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf);
	static void asyncCallback(uv_async_t* handle);
	static void closeWalkCallback(uv_handle_t* handle, void* arg);
	static void loopRunThread(void* arg);              
	static void reconnectTimer(uv_timer_t* handle);
	static void heartbeatTimer(uv_timer_t* handle);
	bool startReconnect(void);
	void stopReconnect(void);
private:
	std::string getUVError(int err);
	std::string serverIp_;
	int serverPort_;
	uv_loop_t loop_; 
	uv_connect_t	connectHandle_;
	uv_thread_t		runThreadHandle_;
	uv_async_t		asyncHandle_;
	uv_timer_t 		reconnectTimer_;				//重连定时器
	uv_timer_t 		heartbeatTimer_;				//心跳定时器(检测心跳)
	uv_mutex_t	 	mutexWrite_;					//mutex of writeReqList_
	TcpClientContext 	*clientContext_;
	userRecvCallback 	recvcb_;				//接受数据回调
	voidParamCallback 	reconnectcb_;         	//断线重连回调用户函数
	voidParamCallback 	heartbeatcb_;         	//心跳回调函数
	bool 	isreconnecting_;					//是否开启重连
	int 	repeatTime_;						//重连时间间隔
	int 	maxReceivePackageSize_;				//接受缓冲区最大值
	int 	maxSendPackageSize_;				//发送缓冲区最大值
	bool 	isheartbeat_;         				//是否开启心跳
	int 	heartbeatTime_;						//心跳时间间隔
	std::list<WriteReq_t*> 	writeReqList_;  	//写缓冲区,后期用ringbuf
};