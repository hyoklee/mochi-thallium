/*
 * Copyright (c) 2017 UChicago Argonne, LLC
 *
 * See COPYRIGHT in top-level directory.
 */

#ifndef __THALLIUM_POOL_HPP
#define __THALLIUM_POOL_HPP

#include <abt.h>
#include <functional>
#include <memory>
#include <thallium/abt_errors.hpp>
#include <thallium/anonymous.hpp>
#include <thallium/exception.hpp>
#include <thallium/managed.hpp>
#include <thallium/unit_type.hpp>

namespace thallium {

class xstream;
class scheduler;
class task;
class thread;

/**
 * Exception class thrown by the pool class.
 */
class pool_exception : public exception {
  public:
    template <typename... Args>
    pool_exception(Args&&... args)
    : exception(std::forward<Args>(args)...) {}
};

#define TL_POOL_EXCEPTION(__fun, __ret)                                        \
    pool_exception(#__fun, " returned ", abt_error_get_name(__ret), " (",      \
                   abt_error_get_description(__ret), ") in ", __FILE__, ":",   \
                   __LINE__);

#define TL_POOL_ASSERT(__call)                                                 \
    {                                                                          \
        int __ret = __call;                                                    \
        if(__ret != ABT_SUCCESS) {                                             \
            throw TL_POOL_EXCEPTION(__call, __ret);                            \
        }                                                                      \
    }

/**
 * @brief Wrapper for Argobots' ABT_pool.
 *
 * NOTE: This class does not provide all the functionalities of
 * Argobot's pool, in particular custom definitions.
 */
class pool {
  public:
    /**
     * @brief Types of accesses enabled by the pool:
     * private, single-producer-single-consumer,
     * multiple-producer-single-consumer, single-
     * producer-multiple-consumer, or multiple-producer-
     * multiple-consumer.
     */
    enum class access : std::int32_t {
        priv = ABT_POOL_ACCESS_PRIV, /* Used by only one ES */
        spsc = ABT_POOL_ACCESS_SPSC, /* Producers on ES1, consumers on ES2 */
        mpsc = ABT_POOL_ACCESS_MPSC, /* Producers on any ES, consumers on the same ES */
        spmc = ABT_POOL_ACCESS_SPMC, /* Producers on the same ES, consumers on any ES */
        mpmc = ABT_POOL_ACCESS_MPMC  /* Producers on any ES, consumers on any ES */
    };

    /**
     * @brief Kind of pool.
     */
    enum class kind : std::int32_t {
        fifo      = ABT_POOL_FIFO,     /* FIFO pool */
        fifo_wait = ABT_POOL_FIFO_WAIT /* FIFO pool with ability to wait for units */
    };

  private:

    template <typename P, typename U, typename Palloc = std::allocator<P>,
              typename Ualloc = std::allocator<U>>
    struct pool_def {
      private:
        static Palloc pool_allocator;
        static Ualloc unit_allocator;

      public:
        static ABT_unit_type u_get_type(ABT_unit u) {
            auto uu = reinterpret_cast<U*>(u);
            return (ABT_unit_type)(uu->get_type());
        }

        static ABT_thread u_get_thread(ABT_unit u) {
            auto uu = reinterpret_cast<U*>(u);
            return uu->get_thread().native_handle();
        }

        static ABT_task u_get_task(ABT_unit u) {
            auto uu = reinterpret_cast<U*>(u);
            return uu->get_task().native_handle();
        }

        static ABT_bool u_is_in_pool(ABT_unit u) {
            auto uu = reinterpret_cast<U*>(u);
            return (ABT_bool)(uu->is_in_pool());
        }

        static ABT_unit u_create_from_thread(ABT_thread t);

        static ABT_unit u_create_from_task(ABT_task t);

        static void u_free(ABT_unit* u) {
            auto uu = reinterpret_cast<U*>(*u);
            std::allocator_traits<Ualloc>::destroy(unit_allocator, uu);
            std::allocator_traits<Ualloc>::deallocate(unit_allocator, uu, 1);
            *u = nullptr;
        }

        static int p_init(ABT_pool p, ABT_pool_config cfg) {
            (void)cfg;
            P* impl =
                std::allocator_traits<Palloc>::allocate(pool_allocator, 1);
            std::allocator_traits<Palloc>::construct(pool_allocator, impl);
            int ret = ABT_pool_set_data(p, reinterpret_cast<void*>(impl));
            return ret;
        }

        static size_t p_get_size(ABT_pool p) {
            void* data;
            TL_POOL_ASSERT(ABT_pool_get_data(p, &data));
            auto  impl = reinterpret_cast<P*>(data);
            return impl->get_size();
        }

        static void p_push(ABT_pool p, ABT_unit u) {
            void* data;
            TL_POOL_ASSERT(ABT_pool_get_data(p, &data));
            auto  impl = reinterpret_cast<P*>(data);
            impl->push(reinterpret_cast<U*>(u));
        }

        static int p_remove(ABT_pool p, ABT_unit u) {
            void* data;
            int   ret  = ABT_pool_get_data(p, &data);
            if(ret != ABT_SUCCESS) return ret;
            auto  impl = reinterpret_cast<P*>(data);
            impl->remove(reinterpret_cast<U*>(u));
            return ABT_SUCCESS;
        }

        static ABT_unit p_pop(ABT_pool p) {
            void* data;
            TL_POOL_ASSERT(ABT_pool_get_data(p, &data));
            auto  impl = reinterpret_cast<P*>(data);
            U*    u    = impl->pop();
            return reinterpret_cast<ABT_unit>(u);
        }

        static int p_free(ABT_pool p) {
            void* data;
            int   ret = ABT_pool_get_data(p, &data);
            if(ret != ABT_SUCCESS)
                return ret;
            auto impl = reinterpret_cast<P*>(data);
            std::allocator_traits<Palloc>::destroy(pool_allocator, impl);
            std::allocator_traits<Palloc>::deallocate(pool_allocator, impl, 1);
            return ABT_SUCCESS;
        }
    };

    ABT_pool m_pool;

    friend class managed<pool>;
    friend class xstream;
    friend class scheduler;
    friend class task;
    friend class thread;

    static void forward_work_unit(void* fp) {
        auto f = reinterpret_cast<std::function<void(void)>*>(fp);
        (*f)();
        delete f;
    }

    void destroy() {
        if(m_pool != ABT_POOL_NULL)
            ABT_pool_free(&m_pool);
    }

  public:
    /**
     * @brief Constructor used to build a pool  out of an existing handle.
     *
     * @param p existing ABT_pool handle. May be null.
     */
    explicit pool(ABT_pool p) noexcept
    : m_pool(p) {}

    /**
     * @brief Default constructor handles a null pool.
     */
    pool() noexcept
    : m_pool(ABT_POOL_NULL) {}

    /**
     * @brief Type of the underlying native handle.
     */
    typedef ABT_pool native_handle_type;

    /**
     * @brief Get the underlying native handle.
     *
     * @return the underlying native handle.
     */
    native_handle_type native_handle() const noexcept { return m_pool; }

    /**
     * @brief Create a pool with user-provided pool type P
     * and unit type U. Returns it as a managed<pool> object.
     *
     * @tparam P Custom pool type
     * @tparam U Custom unit type
     * @tparam Palloc Special allocator to allocate a pool
     * @tparam Ualloc Special allocator to allocate a unit
     *
     * @return a managed<pool> object.
     *
     * IMPORTANT: The destructor of a managed<pool> will try
     * to destroy the pool. If the pool is still attached to
     * a scheduler, this will cause undefined behaviors.
     * Make sure all the schedulers that use this pool have
     * been destroyed before the pool goes out of scope.
     *
     * The type of unit U should adhere to the following model:
     *
     * \code{.cpp}
     *
     * class U {
     *   public:
     *     // initialization from a thread
     *     Unit(const thread& t);
     *     // initialization from a task
     *     Unit(const task& t);
     *     // returns the type of unit
     *     unit_type get_type() const;
     *     // returns a thread if the unit_type is thread
     *     const tl::thread& get_thread() const;
     *     // returns a task if the unit_type is task
     *     const tl::task& get_task() const;
     *     // returns whether the unit is in a pool
     *     bool is_in_pool() const;
     * };
     *
     * \endcode
     *
     * The type of pool P should adhere to the following model:
     *
     * \code{.cpp}
     *
     * class Pool {
     *   public:
     *     // access type
     *     static const pool::access access_type;
     *     // get the size of the pool
     *     size_t get_size() const;
     *     // push a unit into the pool
     *     void push(U* u);
     *     // pop a unit from the pool
     *     my_unit* pop();
     *     // remove a unit from the pool
     *     void remove(my_unit* u);
     * };
     *
     * \endcode
     *
     */
    template <typename P, typename U,
              typename Palloc = std::allocator<P>,
              typename Ualloc = std::allocator<U>>
    static managed<pool> create() {
        auto A = P::access_type;
        using D = pool_def<P, U, Palloc, Ualloc>;
        ABT_pool_def def;
        def.access               = (ABT_pool_access)A;
        def.u_get_type           = D::u_get_type;
        def.u_get_thread         = D::u_get_thread;
        def.u_get_task           = D::u_get_task;
        def.u_is_in_pool         = D::u_is_in_pool;
        def.u_create_from_thread = D::u_create_from_thread;
        def.u_create_from_task   = D::u_create_from_task;
        def.u_free               = D::u_free;
        def.p_init               = D::p_init;
        def.p_get_size           = D::p_get_size;
        def.p_push               = D::p_push;
        def.p_pop                = D::p_pop;
        def.p_remove             = D::p_remove;
        def.p_free               = D::p_free;
        ABT_pool p;
        TL_POOL_ASSERT(ABT_pool_create(&def, ABT_POOL_CONFIG_NULL, &p));
        return make_managed<pool>(p);
    }

    /**
     * @brief Builds a pool using a default implementation from Argobots.
     *
     * @param access Access type enabled by the pool.
     * @param kind Kind of pool (fifo or fifo_wait).
     *
     * IMPORTANT: The destructor of a managed<pool> will try
     * to destroy the pool. If the pool is still attached to
     * a scheduler, this will cause undefined behaviors.
     * Make sure all the schedulers that use this pool have
     * been destroyed before the pool goes out of scope.
     */
    static managed<pool> create(access a, kind k = kind::fifo) {
        ABT_pool p;
        TL_POOL_ASSERT(ABT_pool_create_basic((ABT_pool_kind)k, (ABT_pool_access)a,
                                             ABT_FALSE, &p));
        return make_managed<pool>(p);
    }

    /**
     * @brief Copy constructor.
     */
    pool(const pool& other) = default;

    /**
     * @brief Move constructor.
     */
    pool(pool&& other) noexcept
    : m_pool(other.m_pool) {
        other.m_pool = ABT_POOL_NULL;
    }

    /**
     * @brief Copy assignment operator.
     */
    pool& operator=(const pool& other) = default;

    /**
     * @brief Move assignment operator.
     */
    pool& operator=(pool&& other) noexcept {
        if(this == &other)
            return *this;
        m_pool       = other.m_pool;
        other.m_pool = ABT_POOL_NULL;
        return *this;
    }

    /**
     * @brief Equality operator.
     */
    bool operator==(const pool& other) const noexcept {
        return m_pool == other.m_pool;
    }

    /**
     * @brief Non-equality operator.
     */
    bool operator!=(const pool& other) const noexcept {
        return !(*this == other);
    }

    /**
     * @brief Destructor.
     */
    ~pool() = default;

    /**
     * @brief Check if the underlying pool handle is null.
     *
     * @return true if the pool handle is null, false otherwise.
     */
    bool is_null() const noexcept { return m_pool == ABT_POOL_NULL; }

    /**
     * @brief Returns true if the pool handle is not null.
     */
    operator bool() const noexcept { return !is_null(); }

    /**
     * @brief Get the access type of the pool.
     *
     * @return the access type of the pool.
     */
    access get_access() const {
        ABT_pool_access a;
        TL_POOL_ASSERT(ABT_pool_get_access(m_pool, &a));
        return (access)a;
    }

    /**
     * @brief Get the total number of elements
     * present in the pool, including blocked ULTs
     * and migrating ULTs.
     *
     * @return total number of elements in the pool.
     */
    std::size_t total_size() const {
        std::size_t s;
        TL_POOL_ASSERT(ABT_pool_get_total_size(m_pool, &s));
        return s;
    }

    /**
     * @brief Get the number of elements in the pool,
     * not including the ULTs that are blocked.
     *
     * @return the number of elements in the pool.
     */
    std::size_t size() const {
        std::size_t s;
        TL_POOL_ASSERT(ABT_pool_get_size(m_pool, &s));
        return s;
    }

    /**
     * @brief Get the id of the pool.
     *
     * @return the id of the pool.
     */
    int id() const {
        int i;
        TL_POOL_ASSERT(ABT_pool_get_id(m_pool, &i));
        return i;
    }

    /**
     * @brief Pops a unit of work out of the pool.
     *
     * @tparam U Type of the unit of work.
     *
     * @return A pointer to a unit of work.
     */
    template <typename U> inline U* pop() {
        ABT_unit u;
        TL_POOL_ASSERT(ABT_pool_pop(m_pool, &u));
        return reinterpret_cast<U*>(u);
    }

    /**
     * @brief Pushes a unit of work into the pool.
     * The pool must be a custom pool that manages units of type U.
     * The work unit must have been popped from a pool managing the
     * same type of work units U. The work unit must not have been
     * created manually.
     *
     * @tparam U type of work unit.
     * @param unit work unit.
     */
    template <typename U> inline void push(U* unit) {
        TL_POOL_ASSERT(ABT_pool_push(m_pool, reinterpret_cast<ABT_unit>(unit)));
    }

    /**
     * @brief Removes a work unit from the pool.
     * The pool must be a custom pool that manages units of type U.
     *
     * @tparam U type of work unit.
     * @param unit work unit.
     */
    template <typename U> inline void remove(U* unit) {
        TL_POOL_ASSERT(
            ABT_pool_remove(m_pool, reinterpret_cast<ABT_unit>(unit)));
    }

    /**
     * @brief This function should be called inside a custom scheduler
     * to run a work unit on the ES the scheduler runs on. The type of work
     * unit U should match the type used by the custom pool.
     *
     * @tparam U type of work unit.
     * @param unit Work unit.
     */
    template <typename U> inline void run_unit(U* unit) {
        TL_POOL_ASSERT(
            ABT_xstream_run_unit(reinterpret_cast<ABT_unit>(unit), m_pool));
    }

    /**
     * @brief Push a scheduler to a pool.
     * By pushing a scheduler, the user can change the running scheduler:
     * when the top scheduler (the running scheduler) will pick it from
     * the pool and run it, it will become the new scheduler. This new
     * scheduler will be in charge until it explicitly yields, except
     * if scheduler::finish() or scheduler::exit() are called.
     *
     * @param sched Scheduler to push.
     */
    void add_sched(const scheduler& sched);

    /**
     * @brief Create a task running the specified function and push it
     * into the pool.
     *
     * @tparam F type of function to run as a task. Must have operator()().
     * @param f Function to run as a task.
     *
     * @return a task object managing the created task.
     */
    template <typename F> managed<task> make_task(F&& f);

    template <typename F> void make_task(F&& f, const anonymous& a);

    /**
     * @brief Create a thread running the specified function and push it
     * into the pool.
     *
     * @tparam F type of function to run as a task. Must have operator()().
     * @param f Function to run as a task.
     *
     * @return a thread object managing the created thread.
     */
    template <typename F> managed<thread> make_thread(F&& f);

    template <typename F> void make_thread(F&& f, const anonymous& a);

    /**
     * @brief Create a thread running the specified function and push it
     * into the pool.
     *
     * @tparam F type of function to run as a task. Must have operator()().
     * @tparam Attr type of thread attribute (must be thread::attribute).
     * @param f Function to run as a task.
     * @param attr Thread attributes.
     *
     * @return a thread object managing the created thread.
     */
    template <typename F, typename Attr>
    managed<thread> make_thread(F&& f, const Attr& attr);

    template <typename F, typename Attr>
    void make_thread(F&& f, const Attr& attr, const anonymous& a);

    /**
     * @brief Use an existing thread that has been joined to
     * start working on a new function.
     *
     * @tparam F Function type
     * @param t thread to revive
     * @param fun Function
     */
    template <typename F>
    void revive_thread(thread& t, F&& fun);

    /**
     * @brief Use an existing task that has been joined to
     * start working on a new function.
     *
     * @tparam F Function type
     * @param t task to revive
     * @param fun Function
     */
    template <typename F>
    void revive_task(task& t, F&& fun);
};

template <typename P, typename U, typename Palloc, typename Ualloc>
Palloc pool::pool_def<P, U, Palloc, Ualloc>::pool_allocator;

template <typename P, typename U, typename Palloc, typename Ualloc>
Ualloc pool::pool_def<P, U, Palloc, Ualloc>::unit_allocator;

} // namespace thallium

#include <thallium/task.hpp>
#include <thallium/thread.hpp>
#include <thallium/scheduler.hpp>

namespace thallium {

inline void pool::add_sched(const scheduler& sched) {
    int ret = ABT_pool_add_sched(m_pool, sched.native_handle());
    if(ret != ABT_SUCCESS) {
        throw pool_exception(
            "ABT_pool_add_sched(m_pool, sched.native_handle()) returned ",
            abt_error_get_name(ret), " (", abt_error_get_description(ret),
            ") in ", __FILE__, ":", __LINE__);
    }
}

template <typename F>
managed<task> pool::make_task(F&& f) {
    auto fp = new std::function<void(void)>(std::forward<F>(f));
    return task::create_on_pool(m_pool, forward_work_unit,
            reinterpret_cast<void*>(fp));
}

template <typename F>
void pool::make_task(F&& f, const anonymous& a) {
    auto fp = new std::function<void(void)>(std::forward<F>(f));
    task::create_on_pool(m_pool, forward_work_unit,
            reinterpret_cast<void*>(fp), a);
}
    
template <typename F>
managed<thread> pool::make_thread(F&& f) {
    auto fp = new std::function<void(void)>(std::forward<F>(f));
    return thread::create_on_pool(m_pool, forward_work_unit,
            reinterpret_cast<void*>(fp));
}

template <typename F>
void pool::make_thread(F&& f, const anonymous& a) {
    auto fp = new std::function<void(void)>(std::forward<F>(f));
    thread::create_on_pool(m_pool, forward_work_unit,
            reinterpret_cast<void*>(fp), a);
}

template <typename F, typename Attr>
managed<thread> pool::make_thread(F&& f, const Attr& attr) {
    auto fp = new std::function<void(void)>(std::forward<F>(f));
    return thread::create_on_pool(m_pool, forward_work_unit,
            reinterpret_cast<void*>(fp), attr);
}

template <typename F, typename Attr>
void pool::make_thread(F&& f, const Attr& attr, const anonymous& a) {
    auto fp = new std::function<void(void)>(std::forward<F>(f));
    thread::create_on_pool(m_pool, forward_work_unit,
            reinterpret_cast<void*>(fp), attr, a);
}

template <typename F>
void pool::revive_thread(thread& t, F&& f) {
    auto fp = new std::function<void(void)>(std::forward<F>(f));
    TL_POOL_ASSERT(ABT_thread_revive(m_pool, forward_work_unit,
                      reinterpret_cast<void*>(fp), &(t.m_thread)));
}

template <typename F>
void pool::revive_task(task& t, F&& f) {
    auto fp = new std::function<void(void)>(std::forward<F>(f));
    TL_POOL_ASSERT(ABT_task_revive(m_pool, forward_work_unit,
                      reinterpret_cast<void*>(fp), &(t.m_task)));
}

template <typename P, typename U, typename Palloc, typename Ualloc>
ABT_unit pool::pool_def<P,U,Palloc,Ualloc>::u_create_from_thread(ABT_thread t) {
    auto uu =
        std::allocator_traits<Ualloc>::allocate(unit_allocator, 1);
    std::allocator_traits<Ualloc>::construct(unit_allocator, uu,
            thread(t));
    return reinterpret_cast<ABT_unit>(uu);
}
        
template <typename P, typename U, typename Palloc, typename Ualloc>
ABT_unit pool::pool_def<P,U,Palloc,Ualloc>::u_create_from_task(ABT_task t) {
    auto uu =
        std::allocator_traits<Ualloc>::allocate(unit_allocator, 1);
    std::allocator_traits<Ualloc>::construct(unit_allocator, uu,
            task(t));
    return reinterpret_cast<ABT_unit>(uu);
}

}

#undef TL_POOL_EXCEPTION
#undef TL_POOL_ASSERT

#endif /* end of include guard */
