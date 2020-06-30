#include "framework.h"
#include <regex>
#include <shlwapi.h>
#include <ws2tcpip.h>
#include "http_tunnel.h"

#pragma comment(lib, "Shlwapi.lib")

static constexpr ULONG INVALID_HEADER_LEN = ~1UL;

static constexpr const char* HTTP_METHODS[] = {
		"GET",
		"POST",
		"CONNECT",
		"OPTIONS",
		"HEAD",
		"PUT",
		"DELETE",
		"TRACE",
		"PATCH",
};

enum SocketState : unsigned long
{
	ClientSocketTunnel = 1, // 客户端HTTP隧道
	ClientSocketHttp = 2, // 客户端普通HTTP协议
	ServerSocket = 100, // 远程服务端
};

static constexpr int CONNECT_INDEX = 2; // CONNECT方法索引位置

CHttpTunnel::CHttpTunnel()
{
}

bool CHttpTunnel::handleRecvBuffer(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pBuffer, DWORD dwLen)
{
	auto pPeerHandle = getTunnelHandle(pHandleData);
	if (pPeerHandle != NULL)
	{
		auto pSendBuffer = pPeerHandle->sendBuf;
		pSendBuffer->SetPayload(pBuffer->buffer, dwLen);
		if (!PostSend(pPeerHandle, pSendBuffer))
		{
			assert(0);
			return false;
		}
	}
	else
	{
		sendHttpResponse(pHandleData, "HTTP/1.1 504 Gateway Timeout\r\n\r\n");
		return false;
	}
	return true;
}

bool CHttpTunnel::handleAcceptBuffer(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pBuffer, DWORD dwLen)
{
	const char* pBase = pBuffer->buffer;
	if (pBase != pBuffer->wsaBuffer.buf)
	{
		assert(pBuffer->wsaBuffer.buf > pBase);
		dwLen += (pBuffer->wsaBuffer.buf - pBase);
		assert(dwLen <= HTTPPROXY_BUFFER_LENGTH);
		if (dwLen > HTTPPROXY_BUFFER_LENGTH)
		{
			pBuffer->Reset(IO_OPT_TYPE::RECV_POSTED);
			sendHttpResponse(pHandleData, "HTTP/1.1 500 Internal Server Error\r\n\r\n");
			return false;
		}
	}

	int methodIndex = getHttpProtocol(pBuffer->buffer, dwLen);
	if (methodIndex < 0)
	{
		pBuffer->Reset(IO_OPT_TYPE::RECV_POSTED);
		sendHttpResponse(pHandleData, "HTTP/1.1 400 Bad Request\r\n\r\n");

		return false;
	}
	auto dwHeaderLen = readHeader(pHandleData, pBuffer, dwLen);
	if (dwHeaderLen == INVALID_HEADER_LEN)
	{
		dwHeaderLen = dwLen;
		//if (dwLen == HTTPPROXY_BUFFER_LENGTH)
		//{
		//	// header is too large
		//	pBuffer->Reset(IO_OPT_TYPE::RECV_POSTED);
		//	sendHttpResponse(pHandleData, "HTTP/1.1 431 Request Header Fields Too Large\r\n\r\n");
		//	return false;
		//}
		//else
		//{
		//	pBuffer->wsaBuffer.buf = &pBuffer->buffer[dwLen];
		//	pBuffer->wsaBuffer.len = HTTPPROXY_BUFFER_LENGTH - dwLen;
		//	return !PostRecv(pHandleData, pBuffer);
		//}
	}

	if (methodIndex == CONNECT_INDEX)
	{
		pHandleData->uUser = SocketState::ClientSocketTunnel;
	}
	else
	{
		pHandleData->uUser = SocketState::ClientSocketHttp;
	}

	std::string host;
	std::string port;
	if (!extractHost(pBuffer->buffer, dwHeaderLen, host, port))
	{
		pBuffer->Reset(IO_OPT_TYPE::RECV_POSTED);
		sendHttpResponse(pHandleData, "HTTP/1.1 422 Unprocessable Entity\r\n\r\n");
		return false;
	}

	addrinfo hints = { 0 };
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	addrinfo* pResult = NULL;
	if (GetAddrInfoA(host.c_str(), port.c_str(), &hints, &pResult) == 0)
	{
		addrinfo* pCursor = NULL;
		for (pCursor = pResult; pCursor != NULL; pCursor = pCursor->ai_next)
		{
			if (pCursor->ai_family == AF_INET)
			{
				break;
			}
		}
		if (pCursor != NULL)
		{
			LPPER_HANDLE_DATA pServerHandle = NULL;
			if (0 == createTunnelHandle(reinterpret_cast<const SOCKADDR_IN*>(pCursor->ai_addr), &pServerHandle)
				&& pServerHandle != NULL)
			{
				putTunnelHandle(pHandleData, pServerHandle);
				pServerHandle->recvBuf->Reset(IO_OPT_TYPE::RECV_POSTED);
				if (!PostRecv(pServerHandle, pServerHandle->recvBuf))
				{
					destroyTunnelHandle(pServerHandle);
					return false;
				}
			}
		}
		FreeAddrInfoA(pResult);

		if (methodIndex == CONNECT_INDEX)
		{
			sendHttpResponse(pHandleData, "HTTP/1.1 200 Connection Established\r\n\r\n");
			return false;
		}
	}
	return true;
}

ULONG CHttpTunnel::readHeader(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pBuffer, DWORD dwLen)
{
	const char* pBase = pBuffer->buffer;
	auto pHeaderEnd = StrStrA(pBase, "\r\n\r\n");
	if (pHeaderEnd == NULL)
	{
		return INVALID_HEADER_LEN;
	}
	else
	{
		return pHeaderEnd - pBase + 4;
	}
	return INVALID_HEADER_LEN;
}

int CHttpTunnel::readData()
{
	return 0;
}

void CHttpTunnel::sendHttpResponse(LPPER_HANDLE_DATA pHandleData, const char* payload)
{
	auto pBuffer = pHandleData->sendBuf;
	pBuffer->SetPayload(payload, strlen(payload));
	if (!PostSend(pHandleData, pBuffer))
	{
		destroyTunnelHandle(pHandleData);
		assert(0);
	}
}

bool CHttpTunnel::extractHost(const char* header, DWORD dwSize, std::string& host, std::string& port)
{
	constexpr char connectMethod[] = { "CONNECT" };
	constexpr DWORD connectLength = sizeof(connectMethod) - 1;

	if (dwSize <= connectLength)
	{
		return false;
	}

	const char* pBase = NULL;
	const char* pLast = header + dwSize;
	if (_strnicmp(header, connectMethod, connectLength) == 0)
	{
		// http隧道
		pBase = header + connectLength;
	}
	else
	{
		// 普通http头
		constexpr auto HostHeader = "\r\nHost:";
		pBase = StrStrIA(header, HostHeader);
		if (pBase == NULL)
		{
			return false;
		}
		pBase += strlen(HostHeader);
	}
	while (pBase < pLast && *pBase == ' ')
	{
		++pBase;
	}
	const char* pColon = NULL;
	const char* pHostEnd = NULL;
	for (auto pIndex = pBase; pIndex < pLast; pIndex++)
	{
		if (std::isspace(*pIndex))
		{
			pHostEnd = pIndex;
			break;
		}
		if (*pIndex == ':')
		{
			pColon = pIndex;
		}
	}
	if (pHostEnd == NULL)
	{
		return false;
	}
	if (pColon == NULL)
	{
		host = std::move(std::string(pBase, pHostEnd));
		port = std::move(std::string("80"));
	}
	else
	{
		host = std::move(std::string(pBase, pColon));
		port = std::move(std::string(pColon + 1, pHostEnd));
	}

	return true;
}

int CHttpTunnel::getHttpProtocol(const char* header, DWORD dwSize)
{
	constexpr DWORD minHttpProtocolLen = 7;
	if (dwSize <= minHttpProtocolLen)
	{
		return -1;
	}
	for (int index = 0; index < _countof(HTTP_METHODS); ++index)
	{
		if (_strnicmp(header, HTTP_METHODS[index], strlen(HTTP_METHODS[index])) == 0)
		{
			return index;
		}
	}
	return -1;
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
		assert(0);
		return ::WSAGetLastError();
	}

	if (0 != ::WSAConnect(hSocket, reinterpret_cast<const sockaddr*>(peerAddr), sizeof(SOCKADDR_IN),
		NULL, NULL, NULL, NULL))
	{
		::closesocket(hSocket);
		return ::WSAGetLastError();
	}

	*pData = PER_HANDLE_DATA::Create(hSocket, reinterpret_cast<const SOCKADDR_STORAGE*>(peerAddr),
		sizeof(SOCKADDR_IN), SocketState::ServerSocket);

	if (NULL == this->AssociateWithServer(reinterpret_cast<HANDLE>(hSocket), reinterpret_cast<ULONG_PTR>(*pData), 0))
	{
		assert(0);
		delete (*pData);
		*pData = NULL;
		return ::WSAGetLastError();
	}

	return ERROR_SUCCESS;
}

void CHttpTunnel::destroyTunnelHandle(LPPER_HANDLE_DATA pKey)
{
	assert(pKey != NULL);
	LPPER_HANDLE_DATA pValue = NULL;
	{
		std::lock_guard<decltype(m_tunnelGuard)> _(m_tunnelGuard);
		pValue = getTunnelHandle(pKey);
		m_tunnelTable.erase(pKey);
		if (pValue != NULL)
		{
			m_tunnelTable.erase(pValue);
		}
	}

	if (pValue != NULL)
	{
		pValue->Close();
	}

	pKey->Close();
}

void CHttpTunnel::removeTunnelHandle(const LPPER_HANDLE_DATA pKey)
{
	std::lock_guard<decltype(m_tunnelGuard)> _(m_tunnelGuard);

	auto pData = getTunnelHandle(pKey);
	if (pData != NULL)
	{
		m_tunnelTable.erase(pData);
	}

	m_tunnelTable.erase(pKey);
}

bool CHttpTunnel::onAcceptPosted(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData, DWORD dwLen)
{
	if (handleAcceptBuffer(pHandleData, pIoData, dwLen))
	{
		return onRecvPosted(pHandleData, pIoData, dwLen);
	}
	else
	{
		return true;
	}
}

bool CHttpTunnel::onRecvPosted(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData, DWORD dwLen)
{
	if (!handleRecvBuffer(pHandleData, pIoData, dwLen))
	{
		return false;
	}
	auto pTarget = getTunnelHandle(pHandleData);
	if (pTarget != NULL)
	{
		auto pSendBuffer = pTarget->sendBuf;
		pSendBuffer->SetPayload(pIoData->wsaBuffer.buf, pIoData->wsaBuffer.len);
		return PostSend(pTarget, pSendBuffer);
	}
	else
	{
		destroyTunnelHandle(pHandleData);
		return false;
	}
	return false;
}

bool CHttpTunnel::onSendPosted(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData, DWORD dwLen)
{
	if (dwLen < pIoData->wsaBuffer.len)
	{
		pIoData->wsaBuffer.buf = pIoData->wsaBuffer.buf + dwLen;
		pIoData->wsaBuffer.len = pIoData->wsaBuffer.len - dwLen;
		return PostSend(pHandleData, pIoData);
	}
	else
	{
		auto pPeer = getTunnelHandle(pHandleData);
		if (pPeer != NULL)
		{
			auto pRecvBuffer = pPeer->recvBuf;
			return PostRecv(pPeer, pRecvBuffer);
		}
		else
		{
			destroyTunnelHandle(pHandleData);
			return false;
		}
	}
}

void CHttpTunnel::onServerError(LPPER_HANDLE_DATA pHandleData, DWORD dwErr)
{
	return __super::onServerError(pHandleData, dwErr);
}

void CHttpTunnel::onDisconnected(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData)
{
	destroyTunnelHandle(pHandleData);
	return __super::onDisconnected(pHandleData, pIoData);
}
