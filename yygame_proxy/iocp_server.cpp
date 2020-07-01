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


DWORD CIOCPServer::StartServer(unsigned short port)
{
	DWORD dwResult = init();
	if (ERROR_SUCCESS != dwResult)
	{
		return dwResult;
	}

	SOCKADDR_IN addr = { 0 };
	addr.sin_family = AF_INET;
	//InetPtonW(AF_INET, L"127.0.0.1", &addr.sin_addr.s_addr);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(port);

	do
	{
		SOCKET l = ::WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
		if (l == INVALID_SOCKET)
		{
			break;
		}

		// 设置SO_REUSEADDR，防止程序异常退出，重启之后无法绑定对应端口
		//int optReuse = 1;
		//::setsockopt(l, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&optReuse), sizeof(optReuse));
		AssociateWithServer(reinterpret_cast<HANDLE>(l), NULL, 0);

		if (SOCKET_ERROR == ::bind(l, (const PSOCKADDR)&addr, sizeof(addr)))
		{
			::closesocket(l);
			break;
		}
		if (SOCKET_ERROR == ::listen(l, SOMAXCONN))
		{
			::closesocket(l);
			break;
		}

		m_hListenSocket = l;

		for (int i = 0; i < 5; i++)
		{
			VERIFY(prepareSocket() != NULL);
		}

		::WaitForSingleObject(m_hStopEvt, INFINITE);

	} while (0);

	dwResult = ::WSAGetLastError();

	uninit();

	return dwResult;
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
	m_hListenSocket = INVALID_SOCKET;
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

	if (m_hListenSocket != INVALID_SOCKET)
	{
		::closesocket(m_hListenSocket);
		m_hListenSocket = INVALID_SOCKET;
	}
	clearSockets();

}

bool CIOCPServer::handleAccept(LPIOContext pIoCtx, DWORD dwBytesRecv)
{
	SOCKADDR_IN srvAddr = { 0 };
	srvAddr.sin_addr.s_addr = INADDR_NONE;
	if (!onAcceptPosted(pIoCtx->GetSocketContext(), pIoCtx, dwBytesRecv, &srvAddr))
	{
		return false;
	}
	if (srvAddr.sin_addr.s_addr == INADDR_NONE || srvAddr.sin_port == 0)
	{
		return false;
	}

	bool bAcceptOk = pIoCtx->GetSocketContext()->CompleteAccept(m_hListenSocket, &srvAddr);
	bAcceptOk = onServerConnectPosted(pIoCtx->GetSocketContext(), dwBytesRecv, bAcceptOk);
	if (!bAcceptOk)
	{
		return false;
	}

	constexpr auto AddrLength = sizeof(SOCKADDR_IN) + 16;
	if (dwBytesRecv < AddrLength * 2)
	{
		return false;
	}

	//SOCKADDR_IN* remoteAddr = NULL, * localAddr = NULL;
	//int remoteLen = sizeof(SOCKADDR_IN);
	//int localLen = sizeof(SOCKADDR_IN);
	//PtrGetAcceptExSockAddrs(pIoCtx->buffer,
	//	dwBytesRecv - (AddrLength * 2),
	//	AddrLength,
	//	AddrLength,
	//	reinterpret_cast<LPSOCKADDR*>(&localAddr),
	//	&localLen,
	//	reinterpret_cast<LPSOCKADDR*>(&remoteAddr),
	//	&remoteLen);

	return true;
}

void CIOCPServer::iocpWorker()
{
	DWORD dwTransferred = 0;
	LPSocketContext pSocketCtx = NULL;
	LPIOContext pIoCtx = NULL;
	LPOVERLAPPED pOverlapped = NULL;
	while (m_bRun)
	{
		pSocketCtx = NULL;
		pOverlapped = NULL;

		BOOL bResult = ::GetQueuedCompletionStatus(m_hIOCP, &dwTransferred,
			reinterpret_cast<PULONG_PTR>(&pSocketCtx), &pOverlapped, INFINITE);
		if (pOverlapped == NULL)
		{
			// 退出通知
			break;
		}
		pIoCtx = CONTAINING_RECORD(pOverlapped, IOContext, overlapped);
		if (!bResult)
		{
			// 异常
			if (!handleError(pIoCtx, ::GetLastError()))
			{
				// IOCP 异常，退出工作线程
				break;
			}
			else
			{
				continue;
			}
		}
		if (pSocketCtx == NULL)
		{
			// 从listen socket 过来的连接
			assert(pIoCtx->opType == IO_OPT_TYPE::ACCEPT_POSTED);
			pSocketCtx = pIoCtx->GetSocketContext();
		}

		if (dwTransferred == 0
			&& (pIoCtx->opType != IO_OPT_TYPE::ACCEPT_POSTED))
		{
			// 客户端断开
			handleDisconnected(pSocketCtx, pIoCtx);
			continue;
		}

		bool bOk = true;
		switch (pIoCtx->opType)
		{
		case IO_OPT_TYPE::ACCEPT_POSTED:
		{
			// 准备一个新的socket供后续新的连接使用
			VERIFY(prepareSocket() != NULL);

			bOk = handleAccept(pIoCtx, dwTransferred);
		}
		break;
		case IO_OPT_TYPE::RECV_POSTED:
			bOk = onRecvPosted(pSocketCtx, pIoCtx, dwTransferred);
			break;
		case IO_OPT_TYPE::SEND_POSTED:
			bOk = onSendPosted(pSocketCtx, pIoCtx, dwTransferred);
			break;
		case IO_OPT_TYPE::NONE_POSTED:
		default:
			assert(0);
			bOk = true;
			break;
		}
		if (!bOk)
		{
			CloseIoSocket(pIoCtx);
			::OutputDebugString(L"process IO_OPT_TYPE error\n");
		}
	}

}

bool CIOCPServer::onAcceptPosted(LPSocketContext pSocketCtx, LPIOContext pIoCtx, DWORD, SOCKADDR_IN*)
{
	assert(pIoCtx->GetSocketContext() == pSocketCtx);
	//peerAddr->sin_family = AF_INET;
	//peerAddr->sin_port = htons(1081);
	//peerAddr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	return true;
}

bool CIOCPServer::onServerConnectPosted(LPSocketContext pSocketCtx, DWORD dwLen, bool success)
{
	if (success)
	{
		return pSocketCtx->GetServerToUserContext()->PostRecv()
			&& pSocketCtx->GetUserToServerContext()->PostSend(dwLen);
	}
	return false;
}

bool CIOCPServer::onRecvPosted(LPSocketContext pSocketCtx, LPIOContext pIoCtx, DWORD dwLen)
{
	assert(pIoCtx->GetSocketContext() == pSocketCtx);
	return pIoCtx->PostSend(dwLen);
}

bool CIOCPServer::onSendPosted(LPSocketContext pSocketCtx, LPIOContext pIoCtx, DWORD)
{
	assert(pIoCtx->GetSocketContext() == pSocketCtx);
	pIoCtx->ResetBuffer();
	return pIoCtx->PostRecv();
}

void CIOCPServer::onServerError(LPIOContext, DWORD)
{

}

void CIOCPServer::onDisconnected(LPSocketContext, LPIOContext)
{
}

void CIOCPServer::CloseIoSocket(LPIOContext pIo)
{
	std::lock_guard<std::recursive_mutex> _(m_connTableGuard);
	if (pIo->GetSocketContext()->Close(pIo))
	{
		deleteSocket(pIo->GetSocketContext());
	}
}

LPSocketContext CIOCPServer::prepareSocket()
{
	auto pSocktCtx = new SocketContext();
	if (!pSocktCtx->Init(m_hIOCP) || !pSocktCtx->PostAccept(m_hListenSocket))
	{
		delete pSocktCtx;
		assert(0);
		return NULL;
	}

	{
		std::lock_guard<std::recursive_mutex> _(m_connTableGuard);
		m_connTable.insert(pSocktCtx);
	}

	return pSocktCtx;
}

void CIOCPServer::deleteSocket(LPSocketContext pCtx)
{
	{
		std::lock_guard<std::recursive_mutex> _(m_connTableGuard);
		m_connTable.erase(pCtx);
	}
	delete pCtx;
}

void CIOCPServer::clearSockets()
{
	std::lock_guard<std::recursive_mutex> _(m_connTableGuard);
	for (LPSocketContext pCtx : m_connTable)
	{
		delete pCtx;
	}
	m_connTable.clear();
}

HANDLE CIOCPServer::AssociateWithServer(HANDLE hFile, ULONG_PTR CompletionKey, DWORD NumberOfConcurrentThreads)
{
	return ::CreateIoCompletionPort(hFile, m_hIOCP, CompletionKey, NumberOfConcurrentThreads);
}

bool CIOCPServer::handleError(LPIOContext pIoCtx, DWORD dwErr)
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

	onServerError(pIoCtx, dwErr);
	assert(pIoCtx != NULL);
	CloseIoSocket(pIoCtx);
	return bResult;
}

void CIOCPServer::handleDisconnected(LPSocketContext pSocketCtx, LPIOContext pIoCtx)
{
	onDisconnected(pSocketCtx, pIoCtx);
	CloseIoSocket(pIoCtx);
}

IOContext::IOContext(SocketContext* pSocketCtx, SOCKET& inSocket, SOCKET& outSocket)
	: hInSocket(inSocket), hOutSocket(outSocket)
{
	this->pSocketCtx = pSocketCtx;
	this->opType = IO_OPT_TYPE::NONE_POSTED;
	ResetBuffer();
}

void IOContext::ResetBuffer()
{
	::ZeroMemory(this->buffer, _countof(buffer));
	::ZeroMemory(&overlapped, sizeof(overlapped));
}

bool IOContext::SetPayload(const char* src, DWORD dwLen)
{
	return 0 == ::memcpy_s(buffer, HTTPPROXY_BUFFER_LENGTH, src, dwLen);
}

bool IOContext::PostSend(DWORD dwLength)
{
	assert(hOutSocket != INVALID_SOCKET && hOutSocket != NULL);
	assert(dwLength != 0);

	this->opType = IO_OPT_TYPE::SEND_POSTED;
	DWORD dwFlag = 0;
	WSABUF sendBuf;
	sendBuf.buf = this->buffer;
	sendBuf.len = dwLength;
	if (::WSASend(hOutSocket, &sendBuf, 1, NULL, dwFlag, &overlapped, NULL) != 0)
	{
		return ::WSAGetLastError() == WSA_IO_PENDING;
	}
	return true;
}

bool IOContext::PostRecv()
{
	assert(hInSocket != INVALID_SOCKET && hInSocket != NULL);

	this->opType = IO_OPT_TYPE::RECV_POSTED;
	DWORD dwFlag = 0;
	WSABUF recvBuf;
	recvBuf.buf = this->buffer;
	recvBuf.len = HTTPPROXY_BUFFER_LENGTH;
	if (::WSARecv(hInSocket, &recvBuf, 1, NULL, &dwFlag, &overlapped, NULL) != 0)
	{
		return ::WSAGetLastError() == WSA_IO_PENDING;
	}
	return true;
}

void IOContext::Close()
{
	assert(hInSocket != INVALID_SOCKET && hInSocket != NULL);
	assert(hOutSocket != INVALID_SOCKET && hOutSocket != NULL);
	::shutdown(hInSocket, SD_RECEIVE);
	::shutdown(hOutSocket, SD_SEND);
}

SocketContext* IOContext::GetSocketContext()
{
	return pSocketCtx;
}

bool SocketContext::Init(HANDLE hIocp)
{
	auto user = ::WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (user == INVALID_SOCKET)
	{
		assert(::WSAGetLastError() == 0);
		return false;
	}
	auto server = ::WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (server == INVALID_SOCKET)
	{
		assert(::WSAGetLastError() == 0);
		::closesocket(user);
		return false;
	}

	if (::CreateIoCompletionPort(reinterpret_cast<HANDLE>(user), hIocp, reinterpret_cast<ULONG_PTR>(this), 0)
		== NULL)
	{
		::closesocket(user);
		::closesocket(server);
		return false;
	}
	if (::CreateIoCompletionPort(reinterpret_cast<HANDLE>(server), hIocp, reinterpret_cast<ULONG_PTR>(this), 0)
		== NULL)
	{
		::closesocket(user);
		::closesocket(server);
		return false;
	}

	userSocket = user;
	serverSocket = server;
	connCount = 2;
	return true;
}

bool SocketContext::Close(LPIOContext pIO)
{
	if (connCount == 0)
	{
		return true;
	}

	pIO->Close();

	if (--connCount == 0)
	{
		this->close();
		return true;
	}
	return false;
}

bool SocketContext::PostAccept(SOCKET hListen)
{
	constexpr auto AddrLength = sizeof(SOCKADDR_IN) + 16;
	usr2SrvCtx.opType = IO_OPT_TYPE::ACCEPT_POSTED;
	if (!PtrAcceptEx(hListen, userSocket, this->usr2SrvCtx.buffer,
		HTTPPROXY_BUFFER_LENGTH - (AddrLength * 2),
		AddrLength,
		AddrLength,
		NULL,
		&this->usr2SrvCtx.overlapped))
	{
		return WSA_IO_PENDING == ::WSAGetLastError();
	}
	return true;
}

bool SocketContext::CompleteAccept(SOCKET hListen, const SOCKADDR_IN* addr)
{
	::setsockopt(userSocket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, reinterpret_cast<const char*>(&hListen),
		sizeof(SOCKET));

	if (0 != ::WSAConnect(serverSocket, reinterpret_cast<const sockaddr*>(addr), sizeof(SOCKADDR_IN),
		NULL, NULL, NULL, NULL))
	{
		//assert(0 == ::WSAGetLastError());
		return false;
	}
	return true;
	//if (!srv2UsrCtx.PostRecv())
	//{
	//	return false;
	//}

	//if (dwBytesRecv != 0)
	//{
	//	return usr2SrvCtx.PostSend(dwBytesRecv);
	//}
	//else
	//{
	//	return true;
	//}
}

LPIOContext SocketContext::GetUserToServerContext()
{
	return &usr2SrvCtx;
}

LPIOContext SocketContext::GetServerToUserContext()
{
	return &srv2UsrCtx;
}

unsigned long SocketContext::GetCustomData() const
{
	return this->data;
}

void SocketContext::SetCustomData(unsigned long x)
{
	this->data = x;
}

SocketContext::~SocketContext()
{
	this->close();
}

SocketContext::SocketContext()
	: userSocket(INVALID_SOCKET), serverSocket(INVALID_SOCKET),
	usr2SrvCtx(this, userSocket, serverSocket),
	srv2UsrCtx(this, serverSocket, userSocket),
	connCount(0), data(0)
{

}

void SocketContext::close()
{
	if (userSocket != INVALID_SOCKET)
	{
		::closesocket(userSocket);
		userSocket = INVALID_SOCKET;
	}
	if (serverSocket != INVALID_SOCKET)
	{
		::closesocket(serverSocket);
		serverSocket = INVALID_SOCKET;
	}
}
