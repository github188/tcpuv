#include "HttpServer.h"
#include <stdlib.h>
#include <assert.h> 
#include <string.h>
#include "yt/log/log.h"
using namespace std;

HttpRequest* allocHttpRequestCtx(int size, void* parentserver)
{
    // HttpRequest* ctx = (HttpRequest*)malloc(sizeof(HttpRequest));
    HttpRequest* ctx = new HttpRequest;
    ctx->recvBuf.base = (char*)malloc(size);
    ctx->recvBuf.len = size;
    ctx->parent_server = parentserver;	
    return ctx;
}

void freeHttpRequestCtx(HttpRequest* ctx)
{
    free(ctx->recvBuf.base);
    free(ctx);
} 

WriteReq_t * allocWriteParam(int size)
{
	WriteReq_t * writeReq = (WriteReq_t*)malloc(sizeof(WriteReq_t));
	writeReq->buf.base = (char*)malloc(size);
	return writeReq;
}

void freeWriteParam(WriteReq_t* param)
{
	free(param->buf.base);
    free(param);
}

HttpServer::HttpServer(int maxClientNum, int maxPackageSize):maxPackageSize_(maxPackageSize),maxClientNum_(maxClientNum)
{
	
}

HttpServer::~HttpServer()
{
    uv_mutex_destroy(&mutexWrite_);
    //uv_mutex_destroy(&mutexContext_);
    for (std::map<int, HttpRequest *>::iterator it = requestMap_.begin(); it != requestMap_.end(); ++it) {         
        freeHttpRequestCtx(it->second);
    }
    requestMap_.clear();
    for (std::list<WriteReq_t*>::iterator it = writeReqList_.begin(); it != writeReqList_.end(); ++it) {
        freeWriteParam(*it);
    }
    writeReqList_.clear();
}

void HttpServer::join()
{
    uv_thread_join(&runThreadHandle_);
}

void HttpServer::close()
{
    uv_stop(&loop_);
    uv_walk(&loop_, closeWalkCallback, this);  
}

void HttpServer::closeWalkCallback(uv_handle_t* handle, void* arg)  //回调多次
{
    //TCPClient* pclient = (TCPClient*)arg;
    if (uv_is_active(handle)) {      
        uv_close(handle, NULL);
        if (handle->type == UV_ASYNC) {         
            uv_async_send((uv_async_t*)handle);
        }    
    }
}

bool HttpServer::init()
{
	int ret = uv_loop_init(&loop_);
    if (ret) {
        printf("uv_loop_init error,%s", getUVError(ret).c_str());
        return false;
    }

    ret = uv_mutex_init(&mutexWrite_);
    if (ret) {
         printf("uv_mutex_init error,%s", getUVError(ret).c_str());
         return false;
    }

	ret = uv_async_init(&loop_, &asyncHandle_, asyncCallback);
	if (ret) {
		 printf("uv_async_init error,%s", getUVError(ret).c_str());
		return false;
	}
	asyncHandle_.data = this;
   
	ret = uv_tcp_init(&loop_, &serverTcpHandle_);
	if (ret) {
		printf("uv_tcp_init error,%s", getUVError(ret).c_str());
		return false;
	}
    serverTcpHandle_.data = this;
    

    ret = uv_tcp_nodelay(&serverTcpHandle_,  1);
    if (ret) {
        printf("uv_tcp_nodelay error,%s", getUVError(ret).c_str());
        return false;
    }

    parserSettings_.on_url = httpUrlCallback;
    parserSettings_.on_body = httpBodyCallback;
    parserSettings_.on_header_field = httpHeaderFeildCallback;
    parserSettings_.on_header_value = httpHeaderValueCallback;
    parserSettings_.on_headers_complete = httpHeadersCompleteCallback;
    parserSettings_.on_message_begin = httpMessageBeginCallback;
    parserSettings_.on_message_complete = httpMessageCompleteCallBack;


	return true;
}

bool HttpServer::start(const char* ip, int port)
{
	init();
	serverIp_ = ip;
	serverPort_ = port;

	struct sockaddr_in bindAddr;
	uv_ip4_addr(ip, port, &bindAddr);

    uv_tcp_bind(&serverTcpHandle_, (struct sockaddr*)&bindAddr, 0);
    
	int r = uv_listen((uv_stream_t*) &serverTcpHandle_, 128, onAcceptConnectionCallback);
    if(r) {
        if (r == UV_EADDRINUSE) {
            printf("the port already be used!");
        } else {
            printf("listen error, %s",  getUVError(r).c_str());
        }
        return false;
    }
    
	r = uv_thread_create(&runThreadHandle_, loopRunThread, this);  
    if (r) {
      	printf("uv_thread_create error,%s", getUVError(r).c_str());
        return false;
    }
	return true;
}

void HttpServer::loopRunThread(void* arg)
{
	HttpServer* pServer = (HttpServer*)arg;
    pServer->run();
}

void HttpServer::onAcceptConnectionCallback(uv_stream_t* server, int status)
{   
	HttpServer* parent = (HttpServer*)server->data;
    if (parent == NULL) {
        printf("Lost HttpServer handle");
        return;
    }
    //assert(parent);

	if (status) {
        printf("client Connect failed,%s", parent->getUVError(status).c_str());
        return;
    }
    
    HttpRequest* request = allocHttpRequestCtx(parent->maxPackageSize_, parent);
    int r = uv_tcp_init(&parent->loop_, &request->tcpHandle);
    if (r) {
        printf("client tcp_init error,%s", parent->getUVError(r).c_str());
        return;
    }
    request->tcpHandle.data = request;
    request->parser.data = request;

    r = uv_accept(server, (uv_stream_t*) &request->tcpHandle);
    if (r) {
        printf("error accepting connection ");
        uv_close((uv_handle_t*) &request->tcpHandle, NULL);
        return;
    } else {
        request->clientid = parent->getAvailableClientID();
        printf("client %d connected!\n", request->clientid);
        http_parser_init(&request->parser, HTTP_REQUEST);
    }
    
    parent->requestMap_.insert(make_pair(request->clientid, request));

    r = uv_read_start((uv_stream_t *)&request->tcpHandle, allocBufForRecvCallback, onReadCallback);
	if (r) {
        uv_close((uv_handle_t*)&request->tcpHandle, onClientCloseCallback);
		printf("uv_read_start error:%s", parent->getUVError(r).c_str());
        return;
	}
}

void HttpServer::asyncCallback(uv_async_t* handle)
{
    HttpServer* self = (HttpServer*)handle->data;
    self->sendToClient();                      //回调后发送数据
}

int HttpServer::send(int client_id, const char* data, int len)
{
    if (!data || len < 0) {
        return -1;
    }
    uv_async_send(&asyncHandle_);              //产生回调, 查看之前的数据发送完没有，没完就发送                       
    WriteReq_t *writereq = allocWriteParam(maxPackageSize_);
    writereq->req.data = this;
    memcpy(writereq->buf.base, data, len);
    writereq->buf.len = len;
    writereq->clientid = client_id;
    writeReqList_.push_back(writereq);
    uv_async_send(&asyncHandle_);
    return 0;
}

int HttpServer::sendToClient()
{
    WriteReq_t* writereq = NULL;
    while (!writeReqList_.empty()) {
        uv_mutex_lock(&mutexWrite_);
        writereq = writeReqList_.front();
        writeReqList_.pop_front();
        uv_mutex_unlock(&mutexWrite_);

        map<int, HttpRequest *>::iterator iter = requestMap_.find(writereq->clientid);
        if (iter == requestMap_.end()) {
            continue;
        }   

        uv_write((uv_write_t*)&writereq->req, (uv_stream_t*)&iter->second->tcpHandle, &writereq->buf, 1, onWriteCallback);
    }
    return 0;
}

void HttpServer::onWriteCallback(uv_write_t* req, int status)
{
    HttpServer* self = (HttpServer*)req->data;
    WriteReq_t *writereq = (WriteReq_t*)req;
    if (status < 0) {
        self->writeReqList_.push_back(writereq); //发送错误,把数据再放回去
        printf("send response error, %s", self->getUVError(status).c_str());
        return;
    } 
    uv_close((uv_handle_t*) req->handle, NULL);
}

void HttpServer::onClientCloseCallback(uv_handle_t* handle)
{
    HttpRequest* request = (HttpRequest*)handle->data;
    HttpServer *parent = (HttpServer *)request->parent_server;

    map<int, HttpRequest *>::iterator iter = parent->requestMap_.find(request->clientid);
    if (iter != parent->requestMap_.end()) {
        freeHttpRequestCtx(iter->second);
        parent->requestMap_.erase(iter);
    }
    parent->availableClientIDList_.push_back(request->clientid);
}

void HttpServer::allocBufForRecvCallback(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf)
{
	HttpRequest * request = (HttpRequest*)handle->data;
    *buf = request->recvBuf;
}

void HttpServer::onReadCallback(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf)
{
	HttpRequest* request = (HttpRequest*)stream->data;
    if (request == NULL) {
        printf("Lost HttpRequest, can not distinguist tcpclient!");
        return;
    }
    //assert(request);
	HttpServer* parent = (HttpServer*)request->parent_server;
    if (parent == NULL) {
        printf("Lost HttpServer handle");
        return;
    }
    if (nread < 0) {
        if (nread == UV_EOF) {
            printf("client(%d) close(EOF)", request->clientid);
           
        } else if (nread == UV_ECONNRESET) {
            printf("client(%d) close(conn reset)", request->clientid);
           
        } else {
            printf("client(%d) close", request->clientid);      
        }
        uv_close((uv_handle_t*)&request->tcpHandle, onClientCloseCallback);   
        return;
    }
    printf("receive data:%s\n", buf->base);
    int parsed = http_parser_execute(&request->parser, &parent->parserSettings_, (char*)buf->base, nread);

    if (request->parser.upgrade) {
        //此处一般为websocket协议
    } else if (parsed != nread) {
        printf("http parser error, recvlen:%d, parserlen:%d\n", (int)nread, parsed);
        uv_close((uv_handle_t*)&request->tcpHandle, onClientCloseCallback);
    }
}

void HttpServer::setReceiveCallback(userRecvCallback callback)
{
	recvcb_ = callback;
}

bool HttpServer::run()
{
	int ret = uv_run(&loop_, UV_RUN_DEFAULT);
	if (ret) {
		printf("there are still active handles or requests");
		return false;
	}
	return true;
} 



int HttpServer::getAvailableClientID()
{
    static int s_id = 0;
    if (availableClientIDList_.empty()) {
        return ++s_id;
    }
    int id = availableClientIDList_.front();
    availableClientIDList_.pop_front();
    return id;
}

string HttpServer::getUVError(int errcode)
{
    if(errcode == 0) {
        return "";
    }
    string errMsg;
    const char* tmpChar = uv_err_name(errcode);
    if (tmpChar) {
        errMsg = tmpChar;
        errMsg += ":";
    } else {
        errMsg = "unknown system errcode";
        errMsg += ":";
    }
    
    tmpChar = uv_strerror(errcode);
    if (tmpChar) {
        errMsg += tmpChar;
    }
    tmpChar = NULL;
    return errMsg;

}
//通知回调,开始解析HTTP消息
int HttpServer::httpMessageBeginCallback(http_parser* parser)
{
    HttpRequest * request = (HttpRequest*)parser->data;
    request->headerLinesNum = 0;
    return 0;
}

int HttpServer::httpUrlCallback(http_parser* parser, const char* chunk, size_t len)
{
    HttpRequest *request = (HttpRequest*)parser->data;

    string tmpStr = chunk;
    request->url = tmpStr.substr(0, len); 
    request->method = http_method_str((enum http_method)parser->method);
    return 0;
}

int HttpServer::httpHeaderFeildCallback(http_parser* parser, const char* chunk, size_t len)
{
    HttpRequest *request = (HttpRequest*)parser->data;
    HttpHeaderLine *headerLine = &request->headerLines[request->headerLinesNum];
    string tmpStr = chunk;
    headerLine->field =tmpStr.substr(0, len);
    return 0;
}

int HttpServer::httpHeaderValueCallback(http_parser* parser, const char* chunk, size_t len)
{
    HttpRequest *request = (HttpRequest*)parser->data;
    HttpHeaderLine *headerLine = &request->headerLines[request->headerLinesNum];
    string tmpStr = chunk;
    headerLine->value = tmpStr.substr(0, len);
    ++request->headerLinesNum;
    return 0;
}

//通知回调,http报文头部解析完毕
int HttpServer::httpHeadersCompleteCallback(http_parser* parser)
{
    //HttpRequest *request = (HttpRequest*)parser->data;
    //request->method = http_method_str(parser->method);
    return 0;
}

int HttpServer::httpBodyCallback(http_parser* parser, const char* chunk, size_t len)
{
    HttpRequest *request = (HttpRequest*)parser->data;
    string tmpStr = chunk;
    request->body = tmpStr.substr(0, len);
    return 0;
}

//通知回调,消息解析完毕
int HttpServer::httpMessageCompleteCallBack(http_parser* parser)
{
    HttpRequest *request = (HttpRequest*)parser->data;
    if (request->method == "GET") {
        string url = request->url;
        size_t npos = url.find('?'); 
        if ( npos != string::npos) {
            request->body = url.substr(npos + 1);
            request->url = url.substr(0, npos);
        }
    }

    // printf("url: %s\n", request->url.c_str());
    // printf("method: %s\n", request->method.c_str());
    std::map<string, string> headersMap;
    for (int i = 0; i < request->headerLinesNum; i++) {
        HttpHeaderLine* header = &request->headerLines[i];
        headersMap.insert(make_pair(header->field, header->value));
        // if (!header->field.empty())
        //     printf("Header: %s: %s\n", header->field.c_str(), header->value.c_str());
    }
    // printf("body: %s\n", request->body.c_str());
    // printf("\r\n");
    
    HttpServer* parent = (HttpServer*)request->parent_server;
    if (parent == NULL) {
        printf("Lost HttpServer handle");
        return -1;
    }

    if (parent->recvcb_) {
    	parent->recvcb_(request->clientid, request->method, request->url, headersMap, request->body); 
    }	

    return 0;
}