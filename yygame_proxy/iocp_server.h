#pragma once
#include <WinSock2.h>

constexpr int HTTPPROXY_BUFFER_LENGTH = ((2 << 10) - 1);

enum class IO_OPT_TYPE
{
	NONE_POSTED,
	ACCEPT_POSTED,
	RECV_POSTED,
	SEND_POSTED,
};

class SocketContext;

class IOContext
{
public:
	IOContext(SocketContext *pSocketCtx, SOCKET& inSocket, SOCKET& outSocket);
	void ResetBuffer();
	bool SetPayload(const char* src, DWORD dwLen);
	bool PostSend(DWORD dwLength);
	bool PostRecv();
	void Close();
	SocketContext* GetSocketContext();

public:
	WSAOVERLAPPED overlapped;
	char buffer[HTTPPROXY_BUFFER_LENGTH + 1];
	IO_OPT_TYPE opType;

private:
	SocketContext* pSocketCtx;
	SOCKET& hInSocket;
	SOCKET& hOutSocket;
};
typedef IOContext *LPIOContext;

class SocketContext
{
public:
	bool Init(HANDLE hIocp);
	bool Close(LPIOContext pIO);
	bool PostAccept(SOCKET hListen);
	bool CompleteAccept(SOCKET hListen, const SOCKADDR_IN *addr);

	LPIOContext GetUserToServerContext();
	LPIOContext GetServerToUserContext();

	unsigned long GetCustomData() const;
	void SetCustomData(unsigned long x);

	~SocketContext();
	SocketContext();

private:
	void close();

private:
	SOCKET userSocket;
	SOCKET serverSocket;

	IOContext usr2SrvCtx;
	IOContext srv2UsrCtx;
	std::atomic_int connCount;
	std::atomic_ulong data;
};
typedef SocketContext *LPSocketContext;

class CIOCPServer
{
public:
	DWORD StartServer(unsigned short port);
	void StopServer();

	HANDLE AssociateWithServer(HANDLE hFile, ULONG_PTR CompletionKey, DWORD NumberOfConcurrentThreads);
	void CloseIoSocket(LPIOContext pIo);

	CIOCPServer();
	virtual ~CIOCPServer();

private:
	DWORD init();
	void uninit();
	bool handleAccept(LPIOContext pIoCtx, DWORD dwBytesRecv);
	void iocpWorker();
	bool handleError(LPIOContext pIoCtx, DWORD dwErr);
	void handleDisconnected(LPSocketContext pSocketCtx, LPIOContext pIoCtx);
	static DWORD WINAPI workforAccepted(_In_ LPVOID lpParameter);
	LPSocketContext prepareSocket();
	void deleteSocket(LPSocketContext pCtx);
	void clearSockets();

protected:
	virtual bool onAcceptPosted(LPSocketContext pSocketCtx, LPIOContext pIoCtx, DWORD dwLen, SOCKADDR_IN* peerAddr);
	virtual bool onServerConnectPosted(LPSocketContext pSocketCtx, DWORD dwLen, bool success);
	virtual bool onRecvPosted(LPSocketContext pSocketCtx, LPIOContext pIoCtx, DWORD dwLen);
	virtual bool onSendPosted(LPSocketContext pSocketCtx, LPIOContext pIoCtx, DWORD dwLen);
	virtual void onServerError(LPIOContext pIoCtx, DWORD dwErr);
	virtual void onDisconnected(LPSocketContext pSocketCtx, LPIOContext pIoCtx);

private:
	HANDLE m_hIOCP;
	std::vector<std::shared_ptr<std::thread>> m_workers;
	std::set<LPSocketContext> m_connTable;
	std::recursive_mutex m_connTableGuard;
	std::atomic_bool m_bRun;
	SOCKET m_hListenSocket;
	HANDLE m_hStopEvt;
};
