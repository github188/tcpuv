#include "TCPClient.h"
#include <stdlib.h>
#include <assert.h> 
#include <string.h>
#include "yt/log/log.h"
using namespace std;

ClientContext* allocClientCtx(int packageSize, void* parentserver)
{
    ClientContext* ctx = (ClientContext*)malloc(sizeof(ClientContext));
    ctx->recvBuf.base = (char*)malloc(packageSize);
    ctx->recvBuf.len = packageSize;
    ctx->parent_server = parentserver;	//store TCPClient
    return ctx;
}

void freeClientCtx(ClientContext* ctx)
{
    free(ctx->recvBuf.base);
    free(ctx);
}

WriteReq * allocWriteReqParam(int packageSize)
{
	WriteReq * writeReq = (WriteReq*)malloc(sizeof(WriteReq));
	writeReq->buf.base = (char*)malloc(packageSize);
	return writeReq;
}

void freeWriteReqParam(WriteReq* param)
{
	free(param->buf.base);
    free(param);
}

TCPClient::TCPClient(int reconnectTimeout, int maxReceivePackageSize, int maxSendPackageSize)
:clientContext_(NULL), recvcb_(NULL),reconnectcb_(NULL),isreconnecting_(true),repeatTime_(reconnectTimeout),
maxReceivePackageSize_(maxReceivePackageSize),maxSendPackageSize_(maxSendPackageSize),isheartbeat_(false)
{
	clientContext_ = allocClientCtx(maxReceivePackageSize_,this);
}

TCPClient::~TCPClient()
{
    uv_mutex_destroy(&mutexWrite_);
    freeClientCtx(clientContext_);
    for (std::list<WriteReq*>::iterator it = writeReqList_.begin(); it != writeReqList_.end(); ++it) {
        freeWriteReqParam(*it);
    }
    writeReqList_.clear();
}

void TCPClient::close()
{
    uv_timer_stop(&heartbeatTimer_);
    stopReconnect();
    uv_stop(&loop_);
    uv_walk(&loop_, closeWalkCallback, this);  
}

void TCPClient::join()
{
    uv_thread_join(&runThreadHandle_);
}

void TCPClient::closeWalkCallback(uv_handle_t* handle, void* arg)  //回调多次
{
    //TCPClient* pclient = (TCPClient*)arg;
    if (uv_is_active(handle)) {      
        uv_close(handle, NULL);
        if (handle->type == UV_ASYNC) {         
            uv_async_send((uv_async_t*)handle);
        }    
    }
}

bool TCPClient::init()
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
   
	ret = uv_tcp_init(&loop_, &clientContext_->tcpHandle);
	if (ret) {
		AC_ERROR("uv_tcp_init error,%s", getUVError(ret).c_str());
		return false;
	}
   
	ret = uv_timer_init(&loop_, &reconnectTimer_);
	if (ret) {
		AC_ERROR("uv_timer_init error,%s", getUVError(ret).c_str());
		return false;
	}
	reconnectTimer_.data = this;

    ret = uv_timer_init(&loop_, &heartbeatTimer_);
    if (ret) {
        AC_ERROR("uv_timer_init error,%s", getUVError(ret).c_str());
        return false;
    }
    heartbeatTimer_.data = this;

	clientContext_->tcpHandle.data = clientContext_;
    
	return true;
}

void TCPClient::asyncCallback(uv_async_t* handle)
{
	TCPClient* self = (TCPClient*)handle->data;
    if (self == NULL) { 
        AC_ERROR("lost TCPClient handle!");
        return;
    }
	self->sendToServer();                      //回调后发送数据
}

int TCPClient::send(char* data, int len)
{
	if (!data || len < 0) {
		return -1;
	}
	uv_async_send(&asyncHandle_);              //产生回调, 查看之前的数据发送完没有，没完就发送						
	WriteReq *writereq = allocWriteReqParam(maxSendPackageSize_);
    writereq->req.data = this;
    memcpy(writereq->buf.base, data, len);
    writereq->buf.len = len;
   	writeReqList_.push_back(writereq);
    uv_async_send(&asyncHandle_);
	return 0;
}

int TCPClient::sendToServer()
{
    WriteReq* writereq = NULL;
    while (!writeReqList_.empty()) {
        uv_mutex_lock(&mutexWrite_);
        writereq = writeReqList_.front();
        writeReqList_.pop_front();
        uv_mutex_unlock(&mutexWrite_);

        uv_write((uv_write_t*)&writereq->req, (uv_stream_t*)&clientContext_->tcpHandle, &writereq->buf, 1, onWriteCallback);

   }
    return 0;
}

void TCPClient::onWriteCallback(uv_write_t* req, int status)
{
    static int error_count = 0;
    TCPClient* self = (TCPClient*)req->data;
    if (self == NULL) { 
        AC_ERROR("lost TCPClient handle!");
        return;
    }
    WriteReq *writereq = (WriteReq*)req;
    if (status < 0) {
        error_count++;
        if (error_count > 5) {   //连续5次都发送失败,可能服务器异常, 尝试断开重连
             for (std::list<WriteReq*>::iterator it = self->writeReqList_.begin(); it != self->writeReqList_.end(); ++it) {
                freeWriteReqParam(*it);
            }
            self->writeReqList_.clear();
            error_count = 0;
            self->reconnect();
            return;
        }
        self->writeReqList_.push_back(writereq); //发送错误,把数据再放回去
        AC_ERROR("errCount:%d send data error, errNo:%d ", error_count, self->getUVError(status).c_str());
        return;
    } else { 
        error_count = 0;
        if (req) {
            free(writereq);
        }       
    }
}

bool TCPClient::connect(const char* ip, int port)
{
	if (!init()) {
        AC_ERROR("TCPClient init error!");
        return false;
    }
	serverIp_ = ip;
	serverPort_ = port;

	struct sockaddr_in bindAddr;
	uv_ip4_addr(ip, port, &bindAddr);

	int r = uv_tcp_connect(&connectHandle_, &clientContext_->tcpHandle, (struct sockaddr*) &bindAddr, onConnectCallback);
	if (r) {
		AC_ERROR("uv_tcp_connect error,%s", getUVError(r).c_str());
		return false;
	}
   
	r = uv_thread_create(&runThreadHandle_, loopRunThread, this);  
    if (r) {
      	AC_ERROR("uv_thread_create error,%s", getUVError(r).c_str());
        return false;
    }
	
	return true;
}

void TCPClient::loopRunThread(void* arg)
{
	TCPClient* pclient = (TCPClient*)arg;
    pclient->run();
}

void TCPClient::onConnectCallback(uv_connect_t* connHandle, int status)
{
	ClientContext* self = (ClientContext*)connHandle->handle->data;
	TCPClient* parent = (TCPClient*)self->parent_server;
    if (parent == NULL) { 
        AC_ERROR("lost TCPClient handle!");
        return;
    }
	if (status) {      
        AC_ERROR("connect error,%s", parent->getUVError(status).c_str());    
        if (parent->isreconnecting_) {      
            uv_timer_stop(&parent->reconnectTimer_);
            uv_timer_start(&parent->reconnectTimer_, TCPClient::reconnectTimer, parent->repeatTime_, parent->repeatTime_);
        }
		return;
	} 
	AC_INFO("connect to srever:%s, port:%d", parent->serverIp_.c_str(), parent->serverPort_);
	int r = uv_read_start(connHandle->handle, allocBufForRecvCallback, onReadCallback);
	if (r) {
		AC_ERROR("uv_read_start error:%d", r);
	}

    if (parent->isreconnecting_) {
       //AC_INFO("reconnect to srever:%s, port:%d\n", parent->serverIp_.c_str(), parent->serverPort_);
        parent->stopReconnect();        //reconnect succeed.
        if (parent->reconnectcb_) {
            parent->reconnectcb_();      //重连成功执行用户回调函数
        }
    }

    if(parent->isheartbeat_) {
        uv_timer_stop(&parent->heartbeatTimer_);
        uv_timer_start(&parent->heartbeatTimer_, TCPClient::heartbeatTimer, 1e4, parent->heartbeatTime_);
    }

}


void TCPClient::allocBufForRecvCallback(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf)
{
	ClientContext* self = (ClientContext*)handle->data;
    assert(self);
    *buf = self->recvBuf;
}

void TCPClient::onReadCallback(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf)
{
	ClientContext* self = (ClientContext*)stream->data;
    assert(self);
	TCPClient* parent = (TCPClient*)self->parent_server;
    if (parent == NULL) { 
        AC_ERROR("lost TCPClient handle!");
        return;
    }
    if (nread < 0) {
        if (nread == UV_EOF) {
            AC_ERROR("Server close(EOF), Client %p ", stream);    
        } else if (nread == UV_ECONNRESET) {
            AC_ERROR("Server close(conn reset),Client %p ", stream);
        } else {
            AC_ERROR("Server close,Client %p ", stream);
        }
        if (!parent->startReconnect()) {
            AC_ERROR("Start Reconnect Failure.");
            return;
        }
        if (!uv_is_closing((uv_handle_t*)stream)) {
            uv_close((uv_handle_t*)stream, onClientCloseCallback);   
        }
        
        return;
    }
    if (parent->recvcb_) {
    	parent->recvcb_((const char*) buf->base, nread); 
    }
	
}

void TCPClient::setReceiveCallback(RecvCallback callback)
{
	recvcb_ = callback;
}

void TCPClient::setReconnectCallback(voidParamCallback callback)
{
	reconnectcb_ = callback;
}

void TCPClient::setHeartbeatCallback(voidParamCallback callback, bool enable, int time)
{
    heartbeatcb_ = callback;
    isheartbeat_ = enable;                     
    heartbeatTime_ = time;
}

bool TCPClient::startReconnect(void)
{
	isreconnecting_ = true;
    clientContext_->tcpHandle.data = this;
    return true;
}

void TCPClient::stopReconnect(void)
{   
   
	isreconnecting_ = false;
    clientContext_->tcpHandle.data = clientContext_;
    uv_timer_stop(&reconnectTimer_);
}

bool TCPClient::reconnect()
{
    if (!uv_is_closing((uv_handle_t*)&clientContext_->tcpHandle)) {
        uv_close((uv_handle_t*)&clientContext_->tcpHandle, NULL);
    }
    startReconnect();
    uv_timer_stop(&reconnectTimer_);
    int iret = uv_timer_start(&reconnectTimer_, reconnectTimer, repeatTime_, repeatTime_);
    if (iret) {
        uv_close((uv_handle_t*)&reconnectTimer_, onClientCloseCallback);
        return false;
    }
    return true;
}

bool TCPClient::run()
{
	int ret = uv_run(&loop_, UV_RUN_DEFAULT);
	if (ret) {
		AC_ERROR("there are still active handles or requests");
		return false;
	}
	return true;
} 

void TCPClient::onClientCloseCallback(uv_handle_t* handle)
{
	TCPClient* parent = (TCPClient*)handle->data;
    if (parent == NULL) { 
        AC_ERROR("lost TCPClient handle!");
        return;
    }
    if (handle == (uv_handle_t*)&parent->clientContext_->tcpHandle && parent->isreconnecting_) {
        int iret = 0;
        iret = uv_timer_start(&parent->reconnectTimer_, reconnectTimer, parent->repeatTime_, parent->repeatTime_);
        if (iret) {
            uv_close((uv_handle_t*)&parent->reconnectTimer_, onClientCloseCallback);
            return;
        }
    }
}

void TCPClient::reconnectTimer(uv_timer_t* handle)
{
	TCPClient* theclass = (TCPClient*)handle->data;
    if (theclass == NULL || !theclass->isreconnecting_) {
        return;
    }
    AC_INFO("start reconnect to server:%s, port:%d ", theclass->serverIp_.c_str(), theclass->serverPort_);
   
    int iret = uv_tcp_init(&theclass->loop_, &theclass->clientContext_->tcpHandle);
    if (iret) {
        return;   //出错跳出循环
    }
    theclass->clientContext_->tcpHandle.data = theclass->clientContext_;
    theclass->clientContext_->parent_server = theclass;
    struct sockaddr* pAddr;
            
    struct sockaddr_in bind_addr;
    iret = uv_ip4_addr(theclass->serverIp_.c_str(), theclass->serverPort_, &bind_addr);
    if (iret) {        
        uv_close((uv_handle_t*)&theclass->clientContext_->tcpHandle, NULL);
        return;
    }
    pAddr = (struct sockaddr*)&bind_addr;
        
    iret = uv_tcp_connect(&theclass->connectHandle_, &theclass->clientContext_->tcpHandle, (const sockaddr*)pAddr, onConnectCallback);
    if (iret) {
        uv_close((uv_handle_t*)&theclass->clientContext_->tcpHandle, NULL);
        return;
    }
}

void TCPClient::heartbeatTimer(uv_timer_t* handle)
{
    TCPClient* theclass = (TCPClient*)handle->data;
    if (theclass == NULL) { 
        AC_ERROR("lost TCPClient handle!");
        return;
    }
    if (!theclass->isheartbeat_) {
        return;
    }
    if (theclass->heartbeatcb_) {
        theclass->heartbeatcb_();      //执行心跳回调函数
    }
}

string TCPClient::getUVError(int errcode)
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