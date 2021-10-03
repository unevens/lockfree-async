# lockfree-async

[lockfree-async](https://github.com/unevens/lockfree-async) is a C++ header-only simple template library for lock-free
inter-thread communication.

The fundamental building block of this library is the "IMB Freelist" multiple-producer multiple-consumer LIFO stack, as
implemented by Ross Bencina in [Queue World](https://github.com/RossBencina/QueueWorld).

## LockFreeMessenger.hpp

The template class `LockFreeMessenger<T>` is a wrapper around the Queue World's multiple-producer multiple-consumer LIFO
stack. It implements functionality to send and receive data of type `T` between threads in a lock-free way, and to
preallocate the resources to do so.

## PreAllocated.hpp

The template class `PreAllocated<T>` owns an object of class `T` which can be shared between a non realtime thread and a
realtime thread, so that the non realtime thread can perform any blocking operation, such as creating the object, and the realtime thread can use the
object.

## Development

There is more on the dev branch, but it is a work in progress to which the author is not particularly committed.
