#pragma once
#include <WinSock2.h>

constexpr int HTTPPROXY_BUFFER_LENGTH = ((4 << 10) - 1);

enum class IO_OPT_TYPE
{
	NONE_POSTED,
	ACCEPT_POSTED,
	RECV_POSTED,
	SEND_POSTED,
	CLOSE_POSTED,
};

typedef struct _PER_IO_DATA
{
	OVERLAPPED overlapped;
	WSABUF wsaBuffer;
	char buffer[HTTPPROXY_BUFFER_LENGTH + 1];
	IO_OPT_TYPE opType;

	explicit _PER_IO_DATA(IO_OPT_TYPE);
	void Reset(IO_OPT_TYPE optType = IO_OPT_TYPE::NONE_POSTED);
	bool SetPayload(const char* src, size_t length);
} PER_IO_DATA, * LPPER_IO_DATA;

typedef struct _PER_HANDLE_DATA
{
	SOCKET hPeer;
	SOCKADDR_STORAGE peerAddr;
	std::atomic_ulong uUser;

	LPPER_IO_DATA AcquireBuffer(IO_OPT_TYPE bufferType);
	void ReleaseBuffer(LPPER_IO_DATA data);
	void Destroy();
	static _PER_HANDLE_DATA* Create(SOCKET hSock, const SOCKADDR_STORAGE* pAddr, 
		size_t length, unsigned long user = 0UL);
	~_PER_HANDLE_DATA();

private:
	_PER_HANDLE_DATA();

	std::map<ULONG_PTR, std::shared_ptr<PER_IO_DATA>> usedIoList;
	std::vector<std::shared_ptr<PER_IO_DATA>> freeIoList;
	std::mutex ioGuard;
} PER_HANDLE_DATA, * LPPER_HANDLE_DATA;

class CIOCPServer
{
public:
	DWORD StartServer(unsigned short port);
	void StopServer();

	bool PostRecv(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData);
	bool PostSend(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData);
	bool PostClose(LPPER_HANDLE_DATA pHandleData);
	HANDLE AssociateWithServer(HANDLE hFile, ULONG_PTR CompletionKey, DWORD NumberOfConcurrentThreads);

	CIOCPServer();
	virtual ~CIOCPServer();

private:
	DWORD init();
	void uninit();
	void handleAccept(SOCKET hListen);
	void iocpWorker();
	bool handleError(LPPER_HANDLE_DATA& pHandleData, DWORD dwErr);
	static DWORD WINAPI associateWithIOCP(_In_ LPVOID lpParameter);

protected:
	virtual bool onAcceptPosted(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData);
	virtual bool onRecvPosted(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData, DWORD dwLen);
	virtual bool onSendPosted(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData, DWORD dwLen);
	virtual bool onClosePosted(LPPER_HANDLE_DATA pHandleData);
	virtual void onServerError(LPPER_HANDLE_DATA pHandleData, DWORD dwErr);
	virtual void onDisconnected(LPPER_HANDLE_DATA pHandleData);

private:
	HANDLE m_hIOCP;
	std::vector<std::shared_ptr<std::thread>> m_workers;
	std::atomic_bool m_bRun;
};
