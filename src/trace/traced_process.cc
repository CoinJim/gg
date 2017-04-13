/* -*-mode:c++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */

#include "traced_process.hh"

#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/reg.h>
#include <sys/user.h>
#include <sys/stat.h>
#include <signal.h>
#include <cstring>
#include <exception>

#include "exception.hh"

using namespace std;

int do_fork()
{
  /* Verify that process is single-threaded before forking */
  {
    struct stat my_stat;
    CheckSystemCall( "stat", stat( "/proc/self/task", &my_stat ) );

    if ( my_stat.st_nlink != 3 ) {
      throw runtime_error( "ChildProcess constructed in multi-threaded program" );
    }
  }

  return CheckSystemCall( "fork", fork() );
}

TracedProcess::TracedProcess( char * args[], const int termination_signal )
  : pid_( do_fork() ),
    process_state_( NOT_STARTED ),
    exit_status_(),
    graceful_termination_signal_( termination_signal ),
    died_on_signal_( false ),
    moved_away_( false )
{
  if ( pid_ == 0 ) { /* child */
    CheckSystemCall( "ptrace(TRACEME)", ptrace( PTRACE_TRACEME ) );
    kill( getpid(), SIGSTOP );
    CheckSystemCall( "execvp", execvp( args[ 0 ], &args[ 0 ] ) );
  }
  else {
    int child_ret;
    CheckSystemCall( "waitpid", waitpid( pid_, &child_ret, 0 ) );

    CheckSystemCall( "ptrace(SETOPTIONS)", ptrace( PTRACE_SETOPTIONS, pid_, 0,
                                                   PTRACE_O_TRACEEXEC | PTRACE_O_TRACESYSGOOD ) );
  }
}

void TracedProcess::resume()
{
  process_state_ = RUNNING;
  CheckSystemCall( "ptrace(CONT)", ptrace( PTRACE_CONT, pid_, NULL, NULL ) );
}

TracedProcess::~TracedProcess()
{}

TracedProcess::TracedProcess( TracedProcess && other )
  : pid_( other.pid_ ),
    process_state_( other.process_state_ ),
    exit_status_( other.exit_status_ ),
    graceful_termination_signal_( other.graceful_termination_signal_ ),
    died_on_signal_( other.died_on_signal_ ),
    moved_away_( other.moved_away_ )
{
    assert( !other.moved_away_ );

    other.moved_away_ = true;
}

int ptrace_syscall( pid_t pid )
{
  int status;

  while ( true ) {
    CheckSystemCall( "ptrace(SYSCALL)", ptrace( PTRACE_SYSCALL, pid, 0, 0 ) );
    waitpid( pid, &status, 0 );

    if ( WIFSTOPPED( status ) && WSTOPSIG( status ) & 0x80 ) {
      return 0;
    }
    else if ( WIFEXITED( status ) ) {
      return 1;
    }
  }
}

bool TracedProcess::wait_for_syscall( function<void( SystemCallEntry )> before_entry,
                                      function<void( SystemCallEntry, long int )> after_exit )
{
  process_state_ = RUNNING;
  if ( ptrace_syscall( pid_ ) != 0 ) {
    process_state_ = TERMINATED;
    return false;
  }

  process_state_ = STOPPED;
  before_entry( SystemCall::get_syscall( ptrace( PTRACE_PEEKUSER, pid_, 8 * ORIG_RAX ) ) );

  process_state_ = RUNNING;
  if ( ptrace_syscall( pid_ ) != 0 ) {
    process_state_ = TERMINATED;
    return false;
  }

  process_state_ = STOPPED;
  after_exit( SystemCall::get_syscall( ptrace( PTRACE_PEEKUSER, pid_, 8 * ORIG_RAX ) ),
              ptrace( PTRACE_PEEKUSER, pid_, 8 * RAX ) );

  return true;
}
