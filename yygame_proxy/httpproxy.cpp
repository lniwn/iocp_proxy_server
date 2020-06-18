#include "framework.h"
#include <WinSock2.h>
#include <ws2tcpip.h>
#include <cassert>
#include <functional>
#include "httpproxy.h"

#pragma comment(lib, "Ws2_32.lib")

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

	while (m_bRun)
	{
		handleAccept(l);
	}

	::closesocket(l);

	uninit();

	return ERROR_SUCCESS;
}

void CIOCPServer::StopServer()
{
	m_bRun = false;
}

CIOCPServer::CIOCPServer()
{
	m_bRun = true;
	m_hIOCP = NULL;
}

DWORD CIOCPServer::init()
{
	m_hIOCP = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, 0);
	SYSTEM_INFO sysInfo = { 0 };
	::GetSystemInfo(&sysInfo);

	for (DWORD i = 0; i < sysInfo.dwNumberOfProcessors * 2; i++)
	{
		auto worker = std::make_shared<std::thread>(&CIOCPServer::iocpWorker, this);
		m_workers.push_back(worker);
	}
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

}

void CIOCPServer::handleAccept(SOCKET hListen)
{
	SOCKADDR_IN clientAddr = { 0 };
	int addrLen = sizeof(clientAddr);
	SOCKET hClient = ::WSAAccept(hListen, (PSOCKADDR)&clientAddr, &addrLen, NULL, NULL);

	auto fnLambda = new std::function<void()>(
		[=]() {
			LPPER_HANDLE_DATA pHandleData = new PER_HANDLE_DATA;
			pHandleData->hClient = hClient;
			memcpy_s(&pHandleData->clientAddr, sizeof(pHandleData->clientAddr), &clientAddr, addrLen);

			::CreateIoCompletionPort(reinterpret_cast<HANDLE>(pHandleData->hClient), m_hIOCP, reinterpret_cast<ULONG_PTR>(pHandleData), 0);

			auto pIoData = pHandleData->AcquireBuffer(IO_OPT_TYPE::ACCEPT_POSTED);
			::PostQueuedCompletionStatus(m_hIOCP, 0, reinterpret_cast<ULONG_PTR>(pHandleData), &pIoData->overlapped);

		});

	if (!::QueueUserWorkItem(associateWithIOCP, reinterpret_cast<PVOID>(fnLambda), WT_EXECUTEDEFAULT))
	{
		::closesocket(hClient);
		assert(0);
	}
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
		if (!bResult)
		{
			// 异常
			if (!handleError(pHandleData, ::GetLastError()))
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
			m_bRun = false;
			break;
		}
		pIoData = CONTAINING_RECORD(pOverlapped, PER_IO_DATA, overlapped);

		if (dwTransferred == 0 && pIoData->opType != IO_OPT_TYPE::ACCEPT_POSTED)
		{
			// 客户端断开
			delete pHandleData;
			pHandleData = NULL;
			continue;
		}

		bool bOk = true;
		switch (pIoData->opType)
		{
		case IO_OPT_TYPE::ACCEPT_POSTED:
			bOk = onAcceptPosted(pHandleData, pIoData);
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
			::OutputDebugString(L"process IO_OPT_TYPE error");
		}
	}

	assert(pHandleData == NULL);
}

bool CIOCPServer::onAcceptPosted(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData)
{
	// recv与accept公用一个PER_IO_DATA
	pIoData->Reset();
	if (!postRecv(pHandleData, pIoData))
	{
		return false;
	}
	// 预先创建发送缓冲区
	pHandleData->ReleaseBuffer(pHandleData->AcquireBuffer(IO_OPT_TYPE::SEND_POSTED));
	return true;
}

bool CIOCPServer::postRecv(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData)
{
	pIoData->opType = IO_OPT_TYPE::RECV_POSTED;
	DWORD dwFlag = 0;
	if (::WSARecv(pHandleData->hClient, &pIoData->wsaBuffer, 1, NULL, &dwFlag, &pIoData->overlapped, NULL) != 0)
	{
		return ::WSAGetLastError() == WSA_IO_PENDING;
	}
	return true;
}

bool CIOCPServer::onRecvPosted(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData, DWORD dwLen)
{
	auto pSendData = pHandleData->AcquireBuffer(IO_OPT_TYPE::SEND_POSTED);
	pSendData->SetPayload(pIoData->buffer, dwLen);
	return postRecv(pHandleData, pIoData) && postSend(pHandleData, pSendData);
}

bool CIOCPServer::onSendPosted(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData, DWORD dwLen)
{
	return true;
}

bool CIOCPServer::postSend(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData)
{
	pIoData->opType = IO_OPT_TYPE::SEND_POSTED;
	DWORD dwFlag = 0;
	if (::WSASend(pHandleData->hClient, &pIoData->wsaBuffer, 1, NULL, dwFlag, &pIoData->overlapped, NULL) != 0)
	{
		return ::WSAGetLastError() == WSA_IO_PENDING;
	}
	return true;
}

bool CIOCPServer::handleError(LPPER_HANDLE_DATA& pHandleData, DWORD dwErr)
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

	delete pHandleData;
	pHandleData = NULL;
	return bResult;
}

DWORD WINAPI CIOCPServer::associateWithIOCP(_In_ LPVOID lpParameter)
{
	auto fnLambda = reinterpret_cast<std::function<void()>*>(lpParameter);
	(*fnLambda)();
	delete fnLambda;
	return 0;
}

_PER_IO_DATA::_PER_IO_DATA(IO_OPT_TYPE opType)
{
	Reset();
	this->opType = opType;
}

void _PER_IO_DATA::Reset()
{
	::ZeroMemory(this, sizeof(*this));
	wsaBuffer.buf = buffer;
	wsaBuffer.len = _countof(buffer);
	opType = IO_OPT_TYPE::NONE_POSTED;
}

bool _PER_IO_DATA::SetPayload(const char* src, size_t length)
{
	static_assert(sizeof(this->buffer) == HTTPPROXY_BUFFER_LENGTH, "assert error");
	::ZeroMemory(this->buffer, HTTPPROXY_BUFFER_LENGTH);

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
	hClient = INVALID_SOCKET;
	::ZeroMemory(&clientAddr, sizeof(SOCKADDR_STORAGE));
}

_PER_HANDLE_DATA::~_PER_HANDLE_DATA()
{
	if (hClient != INVALID_SOCKET)
	{
		::closesocket(hClient);
		hClient = INVALID_SOCKET;
	}
}

LPPER_IO_DATA _PER_HANDLE_DATA::AcquireBuffer(IO_OPT_TYPE bufferType)
{
	std::lock_guard<std::mutex> guard(ioGuard);
	if (!freeIoList.empty())
	{
		auto data = freeIoList.back();
		freeIoList.pop_back();
		usedIoList.insert({ reinterpret_cast<ULONG_PTR>(data.get()), data });

		data->Reset();
		data->opType = bufferType;
		return data.get();
	}
	else
	{
		auto data = std::make_shared<PER_IO_DATA>(bufferType);
		usedIoList.insert({ reinterpret_cast<ULONG_PTR>(data.get()), data });
		return data.get();
	}
}

void _PER_HANDLE_DATA::ReleaseBuffer(LPPER_IO_DATA data)
{
	std::lock_guard<std::mutex> guard(ioGuard);
	auto itUsed = usedIoList.find(reinterpret_cast<ULONG_PTR>(data));
	if (itUsed != usedIoList.end())
	{
		freeIoList.push_back(itUsed->second);
		usedIoList.erase(itUsed);
	}
	else
	{
		assert(false && static_cast<int>(data->opType));
	}
}
