
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <proxc/config.hpp>

#include <proxc/alt.hpp>
#include <proxc/context.hpp>
#include <proxc/exceptions.hpp>
#include <proxc/scheduler.hpp>

#include <proxc/scheduling_policy/policy_base.hpp>
#include <proxc/scheduling_policy/round_robin.hpp>
#include <proxc/scheduling_policy/work_stealing.hpp>

#include <proxc/detail/num_cpus.hpp>
#include <proxc/spinlock.hpp>

#include <boost/assert.hpp>
#include <boost/intrusive_ptr.hpp>

PROXC_NAMESPACE_BEGIN

namespace hook = detail::hook;

namespace detail {

struct WaitGroup
{
    std::mutex                 mtx_;
    std::condition_variable    cv_;
    std::size_t                count_{ 0 };

    void add( std::size_t count ) noexcept
    { count_ = count; }

    void wait() noexcept
    {
        std::unique_lock< std::mutex > lk{ mtx_ };
        if ( --count_ == 0 ) {
            lk.unlock();
            cv_.notify_all();
        } else {
            cv_.wait( lk, [this]{ return count_ == 0; } );
        }
    }
};

void multicore_scheduler_fn( WaitGroup & wg )
{
    // this will allocated the scheduler for this thread
    auto self = Scheduler::self();
    // need to wait for all of the threads to finish initialize the scheduler
    wg.wait();
    self->resume();
    // when returned, the scheduler has exited and ready to cleanup
}

struct SchedulerInitializer
{
    thread_local static Scheduler *      self_;
    thread_local static std::size_t      self_counter_;

    static std::atomic< std::size_t >    sched_counter_;
    static WaitGroup                     wg_;
    static std::vector< std::thread >    thread_vec_;
    static std::vector< Scheduler * >    sched_vec_;

    static Spinlock                      splk_;

    SchedulerInitializer()
    {
        if ( self_counter_++ == 0 ) {
            // FIXME: new alignment issues?
            auto scheduler = new Scheduler{};
            self_ = scheduler;
            BOOST_ASSERT( Scheduler::running()->is_type( Context::Type::Main ) );

            if ( sched_counter_++ == 0 ) {
                auto num_sched = detail::num_cpus();
                sched_vec_.reserve( num_sched - 1 );
                thread_vec_.reserve( num_sched - 1 );

                wg_.add( num_sched );
                for ( std::size_t i = 0; i < num_sched - 1; ++i ) {
                    thread_vec_.emplace_back( multicore_scheduler_fn, std::ref( wg_ ) );
                }
                wg_.wait();
                sched_counter_ = 0;

            } else {
                std::unique_lock< Spinlock > lk{ splk_ };
                sched_vec_.push_back( self_ );
            }
        }
    }

    ~SchedulerInitializer()
    {
        if ( --self_counter_ == 0 ) {
            if ( sched_counter_++ == 0 ) {
                for ( const auto& sched : sched_vec_ ) {
                    sched->signal_exit();
                }
                for ( auto& th : thread_vec_ ) {
                    th.join();
                }
            }

            BOOST_ASSERT( Scheduler::running()->is_type( Context::Type::Main ) );
            auto scheduler = self_;
            delete scheduler;
        }
    }
};

thread_local Scheduler *   SchedulerInitializer::self_{ nullptr };
thread_local std::size_t   SchedulerInitializer::self_counter_{ 0 };

std::atomic< std::size_t > SchedulerInitializer::sched_counter_{ 0 };
WaitGroup                  SchedulerInitializer::wg_{};
std::vector< std::thread > SchedulerInitializer::thread_vec_{};
std::vector< Scheduler * > SchedulerInitializer::sched_vec_{};

Spinlock                   SchedulerInitializer::splk_{};

} // namespace detail

Scheduler * Scheduler::self() noexcept
{
    //thread_local static boost::context::detail::activation_record_initializer ac_rec_init;
    thread_local static detail::SchedulerInitializer sched_init;
    return detail::SchedulerInitializer::self_;
}

Context * Scheduler::running() noexcept
{
    BOOST_ASSERT( Scheduler::self() != nullptr );
    BOOST_ASSERT( Scheduler::self()->running_ != nullptr );
    return Scheduler::self()->running_;
}

Scheduler::Scheduler()
    : policy_{ new scheduling_policy::WorkStealing{} }
    , main_ctx_{ new Context{ context::main_type } }
    , scheduler_ctx_{ new Context{ context::scheduler_type,
        [this]( void * vp ) { run_( vp ); } } }
{
    main_ctx_->scheduler_      = this;
    scheduler_ctx_->scheduler_ = this;
    running_                   = main_ctx_.get();

    schedule( scheduler_ctx_.get() );
}

Scheduler::~Scheduler()
{
    BOOST_ASSERT( main_ctx_.get() != nullptr );
    BOOST_ASSERT( scheduler_ctx_.get() != nullptr );
    BOOST_ASSERT( running_ == main_ctx_.get() );

    exit_.store( true, std::memory_order_relaxed );
    join( scheduler_ctx_.get() );

    BOOST_ASSERT( main_ctx_->wait_queue_.empty() );
    BOOST_ASSERT( scheduler_ctx_->wait_queue_.empty() );

    scheduler_ctx_.reset();
    main_ctx_.reset();

    for (auto et = work_queue_.begin(); et != work_queue_.end(); ) {
        auto ctx = &( *et );
        et = work_queue_.erase( et );
        intrusive_ptr_release( ctx );
    }
    running_ = nullptr;

    BOOST_ASSERT( work_queue_.empty() );
    BOOST_ASSERT( sleep_queue_.empty() );
    BOOST_ASSERT( alt_sleep_queue_.empty() );
    BOOST_ASSERT( terminated_queue_.empty() );
}

void Scheduler::resume_( Context * to_ctx, CtxSwitchData * data ) noexcept
{
    BOOST_ASSERT(   to_ctx != nullptr );
    BOOST_ASSERT(   running_ != nullptr );
    BOOST_ASSERT( ! to_ctx->is_linked< hook::Ready >() );
    BOOST_ASSERT( ! to_ctx->is_linked< hook::Wait >() );
    BOOST_ASSERT( ! to_ctx->is_linked< hook::Sleep >() );
    BOOST_ASSERT( ! to_ctx->is_linked< hook::Terminated >() );

    std::swap( to_ctx, running_ );

    // context switch
    void * vp = static_cast< void * >( data );
    vp = running_->resume( vp );
    data = static_cast< CtxSwitchData * >( vp );
    resolve_ctx_switch_data( data );
}

void Scheduler::wait() noexcept
{
    resume();
}

void Scheduler::wait( Context * ctx ) noexcept
{
    CtxSwitchData data{ ctx };
    resume( std::addressof( data ) );
}

void Scheduler::wait( std::unique_lock< Spinlock > & splk, bool lock ) noexcept
{
    CtxSwitchData data{ std::addressof( splk ) };
    resume( std::addressof( data ) );
    if ( lock ) {
        splk.lock();
    }
}

bool Scheduler::wait_until( TimePointT const & time_point ) noexcept
{
    return sleep_until( time_point );
}

bool Scheduler::wait_until( TimePointT const & time_point, Context * ctx ) noexcept
{
    CtxSwitchData data{ ctx };
    return sleep_until( time_point, std::addressof( data ) );
}

bool Scheduler::wait_until( TimePointT const & time_point, std::unique_lock< Spinlock > & splk, bool lock ) noexcept
{
    CtxSwitchData data{ std::addressof( splk ) };
    auto ret = sleep_until( time_point, std::addressof( data ) );
    if ( ! ret && lock ) {
        splk.lock();
    }
    return ret;
}

void Scheduler::alt_wait( Alt * alt, std::unique_lock< Spinlock > & splk, bool lock ) noexcept
{
    BOOST_ASSERT(   alt != nullptr );
    BOOST_ASSERT(   running_->alt_ == nullptr );
    BOOST_ASSERT(   running_->is_type( Context::Type::Process ) );
    BOOST_ASSERT( ! running_->is_linked< hook::Ready >() );
    BOOST_ASSERT( ! running_->is_linked< hook::Sleep >() );
    BOOST_ASSERT( ! running_->is_linked< hook::AltSleep >() );
    BOOST_ASSERT( ! running_->is_linked< hook::Terminated >() );

    if ( alt->time_point_ < TimePointT::max() ) {
        running_->alt_        = alt;
        running_->time_point_ = alt->time_point_;
        running_->link( sleep_queue_ );
    }

    CtxSwitchData data{ std::addressof( splk ) };
    resume( std::addressof( data ) );

    running_->alt_        = nullptr;
    running_->time_point_ = TimePointT::max();
    if ( running_->is_linked< hook::Sleep >() ) {
        running_->unlink< hook::Sleep >();
    }

    if ( lock ) {
        splk.lock();
    }
}

void Scheduler::resume( CtxSwitchData * data ) noexcept
{
    resume_( policy_->pick_next(), data );
}

void Scheduler::resume( Context * to_ctx, CtxSwitchData * data ) noexcept
{
    resume_( to_ctx, data );
}

void Scheduler::terminate( Context * ctx ) noexcept
{
    BOOST_ASSERT( ctx != nullptr );
    BOOST_ASSERT( running_ == ctx );

    // FIXME: is_type( Dynamic ) instead?
    BOOST_ASSERT(   ctx->is_type( Context::Type::Work ) );
    BOOST_ASSERT( ! ctx->is_linked< hook::Ready >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Sleep >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Wait >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Terminated >() );

    std::unique_lock< Spinlock > lk{ ctx->splk_ };

    ctx->terminate();
    ctx->link( terminated_queue_ );
    ctx->unlink< hook::Work >();

    wakeup_waiting_on( ctx );

    lk.unlock();
    resume();
}

void Scheduler::schedule_local_( Context * ctx ) noexcept
{
    BOOST_ASSERT( ctx != nullptr );
    BOOST_ASSERT( ! ctx->is_linked< hook::Ready >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Wait >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Terminated >() );
    BOOST_ASSERT( ! ctx->has_terminated() );

    if ( ctx->is_linked< hook::Sleep >() ) {
        ctx->unlink< hook::Sleep >();
    }
    if ( ctx->is_linked< hook::AltSleep >() ) {
        ctx->unlink< hook::AltSleep >();
    }

    policy_->enqueue( ctx );
}

void Scheduler::schedule_remote_( Context * ctx ) noexcept
{
    BOOST_ASSERT( ctx != nullptr );
    BOOST_ASSERT( ! ctx->is_type( Context::Type::Scheduler ) );
    BOOST_ASSERT( ! ctx->is_linked< hook::Ready >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Wait >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Terminated >() );
    BOOST_ASSERT( ! ctx->has_terminated() );
    BOOST_ASSERT(   ctx->scheduler_ == this );

    /* std::unique_lock< Spinlock > lk{ splk_ }; */
    /* if ( ctx->is_linked< hook::Sleep >() ) { */
    /*     ctx->unlink< hook::Sleep >(); */
    /* } */
    /* lk.unlock(); */

    remote_queue_.push( ctx );
    policy_->notify();
}

void Scheduler::schedule( Context * ctx ) noexcept
{
    BOOST_ASSERT( ctx != nullptr );
    BOOST_ASSERT( ctx->scheduler_ != nullptr );

    // FIXME: synchronize this?
    if ( ctx->scheduler_ == this ) {
        schedule_local_( ctx );
    } else {
        ctx->scheduler_->schedule_remote_( ctx );
    }
}

void Scheduler::attach( Context * ctx ) noexcept
{
    BOOST_ASSERT(   ctx != nullptr );
    // FIXME: is_type( Dynamic ) instead?
    BOOST_ASSERT(   ctx->is_type( Context::Type::Work ) );
    BOOST_ASSERT( ! ctx->is_linked< hook::Work >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Ready >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Wait >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Sleep >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Terminated >() );
    BOOST_ASSERT(   ctx->scheduler_ == nullptr );

    ctx->link( work_queue_ );
    ctx->scheduler_ = this;
}

void Scheduler::detach( Context * ctx ) noexcept
{
    BOOST_ASSERT( ctx != nullptr );
    // FIXME: is_type( Dynamic ) instead?
    BOOST_ASSERT(   ctx->is_type( Context::Type::Work ) );
    BOOST_ASSERT(   ctx->is_linked< hook::Work >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Ready >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Wait >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Sleep >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Terminated >() );
    BOOST_ASSERT(   ctx->scheduler_ != nullptr );

    ctx->unlink< hook::Work >();
    ctx->scheduler_ = nullptr;
}

void Scheduler::commit( Context * ctx ) noexcept
{
    BOOST_ASSERT(   ctx != nullptr );
    // FIXME: is_type( Dynamic ) instead?
    BOOST_ASSERT(   ctx->is_type( Context::Type::Work ) );
    BOOST_ASSERT( ! ctx->is_linked< hook::Work >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Ready >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Wait >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Sleep >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Terminated >() );

    attach( ctx );
    schedule( ctx );
}

void Scheduler::yield() noexcept
{
    auto ctx = Scheduler::running();
    BOOST_ASSERT(   ctx != nullptr );
    BOOST_ASSERT(   ctx->is_type( Context::Type::Process ) );
    BOOST_ASSERT( ! ctx->is_linked< hook::Ready >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Wait >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Sleep >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Terminated >() );

    auto next = policy_->pick_next();
    if ( next != nullptr ) {
        schedule( ctx );
        resume( next );
        BOOST_ASSERT( ctx == Scheduler::running() );
    }
}

void Scheduler::join( Context * ctx ) noexcept
{
    BOOST_ASSERT( ctx != nullptr );

    Context * running_ctx = Scheduler::running();
    // FIXME: this might need rework

    std::unique_lock< Spinlock > lk{ ctx->splk_ };
    if ( ! ctx->has_terminated() ) {
        running_ctx->link( ctx->wait_queue_ );
        lk.unlock();
        resume();
        BOOST_ASSERT( Scheduler::running() == running_ctx );
    }
}

bool Scheduler::sleep_until( TimePointT const & time_point, CtxSwitchData * data ) noexcept
{
    BOOST_ASSERT(   running_ != nullptr );
    BOOST_ASSERT(   running_->is_type( Context::Type::Process ) );
    BOOST_ASSERT( ! running_->is_linked< hook::Ready >() );
    BOOST_ASSERT( ! running_->is_linked< hook::Wait >() );
    BOOST_ASSERT( ! running_->is_linked< hook::Sleep >() );
    BOOST_ASSERT( ! running_->is_linked< hook::Terminated >() );

    if (ClockT::now() < time_point) {
        running_->time_point_ = time_point;
        running_->link( sleep_queue_ );
        resume( data );
        return ClockT::now() >= time_point;
    } else {
        return true;
    }
}

void Scheduler::wakeup_sleep() noexcept
{
    std::unique_lock< Spinlock > lk{ splk_ };
    auto now = ClockT::now();
    auto sleep_it = sleep_queue_.begin();
    while ( sleep_it != sleep_queue_.end() ) {
        auto ctx = &( *sleep_it );

        BOOST_ASSERT(   ctx->is_type( Context::Type::Process ) );
        BOOST_ASSERT( ! ctx->is_linked< hook::Ready >() );
        BOOST_ASSERT( ! ctx->is_linked< hook::Terminated >() );

        if ( ctx->mpsc_next_.load( std::memory_order_acquire ) ) {
            std::cout << "??" << std::endl;
        }

        // Keep advancing the queue if deadline is reached,
        // break if not.
        if ( ctx->time_point_ > now ) {
            break;
        }
        sleep_it = sleep_queue_.erase( sleep_it );
        ctx->time_point_ = TimePointT::max();
        if ( ctx->alt_ == nullptr || ctx->alt_->try_timeout() ) {
            schedule( ctx );
        }
    }
    auto alt_sleep_it = alt_sleep_queue_.begin();
    while ( alt_sleep_it != alt_sleep_queue_.end() ) {
        auto ctx = &( *alt_sleep_it );

        BOOST_ASSERT(   ctx->is_type( Context::Type::Process ) );
        BOOST_ASSERT( ! ctx->is_linked< hook::Ready >() );
        BOOST_ASSERT( ! ctx->is_linked< hook::Terminated >() );
        BOOST_ASSERT(   ctx->alt_ != nullptr );

        // Keep advancing the queue if deadline is reached,
        // break if not.
        if ( ctx->time_point_ > now ) {
            break;
        }
        alt_sleep_it = alt_sleep_queue_.erase( alt_sleep_it );
        ctx->time_point_ = TimePointT::max();
        if ( ctx->alt_->try_timeout() ) {
            schedule( ctx );
        }
    }
}

void Scheduler::wakeup_waiting_on( Context * ctx ) noexcept
{
    BOOST_ASSERT(   ctx != nullptr );
    BOOST_ASSERT( ! ctx->is_linked< hook::Ready >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Wait >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Sleep >() );
    BOOST_ASSERT(   ctx->has_terminated() );

    while ( ! ctx->wait_queue_.empty() ) {
        auto waiting_ctx = & ctx->wait_queue_.front();
        ctx->wait_queue_.pop_front();
        schedule( waiting_ctx );
    }

    BOOST_ASSERT( ctx->wait_queue_.empty() );
}

void Scheduler::transition_remote() noexcept
{
    for ( Context * ctx = remote_queue_.pop();
          ctx != nullptr;
          ctx = remote_queue_.pop() ) {
        schedule_local_( ctx );
    }
}

void Scheduler::cleanup_terminated() noexcept
{
    while ( ! terminated_queue_.empty() ) {
        auto ctx = & terminated_queue_.front();
        terminated_queue_.pop_front();

        // FIXME: is_type( Dynamic ) instead?
        BOOST_ASSERT(   ctx->is_type( Context::Type::Work ) );
        BOOST_ASSERT( ! ctx->is_type( Context::Type::Static ) );
        BOOST_ASSERT( ! ctx->is_linked< hook::Ready >() );
        BOOST_ASSERT( ! ctx->is_linked< hook::Work >() );
        BOOST_ASSERT( ! ctx->is_linked< hook::Wait >() );
        BOOST_ASSERT( ! ctx->is_linked< hook::Sleep >() );

        // FIXME: might do a more reliable cleanup here.
        intrusive_ptr_release( ctx );
    }
}

void Scheduler::print_debug() noexcept
{
    std::cout << "Scheduler: " << std::endl;
    std::cout << "  Scheduler Ctx: " << std::endl;
    scheduler_ctx_->print_debug();
    std::cout << "  Main Ctx: " << std::endl;
    main_ctx_->print_debug();
    std::cout << "  Running: " << std::endl;
    running_->print_debug();
    std::cout << "  Work Queue:" << std::endl;
    for ( auto& ctx : work_queue_ ) {
        ctx.print_debug();
    }
    std::cout << "  Sleep Queue:" << std::endl;
    for ( auto& ctx : sleep_queue_ ) {
        std::cout << "    | " << ctx.get_id() << std::endl;
    }
    std::cout << "  AltSleep Queue:" << std::endl;
    for ( auto& ctx : alt_sleep_queue_ ) {
        std::cout << "    | " << ctx.get_id() << std::endl;
    }
    std::cout << "  Terminated Queue:" << std::endl;
    for ( auto& ctx : terminated_queue_ ) {
        std::cout << "    | " << ctx.get_id() << std::endl;
    }
}

void Scheduler::resolve_ctx_switch_data( CtxSwitchData * data ) noexcept
{
    if ( data != nullptr ) {
        if ( data->ctx_ != nullptr ) {
            schedule( data->ctx_ );
        }
        if ( data->splk_ != nullptr ) {
            data->splk_->unlock();
        }
    }
}


// called by main ctx in new threads when multi-core
void Scheduler::join_scheduler() noexcept
{

}

void Scheduler::signal_exit() noexcept
{
    exit_.store( true, std::memory_order_release );
    policy_->notify();
}

void noop() {}

// Scheduler context loop
void Scheduler::run_( void * vp )
{
    BOOST_ASSERT( running_ == scheduler_ctx_.get() );
    CtxSwitchData * data = static_cast< CtxSwitchData * >( vp );
    resolve_ctx_switch_data( data );
    for ( ;; ) {
        if ( exit_.load( std::memory_order_acquire ) ) {
            policy_->notify();
            if ( work_queue_.empty( )) {
                break;
            }
        }

        cleanup_terminated();
        transition_remote();
        wakeup_sleep();

        auto ctx = policy_->pick_next();
        if ( ctx != nullptr ) {
            schedule( scheduler_ctx_.get() );
            resume( ctx );
            BOOST_ASSERT( running_ == scheduler_ctx_.get() );

        } else {
            auto sleep_it = sleep_queue_.begin();
            auto suspend_time = ( sleep_it != sleep_queue_.end() )
                ? sleep_it->time_point_
                : ( PolicyT::TimePointT::max )();
            policy_->suspend_until( suspend_time );
        }
    }
    cleanup_terminated();

    scheduler_ctx_->terminate();
    wakeup_waiting_on( scheduler_ctx_.get() );

    if ( main_ctx_->is_linked< hook::Ready >() ) {
        main_ctx_->unlink< hook::Ready >();
    }
    resume( main_ctx_.get() );
    BOOST_ASSERT_MSG( false, "unreachable" );
    throw UnreachableError{};
}

PROXC_NAMESPACE_END

