#pragma once


#include "ptr.h"

#define lambda(x, fx) [&](auto x) { return (fx); }

namespace ck {

  template <typename T>
  class func;

  template <typename Out, typename... In>
  class func<Out(In...)> {
   public:
    template <typename T>
    func& operator=(T t) {
      m_callable_wrapper = make_box<CallableWrapper<T>>(move(t));
      return *this;
    }

    template <typename T>
    func(T&& t) {
      operator=(move(t));
    }

    func(void) { m_callable_wrapper = nullptr; }

    Out operator()(In... args) const {
      assert(m_callable_wrapper);
      return m_callable_wrapper->call(args...);
    }

    operator bool(void) { return m_callable_wrapper.get() != NULL; }

    void clear(void) { m_callable_wrapper = nullptr; }

    using ReturnType = Out;

   private:
    class CallableWrapperBase {
     public:
      virtual ~CallableWrapperBase() {}
      virtual Out call(In...) = 0;
    };

    template <typename CallableType>
    class CallableWrapper final : public CallableWrapperBase {
     public:
      explicit CallableWrapper(CallableType&& callable) : m_callable(move(callable)) {}

      CallableWrapper(const CallableWrapper&) = delete;
      CallableWrapper& operator=(const CallableWrapper&) = delete;

      Out call(In... in) final override { return m_callable(in...); }

     private:
      CallableType m_callable;
    };

    ck::box<CallableWrapperBase> m_callable_wrapper;
  };
}  // namespace ck
