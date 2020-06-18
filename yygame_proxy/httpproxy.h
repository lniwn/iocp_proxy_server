#pragma once

#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <map>
#include <vector>

constexpr int HTTPPROXY_BUFFER_LENGTH = (8 << 10);

enum class IO_OPT_TYPE
{
	NONE_POSTED,
	ACCEPT_POSTED,
	RECV_POSTED,
	SEND_POSTED,
};

typedef struct _PER_IO_DATA
{
	OVERLAPPED overlapped;
	WSABUF wsaBuffer;
	char buffer[HTTPPROXY_BUFFER_LENGTH];
	IO_OPT_TYPE opType;

	explicit _PER_IO_DATA(IO_OPT_TYPE);
	void Reset();
	bool SetPayload(const char* src, size_t length);
} PER_IO_DATA, * LPPER_IO_DATA;

typedef struct _PER_HANDLE_DATA
{
	SOCKET hClient;
	SOCKADDR_STORAGE clientAddr;
	std::map<ULONG_PTR, std::shared_ptr<PER_IO_DATA>> usedIoList;
	std::vector<std::shared_ptr<PER_IO_DATA>> freeIoList;
	std::mutex ioGuard;

	_PER_HANDLE_DATA();
	~_PER_HANDLE_DATA();
	LPPER_IO_DATA AcquireBuffer(IO_OPT_TYPE bufferType);
	void ReleaseBuffer(LPPER_IO_DATA data);
} PER_HANDLE_DATA, * LPPER_HANDLE_DATA;

class CIOCPServer
{
public:
	DWORD StartServer(unsigned short port);
	void StopServer();

	CIOCPServer();

private:
	DWORD init();
	void uninit();
	void handleAccept(SOCKET hListen);
	void iocpWorker();
	bool doAccept(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData);
	bool postRecv(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData);
	bool doRecv(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData);
	bool postSend(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData);
	bool handleError(LPPER_HANDLE_DATA &pHandleData, DWORD dwErr);
	static DWORD WINAPI associateWithIOCP(_In_ LPVOID lpParameter);

private:
	HANDLE m_hIOCP;
	std::vector<std::shared_ptr<std::thread>> m_workers;
	std::atomic_bool m_bRun;
};
