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
#include <sstream>

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

static_assert(std::is_move_constructible<Object>::value, "Object should be noexcept move constructable");

static_assert(std::is_move_assignable<Object>::value, "Object should be noexcept move assignable");

static_assert(std::is_copy_constructible<Object>::value, "Object should be move constructable");

static_assert(std::is_copy_assignable<Object>::value, "Object should be move assignable");

using namespace lockfree;
using AsyncObject = Async<Object, int>;

void test(int numStateChangingThreads, int numGetterThreads)
{
  std::cout << "===========================================================\n";
  std::cout << "TESTING WITH: numStateChangingThreads = " << numStateChangingThreads << ", numGetterThreads+"
            << numGetterThreads << "\n";
  int const serverUpdatePeriodMs = 50;
  int const stateChangePeriodMs = 200;
  int const getterPeriodMs = 100;

  auto asyncThread = AsyncThread(serverUpdatePeriodMs);

  auto asyncObject = AsyncObject(0);
  asyncThread.attachObject(asyncObject);
  asyncThread.start();

  std::atomic<bool> runStateChangingThread = true;
  std::vector<std::thread> stateChangingThreads;
  auto makeStateChangingThread = [&] {
    stateChangingThreads.emplace_back([&] {
      auto producerAccessToken = asyncObject.createProducer();
      asyncObject.getProducer(producerAccessToken).preallocateNodes(1024);
      while (runStateChangingThread) {
        auto& producer = asyncObject.getProducer(producerAccessToken);
        bool const sent = producer.submitChangeIfNodeAvailable(AsyncObject::ChangeSettings([](int& state) {
          std::stringstream s;
          s << "incrementing state. prev amount = " << state << "\n";
          std::cout << s.str();
          state += 1;
        }));
        std::stringstream s;
        s << "sending message from state changing thread: " << (sent ? "success" : "failure") << "\n";
        std::cout << s.str();
        std::this_thread::sleep_for(std::chrono::milliseconds(stateChangePeriodMs));
      }
      std::cout << "state changing thread stopped.\n";
    });
  };

  std::atomic<bool> runGetterThreads = true;
  std::vector<std::thread> getterThreads;
  auto makeGetterThread = [&] {
    getterThreads.emplace_back([&] {
      auto instanceAccessToken = asyncObject.createInstance();
      while (runGetterThreads) {
        auto& instance = asyncObject.getInstance(instanceAccessToken);
        instance.update();
        Object& object = instance.get();
        std::stringstream s;
        s << "from access point thread: " << object.getState() << "\n";
        std::cout << s.str();
        std::this_thread::sleep_for(std::chrono::milliseconds(getterPeriodMs));
      }
      std::cout << "access point thread stopped.\n";
    });
  };

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
  std::cout << "joining getter threads\n";
  for(auto&thread:getterThreads){
    if(thread.joinable())
      thread.join();
  }
  std::cout << "joining state changing threads\n";
  for(auto&thread:stateChangingThreads){
    if(thread.joinable())
      thread.join();
  }
  std::cout << "===========================================================\n\n\n\n";
}

int main()
{
  test(1, 4);
  test(2, 4);
  test(4, 4);
}
