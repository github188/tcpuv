#include "TCPServer.h"
#include <stdlib.h>
#include <assert.h> 
#include <string.h>
#include "yt/log/log.h"
using namespace std;

TcpClientContext* allocTcpClientCtx(int packageSize, void* parentserver)
{
    TcpClientContext* ctx = (TcpClientContext*)malloc(sizeof(TcpClientContext));
    ctx->recvBuf.base = (char*)malloc(packageSize);
    ctx->recvBuf.len = packageSize;
    ctx->parent_server = parentserver;	
    return ctx;
}

void freeTcpClientCtx(TcpClientContext* ctx)
{
    free(ctx->recvBuf.base);
    free(ctx);
} 

WriteReq_t * allocWriteParam(int packageSize)
{
	WriteReq_t * writeReq = (WriteReq_t*)malloc(sizeof(WriteReq_t));
	writeReq->buf.base = (char*)malloc(packageSize);
	return writeReq;
}

void freeWriteParam(WriteReq_t* param)
{
	free(param->buf.base);
    free(param);
}

TCPServer::TCPServer(int maxClientNum, int maxPackageSize):maxPackageSize_(maxPackageSize),maxClientNum_(maxClientNum)
{
	
}

TCPServer::~TCPServer()
{
    uv_mutex_destroy(&mutexWrite_);
    //uv_mutex_destroy(&mutexContext_);
    for (std::map<int, TcpClientContext *>::iterator it = clientContextMap_.begin(); it != clientContextMap_.end(); ++it) {         
        freeTcpClientCtx(it->second);
    }
    clientContextMap_.clear();
    for (std::list<WriteReq_t*>::iterator it = writeReqList_.begin(); it != writeReqList_.end(); ++it) {
        freeWriteParam(*it);
    }
    writeReqList_.clear();
}

void TCPServer::join()
{
    uv_thread_join(&runThreadHandle_);
}

void TCPServer::close()
{
    uv_stop(&loop_);
    uv_walk(&loop_, closeWalkCallback, this);  
}

void TCPServer::closeWalkCallback(uv_handle_t* handle, void* arg)  //回调多次
{
    //TCPClient* pclient = (TCPClient*)arg;
    if (uv_is_active(handle)) {      
        uv_close(handle, NULL);
        if (handle->type == UV_ASYNC) {         
            uv_async_send((uv_async_t*)handle);
        }    
    }
}

bool TCPServer::init()
{
	int ret = uv_loop_init(&loop_);
    if (ret) {
        AC_ERROR("uv_loop_init error,%s", getUVError(ret).c_str());
        return false;
    }

    ret = uv_mutex_init(&mutexWrite_);
    if (ret) {
         AC_ERROR("uv_mutex_init error,%s", getUVError(ret).c_str());
         return false;
    }

	ret = uv_async_init(&loop_, &asyncHandle_, asyncCallback);
	if (ret) {
		 AC_ERROR("uv_async_init error,%s", getUVError(ret).c_str());
		return false;
	}
	asyncHandle_.data = this;
   
	ret = uv_tcp_init(&loop_, &serverTcpHandle_);
	if (ret) {
		AC_ERROR("uv_tcp_init error,%s", getUVError(ret).c_str());
		return false;
	}
    serverTcpHandle_.data = this;
    

    ret = uv_tcp_nodelay(&serverTcpHandle_,  1);
    if (ret) {
        AC_ERROR("uv_tcp_nodelay error,%s", getUVError(ret).c_str());
        return false;
    }

	return true;
}

bool TCPServer::start(const char* ip, int port)
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
            AC_ERROR("the port already be used!");
        } else {
            AC_ERROR("listen error, %s",  getUVError(r).c_str());
        }
        return false;
    }
    
	r = uv_thread_create(&runThreadHandle_, loopRunThread, this);  
    if (r) {
      	AC_ERROR("uv_thread_create error,%s", getUVError(r).c_str());
        return false;
    }
	return true;
}

void TCPServer::loopRunThread(void* arg)
{
	TCPServer* pclient = (TCPServer*)arg;
    pclient->run();
}

void TCPServer::onAcceptConnectionCallback(uv_stream_t* server, int status)
{   
	TCPServer* parent = (TCPServer*)server->data;
    if (parent == NULL) {
        AC_ERROR("Lost TCPServer handle");
        return;
    }
    //assert(parent);

	if (status) {
        AC_ERROR("client Connect failed,%s", parent->getUVError(status).c_str());
        return;
    }
    
    TcpClientContext* clientContext = allocTcpClientCtx(parent->maxPackageSize_, parent);
    int r = uv_tcp_init(&parent->loop_, &clientContext->tcpHandle);
    if (r) {
        AC_ERROR("client tcp_init error,%s", parent->getUVError(r).c_str());
        return;
    }
    clientContext->tcpHandle.data = clientContext;
   
    r = uv_accept(server, (uv_stream_t*) &clientContext->tcpHandle);
    if (r) {
        AC_ERROR("error accepting connection ");
        uv_close((uv_handle_t*) &clientContext->tcpHandle, NULL);
        return;
    } else {
        clientContext->clientid = parent->getAvailableClientID();
        AC_INFO("client %d connected!", clientContext->clientid);
    }
    
    parent->clientContextMap_.insert(make_pair(clientContext->clientid, clientContext));

    r = uv_read_start((uv_stream_t *)&clientContext->tcpHandle, allocBufForRecvCallback, onReadCallback);
	if (r) {
        uv_close((uv_handle_t*)&clientContext->tcpHandle, onClientCloseCallback);
		AC_ERROR("uv_read_start error:%d", parent->getUVError(r).c_str());
        return;
	}
}

void TCPServer::asyncCallback(uv_async_t* handle)
{
    TCPServer* self = (TCPServer*)handle->data;
    self->sendToClient();                      //回调后发送数据
}

int TCPServer::send(int client_id, const char* data, int len)
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

int TCPServer::sendToClient()
{
    WriteReq_t* writereq = NULL;
    while (!writeReqList_.empty()) {
        uv_mutex_lock(&mutexWrite_);
        writereq = writeReqList_.front();
        writeReqList_.pop_front();
        uv_mutex_unlock(&mutexWrite_);

        map<int, TcpClientContext *>::iterator iter = clientContextMap_.find(writereq->clientid);
        if (iter == clientContextMap_.end()) {
            continue;
        }   

        uv_write((uv_write_t*)&writereq->req, (uv_stream_t*)&iter->second->tcpHandle, &writereq->buf, 1, onWriteCallback);
    }
    return 0;
}

void TCPServer::onWriteCallback(uv_write_t* req, int status)
{
    TCPServer* self = (TCPServer*)req->data;
    WriteReq_t *writereq = (WriteReq_t*)req; 
    if (status < 0) {
        self->writeReqList_.push_back(writereq); //发送错误,把数据再放回去
        AC_ERROR("send data error, %d ", self->getUVError(status).c_str());
        return;
    } else { 
        if (req) {
            free(writereq);
        }       
    }
}

int TCPServer::broadcast(const char* data, int len)
{
    if (!data || len < 0) {
        return -1;
    }

    if (!clientContextMap_.empty()) {
        for (map<int, TcpClientContext *>::iterator it = clientContextMap_.begin(); it != clientContextMap_.end(); ++it) {         
            send(it->first, data, len);
        }
    }
    
    return 0;
}

void TCPServer::onClientCloseCallback(uv_handle_t* handle)
{
    TcpClientContext* clientContext = (TcpClientContext*)handle->data;
    TCPServer *parent = (TCPServer *)clientContext->parent_server;

    map<int, TcpClientContext *>::iterator iter = parent->clientContextMap_.find(clientContext->clientid);
    if (iter != parent->clientContextMap_.end()) {
        freeTcpClientCtx(iter->second);
        parent->clientContextMap_.erase(iter);
    }
    parent->availableClientIDList_.push_back(clientContext->clientid);  //id循环利用
}

void TCPServer::allocBufForRecvCallback(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf)
{
	TcpClientContext* self = (TcpClientContext*)handle->data;
    assert(self);
    *buf = self->recvBuf;
}

void TCPServer::onReadCallback(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf)
{
	TcpClientContext* clientContext = (TcpClientContext*)stream->data;
    if (clientContext == NULL) {
        AC_ERROR("Lost TcpClientContext, can not distinguist tcpclient!");
        return;
    }
    //assert(clientContext);
	TCPServer* parent = (TCPServer*)clientContext->parent_server;
    if (parent == NULL) {
        AC_ERROR("Lost TCPServer handle");
        return;
    }
    if (nread < 0) {
        if (nread == UV_EOF) {
            AC_INFO("client(%d) close(EOF)", clientContext->clientid);
           
        } else if (nread == UV_ECONNRESET) {
            AC_INFO("client(%d) close(conn reset)", clientContext->clientid);
           
        } else {
            AC_INFO("client(%d) close", clientContext->clientid);      
        }
        uv_close((uv_handle_t*)&clientContext->tcpHandle, onClientCloseCallback);   
        return;
    }
    if (parent->recvcb_) {
    	parent->recvcb_(clientContext->clientid, (const char*) buf->base, nread); 
    }	
}

void TCPServer::setReceiveCallback(userRecvCallback callback)
{
	recvcb_ = callback;
}

bool TCPServer::run()
{
	int ret = uv_run(&loop_, UV_RUN_DEFAULT);
	if (ret) {
		AC_ERROR("there are still active handles or requests");
		return false;
	}
	return true;
} 

void TCPServer::closeClient(int clientid)
{
    map<int, TcpClientContext *>::iterator iter = clientContextMap_.find(clientid);
    if (iter != clientContextMap_.end()) {
        uv_close((uv_handle_t*)&(iter->second->tcpHandle), onClientCloseCallback);
    }   
}

int TCPServer::getAvailableClientID() 
{
    static int s_id = 0;
    if (availableClientIDList_.empty()) {
        return ++s_id;
    }
    int id = availableClientIDList_.front();
    availableClientIDList_.pop_front();
    return id;
}

string TCPServer::getUVError(int errcode)
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