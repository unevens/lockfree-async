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

#include "QwMpmcPopAllLifoStack.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <functional>
#include <memory>

namespace lockfree {

/*
The LifoMessanger class uses QwMpmcPopAllLifoStack from
https://github.com/RossBencina/QueueWorld by Ross Bencina.

QwMpmcPopAllLifoStack is a multiple-producer
multiple-consumer LIFO stack that supports push() and pop_all() operations,
but not pop().
See QwMpmcPopAllLifoStack.h
*/

/**
 * Template class for the nodes of lock-free multiple-producer multiple-consumer
 * LIFO stack.
 * @tparam T data type held by the node.
 * @see QwLinkTraits.h
 */
template<typename T>
class MessageNode final
{
  T message;

public:
  // QwLinkTraits interface begin

  enum
  {
    LINK_INDEX_1 = 0,
    PREV_LINK,
    LINK_COUNT
  };

  MessageNode* links_[2];

  /**
   * Constructor.
   * @param message the message to store in the MessageNode
   */
  explicit MessageNode(T message)
    : message{ std::move(message) }
    , links_{ nullptr, nullptr }
  {}

  /**
   * @return a reference to the message stored in the MessageNode.
   */
  T& get() { return message; }

  /**
   * Sets the message.
   * @param message_ the message to store in the MessageNode object.
   */
  void set(T message_) { message = std::move(message_); }

  /**
   * @return a reference to the next MessageNode in the stack.
   */
  MessageNode*& next() { return links_[0]; }

  /**
   * @return a reference to the previous MessageNode in the stack. This is only
   * used by handleMessageStack, and its result is not valid anywhere else.
   */
  MessageNode*& prev() { return links_[1]; }

  /**
   * @return a reference to the last MessageNode in the stack.
   */
  MessageNode* last()
  {
    auto it = this;
    while (it->next()) {
      it = it->next();
    }
    return it;
  }

  /**
   * @return the number of MessageNodes in the stack, starting from this one.
   */
  int count()
  {
    int num = 0;
    auto it = this;
    while (it) {
      ++num;
      it = it->next();
    }
    return num;
  }
};

/**
 * Handles a stack of received message nodes using a functor. The messages will
 * be handled in the order they were sent.
 * @tparam T data type held by the node.
 * @tparam Action the type of the functor to call on the message nodes, e.g.
 * std::function<void(MessageNode<T>)>
 * @param head the head node of the stack
 * @param action the functor to use
 * @see QwLinkTraits.h
 */
template<typename T, class Action>
inline void
handleMessageStack(MessageNode<T>* head, Action action)
{
  if (!head) {
    return;
  }
  // gotta handle the message in FIFO order, so we cache that order using the
  // MessageNode<T>::prev() link poiner
  head->prev() = nullptr;
  while (head->next()) {
    head->next()->prev() = head;
    head = head->next();
  }
  // and we loop from the new head (which is the last node of the lifo stack)
  // using the prev pointer
  while (head) {
    action(head);
    head = head->prev();
  }
}

/**
 * Frees a stack of message nodes.
 * @tparam T data type held by the node.
 * @see QwLinkTraits.h
 */
template<typename T>
inline void
freeMessageStack(MessageNode<T>* head)
{
  while (head) {
    auto next = head->next();
    delete head;
    head = next;
  }
}

/**
 * An alias for QueueWorld's QwMpmcPopAllLifoStack.
 * @tparam T the type of the data held by the nodes
 */
template<typename T>
using LifoStack =
  QwMpmcPopAllLifoStack<MessageNode<T>*, MessageNode<T>::LINK_INDEX_1>;

/**
 * Wrapper around QueueWorld's QwMpmcPopAllLifoStack with functionality to
 * allocate and recycle nodes.
 * @tparam T the type of the data held by the nodes, char is used as default for
 * Messages that are just notification and do not need to have data.
 */
template<typename T = char>
class Messenger final
{
  LifoStack<T> lifo;
  LifoStack<T> storage;

public:
  /**
   * Sends a message already wrapped in a MessageNode. Non-blocking.
   * @param node the massage node to send.
   */
  void send(MessageNode<T>* node) { lifo.push(node); }

  /**
   * Sends a Message, wrapping in a MessageNode from the storage if there is
   * one available, otherwise it allocates a new one.
   * @param message the massage to send.
   */
  void send(T&& message)
  {
    auto node = storage.pop_all();
    bool fromStorage = true;
    if (!node) {
      node = new MessageNode<T>(std::move(message));
      fromStorage = false;
    }
    if (fromStorage) {
      node->set(std::move(message));
      auto next = node->next();
      node->next() = nullptr;
      if (next) {
        storage.push_multiple(next, next->last());
      }
    }
    lifo.push(node);
  }

  /**
   * Sends a Message, wrapping in a MessageNode from the storage if there is
   * one available, otherwise it does not send the messate and returns false.
   * @param message the massage to send.
   * @return true if the message was sent, false if it was not sent
   */
  bool send(T&& message)
  {
    auto node = storage.pop_all();
    bool fromStorage = true;
    if (!node) {
      return false;
    }
    else {
      node->set(std::move(message));
      auto next = node->next();
      node->next() = nullptr;
      if (next) {
        storage.push_multiple(next, next->last());
      }
    }
    lifo.push(node);
    return true;
  }

  /**
   * Receives the message that was sent for last. Non blocking. If there is any
   * message not yet received, "ReceiveLast" will move the one that was sent for
   * last to its argument, and return true; otherwise it will return false.
   * REMARK: When "receiveLastMessage" is called, ONLY the message sent for last
   * is received, all the others are discarded.
   * @param message an already allocated message to which the received message
   * will be moved.
   */
  bool receiveLastMessage(T& message)
  {
    MessageNode<T>* head = lifo.pop_all();
    if (!head) {
      return false;
    }
    message = std::move(head->get());
    storage.push_multiple(head, head->last());
    return true;
  }

  /**
   * Receives the message that was sent for last, wrapped in the MessageNode it
   * was sent in. Non blocking. Returns nullptr if there are no messages to be
   * received.
   * REMARK: When "receiveLastNode" is called, ONLY the message sent
   * for last is received, all the others are discarded.
   * @return the received message node.
   */
  MessageNode<T>* receiveLastNode()
  {
    MessageNode<T>* head = lifo.pop_all();
    if (!head) {
      return nullptr;
    }
    if (head->next()) {
      storage.push_multiple(head->next(), head->last());
      head->next() = nullptr;
    }
    return head;
  }

  /**
   * Receives all the messages and returned the stack of nodes that hold them.
   * @return the head node of the stack of message nodes.
   */
  MessageNode<T>* receiveAllNodes() { return lifo.pop_all(); }

  /**
   * @return all the preallocated message nodes that are kept in storage.
   */
  MessageNode<T>* popStorage() { return storage.pop_all(); }

  /**
   * Recycles a stack of message nodes.
   * @param stack the stack of nodes to recycle.
   */
  void recycle(MessageNode<T>* stack)
  {
    if (stack) {
      storage.push_multiple(stack, stack->last());
    }
  }

  /**
   * Preallocate message nodes using an initializer functor.
   * @param numNodesToPreallocate the number of nodes to preallocate.
   * @param initializer functor to initialize the nodes.
   */
  void preallocateNodes(
    int numNodesToPreallocate,
    std::function<T(void)> initializer = [] { return T{}; })
  {
    MessageNode<T>* head = nullptr;
    MessageNode<T>* it = nullptr;
    for (int i = 0; i < numNodesToPreallocate; ++i) {
      auto node = new MessageNode<T>(initializer());
      if (it) {
        it->next() = node;
        it = node;
      }
      else if (!head) {
        head = it = node;
      }
    }
    recycle(head);
  }

  void discardAllMessages() { recycle(lifo.pop_all()); }

  void freeStorage() { freeMessageStack(storage.pop_all()); }

  void discardAndFreeAllMessages() { freeMessageStack(lifo.pop_all()); }

  ~Messenger()
  {
    discardAndFreeAllMessages();
    freeStorage();
  }
};

/**
 * Receive messages using a Messenger and handles the with a functor.
 * @tparam T the type of the data held by the nodes
 * @tparam Action the type of the functor to call on the message nodes, e.g.
 * std::function<void(MessageNode<T>)>
 * @param messenger the messenger to receive the messages from
 * @param action the functor to call on the received nodes
 */
template<typename T, class Action>
inline int
receiveAndHandleMessageStack(Messenger<T>& messenger, Action action)
{
  auto messages = messenger.receiveAllNodes();
  auto numMessages = messages->count();
  handleMessageStack(messages, action);
  messenger.recycle(messages);
  return numMessages;
}

/**
 * A class that holds a stack of message nodes initialized with a functor and
 * ready to be sent.
 * @tparam T the type of the data held by the nodes
 */
template<typename T>
class MessageBuffer
{
  LifoStack<T> storage;
  int desiredBufferSize;
  int minSize;
  std::atomic<int> actualSize;
  std::function<T(void)> initializer;

public:
  /**
   * Constructor.
   * @param desiredBufferSize the number of nodes to be keep in the buffer.
   * @param minSize the number of nodes at which to replenish the buffer.
   * @param initializer a functor to initialize the nodes
   */
  MessageBuffer(
    int desiredBufferSize,
    int minSize,
    std::function<T(void)> initializer = [] { return T{}; })
    : desiredBufferSize{ desiredBufferSize }
    , actualSize{ 0 }
    , minSize{ minSize }
    , initializer{ initializer }
  {
    assert(desiredBufferSize >= minSize);
    replenish();
  }

  ~MessageBuffer() { freeMessageStack(storage.pop_all()); }

  /**
   * Replenishes the buffer to desiredBufferSize nodes.
   */
  void replenish()
  {
    auto head = storage.pop_all();
    int numNodes = head ? head->count() : 0;
    auto prev = head ? head->last() : nullptr;
    for (int i = numNodes; i < desiredBufferSize; ++i) {
      auto node = new MessageNode<T>(initializer());
      if (prev) {
        prev->next() = node;
      }
      if (!head) {
        head = node;
      }
      prev = node;
    }
    storage.push_multiple(head, head->last());
    actualSize = desiredBufferSize;
  }

  /**
   * Gets a message node from the buffer.
   * @param onlyIfAvailable set it to true to avoid allocation of new nodes if
   * the buffer is empty.
   * @param emptyBufferFlag if not null, will be set to true if the buffer is
   * empty
   * @return the message node, or nullptr if the buffer is empty and
   * onlyIfAvailable is true
   */
  MessageNode<T>* getMessageNode(bool onlyIfAvailable = false,
                                 bool* emptyBufferFlag = nullptr)
  {
    auto* head = storage.pop_all();
    if (!head) {
      if (emptyBufferFlag) {
        *emptyBufferFlag = true;
      }
      if (onlyIfAvailable) {
        return nullptr;
      }
      return new MessageNode<T>(initializer());
    }
    if (emptyBufferFlag) {
      *emptyBufferFlag = false;
    }
    storage.push_multiple(head->next(), head->last());
    --actualSize;
    head->next() = nullptr;
    return head;
  }

  /**
   * @return the number of available message nodes in the buffer.
   */
  int getNumAvailableNodes() const { return actualSize; }

  /**
   * Replenishes the buffer if it is holding less nodes than the minSize
   * specified in the constructor.
   */
  void maintenance()
  {
    if (actualSize < minSize) {
      replenish();
    }
  }
};

} // namespace lockfree
