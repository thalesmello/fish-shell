// Prototypes for utilities for keeping track of jobs, processes and subshells, as well as signal
// handling functions for tracking children. These functions do not themselves launch new processes,
// the exec library will call proc to create representations of the running jobs as needed.
#ifndef FISH_PROC_H
#define FISH_PROC_H
#include "config.h"  // IWYU pragma: keep

#include <signal.h>
#include <stddef.h>
#include <sys/time.h>  // IWYU pragma: keep
#include <termios.h>
#include <unistd.h>

#include <list>
#include <memory>
#include <vector>

#include "common.h"
#include "io.h"
#include "parse_tree.h"

/// Types of processes.
enum process_type_t {
    /// A regular external command.
    EXTERNAL,
    /// A builtin command.
    INTERNAL_BUILTIN,
    /// A shellscript function.
    INTERNAL_FUNCTION,
    /// A block of commands, represented as a node.
    INTERNAL_BLOCK_NODE,
    /// The exec builtin.
    INTERNAL_EXEC
};

enum {
    JOB_CONTROL_ALL,
    JOB_CONTROL_INTERACTIVE,
    JOB_CONTROL_NONE,
};

/// A structure representing a single fish process. Contains variables for tracking process state
/// and the process argument list. Actually, a fish process can be either a regular external
/// process, an internal builtin which may or may not spawn a fake IO process during execution, a
/// shellscript function or a block of commands to be evaluated by calling eval. Lastly, this
/// process can be the result of an exec command. The role of this process_t is determined by the
/// type field, which can be one of EXTERNAL, INTERNAL_BUILTIN, INTERNAL_FUNCTION, INTERNAL_EXEC.
///
/// The process_t contains information on how the process should be started, such as command name
/// and arguments, as well as runtime information on the status of the actual physical process which
/// represents it. Shellscript functions, builtins and blocks of code may all need to spawn an
/// external process that handles the piping and redirecting of IO for them.
///
/// If the process is of type EXTERNAL or INTERNAL_EXEC, argv is the argument array and actual_cmd
/// is the absolute path of the command to execute.
///
/// If the process is of type INTERNAL_BUILTIN, argv is the argument vector, and argv[0] is the name
/// of the builtin command.
///
/// If the process is of type INTERNAL_FUNCTION, argv is the argument vector, and argv[0] is the
/// name of the shellscript function.
class process_t {
   private:
    null_terminated_array_t<wchar_t> argv_array;

    io_chain_t process_io_chain;

    // No copying.
    process_t(const process_t &rhs);
    void operator=(const process_t &rhs);

   public:
    process_t();

    // Note whether we are the first and/or last in the job
    bool is_first_in_job;
    bool is_last_in_job;

    /// Type of process. Can be one of \c EXTERNAL, \c INTERNAL_BUILTIN, \c INTERNAL_FUNCTION, \c
    /// INTERNAL_EXEC.
    enum process_type_t type;

    /// For internal block processes only, the node offset of the block.
    node_offset_t internal_block_node;

    /// Sets argv.
    void set_argv(const wcstring_list_t &argv) { argv_array.set(argv); }

    /// Returns argv.
    const wchar_t *const *get_argv(void) const { return argv_array.get(); }
    const null_terminated_array_t<wchar_t> &get_argv_array(void) const { return argv_array; }

    /// Returns argv[idx].
    const wchar_t *argv(size_t idx) const {
        const wchar_t *const *argv = argv_array.get();
        assert(argv != NULL);
        return argv[idx];
    }

    /// Returns argv[0], or NULL.
    const wchar_t *argv0() const {
        const wchar_t *const *argv = argv_array.get();
        return argv ? argv[0] : NULL;
    }

    /// IO chain getter and setter.
    const io_chain_t &io_chain() const { return process_io_chain; }

    void set_io_chain(const io_chain_t &chain) { this->process_io_chain = chain; }

    /// Actual command to pass to exec in case of EXTERNAL or INTERNAL_EXEC.
    wcstring actual_cmd;
    /// Process ID
    pid_t pid;
    /// File descriptor that pipe output should bind to.
    int pipe_write_fd;
    /// File descriptor that the _next_ process pipe input should bind to.
    int pipe_read_fd;
    /// True if process has completed.
    volatile int completed;
    /// True if process has stopped.
    volatile int stopped;
    /// Reported status value.
    volatile int status;
    /// Special flag to tell the evaluation function for count to print the help information.
    int count_help_magic;
#ifdef HAVE__PROC_SELF_STAT
    /// Last time of cpu time check.
    struct timeval last_time;
    /// Number of jiffies spent in process at last cpu time check.
    unsigned long last_jiffies;
#endif
};

typedef std::unique_ptr<process_t> process_ptr_t;
typedef std::vector<process_ptr_t> process_list_t;

/// Constants for the flag variable in the job struct.
enum job_flag_t {
    /// Whether the user has been told about stopped job.
    JOB_NOTIFIED = 1 << 0,
    /// Whether this job is in the foreground.
    JOB_FOREGROUND = 1 << 1,
    /// Whether the specified job is completely constructed, i.e. completely parsed, and every
    /// process in the job has been forked, etc.
    JOB_CONSTRUCTED = 1 << 2,
    /// Whether the specified job is a part of a subshell, event handler or some other form of
    /// special job that should not be reported.
    JOB_SKIP_NOTIFICATION = 1 << 3,
    /// Whether the exit status should be negated. This flag can only be set by the not builtin.
    JOB_NEGATE = 1 << 4,
    /// Whether the job is under job control.
    JOB_CONTROL = 1 << 5,
    /// Whether the job wants to own the terminal when in the foreground.
    JOB_TERMINAL = 1 << 6
};

typedef int job_id_t;
job_id_t acquire_job_id(void);
void release_job_id(job_id_t jobid);

/// A struct represeting a job. A job is basically a pipeline of one or more processes and a couple
/// of flags.
class job_t {
    /// The original command which led to the creation of this job. It is used for displaying
    /// messages about job status on the terminal.
    wcstring command_str;

    // The IO chain associated with the block.
    const io_chain_t block_io;

    // No copying.
    job_t(const job_t &rhs) = delete;
    void operator=(const job_t &) = delete;

   public:
    job_t(job_id_t jobid, const io_chain_t &bio);
    ~job_t();

    /// Returns whether the command is empty.
    bool command_is_empty() const { return command_str.empty(); }

    /// Returns the command as a wchar_t *. */
    const wchar_t *command_wcstr() const { return command_str.c_str(); }

    /// Returns the command.
    const wcstring &command() const { return command_str; }

    /// Sets the command.
    void set_command(const wcstring &cmd) { command_str = cmd; }

    /// All the processes in this job.
    process_list_t processes;

    /// Process group ID for the process group that this job is running in.
    /// Set to a nonexistent, non-return-value of getpgid() integer by the constructor
    pid_t pgid;
    /// The saved terminal modes of this job. This needs to be saved so that we can restore the
    /// terminal to the same state after temporarily taking control over the terminal when a job
    /// stops.
    struct termios tmodes;
    /// The job id of the job. This is a small integer that is a unique identifier of the job within
    /// this shell, and is used e.g. in process expansion.
    const job_id_t job_id;
    /// Bitset containing information about the job. A combination of the JOB_* constants.
    unsigned int flags;

    // Get and set flags
    bool get_flag(job_flag_t flag) const;
    void set_flag(job_flag_t flag, bool set);

    /// Returns the block IO redirections associated with the job. These are things like the IO
    /// redirections associated with the begin...end statement.
    const io_chain_t &block_io_chain() const { return this->block_io; }

    /// Fetch all the IO redirections associated with the job.
    io_chain_t all_io_redirections() const;
};

/// Whether we are reading from the keyboard right now.
bool shell_is_interactive(void);

/// Whether we are running a subshell command.
extern bool is_subshell;

/// Whether we are running a block of commands.
extern bool is_block;

/// Whether we are running due to a `breakpoint` command.
extern bool is_breakpoint;

/// Whether this shell is attached to the keyboard at all.
extern bool is_interactive_session;

/// Whether we are a login shell.
extern bool is_login;

/// Whether we are running an event handler. This is not a bool because we keep count of the event
/// nesting level.
extern int is_event;

// List of jobs. We sometimes mutate this while iterating - hence it must be a list, not a vector
typedef std::list<shared_ptr<job_t>> job_list_t;

bool job_list_is_empty(void);

/// A class to aid iteration over jobs list
class job_iterator_t {
    job_list_t *const job_list;
    job_list_t::iterator current, end;

   public:
    void reset(void);

    job_t *next() {
        job_t *job = NULL;
        if (current != end) {
            job = current->get();
            ++current;
        }
        return job;
    }

    explicit job_iterator_t(job_list_t &jobs);
    job_iterator_t();
    size_t count() const;
};

/// Whether a universal variable barrier roundtrip has already been made for the currently executing
/// command. Such a roundtrip only needs to be done once on a given command, unless a universal
/// variable value is changed. Once this has been done, this variable is set to 1, so that no more
/// roundtrips need to be done.
///
/// Both setting it to one when it should be zero and the opposite may cause concurrency bugs.
bool get_proc_had_barrier();
void set_proc_had_barrier(bool flag);

/// Pid of last process started in the background.
extern pid_t proc_last_bg_pid;

/// The current job control mode.
///
/// Must be one of JOB_CONTROL_ALL, JOB_CONTROL_INTERACTIVE and JOB_CONTROL_NONE.
extern int job_control_mode;

/// If this flag is set, fish will never fork or run execve. It is used to put fish into a syntax
/// verifier mode where fish tries to validate the syntax of a file but doesn't actually do
/// anything.
extern int no_exec;

/// Sets the status of the last process to exit.
void proc_set_last_status(int s);

/// Returns the status of the last process to exit.
int proc_get_last_status();

/// Promotes a job to the front of the job list.
void job_promote(job_t *job);

/// Return the job with the specified job id. If id is 0 or less, return the last job used.
job_t *job_get(job_id_t id);

/// Return the job with the specified pid.
job_t *job_get_from_pid(int pid);

/// Tests if the job is stopped.
int job_is_stopped(const job_t *j);

/// Tests if the job has completed, i.e. if the last process of the pipeline has ended.
bool job_is_completed(const job_t *j);

/// Reassume a (possibly) stopped job. Put job j in the foreground.  If cont is true, restore the
/// saved terminal modes and send the process group a SIGCONT signal to wake it up before we block.
///
/// \param j The job
/// \param cont Whether the function should wait for the job to complete before returning
void job_continue(job_t *j, bool cont);

/// Notify the user about stopped or terminated jobs. Delete terminated jobs from the job list.
///
/// \param interactive whether interactive jobs should be reaped as well
int job_reap(bool interactive);

/// Signal handler for SIGCHLD. Mark any processes with relevant information.
void job_handle_signal(int signal, siginfo_t *info, void *con);

/// Send the specified signal to all processes in the specified job.
int job_signal(job_t *j, int signal);

/// Mark a process as failed to execute (and therefore completed).
void job_mark_process_as_failed(job_t *job, const process_t *p);

#ifdef HAVE__PROC_SELF_STAT
/// Use the procfs filesystem to look up how many jiffies of cpu time was used by this process. This
/// function is only available on systems with the procfs file entry 'stat', i.e. Linux.
unsigned long proc_get_jiffies(process_t *p);

/// Update process time usage for all processes by calling the proc_get_jiffies function for every
/// process of every job.
void proc_update_jiffies();
#endif

/// Perform a set of simple sanity checks on the job list. This includes making sure that only one
/// job is in the foreground, that every process is in a valid state, etc.
void proc_sanity_check();

/// Send a process/job exit event notification. This function is a convenience wrapper around
/// event_fire().
void proc_fire_event(const wchar_t *msg, int type, pid_t pid, int status);

/// Initializations.
void proc_init();

/// Clean up before exiting.
void proc_destroy();

/// Set new value for is_interactive flag, saving previous value. If needed, update signal handlers.
void proc_push_interactive(int value);

/// Set is_interactive flag to the previous value. If needed, update signal handlers.
void proc_pop_interactive();

/// Format an exit status code as returned by e.g. wait into a fish exit code number as accepted by
/// proc_set_last_status.
int proc_format_status(int status);

#endif
