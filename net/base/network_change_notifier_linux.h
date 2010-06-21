// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_BASE_NETWORK_CHANGE_NOTIFIER_LINUX_H_
#define NET_BASE_NETWORK_CHANGE_NOTIFIER_LINUX_H_

#include "base/basictypes.h"
#include "base/message_loop.h"
#include "base/non_thread_safe.h"
#include "base/observer_list.h"
#include "net/base/network_change_notifier.h"

#if defined(OS_CHROMEOS)
#include "base/task.h"
#endif

namespace net {

class NetworkChangeNotifierLinux
    : public NetworkChangeNotifier,
      public NonThreadSafe,
      public MessageLoopForIO::Watcher,
      public MessageLoop::DestructionObserver {
 public:
  NetworkChangeNotifierLinux();

  // NetworkChangeNotifier methods:
  virtual void AddObserver(Observer* observer);
  virtual void RemoveObserver(Observer* observer);

  // MessageLoopForIO::Watcher methods:
  virtual void OnFileCanReadWithoutBlocking(int fd);
  virtual void OnFileCanWriteWithoutBlocking(int /* fd */);

  // MessageLoop::DestructionObserver methods:
  virtual void WillDestroyCurrentMessageLoop();

 private:
  virtual ~NetworkChangeNotifierLinux();

  // Starts listening for netlink messages.  Also handles the messages if there
  // are any available on the netlink socket.
  void ListenForNotifications();

  // Attempts to read from the netlink socket into |buf| of length |len|.
  // Returns the bytes read on synchronous success and ERR_IO_PENDING if the
  // recv() would block.  Otherwise, it returns a net error code.
  int ReadNotificationMessage(char* buf, size_t len);

  // Stops watching the netlink file descriptor.
  void StopWatching();

  void NotifyObserversIPAddressChanged();

  // http://crbug.com/36890.
  ObserverList<Observer, false> observers_;

  int netlink_fd_;  // This is the netlink socket descriptor.

#if defined(OS_CHROMEOS)
  ScopedRunnableMethodFactory<NetworkChangeNotifierLinux> factory_;
#endif

  MessageLoopForIO* loop_;
  MessageLoopForIO::FileDescriptorWatcher netlink_watcher_;

  DISALLOW_COPY_AND_ASSIGN(NetworkChangeNotifierLinux);
};

}  // namespace net

#endif  // NET_BASE_NETWORK_CHANGE_NOTIFIER_LINUX_H_
