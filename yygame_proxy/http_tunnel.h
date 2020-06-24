#pragma once

#include "iocp_server.h"

class CHttpTunnel : public CIOCPServer
{
public:
	CHttpTunnel();
	void ProcessBuffer(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pBuffer, DWORD dwLen);

private:
	ULONG readHeader(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pBuffer, DWORD dwLen);
	int readData();
	void sendHttpResponse(LPPER_HANDLE_DATA pHandleData, const char* payload);
	bool extractHost(const char* header, DWORD dwSize, std::string& host, std::string& port);
	int getHttpProtocol(const char* header, DWORD dwSize);


	LPPER_HANDLE_DATA getTunnelHandle(const LPPER_HANDLE_DATA pKey);
	void putTunnelHandle(const LPPER_HANDLE_DATA pKey, const LPPER_HANDLE_DATA pData);
	DWORD createTunnelHandle(const SOCKADDR_IN* peerAddr, LPPER_HANDLE_DATA* pData);
	void destroyTunnelHandle(const LPPER_HANDLE_DATA pKey);
	void removeTunnelHandle(const LPPER_HANDLE_DATA pKey, bool bAll = true);

protected:
	bool onAcceptPosted(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData) override;


	bool onRecvPosted(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData, DWORD dwLen) override;


	bool onSendPosted(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData, DWORD dwLen) override;


	void onServerError(LPPER_HANDLE_DATA pHandleData, DWORD dwErr) override;


	void onDisconnected(LPPER_HANDLE_DATA pHandleData) override;

private:
	std::map<LPPER_HANDLE_DATA, LPPER_HANDLE_DATA> m_tunnelTable;
	std::map<std::string, ULONG> m_dnsCache; // domain ip
	std::recursive_mutex m_tunnelGuard;
};