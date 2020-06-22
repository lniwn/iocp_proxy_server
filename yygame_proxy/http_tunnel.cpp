#include "framework.h"
#include "http_tunnel.h"

CHttpTunnel::CHttpTunnel()
{
}

void CHttpTunnel::ProcessBuffer(_PER_IO_DATA* pBuffer)
{

}

int CHttpTunnel::readHeader()
{
	return 0;
}

int CHttpTunnel::readData()
{
	return 0;
}

LPPER_HANDLE_DATA CHttpTunnel::getTunnelHandle(const LPPER_HANDLE_DATA pKey)
{
	std::lock_guard<decltype(m_tunnelGuard)> _(m_tunnelGuard);
	auto itTarget = m_tunnelTable.find(pKey);
	if (itTarget == m_tunnelTable.end())
	{
		return NULL;
	}
	else
	{
		return itTarget->second;
	}
}

void CHttpTunnel::putTunnelHandle(const LPPER_HANDLE_DATA pKey, const LPPER_HANDLE_DATA pData)
{
	assert(pKey != NULL);
	assert(pData != NULL);
	std::lock_guard<decltype(m_tunnelGuard)> _(m_tunnelGuard);
	if (pKey != NULL)
	{
		m_tunnelTable[pKey] = pData;
	}
	if (pData != NULL)
	{
		m_tunnelTable[pData] = pKey;
	}
}

DWORD CHttpTunnel::createTunnelHandle(const SOCKADDR_IN* peerAddr, LPPER_HANDLE_DATA* pData)
{
	SOCKET hSocket = ::WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (hSocket == INVALID_SOCKET)
	{
		return ::WSAGetLastError();
	}

	if (0 != ::WSAConnect(hSocket, reinterpret_cast<const sockaddr*>(peerAddr), sizeof(SOCKADDR_IN),
		NULL, NULL, NULL, NULL))
	{
		::closesocket(hSocket);
		return ::WSAGetLastError();
	}

	*pData = PER_HANDLE_DATA::Create(hSocket, reinterpret_cast<const SOCKADDR_STORAGE*>(peerAddr),
		sizeof(SOCKADDR_IN));

	this->AssociateWithServer(reinterpret_cast<HANDLE>(hSocket), reinterpret_cast<ULONG_PTR>(*pData), 0);

	return ERROR_SUCCESS;
}

void CHttpTunnel::destroyTunnelHandle(const LPPER_HANDLE_DATA pKey)
{
	removeTunnelHandle(pKey);
	delete pKey;
}

void CHttpTunnel::removeTunnelHandle(const LPPER_HANDLE_DATA pKey, bool bAll)
{
	std::lock_guard<decltype(m_tunnelGuard)> _(m_tunnelGuard);
	if (bAll)
	{
		auto pData = getTunnelHandle(pKey);
		if (pData != NULL)
		{
			m_tunnelTable.erase(pData);
		}
	}

	m_tunnelTable.erase(pKey);
}

bool CHttpTunnel::onAcceptPosted(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData)
{
	return __super::onAcceptPosted(pHandleData, pIoData);
}

bool CHttpTunnel::onRecvPosted(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData, DWORD dwLen)
{
	return __super::onRecvPosted(pHandleData, pIoData, dwLen);
}

bool CHttpTunnel::onSendPosted(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData, DWORD dwLen)
{
	return __super::onSendPosted(pHandleData, pIoData, dwLen);
}

void CHttpTunnel::onServerError(LPPER_HANDLE_DATA pHandleData, DWORD dwErr)
{
	return __super::onServerError(pHandleData, dwErr);
}

void CHttpTunnel::onDisconnected(LPPER_HANDLE_DATA pHandleData)
{
	return __super::onDisconnected(pHandleData);
}
