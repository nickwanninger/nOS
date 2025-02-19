#pragma once

#include <chan.h>
#include <fifo_buf.h>
#include <lock.h>
#include <net/ipv4.h>
#include <net/socket.h>
#include <ck/ptr.h>
#include <sem.h>
#include <ck/single_list.h>
#include <types.h>
#include <wait.h>
#include <fs.h>


#define PFLAGS_SERVER (0x1)
#define PFLAGS_CLIENT (0x2)

namespace net {

  /**
   * The representation of a network socket. Stored in fs::inode.sock when type
   * is T_SOCK
   */
  class Socket : public fs::Node {
   public:
    enum class role : uint8_t { none, accepting, listener, connected, connecting };

    net::Socket::role role = role::none;

    int domain;    // AF_*
    int type;      // SOCK_*
    int protocol;  // ignored in this kernel, but nice to have
    bool connected = false;
    size_t total_sent = 0;
    size_t total_recv = 0;

    // the inode that contains this socket
    ck::ref<fs::Node> ino = nullptr;


    Socket(int domain, int type, int proto);
    virtual ~Socket(void);


    template <typename T>
    T *&priv(void) {
      return (T *&)_private;
    }

    static ck::ref<net::Socket> create(int domain, int type, int protocol, int &err);
    static ck::ref<fs::Node> createi(int domain, int type, int protocol, int &err);


    // ^fs::Node
    int ioctl(fs::File &, unsigned int cmd, off_t arg) override final;
    int seek_check(fs::File &, off_t old_off, off_t new_off) override final { return -EINVAL; }
    ssize_t read(fs::File &f, char *b, size_t s) override final { return recvfrom(f, (void *)b, s, 0, nullptr, 0); }
    ssize_t write(fs::File &f, const char *b, size_t s) override final { return sendto(f, (void *)b, s, 0, nullptr, 0); }
    void close(fs::File &) override final;
    bool is_sock(void) override final { return true; }

    int poll(fs::File &, int events, poll_table &pt) override;



    virtual int connect(struct sockaddr *uaddr, int addr_len);
    virtual ck::ref<net::Socket> accept(struct sockaddr *uaddr, int addr_len, int &err);
    virtual int disconnect(int flags);

    // implemented by the network layer (OSI)
    virtual ssize_t sendto(fs::File &, void *data, size_t len, int flags, const sockaddr *, size_t);
    virtual ssize_t recvfrom(fs::File &, void *data, size_t len, int flags, const sockaddr *, size_t);

    virtual int bind(const struct sockaddr *addr, size_t len);

   private:
    spinlock owners_lock;
    int owners = 0;
    void *_private;
  };

  struct LocalSocket : public net::Socket {
    LocalSocket(int type);
    virtual ~LocalSocket(void);

    virtual int connect(struct sockaddr *uaddr, int addr_len);
    virtual int disconnect(int flags);
    virtual ck::ref<net::Socket> accept(struct sockaddr *uaddr, int addr_len, int &err);

    // implemented by the network layer (OSI)
    virtual ssize_t sendto(fs::File &, void *data, size_t len, int flags, const sockaddr *, size_t);
    virtual ssize_t recvfrom(fs::File &, void *data, size_t len, int flags, const sockaddr *, size_t);

    virtual int bind(const struct sockaddr *addr, size_t len);

    virtual int poll(fs::File &f, int events, poll_table &pt);


    void dump_stats(void);

    // the inode this (server) socket is bound to
    ck::ref<fs::Node> bindpoint = nullptr;

    ck::ref<LocalSocket> peer;

    fifo_buf for_server;  // client writes, server reads
    fifo_buf for_client;  // server writes, client reads

    bool is_server = false;

    chan<ck::ref<LocalSocket>> pending_connections;

    // intrusive linked list so we can store them all
    ck::ref<net::LocalSocket> next, prev;
  };



  // ipc messages cannot be partially consumed,
  // they must be entirely read at once
  struct ipcmsg {
    ck::vec<uint8_t> data;

    inline ipcmsg(void *buf, size_t sz) {
      data.ensure_capacity(sz);
      data.push((uint8_t *)buf, sz);
    }
  };

  struct IPCSock : public net::Socket {
    IPCSock(int type);
    virtual ~IPCSock(void);

    virtual int connect(struct sockaddr *uaddr, int addr_len);
    virtual int disconnect(int flags);
    virtual ck::ref<net::Socket> accept(struct sockaddr *uaddr, int addr_len, int &err);

    // implemented by the network layer (OSI)
    virtual ssize_t sendto(fs::File &, void *data, size_t len, int flags, const sockaddr *, size_t);
    virtual ssize_t recvfrom(fs::File &, void *data, size_t len, int flags, const sockaddr *, size_t);

    virtual int bind(const struct sockaddr *addr, size_t len);

    virtual int poll(fs::File &f, int events, poll_table &pt);


    // the inode this (server) socket is bound to
    ck::ref<fs::Node> bindpoint = nullptr;

    ck::ref<IPCSock> peer;


    struct {
      ck::single_list<ipcmsg> msgs;
      struct wait_queue wq;
      spinlock lock;
      bool closed = false;
    } for_server, for_client;


    chan<ck::ref<IPCSock>> pending_connections;
  };



#ifdef CONFIG_LWIP
  /* A generic LWIP socket binding. This ties into the socket api. The implementation is in
   * /net/lwip/api/sockets.cpp */
  struct ip4sock : public net::Socket {
   private:
    /* The socket binding */
    int sock;
    spinlock lock;

   public:
    ip4sock(int type);
    virtual ~ip4sock(void);


    virtual int connect(struct sockaddr *uaddr, int addr_len);
    virtual int disconnect(int flags);
    virtual ck::ref<net::Socket> accept(struct sockaddr *uaddr, int addr_len, int &err);

    // implemented by the network layer (OSI)
    virtual ssize_t sendto(fs::File &, void *data, size_t len, int flags, const sockaddr *, size_t);
    virtual ssize_t recvfrom(fs::File &, void *data, size_t len, int flags, const sockaddr *, size_t);

    virtual int bind(const struct sockaddr *addr, size_t len);

    virtual int poll(fs::File &f, int events, poll_table &pt);

    int translate_errno(int);
  };
#endif

}  // namespace net
