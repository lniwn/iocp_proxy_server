#pragma once

#include "iocp_server.h"

class CHttpTunnel : public CIOCPServer
{
public:
	CHttpTunnel();

private:
	bool handleAcceptBuffer(LPSocketContext pSocketCtx, LPIOContext pIoCtx, DWORD dwLen,
		SOCKADDR_IN* peerAddr);
	ULONG readHeader(LPIOContext pIoCtx, DWORD dwLen);
	bool sendHttpResponse(LPIOContext pIoCtx, const char* payload);
	bool extractHost(const char* header, DWORD dwSize, std::string& host, std::string& port);
	int getHttpProtocol(const char* header, DWORD dwSize);

protected:
	virtual bool onAcceptPosted(LPSocketContext pSocketCtx, LPIOContext pIoCtx, DWORD dwLen,
		SOCKADDR_IN* peerAddr) override;
	virtual bool onServerConnectPosted(LPSocketContext pSocketCtx, DWORD dwLen, bool success) override;
	virtual bool onRecvPosted(LPSocketContext pSocketCtx, LPIOContext pIoCtx, DWORD dwLen) override;
	virtual bool onSendPosted(LPSocketContext pSocketCtx, LPIOContext pIoCtx, DWORD dwLen) override;
	virtual void onServerError(LPIOContext pIoCtx, DWORD dwErr) override;
	virtual void onDisconnected(LPSocketContext pSocketCtx, LPIOContext pIoCtx) override;

private:
	std::map<std::string, ULONG> m_dnsCache; // domain ip
};