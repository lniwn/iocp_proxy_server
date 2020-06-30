#include "framework.h"
#include <ws2tcpip.h>
#include <mswsock.h>
#include "iocp_server.h"

#pragma comment(lib, "Ws2_32.lib")

static LPFN_CONNECTEX PtrConnectEx;
static LPFN_DISCONNECTEX PtrDisconnectEx;
static LPFN_ACCEPTEX PtrAcceptEx;
static LPFN_GETACCEPTEXSOCKADDRS PtrGetAcceptExSockAddrs;

#define  DEFER_SOCKET(hDummy) \
	std::shared_ptr<SOCKET> _defer_##hDummy(&hDummy, [](SOCKET* hDelegated) { ::closesocket(*hDelegated); })


#include <set>
static std::set<LPPER_HANDLE_DATA> g_handleDataList;
std::mutex g_listGuard;

void PutDataList(LPPER_HANDLE_DATA pData)
{
	std::lock_guard<std::mutex> _(g_listGuard);
	g_handleDataList.insert(pData);
}

void RemoveDataList(LPPER_HANDLE_DATA pData)
{
	std::lock_guard<std::mutex> _(g_listGuard);
	g_handleDataList.erase(pData);
	assert(g_handleDataList.find(pData) == g_handleDataList.end());
}

DWORD CIOCPServer::StartServer(unsigned short port)
{
	DWORD dwResult = init();
	if (ERROR_SUCCESS != dwResult)
	{
		return dwResult;
	}

	SOCKADDR_IN addr = { 0 };
	addr.sin_family = AF_INET;
	InetPtonW(AF_INET, L"127.0.0.1", &addr.sin_addr.s_addr);
	//addr.sin_addr.s_addr = inet_addr("127.0.0.1");
	addr.sin_port = htons(port);

	SOCKET l = ::WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (l == INVALID_SOCKET)
	{
		return ::WSAGetLastError();
	}

	// 设置SO_REUSEADDR，防止程序异常退出，重启之后无法绑定对应端口
	//int optReuse = 1;
	//::setsockopt(l, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&optReuse), sizeof(optReuse));
	m_pListenContext = PER_HANDLE_DATA::Create(l, reinterpret_cast<const SOCKADDR_STORAGE*>(&addr), sizeof(SOCKADDR_IN));
	AssociateWithServer(reinterpret_cast<HANDLE>(l), reinterpret_cast<ULONG_PTR>(m_pListenContext), 0);

	if (SOCKET_ERROR == ::bind(l, (const PSOCKADDR)&addr, sizeof(addr)))
	{
		::closesocket(l);
		return ::WSAGetLastError();
	}
	if (SOCKET_ERROR == ::listen(l, SOMAXCONN))
	{
		::closesocket(l);
		return ::WSAGetLastError();
	}

	postAccept(m_pListenContext->recvBuf);
	postAccept(m_pListenContext->sendBuf);

	::WaitForSingleObject(m_hStopEvt, INFINITE);

	uninit();

	return ERROR_SUCCESS;
}

void CIOCPServer::StopServer()
{
	m_bRun = false;
	if (m_hStopEvt != NULL)
	{
		::SetEvent(m_hStopEvt);
	}
}

CIOCPServer::CIOCPServer()
{
	m_bRun = true;
	m_hIOCP = NULL;
	m_pListenContext = NULL;
	m_hStopEvt = NULL;
}

CIOCPServer::~CIOCPServer()
{

}

DWORD CIOCPServer::init()
{
	SOCKET hDummy = ::WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (hDummy == INVALID_SOCKET)
	{
		return ::WSAGetLastError();
	}
	DEFER_SOCKET(hDummy);

	DWORD dwBytes = 0;
	// ConnectEx
	{
		assert(PtrConnectEx == NULL);
		GUID guidConnectEx = WSAID_CONNECTEX;
		if (SOCKET_ERROR == ::WSAIoctl(hDummy, SIO_GET_EXTENSION_FUNCTION_POINTER, &guidConnectEx,
			sizeof(guidConnectEx), &PtrConnectEx, sizeof(PtrConnectEx), &dwBytes, NULL, NULL))
		{
			return ::WSAGetLastError();
		}
		assert(PtrConnectEx != NULL);
	}
	// AcceptEx
	{
		assert(PtrAcceptEx == NULL);
		GUID id = WSAID_ACCEPTEX;
		if (SOCKET_ERROR == ::WSAIoctl(hDummy, SIO_GET_EXTENSION_FUNCTION_POINTER, &id,
			sizeof(id), &PtrAcceptEx, sizeof(PtrAcceptEx), &dwBytes, NULL, NULL))
		{
			return ::WSAGetLastError();
		}
		assert(PtrAcceptEx != NULL);
	}
	// GetAcceptExSockAddrs
	{
		assert(PtrGetAcceptExSockAddrs == NULL);
		GUID id = WSAID_GETACCEPTEXSOCKADDRS;
		if (SOCKET_ERROR == ::WSAIoctl(hDummy, SIO_GET_EXTENSION_FUNCTION_POINTER, &id,
			sizeof(id), &PtrGetAcceptExSockAddrs, sizeof(PtrGetAcceptExSockAddrs), &dwBytes, NULL, NULL))
		{
			return ::WSAGetLastError();
		}
		assert(PtrGetAcceptExSockAddrs != NULL);
	}

	m_hIOCP = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, 0);
	SYSTEM_INFO sysInfo = { 0 };
	::GetSystemInfo(&sysInfo);

	for (DWORD i = 0; i < sysInfo.dwNumberOfProcessors * 2; i++)
	{
		auto worker = std::make_shared<std::thread>(&CIOCPServer::iocpWorker, this);
		m_workers.push_back(worker);
	}

	m_hStopEvt = ::CreateEvent(NULL, TRUE, FALSE, NULL);

	return ERROR_SUCCESS;
}

void CIOCPServer::uninit()
{
	if (m_pListenContext != NULL)
	{
		delete m_pListenContext;
		m_pListenContext = NULL;
	}

	BOOL bPostOk = TRUE;
	for (size_t i = 0; bPostOk && i < m_workers.size(); i++)
	{
		bPostOk = ::PostQueuedCompletionStatus(m_hIOCP, 0, NULL, NULL);
	}

	if (!bPostOk)
	{
		// 激活所有线程
		::CloseHandle(m_hIOCP);
		m_hIOCP = NULL;
	}

	for (const auto& pThread : m_workers)
	{
		if (pThread->joinable())
		{
			pThread->join();
		}
	}

	if (m_hIOCP != NULL)
	{
		::CloseHandle(m_hIOCP);
		m_hIOCP = NULL;
	}

	if (m_hStopEvt != NULL)
	{
		::CloseHandle(m_hStopEvt);
		m_hStopEvt = NULL;
	}

}

bool CIOCPServer::handleAccept(LPPER_IO_DATA pIoData, LPPER_HANDLE_DATA* ppAcceptSockData, LPPER_IO_DATA* ppAcceptIoData)
{
	::setsockopt(pIoData->hAccept, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
		reinterpret_cast<const char*>(&m_pListenContext->hPeer),
		sizeof(m_pListenContext->hPeer));

	constexpr auto AddrLength = sizeof(SOCKADDR_IN) + 16;
	SOCKADDR_IN* remoteAddr = NULL, * localAddr = NULL;
	int remoteLen = sizeof(SOCKADDR_IN);
	int localLen = sizeof(SOCKADDR_IN);
	PtrGetAcceptExSockAddrs(pIoData->wsaBuffer.buf,
		pIoData->wsaBuffer.len - (AddrLength * 2),
		AddrLength,
		AddrLength,
		reinterpret_cast<LPSOCKADDR*>(&localAddr),
		&localLen,
		reinterpret_cast<LPSOCKADDR*>(&remoteAddr),
		&remoteLen);

	auto pAcceptData = PER_HANDLE_DATA::Create(pIoData->hAccept,
		reinterpret_cast<const SOCKADDR_STORAGE*>(remoteAddr), remoteLen);
	auto pAcceptIoData = pAcceptData->recvBuf;
	pAcceptIoData->Reset(IO_OPT_TYPE::ACCEPT_POSTED);

	memcpy_s(pAcceptIoData->buffer, HTTPPROXY_BUFFER_LENGTH, pIoData->buffer, HTTPPROXY_BUFFER_LENGTH);
	pAcceptIoData->hAccept = pIoData->hAccept;

	::CreateIoCompletionPort(reinterpret_cast<HANDLE>(pAcceptIoData->hAccept), m_hIOCP, reinterpret_cast<ULONG_PTR>(pAcceptData), 0);

	postAccept(pIoData);
	//if (!::QueueUserWorkItem(workforAccepted, this, WT_EXECUTEDEFAULT))
	//{
	//	postAccept();
	//	assert(0);
	//}

	*ppAcceptSockData = pAcceptData;
	*ppAcceptIoData = pAcceptIoData;

	return true;
}

void CIOCPServer::iocpWorker()
{
	DWORD dwTransferred = 0;
	LPPER_HANDLE_DATA pHandleData = NULL;
	LPPER_IO_DATA pIoData = NULL;
	LPOVERLAPPED pOverlapped = NULL;
	while (m_bRun)
	{
		pHandleData = NULL;
		pOverlapped = NULL;

		BOOL bResult = ::GetQueuedCompletionStatus(m_hIOCP, &dwTransferred,
			reinterpret_cast<PULONG_PTR>(&pHandleData), &pOverlapped, INFINITE);
		pIoData = CONTAINING_RECORD(pOverlapped, PER_IO_DATA, overlapped);
		if (!bResult)
		{
			// 异常
			if (!handleError(pHandleData, pIoData, ::GetLastError()))
			{
				break;
			}
			else
			{
				continue;
			}
		}
		if (pHandleData == NULL || pOverlapped == NULL)
		{
			// 退出通知
			break;
		}

		if (dwTransferred == 0
			&& (pIoData->opType == IO_OPT_TYPE::RECV_POSTED || pIoData->opType == IO_OPT_TYPE::SEND_POSTED))
		{
			// 客户端断开
			onDisconnected(pHandleData, pIoData);
			continue;
		}

		bool bOk = true;
		switch (pIoData->opType)
		{
		case IO_OPT_TYPE::ACCEPT_POSTED:
		{
			assert(pHandleData == m_pListenContext);
			LPPER_HANDLE_DATA pAcceptSock = NULL;
			LPPER_IO_DATA pAcceptIo = NULL;
			if (handleAccept(pIoData, &pAcceptSock, &pAcceptIo))
			{
				bOk = onAcceptPosted(pAcceptSock, pAcceptIo, dwTransferred);
			}
			else
			{
				assert(0);
			}
		}
		break;
		case IO_OPT_TYPE::RECV_POSTED:
			bOk = onRecvPosted(pHandleData, pIoData, dwTransferred);
			break;
		case IO_OPT_TYPE::SEND_POSTED:
			bOk = onSendPosted(pHandleData, pIoData, dwTransferred);
			break;
		case IO_OPT_TYPE::NONE_POSTED:
		default:
			assert(0);
			break;
		}
		if (!bOk)
		{
			::OutputDebugString(L"process IO_OPT_TYPE error\n");
		}
	}

	assert(pHandleData == NULL);
}

bool CIOCPServer::onAcceptPosted(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData, DWORD)
{
	assert(pHandleData->recvBuf == pIoData);
	pIoData->Reset(IO_OPT_TYPE::RECV_POSTED);
	return PostRecv(pHandleData, pIoData);
}

bool CIOCPServer::PostRecv(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData)
{
	assert(pHandleData->recvBuf == pIoData);
	pIoData->opType = IO_OPT_TYPE::RECV_POSTED;
	DWORD dwFlag = 0;
	if (::WSARecv(pHandleData->hPeer, &pIoData->wsaBuffer, 1, NULL, &dwFlag, &pIoData->overlapped, NULL) != 0)
	{
		return ::WSAGetLastError() == WSA_IO_PENDING;
	}
	return true;
}

bool CIOCPServer::onRecvPosted(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData, DWORD)
{
	assert(pHandleData->recvBuf == pIoData);
	return PostRecv(pHandleData, pIoData);
}

bool CIOCPServer::onSendPosted(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData, DWORD)
{
	assert(pHandleData->sendBuf == pIoData);
	pIoData->Reset(IO_OPT_TYPE::SEND_POSTED);
	return true;
}

void CIOCPServer::onServerError(LPPER_HANDLE_DATA, DWORD)
{

}

void CIOCPServer::onDisconnected(LPPER_HANDLE_DATA, LPPER_IO_DATA)
{
}

bool CIOCPServer::PostSend(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData)
{
	assert(pIoData == pHandleData->sendBuf);
	pIoData->opType = IO_OPT_TYPE::SEND_POSTED;
	DWORD dwFlag = 0;
	if (::WSASend(pHandleData->hPeer, &pIoData->wsaBuffer, 1, NULL, dwFlag, &pIoData->overlapped, NULL) != 0)
	{
		return ::WSAGetLastError() == WSA_IO_PENDING;
	}
	return true;
}

bool CIOCPServer::postAccept(LPPER_IO_DATA pIoData)
{
	assert(pIoData == m_pListenContext->recvBuf || pIoData == m_pListenContext->sendBuf);
	pIoData->Reset(IO_OPT_TYPE::ACCEPT_POSTED);

	SOCKET hClient = ::WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (INVALID_SOCKET == hClient)
	{
		return false;
	}
	pIoData->hAccept = hClient;
	constexpr auto AddrLength = sizeof(SOCKADDR_IN) + 16;
	return TRUE == PtrAcceptEx(m_pListenContext->hPeer, hClient, pIoData->wsaBuffer.buf,
		pIoData->wsaBuffer.len - (AddrLength * 2),
		AddrLength,
		AddrLength,
		NULL,
		&pIoData->overlapped);
}

HANDLE CIOCPServer::AssociateWithServer(HANDLE hFile, ULONG_PTR CompletionKey, DWORD NumberOfConcurrentThreads)
{
	return ::CreateIoCompletionPort(hFile, m_hIOCP, CompletionKey, NumberOfConcurrentThreads);
}

bool CIOCPServer::handleError(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData, DWORD dwErr)
{
	bool bResult = false;
	switch (dwErr)
	{
	case WAIT_TIMEOUT:
	{
		bResult = true;
	}
	break;
	case ERROR_NETNAME_DELETED:
	{
		bResult = true;
	}
	break;
	case ERROR_ABANDONED_WAIT_0:
	{
		bResult = false;
	}
	break;
	case ERROR_OPERATION_ABORTED:
	case ERROR_CONNECTION_ABORTED:
	case ERROR_REQUEST_ABORTED:
	{
		bResult = true;
	}
	break;
	default:
		bResult = true;
		break;
	}

	onServerError(pHandleData, dwErr);
	assert(pIoData != NULL);
	delete pHandleData;
	return bResult;
}

DWORD WINAPI CIOCPServer::workforAccepted(_In_ LPVOID lpParameter)
{
	return 0;
}

_PER_IO_DATA::_PER_IO_DATA(IO_OPT_TYPE opType)
{
	Reset(opType);
}

void _PER_IO_DATA::Reset(IO_OPT_TYPE opType)
{
	::ZeroMemory(this, sizeof(*this));
	wsaBuffer.buf = buffer;
	wsaBuffer.len = HTTPPROXY_BUFFER_LENGTH;
	this->opType = opType;
	this->hAccept = INVALID_SOCKET;
}

bool _PER_IO_DATA::SetPayload(const char* src, size_t length)
{
	static_assert(sizeof(this->buffer) == HTTPPROXY_BUFFER_LENGTH + 1, "assert error");
	::ZeroMemory(this->buffer, HTTPPROXY_BUFFER_LENGTH + 1);

	assert(length <= HTTPPROXY_BUFFER_LENGTH);
	if (memcpy_s(this->buffer, HTTPPROXY_BUFFER_LENGTH, src, length) == 0)
	{
		this->wsaBuffer.buf = this->buffer;
		this->wsaBuffer.len = length;
		return true;
	}
	else
	{
		return false;
	}
}

_PER_HANDLE_DATA::_PER_HANDLE_DATA()
{
	hPeer = INVALID_SOCKET;
	uUser = 0UL;
	recvBuf = new PER_IO_DATA(IO_OPT_TYPE::NONE_POSTED);
	sendBuf = new PER_IO_DATA(IO_OPT_TYPE::NONE_POSTED);
	::ZeroMemory(&peerAddr, sizeof(SOCKADDR_STORAGE));
}

_PER_HANDLE_DATA::~_PER_HANDLE_DATA()
{
	if (hPeer != INVALID_SOCKET)
	{
		::closesocket(hPeer);
		hPeer = INVALID_SOCKET;
	}
	delete recvBuf;
	delete sendBuf;
	RemoveDataList(this);
}

void _PER_HANDLE_DATA::Close()
{
	::shutdown(hPeer, SD_BOTH);
}

LPPER_HANDLE_DATA _PER_HANDLE_DATA::Create(SOCKET hSock, const SOCKADDR_STORAGE* pAddr,
	size_t length, unsigned long user)
{
	auto pThis = new PER_HANDLE_DATA;
	pThis->hPeer = hSock;
	memcpy_s(&pThis->peerAddr, sizeof(pThis->peerAddr), pAddr, length);
	pThis->uUser = user;
	PutDataList(pThis);
	return pThis;
}
