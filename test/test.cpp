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

#include "lockfree/Async.hpp"
#include <chrono>
#include <iostream>

class Object final
{
  int state;

public:
  int getState()
  {
    return state;
  };
  explicit Object(int state = 0)
    : state(state)
  {}
};

static_assert(std::is_move_constructible<Object>::value, "Object should be noexcept move constructible");

static_assert(std::is_move_assignable<Object>::value, "Object should be noexcept move assignable");

static_assert(std::is_copy_constructible<Object>::value, "Object should be move constructible");

static_assert(std::is_copy_assignable<Object>::value, "Object should be move assignable");

using namespace lockfree;
using AsyncObject = Async<Object, int>;

int main()
{
  int const serverUpdatePeriodMs = 50;
  int const stateChangePeriodMs = 32;
  int const getterPeriodMs = 20;

  auto asyncThread = AsyncThread(serverUpdatePeriodMs);

  auto asyncObjecct = AsyncObject(0);
  asyncObjecct.preallocateNodes(1024);
  asyncThread.addObject(asyncObjecct);
  asyncThread.start();

  std::atomic<bool> runStateChangingThread = true;
  auto makeStateChangingThread = [&] {
    std::thread([&] {
      while (runStateChangingThread) {
        bool const sent = asyncObjecct.submitChange(AsyncObject::ChangeSettings([](int& state) {
          std::cout << "incrementing state. prev amount = " << state << "\n";
          state += 1;
        }));
        std::cout << "sending message from state changing thread: " << (sent ? "success" : "failure") << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(stateChangePeriodMs));
      }
      std::cout << "state changing thread stopped.\n";
    }).detach();
  };

  std::atomic<bool> runGetterThreads = true;
  auto makeGetterThread = [&] {
    std::thread([&] {
      auto instance = asyncObjecct.requestInstance();
      while (runGetterThreads) {
        instance->update();
        Object& object = instance->get();
        std::cout << "from access point thread: " << object.getState() << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(getterPeriodMs));
      }
      std::cout << "access point thread stopped.\n";
    }).detach();
  };

  constexpr auto numStateChangingThreads = 4;
  constexpr auto numGetterThreads = 4;

  for (int i = 0; i < numStateChangingThreads; ++i) {
    makeStateChangingThread();
  }
  for (int i = 0; i < numGetterThreads; ++i) {
    makeGetterThread();
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  std::cout << "main thread slept for 2 second, stopping other threads\n";

  runStateChangingThread = false;
  runGetterThreads = false;

  std::cout << "stopping async thread\n";
  asyncThread.stop();
  std::cout << "asyncThread stopped\n";
  return 0;
}