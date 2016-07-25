#include <tr1/functional>
#include "uv.h"
#include "http_parser.h"
#include <string>
#include <list>
#include <map>

using namespace std;

#ifndef BUFFER_SIZE
#define BUFFER_SIZE (1024*10)
#endif

#define MAX_HTTP_HEADERS 20

//HttpServer接收到request解析后回调给业务层
typedef   std::tr1::function<void (int clientid, string method, string url, std::map<string,string> headers, string data)>   userRecvCallback;


/*****************************************************/
//写请求结构体
typedef struct {
  uv_write_t req;  	//请求
  uv_buf_t buf;		//保存发送的数据
  int clientid;		//发送目标客户端
} WriteReq_t;
//创建一个写请求
WriteReq_t * allocWriteParam(int size);
//销毁一个写请求
void freeWriteParam(WriteReq_t* param);
/*****************************************************/


//单行http请求头部的信息
typedef struct http_head_t
{
    string field;
    string value;
}HttpHeaderLine;

/*****************************************************/
//保存client上下文的结构体
typedef struct http_request_t {
	uv_tcp_t tcp_handle;		//tcp handle
    uv_buf_t receive_buf;		//接受消息的buf
	int clientid;			//客户端标识id
	void* parent_server;	//保存this指针
    http_parser parser;     //每个请求独立的parser对象
    string url;             //请求的url
    string method;          //请求方式
    int header_lines_num;        //请求包含请求头个数
    HttpHeaderLine header_lines[MAX_HTTP_HEADERS];  //保存请求头部字段和其值
    string body;            //请求的数据参数
}HttpRequest;


//创建一个request上下文	
HttpRequest* allocHttpRequestCtx(int size,void* parentserver);
//释放一个request上下文
void freeHttpRequestCtx(HttpRequest* ctx);
/****************************************************/



class HttpServer
{
public:
	/*****************************************************
     * @param maxPackageSize: 	数据包的最大值，读或写,默认10k
     * @param reconnectTimeout:	断开重连时间间隔
     ****************************************************/
	HttpServer(int maxClientNum,int maxPackageSize=BUFFER_SIZE); 

	~HttpServer();
	/*****************************************************
     * @brief 设置接受消息事件回调函数
     * @param callback: 	回调函数名称
     ****************************************************/
	void setReceiveCallback(userRecvCallback callback);

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
	bool start(const char* ip, int port); 

	/*****************************************************
     * @brief 向编号为client_id的客户端发送数据
     * @param data：	发送的数据
     * @param len:		数据长度
     * @return 返回连接结果:
     *  -1: 	数据有误
     *   0: 	数据无误
     ****************************************************/
	int  send(int client_id, const char* data, int len);

	/*****************************************************
     * @brief	关闭client,程序退出时必须调用
     ****************************************************/
	void close();   

    /*****************************************************
     * @brief   等待loop线程退出
     ****************************************************/
    void join();   
					
protected:
	int  getAvailableClientID();
	bool init();
	bool run();
	int  sendToClient();
	//成员函数做回调函数
	static void onAcceptConnectionCallback(uv_stream_t* req, int status);
	static void onReadCallback(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);
	static void onWriteCallback(uv_write_t* req, int status);
	static void onClientCloseCallback(uv_handle_t* handle);
	static void allocBufForRecvCallback(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf);
	static void asyncCallback(uv_async_t* handle);
	static void closeWalkCallback(uv_handle_t* handle, void* arg);
	static void loopRunThread(void* arg);              
	//解析http请求的回调函数
    static int httpMessageBeginCallback(http_parser* parser);
    static int httpUrlCallback(http_parser* parser, const char* chunk, size_t len);
    static int httpHeaderFeildCallback(http_parser* parser, const char* chunk, size_t len);
    static int httpHeaderValueCallback(http_parser* parser, const char* chunk, size_t len);
    static int httpHeadersCompleteCallback(http_parser* parser);
    static int httpBodyCallback(http_parser* parser, const char* chunk, size_t len);
    static int httpMessageCompleteCallBack(http_parser* parser);
	
private:
	std::string getUVError(int err);
	std::string server_ip_;
	int server_port_;
	uv_loop_t loop_; 
	uv_tcp_t 	server_tcp_handle_;
	uv_thread_t	run_thread_handle_;
	uv_async_t	async_handle_;		
    http_parser_settings parser_settings_;
	uv_mutex_t mutex_write_;					//mutex of writeReqList_
	userRecvCallback receive_cb_;				//接受数据回调
	int max_package_size_;					//数据包最大值	
    int max_client_num_;
	std::list<WriteReq_t*> write_req_list_;  	//响应缓冲区
	std::map<int, HttpRequest*> request_map_;
    std::list<int> available_clientid_list_;  //可用ID
    //uv_mutex_t mutexContext_; 
};