#include "session.h"
#include "context.h"
#include "helper.h"
#include <node_buffer.h>
#include <openssl/err.h>

NAN_MODULE_INIT(Session::Init) {
	v8::Local<v8::FunctionTemplate> tpl = Nan::New<v8::FunctionTemplate>(New);
	tpl->SetClassName(Nan::New("Session").ToLocalChecked());
	tpl->InstanceTemplate()->SetInternalFieldCount(1);

	Nan::SetPrototypeMethod(tpl, "handler", handler);
	Nan::SetPrototypeMethod(tpl, "close", close);

	constructor.Reset(Nan::GetFunction(tpl).ToLocalChecked());
	Nan::Set(target, Nan::New("Session").ToLocalChecked(), Nan::GetFunction(tpl).ToLocalChecked());
}

NAN_METHOD(Session::New) {
	ARG_CONTEXT(0, ctx);
	ARG_BUFFER(1, cookie);
	ARG_INT(2, mtu);
	ARG_FUN(3, cbSend);
	ARG_FUN(4, cbMessage);
	ARG_FUN(5, cbConnected);
	ARG_FUN(6, cbError);
	ARG_FUN(7, cbShutdown);

	Session * sess = new Session(ctx->handle, mtu, node::Buffer::Data(cookie), node::Buffer::Length(cookie),
	                             cbSend, cbMessage, cbConnected, cbError, cbShutdown);

	sess->Wrap(info.This());
	info.GetReturnValue().Set(info.This());
}

Session::Session(
	SSL_CTX * ctx,
	int64_t mtu,
	const char * cookie,
	size_t cookieLen,
	v8::Local<v8::Function> & cbSend,
	v8::Local<v8::Function> & cbMessage,
	v8::Local<v8::Function> & cbConnected,
	v8::Local<v8::Function> & cbError,
	v8::Local<v8::Function> & cbShutdown
) :
	handle(NULL),
	cookie(NULL),
	cookieLen(0),
	cbSend(NULL),
	cbMessage(NULL),
	cbConnected(NULL),
	cbError(NULL),
	cbShutdown(NULL)
{
	// Create a new SSL session
	this->handle = SSL_new(ctx);
	if (this->handle == NULL) goto error;

	// Store cookie
	this->cookie = (char*) malloc(cookieLen);
	memcpy(this->cookie, cookie, cookieLen);
	this->cookieLen = cookieLen;

	// Set MTU
	SSL_set_options(this->handle, SSL_OP_NO_QUERY_MTU);
	DTLS_set_link_mtu(this->handle, mtu);

	// Store callbacks
	this->cbSend = new Nan::Callback(cbSend);
	this->cbMessage = new Nan::Callback(cbMessage);
	this->cbConnected = new Nan::Callback(cbConnected);
	this->cbError = new Nan::Callback(cbError);
	this->cbShutdown = new Nan::Callback(cbShutdown);

	return;

error:
	throwGlobalSSLError();
}

Session::~Session() {
	// Remove handle
	if (this->handle) SSL_free(this->handle);
	if (this->cookie) free(this->cookie);
	if (this->cbSend) delete this->cbSend;
	if (this->cbMessage) delete this->cbMessage;
	if (this->cbConnected) delete this->cbConnected;
	if (this->cbError) delete this->cbError;
	if (this->cbShutdown) delete this->cbShutdown;
}

void Session::sendData() {
	// Check whether data is waiting for be sent
	BIO * wbio = SSL_get_wbio(this->handle);
	if (BIO_ctrl_pending(wbio) == 0) return;

	// Read output data
	char * packet = (char*) malloc(4096);
	int n = BIO_read(wbio, packet, 4096);

	// Call send callback
	Nan::MaybeLocal<v8::Object> packetLocalMaybe = Nan::NewBuffer(packet, n);
	v8::Local<v8::Value> packetLocal = packetLocalMaybe.ToLocalChecked();
	Nan::Call(this->cbSend->GetFunction(), Nan::GetCurrentContext()->Global(), 1, &packetLocal);

	// Check if the connection has been shut down and call callback
	if ((SSL_get_shutdown(this->handle) & SSL_SENT_SHUTDOWN) || (SSL_get_state(this->handle) == SSL_ST_ERR)) {
		Nan::Call(this->cbShutdown->GetFunction(), Nan::GetCurrentContext()->Global(), 0, NULL);
	}
}

NAN_METHOD(Session::handler) {
	Session * sess = Nan::ObjectWrap::Unwrap<Session>(info.Holder());
	int rc;
	uint32_t ret = 0;

	// Write data to BIO if a datagram has been received
	Nan::MaybeLocal<v8::Object> dataBufMaybe = Nan::To<v8::Object>(info[0]);
	if (!dataBufMaybe.IsEmpty()) {
		v8::Local<v8::Object> dataBuf = dataBufMaybe.ToLocalChecked();
		bufferToBio(SSL_get_rbio(sess->handle), dataBuf);
	}

	// Depending on the candidate's state we call the right function
	if (!SSL_is_init_finished(sess->handle)) {
		rc = SSL_do_handshake(sess->handle);
		if (rc == 1) {
			// Connection has been established
			Nan::Call(sess->cbConnected->GetFunction(), Nan::GetCurrentContext()->Global(), 0, NULL);
		} else if (SSL_get_error(sess->handle, rc) == SSL_ERROR_SSL) {
			// An error occured
			char errStr[256];
			ERR_error_string_n(ERR_get_error(), errStr, sizeof(errStr));
			Nan::MaybeLocal<v8::String> errLocalMaybe = Nan::New<v8::String>(errStr);
			v8::Local<v8::Value> errLocal = errLocalMaybe.ToLocalChecked();
			Nan::Call(sess->cbError->GetFunction(), Nan::GetCurrentContext()->Global(), 1, &errLocal);
		} else {
			// Handshake isn't finished, yet
			struct timeval dtlsTimeout = {0, 0};
			DTLSv1_get_timeout(sess->handle, (void*) &dtlsTimeout);
			ret = dtlsTimeout.tv_sec * 1000 + dtlsTimeout.tv_usec / 1000;
		}
	} else {
		char buffer[4096];
		int rc = SSL_read(sess->handle, buffer, sizeof(buffer));
		if (rc <= 0 && SSL_get_error(sess->handle, rc) == SSL_ERROR_ZERO_RETURN) {
			// Disconnected!
			SSL_shutdown(sess->handle);
		} else if (rc > 0) {
			// Data received from remote side
			char * data = (char*) malloc(rc);
			memcpy(data, buffer, rc);
			Nan::MaybeLocal<v8::Object> dataLocalMaybe = Nan::NewBuffer(data, rc);
			v8::Local<v8::Value> dataLocal = dataLocalMaybe.ToLocalChecked();
			Nan::Call(sess->cbMessage->GetFunction(), Nan::GetCurrentContext()->Global(), 1, &dataLocal);
		}
	}

	// Send data that might be pending
	sess->sendData();

	// Return amount of millisconds of the retransmit timer
	info.GetReturnValue().Set(Nan::New(ret));
}

NAN_METHOD(Session::close) {
	Session * sess = Nan::ObjectWrap::Unwrap<Session>(info.Holder());
	SSL_shutdown(sess->handle);
	sess->sendData();
}