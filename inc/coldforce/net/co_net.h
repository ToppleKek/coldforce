#ifndef CO_NET_H_INCLUDED
#define CO_NET_H_INCLUDED

#include <coldforce/core/co.h>

#ifdef CO_OS_WIN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

//---------------------------------------------------------------------------//
// platform
//---------------------------------------------------------------------------//

#ifdef _MSC_VER
#   ifdef CO_NET_EXPORTS
#       define CO_NET_API  __declspec(dllexport)
#   else
#       define CO_NET_API  __declspec(dllimport)
#   endif
#else
#   define CO_NET_API
#endif

CO_EXTERN_C_BEGIN

//---------------------------------------------------------------------------//
//---------------------------------------------------------------------------//

CO_NET_API bool co_net_setup(void);
CO_NET_API void co_net_cleanup(void);

//---------------------------------------------------------------------------//
//---------------------------------------------------------------------------//

CO_EXTERN_C_END

#endif // CO_NET_H_INCLUDED
