/*
 * Copyright 2010-2011 Branimir Karadzic. All rights reserved.
 * License: http://www.opensource.org/licenses/BSD-2-Clause
 */

#include <bnet.h>
#include <bx/bx.h>

using namespace bx;

#define BNET_CONFIG_DEBUG 0
extern void dbgPrintf(const char* _format, ...);
extern void dbgPrintfData(const void* _data, uint32_t _size, const char* _format, ...);

#if BNET_CONFIG_DEBUG
#	undef BX_TRACE
#	define BX_TRACE(_format, ...) \
				do { \
					dbgPrintf(BX_FILE_LINE_LITERAL "BNET " _format "\n", ##__VA_ARGS__); \
				} while(0)

#	undef BX_CHECK
#	define BX_CHECK(_condition, _format, ...) \
				do { \
					if (!(_condition) ) \
					{ \
						BX_TRACE(BX_FILE_LINE_LITERAL _format, ##__VA_ARGS__); \
					} \
				} while(0)
#endif // 0

#ifndef BNET_CONFIG_OPENSSL
#	define BNET_CONFIG_OPENSSL (BX_PLATFORM_WINDOWS && BX_COMPILER_MSVC) || BX_PLATFORM_ANDROID
#endif // BNET_CONFIG_OPENSSL

#ifndef BNET_CONFIG_DEBUG
#	define BNET_CONFIG_DEBUG 0
#endif // BNET_CONFIG_DEBUG

#ifndef BNET_CONFIG_CONNECT_TIMEOUT_SECONDS
#	define BNET_CONFIG_CONNECT_TIMEOUT_SECONDS 5
#endif // BNET_CONFIG_CONNECT_TIMEOUT_SECONDS

#ifndef BNET_CONFIG_MAX_INCOMING_BUFFER_SIZE
#	define BNET_CONFIG_MAX_INCOMING_BUFFER_SIZE (64<<10)
#endif // BNET_CONFIG_MAX_INCOMING_BUFFER_SIZE

#if BX_PLATFORM_WINDOWS || BX_PLATFORM_XBOX360
#	if BX_PLATFORM_WINDOWS
#		define _WIN32_WINNT 0x0501
#		include <winsock2.h>
#		include <ws2tcpip.h>
#	elif BX_PLATFORM_XBOX360
#		include <xtl.h>
#	endif
#	define socklen_t int32_t
#	define EWOULDBLOCK WSAEWOULDBLOCK
#	define EINPROGRESS WSAEINPROGRESS
#elif BX_PLATFORM_LINUX || BX_PLATFORM_ANDROID
#	include <memory.h>
#	include <errno.h> // errno
#	include <fcntl.h>
#	include <netdb.h>
#	include <unistd.h>
#	include <sys/socket.h>
#	include <sys/time.h> // gettimeofday
#	include <arpa/inet.h> // inet_addr
#	include <netinet/in.h>
#	include <netinet/tcp.h>
	typedef int SOCKET;
	typedef linger LINGER;
	typedef hostent HOSTENT;
	typedef in_addr IN_ADDR;
	
#	define SOCKET_ERROR (-1)
#	define INVALID_SOCKET (-1)
#	define closesocket close
#elif BX_PLATFORM_NACL
#	include <errno.h> // errno
#	include <stdio.h> // sscanf
#	include <string.h>
#	include <sys/time.h> // gettimeofday
#	include <sys/types.h> // fd_set
#	include "nacl_socket.h"
#endif // BX_PLATFORM_

#include <new> // placement new

#if BNET_CONFIG_OPENSSL
#	include <openssl/err.h>
#	include <openssl/ssl.h>
#else
#	define SSL_CTX void
#	define X509 void
#	define EVP_PKEY void
#endif // BNET_CONFIG_OPENSSL

#if BNET_CONFIG_OPENSSL && BNET_CONFIG_DEBUG

static void getSslErrorInfo()
{
	BIO* bio = BIO_new_fp(stderr, BIO_NOCLOSE);
	ERR_print_errors(bio);
	BIO_flush(bio);
	BIO_free(bio);
}

#	define TRACE_SSL_ERROR() getSslErrorInfo()
#else
#	define TRACE_SSL_ERROR()
#endif // BNET_CONFIG_OPENSSL && BNET_CONFIG_DEBUG

#include <bx/blockalloc.h>
#include <bx/ringbuffer.h>
#include <bx/timer.h>

#include <list>

BX_NO_INLINE void* bnetReallocStub(void* _ptr, size_t _size)
{
	void* ptr = ::realloc(_ptr, _size);
	BX_CHECK(NULL != ptr, "Out of memory!");
	//	BX_TRACE("alloc %d, %p", _size, ptr);
	return ptr;
}

BX_NO_INLINE void bnetFreeStub(void* _ptr)
{
	// 	BX_TRACE("free %p", _ptr);
	::free(_ptr);
}

namespace bnet
{
	static reallocFn s_realloc = bnetReallocStub;
	static freeFn s_free = bnetFreeStub;

	template<typename Ty> class FreeList
	{
	public:
		FreeList(uint16_t _max)
		{
			uint32_t size = BlockAlloc::minElementSize > sizeof(Ty) ? BlockAlloc::minElementSize : sizeof(Ty);
			m_memBlock = s_realloc(NULL, _max*size);
			m_allocator = BlockAlloc(m_memBlock, _max, size);
		}

		~FreeList()
		{
			s_free(m_memBlock);
		}

		uint16_t getIndex(Ty* _obj) const
		{
			return m_allocator.getIndex(_obj);
		}

		Ty* create()
		{
			Ty* obj = static_cast<Ty*>(m_allocator.alloc() );
			obj = ::new (obj) Ty;
			return obj;
		}

		template<typename Arg0> Ty* create(Arg0 _a0)
		{
			Ty* obj = static_cast<Ty*>(m_allocator.alloc() );
			obj = ::new (obj) Ty(_a0);
			return obj;
		}

		template<typename Arg0, typename Arg1> Ty* create(Arg0 _a0, Arg1 _a1)
		{
			Ty* obj = static_cast<Ty*>(m_allocator.alloc() );
			obj = ::new (obj) Ty(_a0, _a1);
			return obj;
		}

		template<typename Arg0, typename Arg1, typename Arg2> Ty* create(Arg0 _a0, Arg1 _a1, Arg2 _a2)
		{
			Ty* obj = static_cast<Ty*>(m_allocator.alloc() );
			obj = ::new (obj) Ty(_a0, _a1, _a2);
			return obj;
		}

		void destroy(Ty* _obj)
		{
			_obj->~Ty();
			m_allocator.free(_obj);
		}

		Ty* getFromIndex(uint16_t _index)
		{
			Ty* obj = static_cast<Ty*>(m_allocator.getFromIndex(_index) );
			return obj;
		}

	private:
		void* m_memBlock;
		BlockAlloc m_allocator;
	};

	int g_errno;

	int getLastError()
	{
#if BX_PLATFORM_WINDOWS || BX_PLATFORM_XBOX360
		g_errno = WSAGetLastError();
		return g_errno;
#elif BX_PLATFORM_LINUX || BX_PLATFORM_NACL || BX_PLATFORM_ANDROID
		g_errno = errno;
		return errno;
#else
		return 0;
#endif // BX_PLATFORM_
	}

	bool isInProgress()
	{
		return EINPROGRESS == getLastError();
	}

	bool isWouldBlock()
	{
		return EWOULDBLOCK == getLastError();
	}

	void setNonBlock(SOCKET _socket)
	{
#if BX_PLATFORM_WINDOWS || BX_PLATFORM_XBOX360
		unsigned long opt = 1 ;
		::ioctlsocket(_socket, FIONBIO, &opt);
#elif BX_PLATFORM_LINUX || BX_PLATFORM_ANDROID
		::fcntl(_socket, F_SETFL, O_NONBLOCK);
#endif // BX_PLATFORM_
	}

	class RecvRingBuffer
	{
	public:
		RecvRingBuffer(RingBufferControl& _control, char* _buffer)
			: m_control(_control)
			, m_write(_control.m_current)
			, m_reserved(0)
			, m_buffer(_buffer)
		{
		}
		
		~RecvRingBuffer()
		{
		}
		
		int recv(SOCKET _socket)
		{
			m_reserved += m_control.reserve(-1);
			uint32_t end = (m_write + m_reserved) % m_control.m_size;
			uint32_t wrap = end < m_write ? m_control.m_size - m_write : m_reserved;
			char* to = &m_buffer[m_write];
			
			int bytes = ::recv(_socket
							, to
							, wrap
							, 0
							);
			
			if (0 < bytes)
			{
				m_write += bytes;
				m_write %= m_control.m_size;
				m_reserved -= bytes;
				m_control.commit(bytes);
			}
			
			return bytes;
		}

#if BNET_CONFIG_OPENSSL
		int recv(SSL* _ssl)
		{
			m_reserved += m_control.reserve(-1);
			uint32_t end = (m_write + m_reserved) % m_control.m_size;
			uint32_t wrap = end < m_write ? m_control.m_size - m_write : m_reserved;
			char* to = &m_buffer[m_write];

			int bytes = SSL_read(_ssl
							, to
							, wrap
							);

			if (0 < bytes)
			{
				m_write += bytes;
				m_write %= m_control.m_size;
				m_reserved -= bytes;
				m_control.commit(bytes);
			}

			return bytes;
		}
#endif // BNET_CONFIG_OPENSSL
		
	private:
		RecvRingBuffer();
		RecvRingBuffer(const RecvRingBuffer&);
		void operator=(const RecvRingBuffer&);

		RingBufferControl& m_control;
		uint32_t m_write;
		uint32_t m_reserved;
		char* m_buffer;
	};

	class MessageQueue
	{
	public:
		MessageQueue()
		{
		}

		~MessageQueue()
		{
		}

		void push(Message* _msg)
		{
			m_queue.push_back(_msg);
		}

		Message* peek()
		{
			if (!m_queue.empty() )
			{
				return m_queue.front();
			}

			return NULL;
		}

		Message* pop()
		{
			if (!m_queue.empty() )
			{
				Message* msg = m_queue.front();
				m_queue.pop_front();
				return msg;
			}

			return NULL;
		}

	private:
		std::list<Message*> m_queue;
	};

	uint16_t ctxAccept(uint16_t _listenHandle, SOCKET _socket, uint32_t _ip, uint16_t _port, bool _raw, X509* _cert, EVP_PKEY* _key);
	void ctxPush(uint16_t _handle, MessageId::Enum _id);
	void ctxPush(Message* _msg);
	Message* ctxAlloc(uint16_t _handle, uint16_t _size, bool _incoming = false);
	void ctxFree(Message* _msg);

	static void setSockOpts(SOCKET _socket)
	{
		int result;

		int win = 256<<10;
		result = ::setsockopt(_socket, SOL_SOCKET, SO_RCVBUF, (char*)&win, sizeof(win) );
		result = ::setsockopt(_socket, SOL_SOCKET, SO_SNDBUF, (char*)&win, sizeof(win) );

		int noDelay = 1;
		result = ::setsockopt(_socket, IPPROTO_TCP, TCP_NODELAY, (char*)&noDelay, sizeof(noDelay) );
	}

	struct Internal
	{
		enum Enum
		{
			None,
			Disconnect,
			Notify,

			Count,
		};
	};

	class Connection
	{
	public:
		Connection(uint16_t _denseIndex)
			: m_socket(INVALID_SOCKET)
			, m_denseIndex(_denseIndex)
			, m_handle(invalidHandle)
			, m_incomingBuffer( (uint8_t*)s_realloc(NULL, BNET_CONFIG_MAX_INCOMING_BUFFER_SIZE) )
			, m_incoming(BNET_CONFIG_MAX_INCOMING_BUFFER_SIZE)
			, m_recv(m_incoming, (char*)m_incomingBuffer)
			, m_len(-1)
			, m_raw(false)
#if BNET_CONFIG_OPENSSL
			, m_ssl(NULL)
			, m_sslHandshake(false)
#endif // BNET_CONFIG_OPENSSL
		{
		}

		~Connection()
		{
			s_free(m_incomingBuffer);
		}

		void init(uint16_t _handle, bool _raw)
		{
			m_handle = _handle;
			m_connected = false;
			m_connectTimeout = getHPCounter() + getHPFrequency()*BNET_CONFIG_CONNECT_TIMEOUT_SECONDS;
			m_len = -1;
			m_raw = _raw;
		}

		void connect(uint16_t _handle, uint32_t _ip, uint16_t _port, bool _raw, SSL_CTX* _sslCtx)
		{
			init(_handle, _raw);

			m_socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (INVALID_SOCKET == m_socket)
			{
				ctxPush(m_handle, MessageId::ConnectFailed);
				return;
			}

			setSockOpts(m_socket);
			setNonBlock(m_socket);

			m_addr.sin_family = AF_INET;
			m_addr.sin_addr.s_addr = htonl(_ip);
			m_addr.sin_port = htons(_port);

			union
			{
				sockaddr* sa;
				sockaddr_in* sain;
			} saintosa;
			saintosa.sain = &m_addr;
			
			int err = ::connect(m_socket, saintosa.sa, sizeof(m_addr) );

			if (0 != err
			&&  !(isInProgress() || isWouldBlock() ) )
			{
				::closesocket(m_socket);
				m_socket = INVALID_SOCKET;

				ctxPush(m_handle, MessageId::ConnectFailed);
				return;
			}

#if BNET_CONFIG_OPENSSL
			if (NULL != _sslCtx)
			{
				m_sslHandshake = true;
				m_ssl = SSL_new(_sslCtx);
				SSL_set_fd(m_ssl, (int)m_socket);
//				BIO* bio = BIO_new_socket(m_socket, BIO_NOCLOSE);
//				SSL_set_bio(m_ssl, bio, bio);
				SSL_set_connect_state(m_ssl);
				SSL_write(m_ssl, NULL, 0);
			}
#endif // BNET_CONFIG_OPENSSL
		}

		void accept(uint16_t _handle, uint16_t _listenHandle, SOCKET _socket, uint32_t _ip, uint16_t _port, bool _raw, SSL_CTX* _sslCtx, X509* _cert, EVP_PKEY* _key)
		{
			init(_handle, _raw);
			
			m_socket = _socket;
			Message* msg = ctxAlloc(m_handle, 9, true);
			msg->data[0] = MessageId::IncomingConnection;
			*( (uint16_t*)&msg->data[1]) = _listenHandle;
			*( (uint32_t*)&msg->data[3]) = _ip;
			*( (uint16_t*)&msg->data[7]) = _port;
			ctxPush(msg);

#if BNET_CONFIG_OPENSSL
			if (NULL != _sslCtx)
			{
				m_sslHandshake = true;
				m_ssl = SSL_new(_sslCtx);
				int result;
				result = SSL_use_certificate(m_ssl, _cert);
				result = SSL_use_PrivateKey(m_ssl, _key);
				result = SSL_set_fd(m_ssl, (int)m_socket);
				SSL_set_accept_state(m_ssl);
				SSL_read(m_ssl, NULL, 0);
			}
#endif // BNET_CONFIG_OPENSSL
		}

		void disconnect(bool _lost = false)
		{
			if (INVALID_SOCKET != m_socket)
			{
				::closesocket(m_socket);
				m_socket = INVALID_SOCKET;
			}

#if BNET_CONFIG_OPENSSL
			if (m_ssl)
			{
				SSL_shutdown(m_ssl);
				SSL_free(m_ssl); 
				m_ssl = NULL;
			}
#endif // BNET_CONFIG_OPENSSL

			for (Message* msg = m_outgoing.pop(); NULL != msg; msg = m_outgoing.pop() )
			{
				free(msg);
			}

			if (_lost)
			{
				ctxPush(m_handle, MessageId::LostConnection);
			}
		}

		void send(Message* _msg)
		{
			if (INVALID_SOCKET != m_socket)
			{
				m_outgoing.push(_msg);
				update();
			}
		}

		void read(WriteRingBuffer& _out, uint32_t _len)
		{
			ReadRingBuffer incoming(m_incoming, (char*)m_incomingBuffer, _len);
			_out.write(incoming, _len);
			incoming.end();
		}

		void read(uint32_t _len)
		{
			m_incoming.consume(_len);
		}

		void read(char* _data, uint32_t _len)
		{
			ReadRingBuffer incoming(m_incoming, (char*)m_incomingBuffer, _len);
			incoming.read(_data, _len);
			incoming.end();
		}

		void peek(char* _data, uint32_t _len)
		{
			ReadRingBuffer incoming(m_incoming, (char*)m_incomingBuffer, _len);
			incoming.read(_data, _len);
		}

		void reassembleMessage()
		{
			if (m_raw)
			{
				uint32_t available = uint32_min(m_incoming.available(), maxMessageSize-1);

				if (0 < available)
				{
					Message* msg = ctxAlloc(m_handle, available+1, true);
					msg->data[0] = MessageId::RawData;
					read( (char*)&msg->data[1], available);
					ctxPush(msg);
				}
			}
			else
			{
				uint32_t available = uint32_min(m_incoming.available(), maxMessageSize);

				while (0 < available)
				{
					if (-1 == m_len)
					{
						if (2 > available)
						{
							return;
						}
						else
						{
							uint16_t len;
							read((char*)&len, 2);
							m_len = len;
						}
					}
					else
					{
						if (m_len > int(available) )
						{
							return;
						}
						else
						{
							Message* msg = ctxAlloc(m_handle, m_len, true);
							read( (char*)msg->data, m_len);
							uint8_t id = msg->data[0];
							msg->data[0] = id < MessageId::UserDefined ? MessageId::UserDefined : id;
							ctxPush(msg);
							
							m_len = -1;
						}
					}
					
					available = uint32_min(m_incoming.available(), maxMessageSize);
				}
			}
		}

		bool updateConnect()
		{
			if (m_connected)
			{
				return true;
			}

			uint64_t now = getHPCounter();
			if (now > m_connectTimeout)
			{
				BX_TRACE("Disconnect - Connect timeout.");
				ctxPush(m_handle, MessageId::ConnectFailed);
				disconnect(false);
				return false;
			}

			fd_set rfds;
			FD_ZERO(&rfds);
			fd_set wfds;
			FD_ZERO(&wfds);
			FD_SET(m_socket, &rfds);
			FD_SET(m_socket, &wfds);

			timeval timeout;
			timeout.tv_sec = 0;
			timeout.tv_usec = 0;

			int result = ::select(2, &rfds, &wfds, NULL, &timeout);
			m_connected = 0 < result;

			return m_connected;
		}

		void update()
		{
			if (INVALID_SOCKET != m_socket
			&&  updateConnect()
			&&  updateSsl() )
			{
				int bytes;

#if BNET_CONFIG_OPENSSL
				if (NULL != m_ssl)
				{
					bytes = m_recv.recv(m_ssl);
				}
				else
#endif // BNET_CONFIG_OPENSSL
				{
					bytes = m_recv.recv(m_socket);
				}

				if (1 > bytes
				&&  !isWouldBlock() )
				{
					if (0 == bytes)
					{
						BX_TRACE("Disconnect - Host closed connection.");
					}
					else
					{
						TRACE_SSL_ERROR();
						BX_TRACE("Disconnect - Receive failed. %d", getLastError() );
					}

					disconnect(true);
					return;
				}

				reassembleMessage();

#if BNET_CONFIG_OPENSSL
				if (!m_sslHandshake)
#endif // BNET_CONFIG_OPENSSL
				{
					if (m_raw)
					{
						for (Message* msg = m_outgoing.peek(); NULL != msg; msg = m_outgoing.peek() )
						{
							Internal::Enum id = Internal::Enum(*(msg->data - 2) );
							if (Internal::None != id
							&&  !processInternal(id, msg) )
							{
								return;
							}
							else if (!send( (char*)msg->data, msg->size) )
							{
								return;
							}

							free(m_outgoing.pop() );
						}
					}
					else
					{
						for (Message* msg = m_outgoing.peek(); NULL != msg; msg = m_outgoing.peek() )
						{
							Internal::Enum id = Internal::Enum(*(msg->data - 2) );
							*( (uint16_t*)msg->data - 1) = msg->size;
							if (Internal::None != id
							&&  !processInternal(id, msg) )
							{
								return;
							}
							else if (!send( (char*)msg->data - 2, msg->size+2) )
							{
								return;
							}

							free(m_outgoing.pop() );
						}
					}
				}
			}
		}

		uint16_t getHandle() const
		{
			return m_handle;
		}

		void setDenseIndex(uint16_t _denseIndex)
		{
			m_denseIndex = _denseIndex;
		}

		uint16_t getDenseIndex() const
		{
			return m_denseIndex;
		}

		bool isConnected() const
		{
			return INVALID_SOCKET != m_socket;
		}

	private:
		bool processInternal(Internal::Enum _id, Message* _msg)
		{
			switch (_id)
			{
			case Internal::Disconnect:
				disconnect(false);
				return false;

			case Internal::Notify:
				{
					Message* msg = ctxAlloc(_msg->handle, _msg->size+1, true);
					msg->data[0] = MessageId::Notify;
					memcpy(&msg->data[1], _msg->data, _msg->size);
					ctxPush(msg);
				}
				return true;
			}

			BX_CHECK(false, "You shoud not be here!");
			return true;
		}

		bool updateSsl()
		{
#if BNET_CONFIG_OPENSSL
			if (NULL != m_ssl)
			{
				if (m_sslHandshake)
				{
					int err = SSL_do_handshake(m_ssl);

					if (1 == err)
					{
						m_sslHandshake = false;
#	if BNET_CONFIG_DEBUG
						X509* cert = SSL_get_peer_certificate(m_ssl);
						BX_TRACE("Server certificate:");

						char* temp;
						temp = X509_NAME_oneline(X509_get_subject_name(cert), 0, 0);
						BX_TRACE("\t subject: %s", temp);
						OPENSSL_free(temp);

						temp = X509_NAME_oneline(X509_get_issuer_name(cert), 0, 0);
						BX_TRACE("\t issuer: %s", temp);
						OPENSSL_free(temp);

						X509_free(cert);
#	endif // BNET_CONFIG_DEBUG

						long result = SSL_get_verify_result(m_ssl);
						if (X509_V_OK != result)
						{
							BX_TRACE("Disconnect - SSL verify failed %d.", result);
							ctxPush(m_handle, MessageId::ConnectFailed);
							disconnect(false);
							return false;
						}

						BX_TRACE("SSL connection using %s", SSL_get_cipher(m_ssl) );
					}
					else
					{
						int sslError = SSL_get_error(m_ssl, err);
						switch (sslError)
						{
						case SSL_ERROR_WANT_READ:
							SSL_read(m_ssl, NULL, 0);
							break;

						case SSL_ERROR_WANT_WRITE:
							SSL_write(m_ssl, NULL, 0);
							break;

						default:
							TRACE_SSL_ERROR();
							break;
						}
					}
				}
			}
#endif // BNET_CONFIG_OPENSSL

			return true;
		}

		bool send(const char* _data, uint32_t _len)
		{
			int bytes;
			uint32_t offset = 0;
			do
			{
#if BNET_CONFIG_OPENSSL
				if (NULL != m_ssl)
				{
					bytes = SSL_write(m_ssl
						, &_data[offset]
						, _len
						);
				}
				else
#endif // BNET_CONFIG_OPENSSL
				{
					bytes = ::send(m_socket
						, &_data[offset]
						, _len
						, 0
						);
				}
				
				if (0 > bytes)
				{
					if (-1 == bytes
					&&  !isWouldBlock() )
					{
						BX_TRACE("Disconnect - Send failed.");
						disconnect(true);
						return false;
					}
				}
				else
				{
					_len -= bytes;
					offset += bytes;
				}
				
			} while (0 < _len);

			return true;
		}

		uint64_t m_connectTimeout;
		SOCKET m_socket;
		uint16_t m_denseIndex;
		uint16_t m_handle;
		uint8_t* m_incomingBuffer;
		RingBufferControl m_incoming;
		RecvRingBuffer m_recv;
		MessageQueue m_outgoing;
#if BNET_CONFIG_OPENSSL
		SSL* m_ssl;
		bool m_sslHandshake;
#endif // BNET_CONFIG_OPENSSL

		sockaddr_in m_addr;
		int m_len;
		bool m_raw;
		bool m_connected;

	private:
		Connection();
	};

	typedef FreeList<Connection> Connections;

	class ListenSocket
	{
	public:
		ListenSocket(uint16_t _denseIndex)
			: m_denseIndex(_denseIndex)
			, m_handle(invalidHandle)
			, m_socket(INVALID_SOCKET)
			, m_raw(false)
			, m_secure(false)
			, m_cert(NULL)
			, m_key(NULL)
		{
		}

		~ListenSocket()
		{
			close();
		}

		void close()
		{
			if (INVALID_SOCKET != m_socket)
			{
				::closesocket(m_socket);
				m_socket = INVALID_SOCKET;
			}

#if BNET_CONFIG_OPENSSL
			if (NULL != m_cert)
			{
				X509_free(m_cert);
				m_cert = NULL;
			}

			if (NULL != m_key)
			{
				EVP_PKEY_free(m_key);
				m_key = NULL;
			}
#endif // BNET_CONFIG_OPENSSL
		}

		void listen(uint16_t _handle, uint32_t _ip, uint16_t _port, bool _raw, const char* _cert, const char* _key)
		{
			m_handle = _handle;
			m_raw = _raw;

#if BNET_CONFIG_OPENSSL
			if (NULL != _cert)
			{
				BIO* mem = BIO_new_mem_buf(const_cast<char*>(_cert), -1);
				m_cert = PEM_read_bio_X509(mem, NULL, NULL, NULL);
				BIO_free(mem);
			}

			if (NULL != _key)
			{
				BIO* mem = BIO_new_mem_buf(const_cast<char*>(_key), -1);
				m_key = PEM_read_bio_PrivateKey(mem, NULL, NULL, NULL);
				BIO_free(mem);
			}

			m_secure = NULL != m_key && NULL != m_cert;
#endif // BNET_CONFIG_OPENSSL

			if (!m_secure
			&&  (NULL != _cert || NULL != _key) )
			{
#if BNET_CONFIG_OPENSSL
				BX_TRACE("Certificate of key is not set correctly.");
#else
				BX_TRACE("BNET_CONFIG_OPENSSL is not enabled.");
#endif // BNET_CONFIG_OPENSSL
				ctxPush(m_handle, MessageId::ListenFailed);
				return;
			}

			m_socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (INVALID_SOCKET == m_socket)
			{
				BX_TRACE("Create socket failed.");
				ctxPush(m_handle, MessageId::ListenFailed);
				return;
			}
			setSockOpts(m_socket);

			m_addr.sin_family = AF_INET;
			m_addr.sin_addr.s_addr = htonl(_ip);
			m_addr.sin_port = htons(_port);

			if (SOCKET_ERROR == ::bind(m_socket, (sockaddr*)&m_addr, sizeof(m_addr) )
			||  SOCKET_ERROR == ::listen(m_socket, SOMAXCONN) )
			{
				::closesocket(m_socket);
				m_socket = INVALID_SOCKET;

				BX_TRACE("Bind or listen socket failed.");
				ctxPush(m_handle, MessageId::ListenFailed);
				return;
			}

			setNonBlock(m_socket);
		}

		void update()
		{
			sockaddr_in addr;
			socklen_t len = sizeof(addr);
			SOCKET socket = ::accept(m_socket, (sockaddr*)&addr, &len);
			if (INVALID_SOCKET != socket)
			{
				uint32_t ip = ntohl(addr.sin_addr.s_addr);
				uint16_t port = ntohs(addr.sin_port);
				uint16_t handle = ctxAccept(m_handle, socket, ip, port, m_raw, m_cert, m_key);
			}
		}

		void setDenseIndex(uint16_t _denseIndex)
		{
			m_denseIndex = _denseIndex;
		}

		uint16_t getDenseIndex() const
		{
			return m_denseIndex;
		}

	private:
		sockaddr_in m_addr;
		SOCKET m_socket;
		uint16_t m_denseIndex;
		uint16_t m_handle;
		bool m_raw;
		bool m_secure;
		X509* m_cert;
		EVP_PKEY* m_key;
	};

	typedef FreeList<ListenSocket> ListenSockets;

	class Context
	{
	public:
		Context()
			: m_connections(NULL)
			, m_connectionDense(NULL)
			, m_listenSockets(NULL)
			, m_listenSocketIndex(NULL)
			, m_numConnections(0)
			, m_numListenSockets(0)
			, m_sslCtx(NULL)
		{
		}

		~Context()
		{
		}

		void init(uint16_t _maxConnections, uint16_t _maxListenSockets, const char* _certs[])
		{
#if BNET_CONFIG_OPENSSL
			SSL_library_init();
#	if BNET_CONFIG_DEBUG
			SSL_load_error_strings();
#	endif // BNET_CONFIG_DEBUG
			m_sslCtx = SSL_CTX_new(SSLv23_client_method() );
			SSL_CTX_set_verify(m_sslCtx, SSL_VERIFY_NONE, NULL);
			if (NULL != _certs)
			{
				X509_STORE* store = SSL_CTX_get_cert_store(m_sslCtx);
				for (const char* cert = *_certs; NULL != cert; cert = *(_certs++) )
				{
					BIO* mem = BIO_new_mem_buf(const_cast<char*>(cert), -1);
					X509* x509 = PEM_read_bio_X509(mem, NULL, NULL, NULL);
					X509_STORE_add_cert(store, x509);
					X509_free(x509);
					BIO_free(mem);
				}
			}
#endif // BNET_CONFIG_OPENSSL

			_maxConnections = _maxConnections == 0 ? 1 : _maxConnections;

			void* connections = s_realloc(NULL, sizeof(Connections) );
			m_connections = ::new(connections) Connections(_maxConnections);
			m_connectionDense = (uint16_t*)s_realloc(NULL, _maxConnections*sizeof(uint16_t) );
			m_numConnections = 0;

			if (0 != _maxListenSockets)
			{
				void* listenSockets = s_realloc(NULL, sizeof(ListenSockets) );
				m_listenSockets = ::new(listenSockets) ListenSockets(_maxListenSockets);
				m_listenSocketIndex = (uint16_t*)s_realloc(NULL, _maxListenSockets*sizeof(uint16_t) );
				m_numListenSockets = 0;
			}
		}

		void shutdown()
		{
			m_connections->~Connections();
			s_free(m_connections);
			s_free(m_connectionDense);

			if (NULL != m_listenSockets)
			{
				m_listenSockets->~ListenSockets();
				s_free(m_listenSockets);
				s_free(m_listenSocketIndex);
			}

#if BNET_CONFIG_OPENSSL
			if (NULL != m_sslCtx)
			{
				SSL_CTX_free(m_sslCtx);
			}
#endif // BNET_CONFIG_OPENSSL
		}

		uint16_t listen(uint32_t _ip, uint16_t _port, bool _raw, const char* _cert, const char* _key)
		{
			ListenSocket* listenSocket = m_listenSockets->create(m_numListenSockets);
			if (NULL != listenSocket)
			{
				uint16_t handle = m_listenSockets->getIndex(listenSocket);
				m_listenSocketIndex[m_numListenSockets] = handle;
				m_numListenSockets++;
				listenSocket->listen(handle, _ip, _port, _raw, _cert, _key);
				return handle;
			}

			return invalidHandle;
		}

		void stop(uint16_t _handle)
		{
			ListenSocket* listenSocket = m_listenSockets->getFromIndex(_handle);
			uint16_t denseIndex = listenSocket->getDenseIndex();
			listenSocket->close();
			m_listenSockets->destroy(listenSocket);
			m_numListenSockets--;

			listenSocket = m_listenSockets->getFromIndex(m_listenSocketIndex[m_numListenSockets]);
			listenSocket->setDenseIndex(denseIndex);
			m_listenSocketIndex[denseIndex] = m_listenSocketIndex[m_numConnections];
		}

		Connection* allocConnection()
		{
			Connection* connection = m_connections->create(m_numConnections);
			if (NULL != connection)
			{
				uint16_t handle = m_connections->getIndex(connection);
				m_connectionDense[m_numConnections] = handle;
				m_numConnections++;
			}
			return connection;
		}

		uint16_t accept(uint16_t _listenHandle, SOCKET _socket, uint32_t _ip, uint16_t _port, bool _raw, X509* _cert, EVP_PKEY* _key)
		{
			Connection* connection = allocConnection();
			if (NULL != connection)
			{
				uint16_t handle = m_connections->getIndex(connection);
				bool secure = NULL != _cert && NULL != _key;
				connection->accept(handle, _listenHandle, _socket, _ip, _port, _raw, secure?m_sslCtx:NULL, _cert, _key);
				return handle;
			}

			return invalidHandle;
		}

		uint16_t connect(uint32_t _ip, uint16_t _port, bool _raw, bool _secure)
		{
			Connection* connection = allocConnection();
			if (NULL != connection)
			{
				uint16_t handle = m_connections->getIndex(connection);
				connection->connect(handle, _ip, _port, _raw, _secure?m_sslCtx:NULL);
				return handle;
			}

			return invalidHandle;
		}

		void disconnect(uint16_t _handle, bool _finish)
		{
			Connection* connection = m_connections->getFromIndex(_handle);
			if (_finish
			&&  connection->isConnected() )
			{
				if (invalidHandle != _handle)
				{
					Message* msg = alloc(_handle, 0);
					uint8_t* data = msg->data - 2;
					data[0] = Internal::Disconnect;
					connection->send(msg);
				}
			}
			else
			{
				uint16_t denseIndex = connection->getDenseIndex();
				connection->disconnect();
				m_connections->destroy(connection);
				m_numConnections--;

				uint16_t temp = m_connectionDense[m_numConnections];
				connection = m_connections->getFromIndex(temp);
				connection->setDenseIndex(denseIndex);
				m_connectionDense[denseIndex] = temp;
			}
		}

		void notify(uint16_t _handle, uint64_t _userData)
		{
			if (invalidHandle != _handle)
			{
				Message* msg = alloc(_handle, sizeof(_userData) );
				uint8_t* data = msg->data - 2;
				data[0] = Internal::Notify;
				memcpy(msg->data, &_userData, sizeof(_userData) );
				Connection* connection = m_connections->getFromIndex(_handle);
				connection->send(msg);
			}
			else
			{
				// loopback
				Message* msg = alloc(_handle, sizeof(_userData)+1, true);
				msg->data[0] = MessageId::Notify;
				memcpy(&msg->data[1], &_userData, sizeof(_userData) );
				ctxPush(msg);
			}
		}

		Message* alloc(uint16_t _handle, uint16_t _size, bool _incoming = false)
		{
			uint16_t offset = _incoming ? 0 : 2;
			Message* msg = (Message*)s_realloc(NULL, sizeof(Message) + offset + _size);
			msg->size = _size;
			msg->handle = _handle;
			uint8_t* data = (uint8_t*)msg + sizeof(Message);
			data[0] = Internal::None;
			msg->data = data + offset;
			return msg;
		}

		void free(Message* _msg)
		{
			s_free(_msg);
		}

		void send(Message* _msg)
		{
			if (invalidHandle != _msg->handle)
			{
				Connection* connection = m_connections->getFromIndex(_msg->handle);
				connection->send(_msg);
			}
			else
			{
				// loopback
				push(_msg);
			}
		}

		Message* recv()
		{
			uint16_t numListenSockets = m_numListenSockets;
			for (uint16_t ii = 0; ii < numListenSockets; ++ii)
			{
				uint16_t handle = m_listenSocketIndex[ii];
				ListenSocket* listenSocket = m_listenSockets->getFromIndex(handle);
				listenSocket->update();
			}

			uint16_t numConnections = m_numConnections;
			for (uint32_t ii = 0; ii < numConnections; ++ii)
			{
				uint16_t handle = m_connectionDense[ii];
				Connection* connection = m_connections->getFromIndex(handle);
				connection->update();
			}

			return pop();
		}

		void push(Message* _msg)
		{
			m_incoming.push(_msg);
		}

		Message* pop()
		{
			return m_incoming.pop();
		}

	private:
		Connections* m_connections;
		uint16_t* m_connectionDense;

		ListenSockets* m_listenSockets;
		uint16_t* m_listenSocketIndex;

		uint16_t m_numConnections;
		uint16_t m_numListenSockets;

		MessageQueue m_incoming;

		SSL_CTX* m_sslCtx;
	};

	static Context s_ctx;
	
	uint16_t ctxAccept(uint16_t _listenHandle, SOCKET _socket, uint32_t _ip, uint16_t _port, bool _raw, X509* _cert, EVP_PKEY* _key)
	{
		return s_ctx.accept(_listenHandle, _socket, _ip, _port, _raw, _cert, _key);
	}

	void ctxPush(uint16_t _handle, MessageId::Enum _id)
	{
		Message* msg = s_ctx.alloc(_handle, 1, true);
		msg->data[0] = _id;
		s_ctx.push(msg);
	}

	void ctxPush(Message* _msg)
	{
		s_ctx.push(_msg);
	}

	Message* ctxAlloc(uint16_t _handle, uint16_t _size, bool _incoming)
	{
		return s_ctx.alloc(_handle, _size, _incoming);
	}

	void ctxFree(Message* _msg)
	{
		s_ctx.free(_msg);
	}

	void init(uint16_t _maxConnections, uint16_t _maxListenSockets, const char* _certs[], reallocFn _realloc, freeFn _free)
	{
		if (NULL != _realloc
		&&  NULL != _free)
		{
			s_realloc = _realloc;
			s_free = _free;
		}

#if BX_PLATFORM_WINDOWS || BX_PLATFORM_XBOX360
		WSADATA wsaData;
		WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif // BX_PLATFORM_WINDOWS || BX_PLATFORM_XBOX360

		s_ctx.init(_maxConnections, _maxListenSockets, _certs);
	}

	void shutdown()
	{
		s_ctx.shutdown();

#if BX_PLATFORM_WINDOWS || BX_PLATFORM_XBOX360
		WSACleanup();
#endif // BX_PLATFORM_WINDOWS || BX_PLATFORM_XBOX360
	}

	uint16_t listen(uint32_t _ip, uint16_t _port, bool _raw, const char* _cert, const char* _key)
	{
		return s_ctx.listen(_ip, _port, _raw, _cert, _key);
	}

	void stop(uint16_t _handle)
	{
		return s_ctx.stop(_handle);
	}

	uint16_t connect(uint32_t _ip, uint16_t _port, bool _raw, bool _secure)
	{
		return s_ctx.connect(_ip, _port, _raw, _secure);
	}

	void disconnect(uint16_t _handle, bool _finish)
	{
		s_ctx.disconnect(_handle, _finish);
	}

	void notify(uint16_t _handle, uint64_t _userData)
	{
		s_ctx.notify(_handle, _userData);
	}

	Message* alloc(uint16_t _handle, uint16_t _size)
	{
		return s_ctx.alloc(_handle, _size);
	}

	void free(Message* _msg)
	{
		s_ctx.free(_msg);
	}

	void send(Message* _msg)
	{
		s_ctx.send(_msg);
	}

	Message* recv()
	{
		return s_ctx.recv();
	}

	uint32_t toIpv4(const char* _addr)
	{
#if BX_PLATFORM_XBOX360 || BX_PLATFORM_NACL
		uint32_t a0, a1, a2, a3;
		sscanf(_addr, "%d.%d.%d.%d", &a0, &a1, &a2, &a3);
		return (a0<<24) | (a1<<16) | (a2<<8) | a3;
#else
		uint32_t ip = 0;
		struct addrinfo* result = NULL;
		struct addrinfo hints;
		memset(&hints, 0, sizeof(hints) );
		hints.ai_family = AF_UNSPEC;

		int res = getaddrinfo(_addr, NULL, &hints, &result);
		BX_TRACE("%p", result);

		if (0 == res)
		{
			while (result)
			{
				sockaddr_in* addr = (sockaddr_in*)result->ai_addr;
				if (AF_INET == result->ai_family
				&&  INADDR_LOOPBACK != addr->sin_addr.s_addr)
				{
					ip = ntohl(addr->sin_addr.s_addr);
					break;
				}

				result = result->ai_next;
			}
		}

		if (NULL != result)
		{
			freeaddrinfo(result);
		}

		return ip;
#endif // BX_PLATFORM_
	}

} // namespace bnet
