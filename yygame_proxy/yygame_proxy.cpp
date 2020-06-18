// yygame_proxy.cpp : 定义应用程序的入口点。
//

#include "framework.h"
#include <winsock2.h>
#include "yygame_proxy.h"
#include "iocp_server.h"


int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    ::WSADATA data;
    ::WSAStartup(MAKEWORD(2, 2), &data);
    CIOCPServer proxy;
    proxy.StartServer(1082);
    ::WSACleanup();

    return 0;
}

