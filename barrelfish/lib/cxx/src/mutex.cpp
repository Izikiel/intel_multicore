//===------------------------- mutex.cpp ----------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is dual licensed under the MIT and the University of Illinois Open
// Source Licenses. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "mutex"
#include "limits"
#include "system_error"
#include "cassert"

_LIBCPP_BEGIN_NAMESPACE_STD

const defer_lock_t  defer_lock = {};
const try_to_lock_t try_to_lock = {};
const adopt_lock_t  adopt_lock = {};

mutex::~mutex()
{
  //    int e = pthread_mutex_destroy(&__m_);
//     assert(e == 0);
}

void
mutex::lock()
{
#ifndef BARRELFISH
    int ec = pthread_mutex_lock(&__m_);
    if (ec)
        __throw_system_error(ec, "mutex lock failed");
#else
    thread_mutex_lock(&__m_);
#endif
}

bool
mutex::try_lock()
{
    return thread_mutex_trylock(&__m_) == 0;
}

void
mutex::unlock()
{
#ifndef BARRELFISH
    int ec = pthread_mutex_unlock(&__m_);
    assert(ec == 0);
#else
    thread_mutex_unlock(&__m_);
#endif
}

// recursive_mutex

recursive_mutex::recursive_mutex()
{
#ifndef BARRELFISH
    pthread_mutexattr_t attr;
    int ec = pthread_mutexattr_init(&attr);
    if (ec)
        goto fail;
    ec = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    if (ec)
    {
        pthread_mutexattr_destroy(&attr);
        goto fail;
    }
    ec = pthread_mutex_init(&__m_, &attr);
    if (ec)
    {
        pthread_mutexattr_destroy(&attr);
        goto fail;
    }
    ec = pthread_mutexattr_destroy(&attr);
    if (ec)
    {
        pthread_mutex_destroy(&__m_);
        goto fail;
    }
    return;
fail:
#endif
    __throw_system_error(1234, "recursive_mutex constructor failed");
}

recursive_mutex::~recursive_mutex()
{
#ifndef BARRELFISH
    int e = pthread_mutex_destroy(&__m_);
    assert(e == 0);
#endif
}

void
recursive_mutex::lock()
{
#ifndef BARRELFISH
    int ec = pthread_mutex_lock(&__m_);
    if (ec)
        __throw_system_error(ec, "recursive_mutex lock failed");
#else
    thread_mutex_lock(&__m_);
#endif
}

void
recursive_mutex::unlock()
{
#ifndef BARRELFISH
    int e = pthread_mutex_unlock(&__m_);
    assert(e == 0);
#else
    thread_mutex_unlock(&__m_);
#endif
}

bool
recursive_mutex::try_lock()
{
    return thread_mutex_trylock(&__m_) == 0;
}

// timed_mutex

timed_mutex::timed_mutex()
    : __locked_(false)
{
}

timed_mutex::~timed_mutex()
{
    lock_guard<mutex> _(__m_);
}

void
timed_mutex::lock()
{
    unique_lock<mutex> lk(__m_);
    while (__locked_)
        __cv_.wait(lk);
    __locked_ = true;
}

bool
timed_mutex::try_lock()
{
    unique_lock<mutex> lk(__m_, try_to_lock);
    if (lk.owns_lock() && !__locked_)
    {
        __locked_ = true;
        return true;
    }
    return false;
}

void
timed_mutex::unlock()
{
    lock_guard<mutex> _(__m_);
    __locked_ = false;
    __cv_.notify_one();
}

// recursive_timed_mutex

recursive_timed_mutex::recursive_timed_mutex()
    : __count_(0),
      __id_(0)
{
}

recursive_timed_mutex::~recursive_timed_mutex()
{
    lock_guard<mutex> _(__m_);
}

void
recursive_timed_mutex::lock()
{
#ifndef BARRELFISH
    pthread_t id = pthread_self();
    unique_lock<mutex> lk(__m_);
    if (pthread_equal(id, __id_))
    {
        if (__count_ == numeric_limits<size_t>::max())
            __throw_system_error(EAGAIN, "recursive_timed_mutex lock limit reached");
        ++__count_;
        return;
    }
    while (__count_ != 0)
        __cv_.wait(lk);
    __count_ = 1;
    __id_ = id;
#else
    __throw_system_error(1234, "recursive_timed_mutex lock failed");
#endif
}

bool
recursive_timed_mutex::try_lock()
{
#ifndef BARRELFISH
    pthread_t id = pthread_self();
    unique_lock<mutex> lk(__m_, try_to_lock);
    if (lk.owns_lock() && (__count_ == 0 || pthread_equal(id, __id_)))
    {
        if (__count_ == numeric_limits<size_t>::max())
            return false;
        ++__count_;
        __id_ = id;
        return true;
    }
    return false;
#else
    __throw_system_error(1234, "recursive_timed_mutex try_lock failed");
    return false;
#endif
}

void
recursive_timed_mutex::unlock()
{
    unique_lock<mutex> lk(__m_);
    if (--__count_ == 0)
    {
        __id_ = 0;
        lk.unlock();
        __cv_.notify_one();
    }
}

// If dispatch_once_f ever handles C++ exceptions, and if one can get to it
// without illegal macros (unexpected macros not beginning with _UpperCase or
// __lowercase), and if it stops spinning waiting threads, then call_once should
// call into dispatch_once_f instead of here. Relevant radar this code needs to
// keep in sync with:  7741191.

#ifndef BARRELFISH

static pthread_mutex_t mut = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv  = PTHREAD_COND_INITIALIZER;

void
__call_once(volatile unsigned long& flag, void* arg, void(*func)(void*))
{
    pthread_mutex_lock(&mut);
    while (flag == 1)
        pthread_cond_wait(&cv, &mut);
    if (flag == 0)
    {
#ifndef _LIBCPP_NO_EXCEPTIONS
        try
        {
#endif  // _LIBCPP_NO_EXCEPTIONS
            flag = 1;
            pthread_mutex_unlock(&mut);
            func(arg);
            pthread_mutex_lock(&mut);
            flag = ~0ul;
            pthread_mutex_unlock(&mut);
            pthread_cond_broadcast(&cv);
#ifndef _LIBCPP_NO_EXCEPTIONS
        }
        catch (...)
        {
            pthread_mutex_lock(&mut);
            flag = 0ul;
            pthread_mutex_unlock(&mut);
            pthread_cond_broadcast(&cv);
            throw;
        }
#endif  // _LIBCPP_NO_EXCEPTIONS
    }
    else
        pthread_mutex_unlock(&mut);
}

#else

static struct thread_mutex mut = THREAD_MUTEX_INITIALIZER;
static struct thread_cond cv  = THREAD_COND_INITIALIZER;

void
__call_once(volatile unsigned long& flag, void* arg, void(*func)(void*))
{
    thread_mutex_lock(&mut);
    while (flag == 1)
        thread_cond_wait(&cv, &mut);
    if (flag == 0)
    {
        // try
        // {
            flag = 1;
            thread_mutex_unlock(&mut);
            func(arg);
            thread_mutex_lock(&mut);
            flag = ~0ul;
            thread_mutex_unlock(&mut);
            thread_cond_broadcast(&cv);
        // }
        // catch (...)
        // {
        //     thread_mutex_lock(&mut);
        //     flag = 0ul;
        //     thread_mutex_unlock(&mut);
        //     thread_cond_broadcast(&cv);
        //     throw;
        // }
    }
    else
        thread_mutex_unlock(&mut);
}

#endif

_LIBCPP_END_NAMESPACE_STD
