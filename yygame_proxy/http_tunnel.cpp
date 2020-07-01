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

static constexpr int CONNECT_INDEX = 2; // CONNECT方法索引位置

static constexpr unsigned long PlanHttpProxy = 1;
static constexpr unsigned long TunnelHttpProxy = 2;

CHttpTunnel::CHttpTunnel()
{
}

bool CHttpTunnel::handleAcceptBuffer(LPSocketContext pSocketCtx, LPIOContext pIoCtx, DWORD dwLen,
	SOCKADDR_IN* peerAddr)
{
	//sendHttpResponse(pHandleData, "HTTP/1.1 500 Internal Server Error\r\n\r\n");

	int methodIndex = getHttpProtocol(pIoCtx->buffer, dwLen);
	if (methodIndex < 0)
	{
		if (!sendHttpResponse(pSocketCtx->GetServerToUserContext(), "HTTP/1.1 400 Bad Request\r\n\r\n"))
		{
			assert(0);
			CloseIoSocket(pSocketCtx->GetServerToUserContext());
		}
		return false;
	}
	auto dwHeaderLen = readHeader(pIoCtx, dwLen);
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

	std::string host;
	std::string port;
	if (!extractHost(pIoCtx->buffer, dwHeaderLen, host, port))
	{
		if (!sendHttpResponse(pSocketCtx->GetServerToUserContext(), "HTTP/1.1 422 Unprocessable Entity\r\n\r\n"))
		{
			assert(0);
			CloseIoSocket(pSocketCtx->GetServerToUserContext());
		}
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
		bool bFind = (pCursor != NULL);
		if (bFind)
		{
			assert(sizeof(SOCKADDR_IN) == pCursor->ai_addrlen);
			memcpy_s(peerAddr, sizeof(SOCKADDR_IN), pCursor->ai_addr, pCursor->ai_addrlen);
		}
		FreeAddrInfoA(pResult);

		if (methodIndex == CONNECT_INDEX)
		{
			pSocketCtx->SetCustomData(TunnelHttpProxy);
		}
		else
		{
			pSocketCtx->SetCustomData(PlanHttpProxy);
		}
		return bFind;
	}
	else
	{
		return false;
	}
}

ULONG CHttpTunnel::readHeader(LPIOContext pIoCtx, DWORD dwLen)
{
	const char* pBase = pIoCtx->buffer;
	auto pHeaderEnd = StrStrA(pBase, "\r\n\r\n");
	if (pHeaderEnd == NULL)
	{
		return INVALID_HEADER_LEN;
	}
	else
	{
		return pHeaderEnd - pBase + 4;
	}
}

bool CHttpTunnel::sendHttpResponse(LPIOContext pIoCtx, const char* payload)
{
	DWORD dwLen = strlen(payload);
	pIoCtx->ResetBuffer();
	pIoCtx->SetPayload(payload, dwLen);
	return pIoCtx->PostSend(dwLen);
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

bool CHttpTunnel::onAcceptPosted(LPSocketContext pSocketCtx, LPIOContext pIoCtx, DWORD dwLen,
	SOCKADDR_IN* peerAddr)
{
	return handleAcceptBuffer(pSocketCtx, pIoCtx, dwLen, peerAddr);
}

bool CHttpTunnel::onServerConnectPosted(LPSocketContext pSocketCtx, DWORD dwLen, bool success)
{
	if (pSocketCtx->GetCustomData() == TunnelHttpProxy)
	{
		return sendHttpResponse(pSocketCtx->GetServerToUserContext(), "HTTP/1.1 200 Connection Established\r\n\r\n")
			&& pSocketCtx->GetUserToServerContext()->PostRecv();
	}
	else
	{
		assert(pSocketCtx->GetCustomData() == PlanHttpProxy);
		return __super::onServerConnectPosted(pSocketCtx, dwLen, success);
	}
}

bool CHttpTunnel::onRecvPosted(LPSocketContext pSocketCtx, LPIOContext pIoCtx, DWORD dwLen)
{
	return __super::onRecvPosted(pSocketCtx, pIoCtx, dwLen);
}

bool CHttpTunnel::onSendPosted(LPSocketContext pSocketCtx, LPIOContext pIoCtx, DWORD dwLen)
{
	return __super::onSendPosted(pSocketCtx, pIoCtx, dwLen);
}

void CHttpTunnel::onServerError(LPIOContext pIoCtx, DWORD dwErr)
{
	return __super::onServerError(pIoCtx, dwErr);
}

void CHttpTunnel::onDisconnected(LPSocketContext pSocketCtx, LPIOContext pIoCtx)
{
	return __super::onDisconnected(pSocketCtx, pIoCtx);
}
