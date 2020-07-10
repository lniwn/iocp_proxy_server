#pragma once

#include <chrono>
#include "iocp_server.h"
#include "lru_cache.h"

class CHttpTunnel : public CIOCPServer
{
public:
	struct DnsCache
	{
		ULONG ip = { 0 }; // 网络序
		std::chrono::system_clock::time_point expire; // 根据ttl算出的UTC过期时间绝对值

		bool IsExpired();
	};

	CHttpTunnel();

private:
	bool handleAcceptBuffer(LPSocketContext pSocketCtx, LPIOContext pIoCtx, DWORD dwLen,
		SOCKADDR_IN* peerAddr);
	ULONG readHeader(LPIOContext pIoCtx, DWORD dwLen);
	DWORD rewriteHeader(LPIOContext pIoCtx, DWORD dwLen);
	bool sendHttpResponse(LPIOContext pIoCtx, const char* payload);
	bool extractHost(const char* header, DWORD dwSize, std::string& host, std::string& port);
	int getHttpProtocol(const char* header, DWORD dwSize);
	bool getIpByHost(const char* host, ULONG* ip);
	DnsCache createDnsCache(ULONG ip, DWORD ttl);

protected:
	virtual bool onAcceptPosted(LPSocketContext pSocketCtx, LPIOContext pIoCtx, DWORD dwLen,
		SOCKADDR_IN* peerAddr) override;
	virtual bool onServerConnectPosted(LPSocketContext pSocketCtx, DWORD dwLen, bool success) override;
	virtual bool onRecvPosted(LPSocketContext pSocketCtx, LPIOContext pIoCtx, DWORD dwLen) override;
	virtual bool onSendPosted(LPSocketContext pSocketCtx, LPIOContext pIoCtx, DWORD dwLen) override;
	virtual void onServerError(LPIOContext pIoCtx, DWORD dwErr) override;
	virtual void onDisconnected(LPSocketContext pSocketCtx, LPIOContext pIoCtx) override;

private:
	LRUCache<std::string, DnsCache> m_dnsCache; // domain ip
};