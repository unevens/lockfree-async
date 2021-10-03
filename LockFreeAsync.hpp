/*
Copyright 2019 Dario Mambro

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#pragma once
#include "LockFreeMessenger.hpp"
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <utility>

namespace lockfree {

class AsyncThread;

class AsyncInterface
{
public:
  virtual void timerCallback() = 0;
  virtual ~AsyncInterface() = default;

protected:
  friend class AsyncThread;
  AsyncThread* asyncThread{ nullptr };
  void setAsyncThread(AsyncThread* asyncThread_) { asyncThread = asyncThread_; }
};

class AsyncThread final
{
public:
  explicit AsyncThread(int timerPeriod = 250)
    : timerPeriod{ timerPeriod }
  {}

  void addObject(AsyncInterface& async)
  {
    auto const lock = std::lock_guard<std::mutex>(mutex);
    asyncObjects.insert(&async);
    async.setAsyncThread(this);
  }

  void removeObject(AsyncInterface& async)
  {
    auto const lock = std::lock_guard<std::mutex>(mutex);
    asyncObjects.erase(&async);
    async.setAsyncThread(nullptr);
  }

  /**
   * Starts the Async's timer, spawning its timer thread.
   */
  void startTimer()
  {
    auto const lock = std::lock_guard<std::mutex>(mutex);
    if (isRunningFlag) {
      return;
    }
    stopTimerFlag = false;
    timer = std::thread([this]() {
      while (true) {
        for (auto asyncObject : asyncObjects)
          asyncObject->timerCallback();
        if (stopTimerFlag) {
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(timerPeriod));
        if (stopTimerFlag) {
          return;
        }
      }
    });
  }

  /**
   * Stops the Async's timer, joining its timer thread.
   */
  void stopTimer()
  {
    auto const lock = std::lock_guard<std::mutex>(mutex);
    stopTimerFlag = true;
    if (timer.joinable()) {
      timer.join();
    }
    isRunningFlag = false;
  }

  /**
   * Sets the period of the timer thread that handles the messages.
   */
  void setTimerPeriod(int period)
  {
    auto const lock = std::lock_guard<std::mutex>(mutex);
    timerPeriod.store(period, std::memory_order_release);
  }

  /**
   * @return true if the timer thread is running, false otherwise.
   */
  bool isRunning() const
  {
    return isRunningFlag.load(std::memory_order_acquire);
  }

  int getTimerPeriod() const
  {
    return timerPeriod.load(std::memory_order_acquire);
  }

  std::mutex& getMutex() { return mutex; }

  ~AsyncThread()
  {
    stopTimer();
    for (auto asyncObject : asyncObjects)
      removeObject(*asyncObject);
  }

private:
  std::unordered_set<AsyncInterface*> asyncObjects;
  std::thread timer;
  std::atomic<bool> stopTimerFlag{ false };
  std::atomic<int> timerPeriod;
  std::atomic<bool> isRunningFlag{ false };
  std::mutex mutex;
};

template<class TObject, class TObjectSettings>
class Async;

/**
 * A class to share and syncrhonize the state of an object among multiple
 * threads, in a lock-free and non-blocking manner.
 * @tparam TObjectSettings the type of the object to share.
 */
template<class TObject, class TObjectSettings>
class Async final
{
  friend class AsyncThread;
public:
  using ObjectSettings = TObjectSettings;
  using Object = TObject;
  using ChangeSettings = std::function<void(ObjectSettings&)>;

  /**
   * A class to get a thread-local copy of the object stored in the Async class,
   * or of an object of an other type constructed from it. Non-blocking version.
   * @tparam TObjectSettings the type of the object stored in the Async object.
   * @tparam TClassToGet the type of object to be returned by the method get of
   * the getter. It must be constructible from a constant reference to a
   * TObjectSettings object.
   */
  class Getter final
  {
  public:
    friend Async;
    using Object = TObject;

    Object localCopy;
    Messenger<Object> toGetter;
    Messenger<Object> toAsync;

    template<class ObjectSettings>
    void onChange(ObjectSettings const& settings)
    {
      freeMessageStack(toGetter.receiveAllNodes()); // free old messages
      toGetter.send(ClassToGet(settings));
    }

    void cleanup() { freeMessageStack(toAsync.receiveAllNodes()); }

    template<class ObjectSettings>
    Getter(ObjectSettings const& settings)
      : localCopy{ settings }
    {}

    /**
     * Updates the local copy to the last version of the object stored by the
     * Async instance, or to a ClassToGet object constructed from it.
     * Non-blocking. Don't call this function while you are using the local
     * copy!
     */
    void update()
    {
      using std::swap;
      auto messageNode = toGetter.receiveLastNode();
      if (messageNode) {
        auto& message = messageNode->get();
        swap(localCopy, message);
        toAsync.send(messageNode);
      }
    }

    /**
     * @return a reference to the local storage.
     */
    Object& get() { return localCopy; }
  };

  /**
   * A class to get a thread-local copy of the object stored in the Async class,
   * or of an object of an other type constructed from it. Blocking version.
   * @tparam TObjectSettings the type of the object stored in the Async object.
   * @tparam TClassToGet the type of object to be returned by the method get of
   * the getter. It must be constructible from a constant reference to a
   * TObjectSettings object.
   */
  template<class TObjectToGet>
  class TBlockingGetter final
  {
  public:
  friend Async;

    using ObjectToGet = TObjectToGet;

    Object localCopy;
    Messenger<ObjectToGet> toGetter;
    Messenger<ObjectToGet> toAsync;
    Messenger<> requestMessenger;
    std::function<int()> getTimerPeriod;

    template<class ObjectSettings>
    void handleRequests(ObjectSettings const& settings)
    {
      freeMessageStack(toAsync.receiveAllNodes());
      auto requests = requestMessenger.receiveAllNodes();
      if (requests) {
        toGetter.send(ClassToGet(settings));
        freeMessageStack(requests);
      }
    }

    template<class ObjectSettings>
    TBlockingGetter(ObjectSettings const& settings,
                    std::function<int()> getTimerPeriod)
      : localCopy{ settings }
      , getTimerPeriod{ std::move(getTimerPeriod) }
    {}

  public:
    /**
     * Updates the local copy to a synchronized instance of the object stored by
     * the Async instance, or to a ClassToGet object constructed from it. This
     * is the blocking version, which will waits for the syncrhonized instance.
     * Don't call this function while you are using the local copy!
     * @param sleepWhileWaiting if true, the thread will sleep while waiting, if
     * flase, it will do a busy wait in a while(true) loop.
     */
    void update(bool sleepWhileWaiting = true)
    {
      using std::swap;
      requestMessenger.send('x');
      while (true) {
        auto messageNode = toGetter.receiveLastNode();
        if (messageNode) {
          auto& message = messageNode->get();
          swap(localCopy, message);
          toAsync.send(messageNode);
          return;
        }
        if (sleepWhileWaiting) {
          std::chrono::milliseconds interval{ getTimerPeriod() + 1 };
          std::this_thread::sleep_for(interval);
        }
      }
    }

    /**
     * @return a reference to the local copy.
     */
    ObjectToGet& get() { return localCopy; }
  };

  using BlockingGetter = TBlockingGetter<Object>;

  /*add getters for the settings. maybe also setters? maybe use the std function
   * that does not allocate. */
private:
  struct AwaiterFakeStorage final
  {
    explicit AwaiterFakeStorage(TObjectSettings const&) = default;
  };
  using AwaiterInternal = TBlockingGetter<AwaiterFakeStorage>;

public:
  class Awaiter final
  {
    AwaiterInternal* awaiter;

  public:
    /**
     * Makes the current thread wait for all already submitted messages to be
     * handled.
     * @param sleepWhileWaiting if true, the thread will sleep while waiting, if
     * flase, it will do a busy wait in a while(true) loop.
     */
    void await(bool sleepWhileWaiting = true)
    {
      awaiter->update(sleepWhileWaiting);
    }

    /**
     * Check if the awaiter is valid. Awaiter objectss will always be valid if
     * requested while the Async's timer is not running, and will always be
     * invalid if requested while the Async's timer is running. Calling await on
     * an invalid Awaiter will try to dereference a nullptr.
     */
    operator bool() { return awaiter != nullptr; }

    /**
     * Awaiter constructor. Can only be used internally by the Async.
     */
    Awaiter(AwaiterInternal* awaiter)
      : awaiter{ awaiter }
    {}
  };

private:
  AsyncThread* asyncThread = nullptr;
  Messenger<ChangeSettings> messenger;

  ObjectSettings settings;
  std::function<void(ObjectSettings&)> onChange;

  std::vector<std::unique_ptr<Getter>> getters;
  std::vector<std::unique_ptr<BlockingGetter>> blockingGetters;

  std::vector<std::unique_ptr<MessageBuffer<ChangeSettings>>> buffers;

  bool handleChanges()
  {
    int numChanges = receiveAndHandleMessageStack(
      messenger, [&](ChangeSettings& change) { change(settings); });
    return numChanges > 0;
  }

  void timerCallback()
  {
    for (auto& getter : getters) {
      getter->cleanup();
    }
    bool anyChange = handleChanges();
    if (anyChange) {
      if (onChange) {
        onChange(settings);
      }
      for (auto& getter : getters) {
        getter->onChange(settings);
      }
    }
    for (auto& getter : blockingGetters) {
      getter->handleRequests(settings);
    }
    for (auto& buffer : buffers) {
      buffer->maintenance();
    }
  }

  Getter& createGetter()
  {
    getters.push_back(std::make_unique<Getter>(settings));
    return getters.back();
  }

  BlockingGetter& createBlockingGetter()
  {
    blockingGetters.push_back(std::make_unique<BlockingGetter>(
      settings, [this] { return asyncThread->getTimerPeriod(); }));
    return blockingGetters.back();
  }

  Awaiter createAwaiter()
  {
    blockingGetters.push_back(
      AwaiterInternal(settings, asyncThread->getTimerPeriod()));
    return Awaiter{ &blockingGetters.back() };
  }

public:
  /**
   * Sends a message already wrapped in a MessageNode. non-blocking.
   * @param messageNode the message node to send.
   */
  void submitChange(MessageNode<ChangeSettings>* messageNode)
  {
    messenger.send(messageNode);
  }

  /**
   * Sends a message that is not already wrapped in a MessageNode. This should
   not be called from realtime threads, because a MessageNode may be allocated
   under the hood (it will be allocated when there are no available ones, which
   will happen if messages are sent more often than how often they are handled
   by the timerCallback).
   * @param message the message to send.
   */
  bool submitChange(ChangeSettings&& message)
  {
    return messenger.send(message);
  }

  /**
   * Sends a Message, wrapping it in a MessageNode from the storage if there is
   * one available, otherwise it does not send the message and returns false.
   * @param message the massage to send.
   * @return true if the message was sent, false if it was not sent
   */
  bool sendIfNodeAvailable(ChangeSettings&& message)
  {
    return messenger.sendIfNodeAvailable(message);
  }

  /**
   * Requests an Getter. This should not be called while the Async's timer
   * is running.
   * @tparam TClassToGet a type that can be constructed from the ObjectSettings,
   * to store what the access point needs from the ObjectSettings.
   * @return pointer to the requested Getter, or nullptr if the Async's timer is
   * running
   */
  template<class TClassToGet = TObjectSettings>
  Getter& requestGetter()
  {
    if (asyncThread) {
      auto const lock = std::lock_guard<std::mutex>(asyncThread->getMutex());
      return createGetter();
    }
    else {
      return createGetter();
    }
  }

  /**
   * Requests a BlockingGetter. This should not be called while the Async's
   * timer is running.
   * @tparam TClassToGet a type that can be constructed from the ObjectSettings,
   * to store what the access point needs from the ObjectSettings.
   * @return pointer to the requested BlockingGetter, or nullptr if the
   * Async's timer is running
   */
  template<class TClassToGet = TObjectSettings>
  BlockingGetter& requestBlockingGetter()
  {
    if (asyncThread) {
      auto const lock = std::lock_guard<std::mutex>(asyncThread->getMutex());
      return createBlockingGetter();
    }
    else {
      return createBlockingGetter();
    }
  }

  /**
   * An awaiter is like a blocking access points, but it does not own a copy of
   * the stored object. You can use it to wait for the next iteration of the
   * timerCallback.
   * @return an Awaiter object
   */
  Awaiter requestAwaiter()
  {
    if (asyncThread) {
      auto const lock = std::lock_guard<std::mutex>(asyncThread->getMutex());
      return createAwaiter();
    }
    else {
      return createAwaiter();
    }
  }

  /**
   * Requests a MessageBuffer. This should not be called while the Async's timer
   * is running.
   * @see MessageBuffer
   * @param initializer functor used by the message buffer to initialize its
   * nodes
   * @param numPreallocatedNodes number of nodes preallcoated by the message
   * buffer
   * @param minNumNodes number of nodes at which the message buffer will
   * replenish its nodes.
   * @return a pointer to the MessageBuffer, or nullptr if the Async's timer is
   * running
   */
  MessageBuffer<ChangeSettings>* requestMessageBuffer(
    std::function<ChangeSettings(void)> initializer,
    int numPreallocatedNodes,
    int minNumNodes)
  {
    auto const lock = std::lock_guard<std::mutex>(asyncThread->getMutex());
    buffers.push_back(std::make_unique<MessageBuffer<ChangeSettings>>(
      numPreallocatedNodes, minNumNodes, initializer));
    return buffers.back().get();
  }

  /**
   * Returns a preallocated node.
   */
  MessageNode<ChangeSettings>* getMessageNodeFromStorage()
  {
    auto head = messenger.PopStorage();
    if (!head) {
      return nullptr;
    }
    messenger.Recycle(head->next());
    head->next() = nullptr;
    return head;
  }

  /**
   * Constructor.
   * @param settings the ObjectSettings object to share
   * @param onChange functor called on the ObjectSettings whenever it
   * is updated by messages
   * @param numNodesToPreallocate number of message nodes to preallocate.
   * @param messageInitialzier functor to initialize the preallocated nodes.
   */
  explicit Async(
    TObjectSettings settings,
    std::function<void(TObjectSettings&)> onChange = nullptr,
    int numNodesToPreallocate = 32,
    std::function<ChangeSettings(void)> messageInitialzier =
      [] { return ChangeSettings(); })
    : settings{ std::move(settings) }
    , onChange{ onChange }
  {
    messenger.preallocateNodes(numNodesToPreallocate, messageInitialzier);
  }

  ~Async()
  {
    if (asyncThread) {
      asyncThread->removeObject(*this);
    }
  }
};

} // namespace lockfree
