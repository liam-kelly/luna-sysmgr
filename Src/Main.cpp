/* @@@LICENSE
*
*      Copyright (c) 2008-2013 LG Electronics, Inc.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
* LICENSE@@@ */
/**
 * @file
 * 
 * Main entry point for Luna
 */




#include "Common.h"

#include "HostBase.h"
#include "ApplicationDescription.h"
#include "ApplicationManager.h"
#include "CpuAffinity.h"
#include "HapticsController.h"
#include "Localization.h"


#include "MemoryMonitor.h"
#include "Settings.h"
#include "SystemService.h"
#include "ApplicationInstaller.h"
#include "Preferences.h"
#include "DeviceInfo.h"
#include "Security.h"
#include "EASPolicyManager.h"
#include "Logging.h"
#include "BackupManager.h"
#include "DisplayManager.h"
#include "EventReporter.h"
#include "InputEventMonitor.h"
#include "BootManager.h"

#include "ApplicationProcessManager.h"
#include "ApplicationLaunchBooster.h"

#include <ProcessKiller.h>

#include <sys/time.h>
#include <sys/resource.h>
#include <glib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>

#include <signal.h>
#include <stdarg.h>
#include <syslog.h>
#include <ucontext.h>
#include <fcntl.h>
#include <sys/file.h>

#include <QApplication>
#include <QtGui>
#include <QtGlobal> 

/* Convenience macro for simulating crashes for debugging purposes only:
#define crash() {                               \
        volatile int *ip = (volatile int *)0;   \
        *ip = 0;                                \
    }
*/


//#define PRINT_MALLOC_STATS
#ifdef __cplusplus
extern "C" {
#endif
extern void malloc_stats(void);
#ifdef __cplusplus
};
#endif

static gchar* s_uiStr = NULL;
static gchar* s_appToLaunchStr = NULL;
static gchar* s_logLevelStr = NULL;
static gboolean s_useSysLog = false;
static gboolean s_colorLog = true;
static gboolean s_useTerminal = false;
static gchar* s_debugTrapStr = NULL;
static gboolean s_forceSoftwareRendering = false;
static gchar* s_mallocStatsFileStr = NULL;
static int s_mallocStatsInterval = -1;

/**
 * Whether or not to debug crashes
 * debugCrashes indicates whether the user has specified "-x on" in the command
 * line args to enable debugging of crashes.
 */ 
static bool debugCrashes = false;

/**
 * Used to allow us to bail from the handler when we're done debugging. 
 *
 * This is meant to be set only from within a gdb seesion.
 */
volatile bool stayInLoop = true;

/**
 * Crash log file descriptor
 */
int crashLogFD = -1;

/**
 * Process ID of the current process
 */
pid_t sysmgrPid;

/**
 * printf for crash logging purposes
 * 
 * Safe printf that does not call malloc (to avoid re-entrancy issue when
 * called from the signal handler.
 * 
 * @param	format		Format of output string
 * @param	...		List of values to include in the output string
 */
static void crash_printf(const char *format, ...) {
    // Allocate the buffer staticly because we don't want to assume that
    // there's a lot of stack space left at the time of the crash:
	static char buf[512];
	va_list ap;

	va_start(ap, format);
	vsnprintf(buf, sizeof(buf), format, ap);
	va_end(ap);
	ssize_t result = write(crashLogFD, buf, strlen(buf));
	Q_UNUSED(result);
}

/**
 * Flushes the crash log to disk
 */
static void crash_flush() {
    fsync(crashLogFD);
}

#if (defined(__i386__) || defined(__x86_64__))

// Register context dumper for ia32 / x64:
#if __WORDSIZE == 64
const char *const regNames[NGREG] = {
    "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15",
    "RDI", "RSI", "RBP" "RBX", "RDX", "RAX", "RCX", "RIP",
    "EFL", "CSGSFS", "ERR", "TRAPNO", "OLDMASK", "CR2"
};
#else // __WORDSIZE == 32
const char *const regNames[NGREG] = {
    "GS", "FS", "ES", "DS", "EDI", "ESI", "EBP", "ESP",
    "EBX", "EDX", "ECX", "EAX", "TRAPNO", "ERR", "EIP",
    "CS", "EFL", "UESP", "SS"
};
#endif

/**
 * Log register values
 * 
 * @param	sig		The signal which triggered this handler
 * @param	info		Pointer to information about the signal
 * @param	data		Pointer to a ucontext_t with info on the crashing process
 */
void logCrashRegisterContext(int sig, siginfo_t *info, void *data) {
    ucontext_t *context = reinterpret_cast<ucontext_t *>(data);
    crash_printf("reg context {\n");

    // Note: For ia32/x64, we dump all the registers because this is not for
    // field deployment.  Hence, there is no risk of violation of user privacy.
    for (int i = 0; i < NGREG; i++) {
        int value = context->uc_mcontext.gregs[i];
        crash_printf("  %2d %8s = 0x%08x %d\n", i, regNames[i], value, value);
    }

    crash_printf("}\n");
    crash_flush();
}

#elif defined(__arm__)

// Register context dumper for ARM:
/**
 * Log register values
 * 
 * @param	sig		The signal which triggered this handler
 * @param	info		Pointer to information about the signal
 * @param	data		Pointer to a ucontext_t with info on the crashing process
 */
void logCrashRegisterContext(int sig, siginfo_t *info, void *data) {
    ucontext_t *context = reinterpret_cast<ucontext_t *>(data);
    sigcontext *scon = &context->uc_mcontext;
    crash_printf("reg context {\n");

    // Dumping non-sensitive register content:
    crash_printf("  // non-sensitive register content:\n");
    crash_printf("  trap_no       = 0x%08x %d\n", scon->trap_no, scon->trap_no);
    crash_printf("  error_code    = 0x%08x %d\n", scon->error_code, scon->error_code);
    crash_printf("  oldmask       = 0x%08x %d\n", scon->oldmask, scon->oldmask);

    crash_printf("  arm_sp        = 0x%08x %d\n", scon->arm_sp, scon->arm_sp);
    crash_printf("  arm_lr        = 0x%08x %d\n", scon->arm_lr, scon->arm_lr);
    crash_printf("  arm_pc        = 0x%08x %d\n", scon->arm_pc, scon->arm_pc);
    crash_printf("  arm_cpsr      = 0x%08x %d\n", scon->arm_cpsr, scon->arm_cpsr);
    crash_printf("  fault_address = 0x%08x %d\n", scon->fault_address, scon->fault_address);

    if (Settings::LunaSettings()->debug_doVerboseCrashLogging) {
        // Dumping sensitive register content for internal debugging only.
        // By default, this data should not be dumped in the field (to avoid
        // potential violation of user privacy issues):
        crash_printf("  arm_r0        = 0x%08x %d\n", scon->arm_r0, scon->arm_r0);
        crash_printf("  arm_r1        = 0x%08x %d\n", scon->arm_r1, scon->arm_r1);
        crash_printf("  arm_r2        = 0x%08x %d\n", scon->arm_r2, scon->arm_r2);
        crash_printf("  arm_r3        = 0x%08x %d\n", scon->arm_r3, scon->arm_r3);
        crash_printf("  arm_r4        = 0x%08x %d\n", scon->arm_r4, scon->arm_r4);
        crash_printf("  arm_r5        = 0x%08x %d\n", scon->arm_r5, scon->arm_r5);
        crash_printf("  arm_r6        = 0x%08x %d\n", scon->arm_r6, scon->arm_r6);
        crash_printf("  arm_r7        = 0x%08x %d\n", scon->arm_r7, scon->arm_r7);
        crash_printf("  arm_r8        = 0x%08x %d\n", scon->arm_r8, scon->arm_r8);
        crash_printf("  arm_r9        = 0x%08x %d\n", scon->arm_r9, scon->arm_r9);
        crash_printf("  arm_r10       = 0x%08x %d\n", scon->arm_r10, scon->arm_r10);
        crash_printf("  arm_fp        = 0x%08x %d\n", scon->arm_fp, scon->arm_fp);
        crash_printf("  arm_ip        = 0x%08x %d\n", scon->arm_ip, scon->arm_ip);
    }

    crash_printf("}\n");
    crash_flush();
}

#endif // __arm__

/**
 * Whether or not innerCrashHandler() has handled a signal
 * 
 * This variable is set to true by {@link innerCrashHandler() innerCrashHandler()} when it handles a signal.
 * 
 * @see innerCrashHandler()
 */
static volatile bool hasCrashedInCrashHandler = false;

static void innerCrashHandler(int sig, siginfo_t *info, void *data);
static void outerCrashHandler(int sig, siginfo_t *info, void *data);

/**
 * Installs innerCrashHandler as the handler for a given signal
 * 
 * @param	sig		The signal to handle using {@link innerCrashHandler innerCrashHandler}
 * @param	previous_crash_action		Pointer at which to store the previous crash handler when installing this one
 */
static void installInnerCrashHandler(int sig,
                                     struct sigaction *previous_crash_action)
{
    struct sigaction crash_action;
    sigset_t block_mask;
    sigfillset(&block_mask);
    crash_action.sa_flags = SA_SIGINFO | SA_RESETHAND;
    crash_action.sa_mask = block_mask;
    crash_action.sa_sigaction = &innerCrashHandler;

    // Install the handler for signals that we want to trap:
    sigaction(sig, &crash_action, previous_crash_action);
}

/**
 * Installs outerCrashHandler as the handler for a given signal
 * 
 * @param	sig		The signal to handle using {@link outerCrashHandler outerCrashHandler}
 */
static void installOuterCrashHandler(int sig)
{
    struct sigaction crash_action;
    sigset_t block_mask;
    sigfillset(&block_mask);
    sigdelset(&block_mask, SIGSEGV);
    crash_action.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_NODEFER;
    crash_action.sa_mask = block_mask;
    crash_action.sa_sigaction = &outerCrashHandler;

    // Install the handler for signals that we want to trap:
    sigaction(sig, &crash_action, NULL);
}

/**
 * Writes malloc statistics to stderr
 * 
 * @param	data			Data of some sort - currently unused
 * 
 * @return				Always returns TRUE
 */
static gboolean mallocStatsCb(gpointer data)
{
	char buf[30];
	time_t cur_time;
	time(&cur_time);
	static pid_t my_pid = 0;
	static char process_name[16] = { 0 };

	if (!my_pid) {
		my_pid = getpid();
	}

	if (!process_name[0]) {
		::prctl(PR_GET_NAME, (unsigned long)process_name, 0, 0, 0);
		process_name[sizeof(process_name) - 1] = '\0';
	}

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);

    flock(STDERR_FILENO, LOCK_EX);

	fflush(stderr);
	fprintf(stderr, "\nMALLOC STATS FOR PROCESS: \"%s\" (PID: %d) AT [%ld.%ld] %s", process_name, my_pid, ts.tv_sec, ts.tv_nsec, ctime_r(&cur_time, buf));
	fflush(stderr);
	malloc_stats();
	fprintf(stderr, "\n\n");
	fflush(stderr);
	fsync(STDERR_FILENO);

    flock(STDERR_FILENO, LOCK_UN);
	
	
	return TRUE;
}

/**
 * Sets up mallocStatsCb to run at a specified time interval
 * 
 * @param	mainLoop		Pointer to main loop of parent
 * @param	secs			Number of seconds between calls to mallocStatsCb
 */
static void initMallocStatsCb(GMainLoop* mainLoop, int secs)
{
	// negative means no stats
	if ((secs < 0) || (s_mallocStatsFileStr == NULL)) return;

	GSource *timeoutSource = g_timeout_source_new_seconds(secs);
	g_source_set_callback(timeoutSource, mallocStatsCb, NULL, NULL);
	g_source_attach(timeoutSource, g_main_loop_get_context(mainLoop));
}

/**
 * Attempts to open a file for logging malloc statistics
 * 
 * Logs a critical error message if unable to open the given file.
 * 
 * @param	mallocStatsFile			Filename of file to log malloc statistics to
 */
static void setupMallocStats(const char* mallocStatsFile)
{
	FILE* file = ::freopen(mallocStatsFile, "a+", stderr);

	if (!file) {
		g_critical("Unable to open file: %s", mallocStatsFile);
	}
}

/**
 * Handle a SIGSEGV signal
 * 
 * If on ARM, this skips instructions which are trying to access non-accessible memory.
 * This function also logs the fact that it has run by setting {@link hasCrashedInCrashHandler hasCrashedInCrashHandler] to true.
 * If it does anything, it also reinstalls itself as the signal handler for the signal type so next time it happens it runs again.
 * 
 * @see hasCrashedInCrashHandler
 * 
 * @param	sig		The signal which triggered this handler
 * @param	info		Pointer to information about the signal
 * @param	data		Pointer to a ucontext_t with info on the crashing process
 */
static void innerCrashHandler(int sig, siginfo_t *info, void *data)
{
    if (sig == SIGSEGV) {
        // The inner crash handler is expecting to only see SIGSEGVs as a
        // result of memory dumps that may have unknowingly touched a
        // non-accessible page of memory.  In this case, we simply bypass
        // the instruction of the memory access and flag the error so that
        // the outer crash handler know not to continue with this region
        // of memory.
#if defined(__arm__)
        ucontext_t *context = reinterpret_cast<ucontext_t *>(data);
        sigcontext *scon = &context->uc_mcontext;
        scon->arm_pc = scon->arm_pc + 4;
#endif
        hasCrashedInCrashHandler = true;

        // Note: We need to reinstall the inner signal handler because the
        // outer handler may rely on it to safe access multiple regions of
        // memory.

        // Note: We only reinstall the inner signal handler if the fault is a
        // known SIGSEGV fault that we know how to handle.  Otherwise, let the
        // normal faulting system handle the crash.
        installInnerCrashHandler(sig, NULL);
    }
}

/**
 * Handle a process signal
 * 
 * Steps are as follows:
 * - Attempt to let WebKit handle the signal.  If it does, keep running and reinstall the handler.  Otherwise, continue.
 * - Log crash diagnostics.
 * - If debugging is enabled, start an infinite loop to give gdb a chance to connect and see what's going on.
 * 
 * @param	sig		The signal which triggered this handler
 * @param	info		Pointer to information about the signal
 * @param	data		Pointer to a ucontext_t with info on the crashing process
 */
static void outerCrashHandler(int sig, siginfo_t *info, void *data)
{
    // Let webkit handle the crash if it wants to.  If it returns true,
    // the signal has been handled.  Just return:
    /*if (Palm::WebGlobal::handleSignal(sig, info, data)) {

        // Webkit side has handled the signal.  Hence, we're not going to crash,
        // and will keep running.  Therefore, we need to re-install the signal
        // handler:
        installOuterCrashHandler(sig);
        return;
    }*/

    // Otherwise, report the crash diagnostics:
    struct sigaction previous_crash_action;
    installInnerCrashHandler(sig, &previous_crash_action);

    const char *logFileName = Settings::LunaSettings()->logFileName.c_str();
    char logFileNameBuffer[64];
    if (Settings::LunaSettings()->debug_doVerboseCrashLogging) {
        snprintf(logFileNameBuffer, 64,
                 "/media/internal/lunasysmgr.%d.verbose.log", sysmgrPid);
        logFileName = logFileNameBuffer;
    }
    
    crashLogFD = open(logFileName, O_WRONLY | O_CREAT | O_TRUNC);

#if 0 // Disable until we can do this without calling malloc and free:
	int memTotal, memFree, swapTotal, swapFree, memchuteFree, memUsage;
#endif


    if (crashLogFD != -1) {
        crash_printf("LunaSysMgr.%d: Caught signal %d\n", sysmgrPid, sig);
        crash_flush();

        // Dump the register context for the crash to the logs:
        logCrashRegisterContext(sig, info, data);

        // Let webkit do some analysis on the crash information and log report
        // as appropriate:
#ifndef FIX_FOR_QT
        Palm::WebGlobal::reportCrashDiagnostics(sig, info, data, crashLogFD,
            Settings::LunaSettings()->debug_doVerboseCrashLogging,
            &hasCrashedInCrashHandler);
#endif
        crash_flush();

        crash_printf("LunaSysMgr.%d: Caught signal %d END report\n",
                     sysmgrPid, sig);
        crash_flush();
    }

    // If the option to debug crashes is enabled, then we'll enter an infinite
    // loop here to capture the crash conditions until a gdb session can be
    // attached.
    //
    // By default, this option is disabled, and the crashHandler will return
    // to the crashing instruction.  Since the crashHandler is set up to
    // fire only once (see SA_RESETHAND option which resets the handler after
    // it has fired), when we return to the crashing pc, we'll fault again.
    // This time, the fault will be handled by the default system handler and
    // generate a core or minicore as appropriate.

    if (debugCrashes || Settings::LunaSettings()->debug_loopInCrashHandler) {
        // Infinite loop until we turn off stayInLoop using gdb:
        while (stayInLoop) {
            // If we get here, sit and wait for someone to come and debug
            // this device.
        }
    }

    // Restore the previous handler before we return.  Otherwise, the inner
    // handler will perpetually skip over faulting addresses:
    sigaction(sig, &previous_crash_action, NULL);

    // Close the file:
    fsync(crashLogFD);
    close(crashLogFD);
    crashLogFD = -1;
}

#if (QT_VERSION < QT_VERSION_CHECK(5, 0, 0))
/**
 * Calls different message logging functions depending on message type
 * 
 * For the different possible values of type, it calls:
 * - QtDebugMsg = g_debug
 * - QtWarningMsg = g_warning
 * - QtCriticalMsg = g_critical
 * - QtFatalMsg = g_error
 * - Anything else = g_message
 * 
 * @param	type		Message type
 * @param	str		Message to log
 */
void qtMsgHandler(QtMsgType type, const char *str) {
    switch(type)
    {
	case QtDebugMsg:
	    g_debug("QDebug: %s", str);
	    break;
	case QtWarningMsg:
	    g_warning("QWarning: %s", str);
	    break;
	case QtCriticalMsg:
	    g_critical("QCritical: %s", str);
	    break;
	case QtFatalMsg:
	    g_error("QFatal: %s", str);
	    break;
	default:
	    g_message("QMessage: %s", str);
	    break;
    }
}
#else
void qtMsgHandler(QtMsgType type, const QMessageLogContext&, const QString& str) {
    switch(type)
    {
    case QtDebugMsg:
        g_debug("QDebug: %s", qPrintable(str));
        break;
    case QtWarningMsg:
        g_warning("QWarning: %s", qPrintable(str));
        break;
    case QtCriticalMsg:
        g_critical("QCritical: %s", qPrintable(str));
        break;
    case QtFatalMsg:
        g_error("QFatal: %s", qPrintable(str));
        break;
    default:
        g_message("QMessage: %s", qPrintable(str));
        break;
    }
}
#endif

/**
 * Parses command-line options
 *
 * This function parses command-line arguments and sets the corresponding variables
 * in Settings::LunaSettings()
 *
 * @param	argc		Number of arguments
 * @param	argv		Pointer to list of char* pointers for each of the arguments
 */
static void parseCommandlineOptions(int argc, char** argv)
{
    GError* error = NULL;
    GOptionContext* context = NULL;

	static GOptionEntry entries[] = {
		{ "ui",  'u',  0, G_OPTION_ARG_STRING, &s_uiStr, "UI type to launch (minimal, luna)", "name" },
		{ "app", 'a', 0, G_OPTION_ARG_STRING,  &s_appToLaunchStr, "App Id of app to launch", "id" },
		{ "logger", 'l', 0, G_OPTION_ARG_STRING,  &s_logLevelStr, "log level", "level"},
		{ "syslog", 's', 0, G_OPTION_ARG_NONE, &s_useSysLog, "Use syslog", NULL },
		{ "colorlogging", 'c', 0, G_OPTION_ARG_NONE, &s_colorLog, "Color logging on or off", NULL },	// use -c=0 or --colorlogging=0 to disable color logging
		{ "terminal", 't', 0, G_OPTION_ARG_NONE, &s_useTerminal, "Use terminal for logs", NULL },
		{ "debug-trap", 'x', 0, G_OPTION_ARG_STRING,  &s_debugTrapStr, "debug trap (on/ off)", "state"},
		{ "force-software-rendering", 'S', 0, G_OPTION_ARG_NONE, &s_forceSoftwareRendering, "Force Software rendering", NULL},
		{ "malloc-stats-file", 'm', 0, G_OPTION_ARG_STRING,  &s_mallocStatsFileStr, "File for logging malloc stats", "file" },
		{ "malloc-stats-interval", 'i', 0, G_OPTION_ARG_INT,  &s_mallocStatsInterval, "Interval at which to log malloc stats", "seconds" },
		{ NULL }
	};

	context = g_option_context_new(NULL);
	g_option_context_add_main_entries (context, entries, NULL);
	g_option_context_parse (context, &argc, &argv, &error);

#if !defined(HAVE_OPENGL)
	// if there's no OpenGL, then implicitely force software rendering
	s_forceSoftwareRendering = true;
#endif

	if (s_uiStr && strcasecmp(s_uiStr, "minimal") == 0)
		Settings::LunaSettings()->uiType = Settings::UI_MINIMAL;
	else
		Settings::LunaSettings()->uiType = Settings::UI_LUNA;

	Settings::LunaSettings()->logger_useSyslog = s_useSysLog;
	Settings::LunaSettings()->logger_useColor = s_colorLog;
	Settings::LunaSettings()->logger_useTerminal = s_useTerminal;
#if SHIPPING_VERSION
	Settings::LunaSettings()->logger_level = G_LOG_LEVEL_CRITICAL;
#else
	Settings::LunaSettings()->logger_level = G_LOG_LEVEL_DEBUG;
#endif
	Settings::LunaSettings()->forceSoftwareRendering = s_forceSoftwareRendering;
	if (s_forceSoftwareRendering)
		Settings::LunaSettings()->atlasEnabled = false;

    if (s_logLevelStr)
    {
        if (0 == strcasecmp(s_logLevelStr, "error"))
            Settings::LunaSettings()->logger_level = G_LOG_LEVEL_ERROR;
        else if (0 == strcasecmp(s_logLevelStr, "critical"))
            Settings::LunaSettings()->logger_level = G_LOG_LEVEL_CRITICAL;
        else if (0 == strcasecmp(s_logLevelStr, "warning"))
            Settings::LunaSettings()->logger_level = G_LOG_LEVEL_WARNING;
        else if (0 == strcasecmp(s_logLevelStr, "message"))
            Settings::LunaSettings()->logger_level = G_LOG_LEVEL_MESSAGE;
        else if (0 == strcasecmp(s_logLevelStr, "info"))
            Settings::LunaSettings()->logger_level = G_LOG_LEVEL_INFO;
        else if (0 == strcasecmp(s_logLevelStr, "debug"))
            Settings::LunaSettings()->logger_level = G_LOG_LEVEL_DEBUG;
    }

    g_option_context_free(context);
}

/**
 * Crashes the program
 */
static void generateGoodBacktraceTerminateHandler()
{
	volatile int* p = 0;
	*p = 0;
	exit(-1);
}

/**
 * Number of arguments Luna was started with
 * 
 * This variable is set in main().
 * 
 * @see main()
 */
int appArgc = 0;

/**
 * Pointer to char* of arguments Luna was started with
 * 
 * This variable is set in main().
 * 
 * @see main()
 */
char** appArgv = 0;

/**
 * Main program entry point
 *
 * This function is the one called by the operating system to start Luna.
 * 
 * This function sets {@link appArgc appArgc} and {@link appArgv appArgv}.
 * 
 * @see appArgc
 * @see appArgv
 *
 * @param	argc		Number of command-line arguments
 * @param	argv		Pointer to list of char* of each of the arguments
 *
 * @return			0 = success, anything else = failure
 */
int main( int argc, char** argv)
{
	appArgc = argc;
	appArgv = argv;

	std::set_terminate(generateGoodBacktraceTerminateHandler);

	g_thread_init(NULL);

	g_debug("SysMgr compiled against Qt %s, running on %s", QT_VERSION_STR, qVersion());

	// Command-Line options
	parseCommandlineOptions(argc, argv);

	if (s_debugTrapStr && 0 == strcasecmp(s_debugTrapStr, "on")) {
		debugCrashes = true;
	}

	if (s_mallocStatsFileStr) {
		setupMallocStats(s_mallocStatsFileStr);
	}

	sysmgrPid = getpid();

	// Load Settings (first!)
	Settings* settings = Settings::LunaSettings();

	// Initialize logging handler
	g_log_set_default_handler(logFilter, NULL);

#if defined(TARGET_DESKTOP)
	// use terminal logging when running on desktop
	settings->logger_useTerminal = true;
#endif

	// disable color logging using an environment variable. Useful when run from QtCreator
	const char* useColor = ::getenv("COLOR_LOGGING");
	if (useColor)
		settings->logger_useColor = (useColor[0] != 0 && useColor[0] != '0');

	HostBase* host = HostBase::instance();
	// the resolution is just a hint, the actual
	// resolution may get picked up from the fb driver on arm
	host->init(settings->displayWidth, settings->displayHeight);

	// Tie LunaSysMgr to Processor 0
	setCpuAffinity(getpid(), 1);

	// Safe to create logging threads now
	logInit();

#if !defined(TARGET_DESKTOP)
	// Set "nice" property
	setpriority(PRIO_PROCESS,getpid(),-1);
#endif

#if (QT_VERSION < QT_VERSION_CHECK(5, 0, 0))
	qInstallMsgHandler(qtMsgHandler);
#else
	qInstallMessageHandler(qtMsgHandler);
#endif

	QCoreApplication app(argc, argv);

	// We need this to start up services and input controls provided by the host
	// implementation
	host->show();

	initMallocStatsCb(HostBase::instance()->mainLoop(), s_mallocStatsInterval);

	// Initialize Preferences handler
	(void) Preferences::instance();

	LocalePreferences* lp = LocalePreferences::instance();
	QObject::connect(lp, SIGNAL(prefsLocaleChanged()), new ProcessKiller(), SLOT(localeChanged()));

	// Initialize Localization handler
	(void) Localization::instance();

	//Register vibration/haptics support
	HapticsController::instance()->startService();

	(void) DeviceInfo::instance();

	// Initialize Security handler
	(void) Security::instance();

	// Initialize BackupManager
	BackupManager::instance()->init(HostBase::instance()->mainLoop());

	// Initialize the System Service
	SystemService::instance()->init();

	// Initialize the Boot Manager
	BootManager::instance();

	// Initialize the application mgr
	ApplicationManager::instance()->init();

	// Initialize the Application Installer
	ApplicationInstaller::instance();

	ApplicationProcessManager::instance();

	ApplicationLaunchBooster::instance();

	// Initialize the Event Reporter
	EventReporter::init(host->mainLoop());

	// Initialize the SysMgr MemoryMonitor
	MemoryMonitor::instance();

	// load all set policies
	EASPolicyManager::instance()->load();

	// Initialize our display manager
	new DisplayManager();

	// Initialize input event monitor
	InputEventMonitor::instance();

	app.exec();

	return 0;
}
