#include "framework.h"
#include <regex>
#include <shlwapi.h>
#include <ws2tcpip.h>
#include <WinDNS.h>
#include "http_tunnel.h"

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Dnsapi.lib")

static constexpr ULONG INVALID_HEADER_LEN = ~1UL;
static constexpr DWORD DEFAULT_DNS_TTL = 8600; // 秒

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
	:m_dnsCache(1000)
{
}

bool CHttpTunnel::handleAcceptBuffer(LPSocketContext pSocketCtx, LPIOContext pIoCtx, DWORD dwLen,
	SOCKADDR_IN* peerAddr)
{
	int methodIndex = getHttpProtocol(pIoCtx->buffer, dwLen);
	if (methodIndex < 0)
	{
		if (!sendHttpResponse(pSocketCtx->GetServerToUserContext(), "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n"))
		{
			assert(0);
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
		if (!sendHttpResponse(pSocketCtx->GetServerToUserContext(), "HTTP/1.1 422 Unprocessable Entity\r\nConnection: close\r\n\r\n"))
		{
			assert(0);
		}
		return false;
	}
	ULONG ip = 0;
	if (getIpByHost(host.c_str(), &ip))
	{
		peerAddr->sin_family = AF_INET;
		peerAddr->sin_port = htons(static_cast<u_short>(StrToIntA(port.c_str())));
		peerAddr->sin_addr.s_addr = ip;

		if (methodIndex == CONNECT_INDEX)
		{
			pSocketCtx->SetCustomData(TunnelHttpProxy);
		}
		else
		{
			pSocketCtx->SetCustomData(PlanHttpProxy);
		}
		return true;
	}
	return false;
}

ULONG CHttpTunnel::readHeader(LPIOContext pIoCtx, DWORD)
{
	const char* pBase = pIoCtx->buffer;
	auto pHeaderEnd = StrStrA(pBase, "\r\n\r\n");
	if (pHeaderEnd == NULL)
	{
		return INVALID_HEADER_LEN;
	}
	else
	{
		return static_cast<ULONG>(pHeaderEnd - pBase + 4);
	}
}

DWORD CHttpTunnel::rewriteHeader(LPIOContext pIoCtx, DWORD dwLen)
{
	auto pFirstLine = StrStrA(pIoCtx->buffer, "\r\n");
	if (pFirstLine == NULL)
	{
		assert(0);
		return dwLen;
	}
	char* pCursor = pIoCtx->buffer;

	//GET http://3g.sina.com.cn HTTP/1.1
	// skip first method
	while (pCursor < pFirstLine && *pCursor != ' ')
	{
		++pCursor;
	}
	// skip space after method
	while (pCursor < pFirstLine && *pCursor == ' ')
	{
		++pCursor;
	}
	char* pMemDst = pCursor;
	// find ://
	for (; pCursor < pFirstLine && *pCursor != ' '; pCursor++)
	{
		if (*pCursor == ':'
			&& ++pCursor < pFirstLine
			&& *pCursor == '/'
			&& ++pCursor < pFirstLine
			&& *pCursor == '/')
		{
			// 让游标移动到"://"的下一位
			++pCursor;
			break;
		}
	}

	// found ://
	if (pCursor < pFirstLine && *pCursor != ' ')
	{
		while (pCursor < pFirstLine && *pCursor != '/' && *pCursor != ' ')
		{
			++pCursor;
		}
	}
	else
	{
		return dwLen;
	}

	if (pCursor >= pFirstLine)
	{
		assert(0);
		return dwLen;
	}

	if (*pCursor == '/')
	{
		assert(pMemDst < pCursor);
		// 此处内存虽然有重叠，但是由于pCursor大于pMemDst，所以仍然可以使用memcpy
		memcpy_s(pMemDst, dwLen - (pMemDst - pIoCtx->buffer), pCursor, dwLen - (pCursor - pIoCtx->buffer));
		return dwLen - static_cast<DWORD>(pCursor - pMemDst);
	}

	// URL路径没有使用'/'
	if (*pCursor == ' ')
	{
		--pCursor; // 回退一位用于存放'/'
		assert(pMemDst < pCursor);
		memcpy_s(pMemDst, dwLen - (pMemDst - pIoCtx->buffer), pCursor, dwLen - (pCursor - pIoCtx->buffer));
		*pMemDst = '/';
		return dwLen - static_cast<DWORD>(pCursor - pMemDst);
	}

	assert(0);
	return dwLen;
}

bool CHttpTunnel::sendHttpResponse(LPIOContext pIoCtx, const char* payload)
{
	DWORD dwLen = static_cast<DWORD>(strlen(payload));
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

bool CHttpTunnel::getIpByHost(const char* host, ULONG* ip)
{
	std::string strHost = host;
	{
		// step1 从缓存获取
		DnsCache dc;
		if (m_dnsCache.Fetch(strHost, &dc))
		{
			if (dc.IsExpired())
			{
				m_dnsCache.Remove(strHost);
			}
			else
			{
				*ip = dc.ip;
				return true;
			}
		}
	}

	{
		// step2 检测是否ip
		static_assert(sizeof(IN_ADDR) == sizeof(ULONG), "sizeof(IN_ADDR) != sizeof(ULONG)");
		if (1 == ::InetPtonA(AF_INET, host, ip))
		{
			return true;
		}
	}

	{
		// step3 DnsQuery_A获取
		PDNS_RECORD pRecord = NULL;
		constexpr unsigned int MemSize = sizeof(IP4_ARRAY) + sizeof(IP4_ADDRESS);
		byte memBuf[MemSize] = { 0 };
		PIP4_ARRAY pSrvList = reinterpret_cast<PIP4_ARRAY>(memBuf);
		pSrvList->AddrCount = 2;
		::InetPtonA(AF_INET, "223.5.5.5", pSrvList->AddrArray);
		::InetPtonA(AF_INET, "119.29.29.29", &pSrvList->AddrArray[1]);
		bool success = false;
		if (0 == ::DnsQuery_A(host, DNS_TYPE_A,
			DNS_QUERY_BYPASS_CACHE | DNS_QUERY_WIRE_ONLY,
			pSrvList, &pRecord, NULL))
		{

			for (auto pCursor = pRecord; pCursor != NULL; pCursor = pCursor->pNext)
			{
				if (pCursor->Flags.S.Section != DnsSectionAnswer)
				{
					continue;
				}
				//if (::DnsNameCompare(pCursor->pName, host))
				//{
				//}
				if (pCursor->wType == DNS_TYPE_A)
				{
					*ip = pCursor->Data.A.IpAddress;
					assert(pCursor->dwTtl != 0);
					m_dnsCache.Insert(strHost, createDnsCache(*ip, pCursor->dwTtl));
					success = true;
					break;
				}
			}
			::DnsRecordListFree(pRecord, DnsFreeRecordListDeep);
		}
		if (success)
		{
			return true;
		}
	}

	{
		// step4 GetAddrInfoA获取
		addrinfo hints = { 0 };
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_DGRAM;
		hints.ai_protocol = IPPROTO_IP;
		addrinfo* pResult = NULL;
		if (GetAddrInfoA(host, "0", &hints, &pResult) == 0)
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
				*ip = reinterpret_cast<SOCKADDR_IN*>(pCursor->ai_addr)->sin_addr.s_addr;
				m_dnsCache.Insert(strHost, createDnsCache(*ip, 0));
			}
			FreeAddrInfoA(pResult);

			if (bFind)
			{
				return true;
			}
		}
	}

	return false;
}

CHttpTunnel::DnsCache CHttpTunnel::createDnsCache(ULONG ip, DWORD ttl)
{
	DnsCache dc;
	dc.ip = ip;
	if (ttl == 0)
	{
		ttl = DEFAULT_DNS_TTL;
	}
	dc.expire = std::chrono::system_clock::now() + std::chrono::seconds(ttl);
	return dc;
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
		if (success)
		{
			return sendHttpResponse(pSocketCtx->GetServerToUserContext(),
				"HTTP/1.1 200 Connection Established\r\nConnection: keep-alive\r\n\r\n")
				&& pSocketCtx->GetUserToServerContext()->PostRecv();
		}
		else
		{
			sendHttpResponse(pSocketCtx->GetServerToUserContext(),
				"HTTP/1.1 408 Request Timeout\r\nConnection: close\r\n\r\n");
			return false;
		}
	}
	else
	{
		assert(pSocketCtx->GetCustomData() == PlanHttpProxy);
		if (success)
		{
			return __super::onServerConnectPosted(pSocketCtx, 
				rewriteHeader(pSocketCtx->GetUserToServerContext(), dwLen), success);
		}
		else
		{
			sendHttpResponse(pSocketCtx->GetServerToUserContext(),
				"HTTP/1.1 408 Request Timeout\r\nConnection: close\r\n\r\n");
			return false;
		}
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

bool CHttpTunnel::DnsCache::IsExpired()
{
	return std::chrono::system_clock::now() > this->expire;
}
