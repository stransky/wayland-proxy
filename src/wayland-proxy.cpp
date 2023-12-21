/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// This code is based on Rust implementation at
// https://github.com/the8472/weyland-p5000

// Version 1.0

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <spawn.h>
#include <poll.h>
#include <vector>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <memory>
#include <cassert>
#include <pthread.h>
#include <sched.h>

#include "wayland-proxy.h"

// The maximum number of fds libwayland can recvmsg at once
#define MAX_LIBWAY_FDS 28
#define MAX_DATA_SIZE 4096
#define POLL_TIMEOUT 30000

// sockaddr_un has hardcoded max len of sun_path
#define MAX_WAYLAND_DISPLAY_NAME_LEN 108

// Name of Wayland display provided by compositor
char sWaylandDisplay[MAX_WAYLAND_DISPLAY_NAME_LEN];

// Name of Wayland display provided by us
char sWaylandProxy[MAX_WAYLAND_DISPLAY_NAME_LEN];

bool sPrintInfo = false;

void Info(const char* aFormat, ...) {
  if (!sPrintInfo) {
    return;
  }
  va_list args;
  va_start(args, aFormat);
  vfprintf(stderr, aFormat, args);
  va_end(args);
}

void Warning(const char* aOperation, bool aPrintErrno = true) {
  fprintf(stderr, "Wayland Proxy warning: %s : %s\n", aOperation,
          aPrintErrno ? strerror(errno) : "");
}

void Error(const char* aOperation, bool aPrintErrno = true) {
  fprintf(stderr, "Wayland Proxy error: %s : %s\n", aOperation,
          aPrintErrno ? strerror(errno) : "");
}

void ErrorPlain(const char* aFormat, ...) {
  va_list args;
  va_start(args, aFormat);
  vfprintf(stderr, aFormat, args);
  va_end(args);
}

class WaylandMessage {
 public:
  bool Write(int aSocket);

  bool Loaded() { return mLoaded && (mFds.size() || mData.size()); }
  bool Failed() { return mFailed; }

  explicit WaylandMessage(int aSocket) { Read(aSocket); }
  ~WaylandMessage();

 private:
  bool Read(int aSocket);

 private:
  bool mLoaded = false;
  bool mFailed = false;

  std::vector<int> mFds;
  std::vector<byte> mData;
};

class ProxiedConnection {
 public:
  bool Init(int aChildSocket);

  struct pollfd* AddToPollFd(struct pollfd* aPfds);
  struct pollfd* LoadPollFd(struct pollfd* aPfds);

  // Process this connection (send/receive data).
  // Returns false if connection is broken and should be removed.
  bool Process();

  ~ProxiedConnection();

 private:
  // Try to connect to compositor. Returns false in case of fatal error.
  bool ConnectToCompositor();

  bool TransferOrQueue(
      int aSourceSocket, int aSourcePollFlags, int aDestSocket,
      std::vector<std::unique_ptr<WaylandMessage>>* aMessageQueue);
  bool FlushQueue(int aDestSocket, int aDestPollFlags,
                  std::vector<std::unique_ptr<WaylandMessage>>& aMessageQueue);

  // We don't have connected compositor yet. Try to connect
  bool mCompositorConnected = false;

  // We're disconnected from app or compositor. We will close this connection.
  bool mFailed = false;

  int mCompositorSocket = -1;
  int mCompositorFlags = 0;

  int mApplicationSocket = -1;
  int mApplicationFlags = 0;

  // Stored proxied data
  std::vector<std::unique_ptr<WaylandMessage>> mToCompositorQueue;
  std::vector<std::unique_ptr<WaylandMessage>> mToApplicationQueue;
};

WaylandMessage::~WaylandMessage() {
  for (auto const fd : mFds) {
    close(fd);
  }
}

bool WaylandMessage::Read(int aSocket) {
  // We don't expect WaylandMessage re-read
  assert(!mLoaded && !mFailed);

  mData.resize(MAX_DATA_SIZE);

  struct msghdr msg = {0};
  struct iovec iov = {mData.data(), mData.size()};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;

  char cmsgdata[(CMSG_LEN(MAX_LIBWAY_FDS * sizeof(int32_t)))] = {0};
  msg.msg_control = &cmsgdata;
  msg.msg_controllen = sizeof(cmsgdata);

  ssize_t ret = recvmsg(aSocket, &msg, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
  if (msg.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) {
    Error("WaylandMessage::Read() data truncated, small buffer?");
    mFailed = true;
    return false;
  }

  if (ret < 1) {
    switch (errno) {
      case EAGAIN:
      case EINTR:
        // Neither loaded nor failed, we'll try again later
        Info("WaylandMessage::Write() failed %s\n", strerror(errno));
        return false;
      default:
        Error("WaylandMessage::Write() failed");
        mFailed = true;
        return false;
    }
  }

  // Set correct data size
  mData.resize(ret);

  // Read cmsg
  struct cmsghdr* header = CMSG_FIRSTHDR(&msg);
  while (header) {
    struct cmsghdr* next = CMSG_NXTHDR(&msg, header);
    if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) {
      header = next;
      continue;
    }

    int* data = (int*)CMSG_DATA(header);
    int filenum = (int)((header->cmsg_len - CMSG_LEN(0)) / sizeof(int));
    for (int i = 0; i < filenum; i++) {
#ifdef DEBUG
      int flags = fcntl(data[i], F_GETFL, 0);
      if (flags == -1 && errno == EBADF) {
        Error("WaylandMessage::Read() invalid fd");
      }
#endif
      mFds.push_back(data[i]);
    }
    header = next;
  }

  mLoaded = true;
  return true;
}

bool WaylandMessage::Write(int aSocket) {
  if (!mLoaded || mFailed) {
    return false;
  }

  struct msghdr msg = {0};
  struct iovec iov = {mData.data(), mData.size()};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;

  int filenum = mFds.size();
  if (filenum) {
    if (filenum >= MAX_LIBWAY_FDS) {
      Error("WaylandMessage::Write() too many files to send\n", false);
      return false;
    }
#ifdef DEBUG
    for (int i = 0; i < filenum; i++) {
      int flags = fcntl(mFds[i], F_GETFL, 0);
      if (flags == -1 && errno == EBADF) {
        Error("WaylandMessage::Write() invalid fd\n");
      }
    }
#endif
    union {
      char buf[CMSG_SPACE(sizeof(int) * MAX_LIBWAY_FDS)];
      struct cmsghdr align;
    } cmsgu;
    memset(cmsgu.buf, 0, sizeof(cmsgu.buf));

    msg.msg_control = cmsgu.buf;
    msg.msg_controllen = sizeof(cmsgu.buf);
    msg.msg_controllen = CMSG_SPACE(filenum * sizeof(int));

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(filenum * sizeof(int));
    memcpy(CMSG_DATA(cmsg), mFds.data(), filenum * sizeof(int));
  }

  ssize_t ret = sendmsg(aSocket, &msg, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
  if (ret < 1) {
    switch (errno) {
      case EAGAIN:
      case EINTR:
        // Neither loaded nor failed, we'll try again later
        Info("WaylandMessage::Write() failed %s\n", strerror(errno));
        return false;
      default:
        Warning("WaylandMessage::Write() failed");
        mFailed = true;
        return false;
    }
  }

  if (ret != (ssize_t)mData.size()) {
    Info("WaylandMessage::Write() failed to write all data! (%d vs. %d)\n", ret,
         mData.size());
  }
  return true;
}

ProxiedConnection::~ProxiedConnection() {
  if (mCompositorSocket != -1) {
    close(mCompositorSocket);
  }
  if (mApplicationSocket != -1) {
    close(mApplicationSocket);
  }
}

bool ProxiedConnection::Init(int aApplicationSocket) {
  mApplicationSocket = aApplicationSocket;
  mCompositorSocket =
      socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (mCompositorSocket == -1) {
    Error("ConnectToCompositor() socket()");
  }
  return mApplicationSocket > 0 && mCompositorSocket > 0;
}

struct pollfd* ProxiedConnection::AddToPollFd(struct pollfd* aPfds) {
  // Listen application's requests
  aPfds->fd = mApplicationSocket;
  aPfds->events = POLLIN;

  // We're connected and we have data for appplication from compositor.
  // Add POLLOUT to request write to app socket.
  if (mCompositorConnected && mToApplicationQueue.size()) {
    aPfds->events |= POLLOUT;
  }
  aPfds++;

  aPfds->fd = mCompositorSocket;
  // We're waiting for connection or we have data for compositor
  if (!mCompositorConnected || mToCompositorQueue.size()) {
    aPfds->events |= POLLOUT;
  }
  if (mCompositorConnected) {
    aPfds->events = POLLIN;
  }
  aPfds++;

  return aPfds;
}

struct pollfd* ProxiedConnection::LoadPollFd(struct pollfd* aPfds) {
  if (aPfds->fd != mApplicationSocket) {
    return aPfds;
  }
  mApplicationFlags = aPfds->revents;
  aPfds++;
  mCompositorFlags = aPfds->revents;
  aPfds++;
  return aPfds;
}

bool ProxiedConnection::ConnectToCompositor() {
  if (!(mCompositorFlags & POLLOUT)) {
    // Try again later
    return true;
  }

  struct sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  strcpy(addr.sun_path, sWaylandDisplay);

  mCompositorConnected =
      connect(mCompositorSocket, (const struct sockaddr*)&addr,
              sizeof(struct sockaddr_un)) != -1;
  if (!mCompositorConnected) {
    switch (errno) {
      case EAGAIN:
      case EALREADY:
      case ECONNREFUSED:
      case EINPROGRESS:
      case EINTR:
      case EISCONN:
      case ETIMEDOUT:
        // We can recover from these errors and try again
        Warning("ConnectToCompositor() try again");
        return true;
      default:
        Error("ConnectToCompositor() connect()");
        return false;
    }
  }
  return true;
}

// Read data from aSourceSocket and try to twite them to aDestSocket.
// If data write fails, append them to aMessageQueue.
// Return
bool ProxiedConnection::TransferOrQueue(
    int aSourceSocket, int aSourcePollFlags, int aDestSocket,
    std::vector<std::unique_ptr<WaylandMessage>>* aMessageQueue) {
  // Don't read if we don't have any data ready
  if (!(aSourcePollFlags & POLLIN)) {
    return true;
  }

  while (1) {
    int availableData = 0;
    if (ioctl(aSourceSocket, FIONREAD, &availableData) < 0) {
      // Broken connection, we're finished here
      Warning("ProxiedConnection::TransferOrQueue() broken source socket %s\n");
      return false;
    }
    if (availableData == 0) {
      return true;
    }

    auto message = std::make_unique<WaylandMessage>(aSourceSocket);
    if (message->Failed()) {
      // Failed to read message due to error
      return false;
    }
    if (!message->Loaded()) {
      // Let's try again
      return true;
    }
    if (!message->Write(aDestSocket)) {
      if (message->Failed()) {
        // Failed to write and we can't recover
        return false;
      }
      aMessageQueue->push_back(std::move(message));
    }
  }
}

// Try to flush all data to aMessageQueue.
bool ProxiedConnection::FlushQueue(
    int aDestSocket, int aDestPollFlags,
    std::vector<std::unique_ptr<WaylandMessage>>& aMessageQueue) {
  // Can't write to destination yet
  if (!(aDestPollFlags & POLLOUT)) {
    return true;
  }

  while (aMessageQueue.size()) {
    if (!aMessageQueue[0]->Write(aDestSocket)) {
      return !aMessageQueue[0]->Failed();
    }
    aMessageQueue.erase(aMessageQueue.begin());
  }
  return true;
}

bool ProxiedConnection::Process() {
  if (mFailed) {
    return false;
  }

  // Check if appplication is still listening
  if (mApplicationFlags & (POLLHUP | POLLERR)) {
    return false;
  }

  // Check if compositor is still listening
  if (mCompositorConnected) {
    if (mCompositorFlags & (POLLHUP | POLLERR)) {
      return false;
    }
  } else {
    // Try to reconnect to compositor.
    if (!ConnectToCompositor()) {
      return false;
    }
    // We're not connected yet but ConnectToCompositor() didn't return
    // fatal error. Try again later.
    if (!mCompositorConnected) {
      return true;
    }
  }

  mFailed =
      !TransferOrQueue(mCompositorSocket, mCompositorFlags, mApplicationSocket,
                       &mToApplicationQueue) ||
      !TransferOrQueue(mApplicationSocket, mApplicationFlags, mCompositorSocket,
                       &mToCompositorQueue) ||
      !FlushQueue(mCompositorSocket, mCompositorFlags, mToCompositorQueue) ||
      !FlushQueue(mApplicationSocket, mApplicationFlags, mToApplicationQueue);

  return !mFailed;
}

bool WaylandProxy::SetupWaylandDisplays() {
  char* waylandDisplay = getenv("WAYLAND_DISPLAY");
  if (!waylandDisplay) {
    Error("Init(), Missing Wayland display, WAYLAND_DISPLAY is empty.", false);
    return false;
  }

  char* XDGRuntimeDir = getenv("XDG_RUNTIME_DIR");
  if (!XDGRuntimeDir) {
    Error("Init() Missing XDG_RUNTIME_DIR", false);
    return false;
  }

  // WAYLAND_DISPLAY can be absolute path
  if (waylandDisplay[0] == '/') {
    if (strlen(sWaylandDisplay) >= MAX_WAYLAND_DISPLAY_NAME_LEN) {
      Error("Init() WAYLAND_DISPLAY is too large.", false);
      return false;
    }
    strcpy(sWaylandDisplay, waylandDisplay);
  } else {
    int ret = snprintf(sWaylandDisplay, MAX_WAYLAND_DISPLAY_NAME_LEN, "%s/%s",
                       XDGRuntimeDir, waylandDisplay);
    if (ret < 0 || ret >= MAX_WAYLAND_DISPLAY_NAME_LEN) {
      Error("Init() WAYLAND_DISPLAY/XDG_RUNTIME_DIR is too large.", false);
      return false;
    }
  }
  int ret = snprintf(sWaylandProxy, MAX_WAYLAND_DISPLAY_NAME_LEN,
                     "%s/wayland-proxy-%d", XDGRuntimeDir, getpid());
  if (ret < 0 || ret >= MAX_WAYLAND_DISPLAY_NAME_LEN) {
    Error("Init() WAYLAND_DISPLAY/XDG_RUNTIME_DIR is too large.", false);
    return false;
  }

  return true;
}

bool WaylandProxy::StartProxyServer() {
  mProxyServerSocket =
      socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (mProxyServerSocket == -1) {
    Error("StartProxyServer(): failed to create socket");
    return false;
  }

  struct sockaddr_un serverName = {0};
  serverName.sun_family = AF_UNIX;
  strcpy(serverName.sun_path, sWaylandProxy);

  if (bind(mProxyServerSocket, (struct sockaddr*)&serverName,
           sizeof(serverName)) == -1) {
    Error("StartProxyServer(): bind() error");
    return false;
  }
  if (listen(mProxyServerSocket, 128) == -1) {
    Error("StartProxyServer(): listen() error");
    return false;
  }

  return true;
}

bool WaylandProxy::Init() {
  if (!SetupWaylandDisplays()) {
    return false;
  }

  if (!StartProxyServer()) {
    return false;
  }
  return true;
}

void WaylandProxy::SetWaylandProxyDisplay() {
  setenv("WAYLAND_DISPLAY", sWaylandProxy, true);
}

void WaylandProxy::SetWaylandDisplay() {
  setenv("WAYLAND_DISPLAY", sWaylandDisplay, true);
}

bool WaylandProxy::IsChildAppTerminated() {
  if (!mApplicationPID) {
    return false;
  }
  int status = 0;
  int ret = waitpid(mApplicationPID, &status, WNOHANG | WUNTRACED | WCONTINUED);
  if (ret == 0) {
    return false;
  }
  if (ret == mApplicationPID) {
    // Child application is terminated, so quit too.
    return true;
  }
  bool terminate = (errno == ECHILD);
  Error("IsChildAppTerminated: waitpid() error");
  return terminate;
}

bool WaylandProxy::PollConnections() {
  int nfds_max = mConnections.size() * 2 + 1;

  struct pollfd pollfds[nfds_max];
  struct pollfd* addedPollfd = pollfds;

  for (auto const& connection : mConnections) {
    addedPollfd = connection->AddToPollFd(addedPollfd);
  }

  // Add extra listening socket
  addedPollfd->fd = mProxyServerSocket;
  addedPollfd->events = POLLIN;
  assert(addedPollfd < pollfds + nfds_max);

  int nfds = (addedPollfd - pollfds) + 1;

  while (1) {
    int ret = poll(pollfds, nfds, POLL_TIMEOUT);
    if (ret == 0) {
      // No change on fds
      continue;
    } else if (ret > 0) {
      // We have FD to read
      break;
    } else if (ret == -1) {
      switch (errno) {
        case EINTR:
        case EAGAIN:
          if (IsChildAppTerminated()) {
            return false;
          }
          continue;
        default:
          Error("Run: poll() error");
          return false;
      }
    }
  }

  struct pollfd* loadedPollfd = pollfds;
  for (auto const& connection : mConnections) {
    loadedPollfd = connection->LoadPollFd(loadedPollfd);
  }

  assert(loadedPollfd == addedPollfd);
  assert(loadedPollfd < pollfds + nfds_max);

  // Create a new connection if there's a new client waiting
  if (loadedPollfd->revents & POLLIN) {
    Info("WaylandProxy: new child connection\n");
    int applicationSocket = accept4(loadedPollfd->fd, nullptr, nullptr,
                                    SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (applicationSocket == -1) {
      switch (errno) {
        case EAGAIN:
        case EINTR:
          // Try again later
          break;
        default:
          Error("Faild to accept connection from application");
          return false;
      }
    } else {
      auto connection = std::make_unique<ProxiedConnection>();
      if (connection->Init(applicationSocket)) {
        mConnections.push_back(std::move(connection));
      }
    }
  }

  return true;
}

bool WaylandProxy::ProcessConnections() {
  std::vector<std::unique_ptr<ProxiedConnection>>::iterator connection;
  for (connection = mConnections.begin(); connection != mConnections.end();) {
    if (!(*connection)->Process()) {
      Info("WaylandProxy: remove connection\n");
      connection = mConnections.erase(connection);
      if (!mConnections.size()) {
        // We removed last connection - quit.
        Info("WaylandProxy: removed last connection, quit\n");
        return false;
      }
    } else {
      connection++;
    }
  }
  return true;
}

void WaylandProxy::Run() {
  while (!IsChildAppTerminated() && PollConnections() && ProcessConnections())
    ;
}

WaylandProxy::~WaylandProxy() {
  Info("WaylandProxy terminated\n");
  if (mThreadRunning) {
    Info("WaylandProxy thread is still running, terminating.\n");
    pthread_cancel(mThread);
    pthread_join(mThread, nullptr);
  }
  unlink(sWaylandProxy);
  if (mProxyServerSocket != -1) {
    close(mProxyServerSocket);
  }
}

void* WaylandProxy::RunProxyThread(WaylandProxy* aProxy) {
#ifdef __linux__
  pthread_setname_np(pthread_self(), "WaylandProxy");
#endif
  aProxy->Run();
  aProxy->mThreadRunning = false;
  Info("WaylandProxy thread exited\n");
  return nullptr;
}

std::unique_ptr<WaylandProxy> WaylandProxy::Create() {
  auto proxy = std::make_unique<WaylandProxy>();
  if (!proxy->Init()) {
    return nullptr;
  }

  Info("WaylandProxyCreated, display %s\n", sWaylandProxy);
  return proxy;
}

bool WaylandProxy::RunChildApplication(char* argv[]) {
  if (!argv[0]) {
    Error("WaylandProxy::RunChildApplication: missing application to run", false);
    return false;
  }

  mApplicationPID = fork();
  if (mApplicationPID == -1) {
    Error("WaylandProxy::RunChildApplication: fork() error");
    return false;
  }
  if (mApplicationPID == 0) {
    SetWaylandProxyDisplay();
    if (execv(argv[0], argv) == -1) {
      ErrorPlain("WaylandProxy::RunChildApplication: failed to run %s error %s\n", argv[0], strerror(errno));
      exit(1);
    }
  }

  Run();
  return true;
}

bool WaylandProxy::RunThread() {
  pthread_attr_t attr;
  if (pthread_attr_init(&attr) != 0) {
    return false;
  }

  sched_param param;
  if (pthread_attr_getschedparam(&attr, &param) == 0) {
    param.sched_priority = sched_get_priority_min(SCHED_RR);
    pthread_attr_setschedparam(&attr, &param);
  }

  SetWaylandProxyDisplay();

  mThreadRunning = pthread_create(&mThread, nullptr, (void* (*)(void*))RunProxyThread, this) == 0;
  if (!mThreadRunning) {
    // If we failed to run proxy thread, set WAYLAND_DISPLAY back.
    SetWaylandDisplay();
  }

  pthread_attr_destroy(&attr);
  return mThreadRunning;
}

void WaylandProxy::SetVerbose(bool aVerbose) { sPrintInfo = aVerbose; }