#pragma once

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
	SOCKET hPeer;
	SOCKADDR_STORAGE peerAddr;
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
	bool onAcceptPosted(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData);
	bool postRecv(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData);
	bool onRecvPosted(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData, DWORD dwLen);
	bool onSendPosted(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData, DWORD dwLen);
	bool postSend(LPPER_HANDLE_DATA pHandleData, LPPER_IO_DATA pIoData);
	bool handleError(LPPER_HANDLE_DATA &pHandleData, DWORD dwErr);
	static DWORD WINAPI associateWithIOCP(_In_ LPVOID lpParameter);

	// http tunnel forward
	LPPER_HANDLE_DATA getTunnelHandle(const LPPER_HANDLE_DATA pKey);
	void putTunnelHandle(const LPPER_HANDLE_DATA pKey, const LPPER_HANDLE_DATA pData);
	DWORD createTunnelHandle(const SOCKADDR_IN& peerAddr, LPPER_HANDLE_DATA* pData);
	void destroyTunnelHandle(const LPPER_HANDLE_DATA pKey);
	void removeTunnelHandle(const LPPER_HANDLE_DATA pKey, bool bAll = true);

private:
	HANDLE m_hIOCP;
	std::vector<std::shared_ptr<std::thread>> m_workers;
	std::map<LPPER_HANDLE_DATA, LPPER_HANDLE_DATA> m_tunnelTable;
	std::map<std::string, ULONG> m_dnsCache; // domain ip
	std::atomic_bool m_bRun;
	std::recursive_mutex m_tunnelGuard;
};
