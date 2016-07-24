#include "TcpClient.h"
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
    ctx->parent_server = parentserver;	//store TcpClient
    return ctx;
}

void freeClientCtx(ClientContext* ctx)
{
    free(ctx->recvBuf.base);
    free(ctx);
    ctx = NULL;
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

TcpClient::TcpClient(int reconnectTimeout, int maxReceivePackageSize, int maxSendPackageSize)
:client_context_(NULL), receive_cb_(NULL),reconnect_cb_(NULL),is_reconnecting_(true),repeat_time_(reconnectTimeout),
max_receive_package_size_(maxReceivePackageSize),max_send_package_size_(maxSendPackageSize),is_heartbeat_(false)
{
	client_context_ = allocClientCtx(max_receive_package_size_,this);
}

TcpClient::~TcpClient()
{
    uv_mutex_destroy(&mutexWrite_);
    freeClientCtx(client_context_);
    for (std::list<WriteReq*>::iterator it = write_request_list_.begin(); it != write_request_list_.end(); ++it) {
        freeWriteReqParam(*it);
    }
    write_request_list_.clear();
    uv_loop_close(loop_);
   
}

void TcpClient::close()
{
    uv_timer_stop(&heartbeat_timer_);
    stopReconnect();
    uv_stop(loop_);
    uv_walk(loop_, closeWalkCallback, this);  
}

void TcpClient::join()
{
    uv_thread_join(&run_thread_handle_);
}

void TcpClient::closeWalkCallback(uv_handle_t* handle, void* arg)  //回调多次
{
    //TcpClient* pclient = (TcpClient*)arg;
    if (uv_is_active(handle)) {      
        uv_close(handle, NULL);
        if (handle->type == UV_ASYNC) {         
            uv_async_send((uv_async_t*)handle);
        }    
    }
}

bool TcpClient::init()
{
	int ret = uv_loop_init(loop_);
    if (ret) {
        AC_ERROR("uv_loop_init error,%s", getUVError(ret).c_str());
        return false;
    }

    ret = uv_mutex_init(&mutex_write_);
    if (ret) {
         AC_ERROR("uv_mutex_init error,%s", getUVError(ret).c_str());
         return false;
    }

	ret = uv_async_init(loop_, &async_handle_, asyncCallback);
	if (ret) {
		 AC_ERROR("uv_async_init error,%s", getUVError(ret).c_str());
		return false;
	}
	async_handle_.data = this;
   
	ret = uv_tcp_init(loop_, &client_context_->tcpHandle);
	if (ret) {
		AC_ERROR("uv_tcp_init error,%s", getUVError(ret).c_str());
		return false;
	}
   
	ret = uv_timer_init(loop_, &reconnect_timer_);
	if (ret) {
		AC_ERROR("uv_timer_init error,%s", getUVError(ret).c_str());
		return false;
	}
	reconnect_timer_.data = this;

    ret = uv_timer_init(loop_, &heartbeat_timer_);
    if (ret) {
        AC_ERROR("uv_timer_init error,%s", getUVError(ret).c_str());
        return false;
    }
    heartbeat_timer_.data = this;

	client_context_->tcpHandle.data = client_context_;
    
	return true;
}

void TcpClient::asyncCallback(uv_async_t* handle)
{
	TcpClient* self = (TcpClient*)handle->data;
    if (self == NULL) { 
        AC_ERROR("lost TcpClient handle!");
        return;
    }
	self->sendToServer();                      //回调后发送数据
}

int TcpClient::send(char* data, int len)
{
	if (!data || len < 0) {
		return -1;
	}
	uv_async_send(&async_handle_);              //产生回调, 查看之前的数据发送完没有，没完就发送						
	WriteReq *writereq = allocWriteReqParam(max_send_package_size_);
    writereq->req.data = this;
    memcpy(writereq->buf.base, data, len);
    writereq->buf.len = len;
   	write_request_list_.push_back(writereq);
    uv_async_send(&async_handle_);
	return 0;
}

int TcpClient::sendToServer()
{
    WriteReq* writereq = NULL;
    while (!write_request_list_.empty()) {
        uv_mutex_lock(&mutexWrite_);
        writereq = write_request_list_.front();
        write_request_list_.pop_front();
        uv_mutex_unlock(&mutexWrite_);

        uv_write((uv_write_t*)&writereq->req, (uv_stream_t*)&client_context_->tcpHandle, &writereq->buf, 1, onWriteCallback);

   }
    return 0;
}

void TcpClient::onWriteCallback(uv_write_t* req, int status)
{
    static int error_count = 0;
    TcpClient* self = (TcpClient*)req->data;
    if (self == NULL) { 
        AC_ERROR("lost TcpClient handle!");
        return;
    }
    WriteReq *writereq = (WriteReq*)req;
    if (status < 0) {
        error_count++;
        if (error_count > 5) {   //连续5次都发送失败,可能服务器异常, 尝试断开重连
             for (std::list<WriteReq*>::iterator it = self->write_request_list_.begin(); it != self->write_request_list_.end(); ++it) {
                freeWriteReqParam(*it);
            }
            self->write_request_list_.clear();
            error_count = 0;
            self->reconnect();
            return;
        }
        self->write_request_list_.push_back(writereq); //发送错误,把数据再放回去
        AC_ERROR("errCount:%d send data error, errNo:%d ", error_count, self->getUVError(status).c_str());
        return;
    } else { 
        error_count = 0;
        if (req) {
            free(writereq);
        }       
    }
}

bool TcpClient::connect(const char* ip, int port)
{
	if (!init()) {
        AC_ERROR("TcpClient init error!");
        return false;
    }
	server_ip_ = ip;
	server_port_ = port;

	struct sockaddr_in bindAddr;
	uv_ip4_addr(ip, port, &bindAddr);

	int r = uv_tcp_connect(&connect_handle_, &client_context_->tcpHandle, (struct sockaddr*) &bindAddr, onConnectCallback);
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

void TcpClient::loopRunThread(void* arg)
{
	TcpClient* pclient = (TcpClient*)arg;
    pclient->run();
}

void TcpClient::onConnectCallback(uv_connect_t* connHandle, int status)
{
	ClientContext* self = (ClientContext*)connHandle->handle->data;
	TcpClient* parent = (TcpClient*)self->parent_server;
    if (parent == NULL) { 
        AC_ERROR("lost TcpClient handle!");
        return;
    }
	if (status) {      
        AC_ERROR("connect error,%s", parent->getUVError(status).c_str());    
        if (parent->isreconnecting_) {      
            uv_timer_stop(&parent->reconnect_timer_);
            uv_timer_start(&parent->reconnect_timer_, TcpClient::reconnectTimer, parent->repeat_time_, parent->repeat_time_);
        }
		return;
	} 
	AC_INFO("connect to srever:%s, port:%d", parent->server_ip_.c_str(), parent->server_port_);
	int r = uv_read_start(connHandle->handle, allocBufForRecvCallback, onReadCallback);
	if (r) {
		AC_ERROR("uv_read_start error:%d", r);
	}

    if (parent->isreconnecting_) {
       //AC_INFO("reconnect to srever:%s, port:%d\n", parent->server_ip_.c_str(), parent->server_port_);
        parent->stopReconnect();        //reconnect succeed.
        if (parent->reconnect_cb_) {
            parent->reconnect_cb_();      //重连成功执行用户回调函数
        }
    }

    if(parent->is_heartbeat_) {
        uv_timer_stop(&parent->heartbeat_timer_);
        uv_timer_start(&parent->heartbeat_timer_, TcpClient::heartbeatTimer, 1e4, parent->heartbeat_time_);
    }

}


void TcpClient::allocBufForRecvCallback(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf)
{
	ClientContext* self = (ClientContext*)handle->data;
    assert(self);
    *buf = self->recvBuf;
}

void TcpClient::onReadCallback(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf)
{
	ClientContext* self = (ClientContext*)stream->data;
    assert(self);
	TcpClient* parent = (TcpClient*)self->parent_server;
    if (parent == NULL) { 
        AC_ERROR("lost TcpClient handle!");
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
    if (parent->receive_cb_) {
    	parent->receive_cb_((const char*) buf->base, nread); 
    }
	
}

void TcpClient::setReceiveCallback(RecvCallback callback)
{
	receive_cb_ = callback;
}

void TcpClient::setReconnectCallback(voidParamCallback callback)
{
	reconnect_cb_ = callback;
}

void TcpClient::setHeartbeatCallback(voidParamCallback callback, bool enable, int time)
{
    heartbeat_cb_ = callback;
    is_heartbeat_ = enable;                     
    heartbeat_time_ = time;
}

bool TcpClient::startReconnect(void)
{
	isreconnecting_ = true;
    client_context_->tcpHandle.data = this;
    return true;
}

void TcpClient::stopReconnect(void)
{   
   
	isreconnecting_ = false;
    client_context_->tcpHandle.data = client_context_;
    uv_timer_stop(&reconnect_timer_);
}

bool TcpClient::reconnect()
{
    if (!uv_is_closing((uv_handle_t*)&client_context_->tcpHandle)) {
        uv_close((uv_handle_t*)&client_context_->tcpHandle, NULL);
    }
    startReconnect();
    uv_timer_stop(&reconnect_timer_);
    int iret = uv_timer_start(&reconnect_timer_, reconnectTimer, repeat_time_, repeat_time_);
    if (iret) {
        uv_close((uv_handle_t*)&reconnect_timer_, onClientCloseCallback);
        return false;
    }
    return true;
}

bool TcpClient::run()
{
	int ret = uv_run(loop_, UV_RUN_DEFAULT);
	if (ret) {
		AC_ERROR("there are still active handles or requests");
		return false;
	}
	return true;
} 

void TcpClient::onClientCloseCallback(uv_handle_t* handle)
{
	TcpClient* parent = (TcpClient*)handle->data;
    if (parent == NULL) { 
        AC_ERROR("lost TcpClient handle!");
        return;
    }
    if (handle == (uv_handle_t*)&parent->client_context_->tcpHandle && parent->isreconnecting_) {
        int iret = 0;
        iret = uv_timer_start(&parent->reconnect_timer_, reconnectTimer, parent->repeat_time_, parent->repeat_time_);
        if (iret) {
            uv_close((uv_handle_t*)&parent->reconnect_timer_, onClientCloseCallback);
            return;
        }
    }
}

void TcpClient::reconnectTimer(uv_timer_t* handle)
{
	TcpClient* theclass = (TcpClient*)handle->data;
    if (theclass == NULL || !theclass->isreconnecting_) {
        return;
    }
    AC_INFO("start reconnect to server:%s, port:%d ", theclass->server_ip_.c_str(), theclass->server_port_);
   
    int iret = uv_tcp_init(&theclass->loop_, &theclass->client_context_->tcpHandle);
    if (iret) {
        return;   //出错跳出循环
    }
    theclass->client_context_->tcpHandle.data = theclass->client_context_;
    theclass->client_context_->parent_server = theclass;
    struct sockaddr* pAddr;
            
    struct sockaddr_in bind_addr;
    iret = uv_ip4_addr(theclass->server_ip_.c_str(), theclass->server_port_, &bind_addr);
    if (iret) {        
        uv_close((uv_handle_t*)&theclass->client_context_->tcpHandle, NULL);
        return;
    }
    pAddr = (struct sockaddr*)&bind_addr;
        
    iret = uv_tcp_connect(&theclass->connectHandle_, &theclass->client_context_->tcpHandle, (const sockaddr*)pAddr, onConnectCallback);
    if (iret) {
        uv_close((uv_handle_t*)&theclass->client_context_->tcpHandle, NULL);
        return;
    }
}

void TcpClient::heartbeatTimer(uv_timer_t* handle)
{
    TcpClient* theclass = (TcpClient*)handle->data;
    if (theclass == NULL) { 
        AC_ERROR("lost TcpClient handle!");
        return;
    }
    if (!theclass->is_heartbeat_) {
        return;
    }
    if (theclass->heartbeat_cb_) {
        theclass->heartbeat_cb_();      //执行心跳回调函数
    }
}

string TcpClient::getUVError(int errcode)
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