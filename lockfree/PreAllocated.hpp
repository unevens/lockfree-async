/*
Copyright 2021 Dario Mambro

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
#include "Messenger.hpp"
#include <cassert>

namespace lockfree {

template<class Object>
class PreAllocated final
{
public:
  Object* getFromRealtimeThread()
  {
    auto newObject = messengerForNewObjects.receiveLastMessage();
    if (newObject) {
      messengerForOldObjects.send(std::move(currentObjectStorage));
      currentObjectStorage = std::move(newObject.value());
      currentObjectPtr.store(currentObjectStorage.get(), std::memory_order_release);
    }
    return currentObjectStorage.get();
  }

  Object* getFromNonRealtimeThread()
  {
    return currentObjectPtr.load(std::memory_order_acquire);
  }

  void set(std::unique_ptr<Object> newObject)
  {
    lockfree::receiveAndHandleMessageStack(messengerForOldObjects, [](auto& object) { object.reset(); });
    messengerForNewObjects.send(std::move(newObject));
  }

  void preallocateMessageNodes(int numNodesToPreallocate)
  {
    messengerForNewObjects.preallocateNodes(numNodesToPreallocate);
    messengerForOldObjects.preallocateNodes(numNodesToPreallocate);
  }

  explicit PreAllocated(int numNodesToPreallocate = 128)
  {
    preallocateMessageNodes(numNodesToPreallocate);
  }

private:
  lockfree::Messenger<std::unique_ptr<Object>> messengerForNewObjects;
  lockfree::Messenger<std::unique_ptr<Object>> messengerForOldObjects;
  std::unique_ptr<Object> currentObjectStorage;
  std::atomic<Object*> currentObjectPtr{ nullptr };
};

} // namespace lockfree
