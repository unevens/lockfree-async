/*
Copyright 2019-2021 Dario Mambro

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

#include "QueueWorld/QwMpmcPopAllLifoStack.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <functional>
#include <memory>
#include <optional>

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
  T& get()
  {
    return message;
  }

  /**
   * Sets the message.
   * @param message_ the message to store in the MessageNode object.
   */
  void set(T message_)
  {
    message = std::move(message_);
  }

  /**
   * @return a reference to the next MessageNode in the stack.
   */
  MessageNode*& next()
  {
    return links_[0];
  }

  /**
   * @return a reference to the previous MessageNode in the stack. This is only
   * used by handleMessageStack, and its result is not valid anywhere else.
   */
  MessageNode*& prev()
  {
    return links_[1];
  }

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
inline void handleMessageStack(MessageNode<T>* head, Action action)
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
    action(head->get());
    head = head->prev();
  }
}

/**
 * Frees a stack of message nodes.
 * @tparam T data type held by the node.
 * @see QwLinkTraits.h
 */
template<typename T>
inline void freeMessageStack(MessageNode<T>* head)
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
using LifoStack = QwMpmcPopAllLifoStack<MessageNode<T>*, MessageNode<T>::LINK_INDEX_1>;

/**
 * Wrapper around QueueWorld's QwMpmcPopAllLifoStack with functionality to
 * allocate and recycle nodes.
 * @tparam T the type of the data held by the nodes, char is used as default for
 * Messages that are just notification and do not need to have data.
 */
template<typename T>
class Messenger final
{
  LifoStack<T> lifo;
  LifoStack<T> storage;

public:
  /**
   * Sends a message already wrapped in a MessageNode. Non-blocking.
   * @param node the massage node to send.
   * @return true if the message was sent using an already allocated node, false
   * if a node was allocated
   */
  void send(MessageNode<T>* node)
  {
    lifo.push(node);
  }

  /**
   * Sends a Message, wrapping in a MessageNode from the storage if there is
   * one available, otherwise it allocates a new one.
   * @param message the massage to send.
   */
  bool send(T&& message)
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
    return fromStorage;
  }

  /**
   * Sends a Message, wrapping it in a MessageNode from the storage if there is
   * one available, otherwise it does not send the message and returns false.
   * @param message the massage to send.
   * @return true if the message was sent, false if it was not sent
   */
  bool sendIfNodeAvailable(T&& message)
  {
    auto node = storage.pop_all();
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
   * @return a std::optional holding the maybe received message
   */
  std::optional<T> receiveLastMessage()
  {
    MessageNode<T>* head = lifo.pop_all();
    if (!head) {
      return std::nullopt;
    }
    auto message = std::optional<T>(std::move(head->get()));
    storage.push_multiple(head, head->last());
    return message;
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
  MessageNode<T>* receiveAllNodes()
  {
    return lifo.pop_all();
  }

  /**
   * @return all the preallocated message nodes that are kept in storage.
   */
  MessageNode<T>* popStorage()
  {
    return storage.pop_all();
  }

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
   * Allocates message nodes using the default constructor of T and puts them into the storage, ready to be used for
   * sending messages.
   * @param numNodesToPreallocate the number of nodes to preallocate.
   */
  void allocateNodes(int numNodesToAllocate)
  {
    MessageNode<T>* head = nullptr;
    MessageNode<T>* it = nullptr;
    for (int i = 0; i < numNodesToAllocate; ++i) {
      auto node = new MessageNode<T>(T{});
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

  /**
   * Allocates message nodes using initializer functor and puts them into the storage, ready to be used for
   * sending messages.
   * @param numNodesToPreallocate the number of nodes to preallocate.
   * @param initializer functor to initialize the nodes.
   */
  void allocateNodes(int numNodesToAllocate, typename std::function<T()> initializer)
  {
    MessageNode<T>* head = nullptr;
    MessageNode<T>* it = nullptr;
    for (int i = 0; i < numNodesToAllocate; ++i) {
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

  /**
   * Discards all messages, recycling their nodes.
   */
  void discardAllMessages()
  {
    recycle(lifo.pop_all());
  }

  /**
   * Frees any nodes currently in the storage.
   */
  void freeStorage()
  {
    freeMessageStack(storage.pop_all());
  }

  /**
   * Discards all messages and frees the nodes that were holding them.
   */
  void discardAndFreeAllMessages()
  {
    freeMessageStack(lifo.pop_all());
  }

  /**
   * Destructor.
   */
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
 * @return the number of handled messages
 */
template<typename T, class Action>
inline int receiveAndHandleMessageStack(Messenger<T>& messenger, Action action)
{
  auto messages = messenger.receiveAllNodes();
  auto numMessages = messages->count();
  handleMessageStack(messages, action);
  messenger.recycle(messages);
  return numMessages;
}

} // namespace lockfree
