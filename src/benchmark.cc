#include "benchmark/benchmark.h"
#include "benchmark/macros.h"
#include "colorprint.h"
#include "commandlineflags.h"
#include "mutex_lock.h"
#include "sleep.h"
#include "stat.h"
#include "sysinfo.h"
#include "walltime.h"

#include <pthread.h>
#include <semaphore.h>
#include <string.h>

#if defined OS_FREEBSD
#include <gnuregex.h>
#else
#include <regex.h>
#endif

#include <algorithm>
#include <iostream>
#include <memory>
#include <sstream>

DEFINE_string(benchmark_filter, ".",
              "A regular expression that specifies the set of benchmarks "
              "to execute.  If this flag is empty, no benchmarks are run.  "
              "If this flag is the string \"all\", all benchmarks linked "
              "into the process are run.");

DEFINE_int32(benchmark_min_iters, 100,
             "Minimum number of iterations per benchmark");

DEFINE_int32(benchmark_max_iters, 1000000000,
             "Maximum number of iterations per benchmark");

DEFINE_double(benchmark_min_time, 0.5,
              "Minimum number of seconds we should run benchmark before "
              "results are considered significant.  For cpu-time based "
              "tests, this is the lower bound on the total cpu time "
              "used by all threads that make up the test.  For real-time "
              "based tests, this is the lower bound on the elapsed time "
              "of the benchmark execution, regardless of number of "
              "threads.");

DEFINE_bool(benchmark_memory_usage, false,
            "Report memory usage for all benchmarks");

DEFINE_int32(benchmark_repetitions, 1,
             "The number of runs of each benchmark. If greater than 1, the "
             "mean and standard deviation of the runs will be reported.");

DEFINE_int32(v, 0, "The level of verbose logging to output");
DEFINE_bool(color_print, true, "Enables colorized logging.");

// Will be non-empty if heap checking is turned on, which would
// invalidate any benchmarks.
DECLARE_string(heap_check);

// The ""'s catch people who don't pass in a literal for "str"
#define strliterallen(str) (sizeof("" str "")-1)

// Must use a string literal for prefix.
#define memprefix(str, len, prefix)                         \
  ( (((len) >= strliterallen(prefix))                       \
     && memcmp(str, prefix, strliterallen(prefix)) == 0)    \
    ? str + strliterallen(prefix)                           \
    : NULL )

namespace benchmark {

namespace {

// kilo, Mega, Giga, Tera, Peta, Exa, Zetta, Yotta.
static const char kBigSIUnits[] = "kMGTPEZY";
// Kibi, Mebi, Gibi, Tebi, Pebi, Exbi, Zebi, Yobi.
static const char kBigIECUnits[] = "KMGTPEZY";
// milli, micro, nano, pico, femto, atto, zepto, yocto.
static const char kSmallSIUnits[] = "munpfazy";

// We require that all three arrays have the same size.
static_assert(arraysize(kBigSIUnits) == arraysize(kBigIECUnits),
              "SI and IEC unit arrays must be the same size");
static_assert(arraysize(kSmallSIUnits) == arraysize(kBigSIUnits),
              "Small SI and Big SI unit arrays must be the same size");
static const int kUnitsSize = arraysize(kBigSIUnits);

void ToExponentAndMantissa(double val, double thresh,
                           int precision, double one_k,
                           std::string* mantissa, int* exponent) {
  std::stringstream mantissa_stream;

  if (val < 0) {
    mantissa_stream << "-";
    val = -val;
  }

  // Adjust threshold so that it never excludes things which can't be rendered
  // in 'precision' digits.
  const double adjusted_threshold =
      std::max(thresh, 1.0 / pow(10.0, precision));
  const double big_threshold = adjusted_threshold * one_k;
  const double small_threshold = adjusted_threshold;

  if (val > big_threshold) {
    // Positive powers
    double scaled = val;
    for (size_t i = 0; i < arraysize(kBigSIUnits); ++i) {
      scaled /= one_k;
      if (scaled <= big_threshold) {
        mantissa_stream << scaled;
        *exponent = i + 1;
        *mantissa = mantissa_stream.str();
        return;
      }
    }
    mantissa_stream << val;
    *exponent = 0;
  } else if (val < small_threshold) {
    // Negative powers
    double scaled = val;
    for (size_t i = 0; i < arraysize(kSmallSIUnits); ++i) {
      scaled *= one_k;
      if (scaled >= small_threshold) {
        mantissa_stream << scaled;
        *exponent = -i - 1;
        *mantissa = mantissa_stream.str();
        return;
      }
    }
    mantissa_stream << val;
    *exponent = 0;
  } else {
    mantissa_stream << val;
    *exponent = 0;
  }
  *mantissa = mantissa_stream.str();
}

std::string ExponentToPrefix(int exponent, bool iec) {
  if (exponent == 0)
    return "";

  const int index = (exponent > 0 ? exponent - 1 : -exponent - 1);
  if (index >= kUnitsSize)
    return "";

  const char *array = (exponent > 0 ? (iec ? kBigIECUnits : kBigSIUnits) :
                       kSmallSIUnits);
  if (iec)
    return array[index] + std::string("i");
  else
    return std::string(1, array[index]);
}

std::string ToBinaryStringFullySpecified(double value, double threshold,
                                         int precision) {
  std::string mantissa;
  int exponent;
  ToExponentAndMantissa(value, threshold, precision, 1024., &mantissa,
                        &exponent);
  return mantissa + ExponentToPrefix(exponent, false);
}

inline void AppendHumanReadable(int n, std::string* str) {
  std::stringstream ss;
  // Round down to the nearest SI prefix.
  ss << "/" << ToBinaryStringFullySpecified(n, 1.0, 0);
  *str += ss.str();
}

inline std::string HumanReadableNumber(double n) {
  // 1.1 means that figures up to 1.1k should be shown with the next unit down;
  // this softens edge effects.
  // 1 means that we should show one decimal place of precision.
  return ToBinaryStringFullySpecified(n, 1.1, 1);
}

}  // end namespace

namespace internal {
struct Benchmark::ThreadStats {
  int64_t bytes_processed;
  int64_t items_processed;

  ThreadStats() { Reset(); }

  void Reset() {
    bytes_processed = 0;
    items_processed = 0;
  }

  void Add(const ThreadStats& other) {
    bytes_processed += other.bytes_processed;
    items_processed += other.items_processed;
  }
};

}  // end namespace internal

namespace {

// Per-thread stats
pthread_key_t thread_stats_key;
internal::Benchmark::ThreadStats* thread_stats = nullptr;

// For non-dense Range, intermediate values are powers of kRangeMultiplier.
static const int kRangeMultiplier = 8;

// List of all registered benchmarks.  Note that each registered
// benchmark identifies a family of related benchmarks to run.
static pthread_mutex_t benchmark_mutex;
static std::vector<internal::Benchmark*>* families = NULL;

bool running_benchmark = false;

// Should this benchmark report memory usage?
bool get_memory_usage;

// Should this benchmark base decisions off of real time rather than
// cpu time?
bool use_real_time;

// Overhead of an empty benchmark.
double overhead = 0.0;

void DeleteThreadStats(void* p) {
  delete (internal::Benchmark::ThreadStats*) p;
}

// Return prefix to print in front of each reported line
const char* Prefix() {
#ifdef NDEBUG
  return "";
#else
  return "DEBUG: ";
#endif
}

// TODO
//static internal::MallocCounter *benchmark_mc;

static bool CpuScalingEnabled() {
  // On Linux, the CPUfreq subsystem exposes CPU information as files on the
  // local file system. If reading the exported files fails, then we may not be
  // running on Linux, so we silently ignore all the read errors.
  for (int cpu = 0, num_cpus = NumCPUs(); cpu < num_cpus; ++cpu) {
    std::stringstream ss;
    ss << "/sys/devices/system/cpu/cpu" << cpu << "/cpufreq/scaling_governor";
    std::string governor_file = ss.str();
    FILE* file = fopen(governor_file.c_str(), "r");
    if (!file)
      break;
    char buff[16];
    size_t bytes_read = fread(buff, 1, sizeof(buff), file);
    fclose(file);
    if (memprefix(buff, bytes_read, "performance") == NULL)
      return true;
  }
  return false;
}

}  // namespace

namespace internal {

BenchmarkReporter::~BenchmarkReporter() {}

void ComputeStats(const std::vector<BenchmarkRunData>& reports,
                  BenchmarkRunData* mean_data,
                  BenchmarkRunData* stddev_data) {
  // Accumulators.
  Stat1_d real_accumulated_time_stat;
  Stat1_d cpu_accumulated_time_stat;
  Stat1_d bytes_per_second_stat;
  Stat1_d items_per_second_stat;
  Stat1MinMax_d max_heapbytes_used_stat;
  int total_iters = 0;

  // Populate the accumulators.
  for (std::vector<BenchmarkRunData>::const_iterator it = reports.begin();
       it != reports.end(); ++it) {
    CHECK_EQ(reports[0].benchmark_name, it->benchmark_name);
    total_iters += it->iterations;
    real_accumulated_time_stat +=
        Stat1_d(it->real_accumulated_time/it->iterations, it->iterations);
    cpu_accumulated_time_stat +=
        Stat1_d(it->cpu_accumulated_time/it->iterations, it->iterations);
    items_per_second_stat += Stat1_d(it->items_per_second, it->iterations);
    bytes_per_second_stat += Stat1_d(it->bytes_per_second, it->iterations);
    max_heapbytes_used_stat += Stat1MinMax_d(it->max_heapbytes_used,
                                             it->iterations);
  }

  // Get the data from the accumulator to BenchmarkRunData's.
  mean_data->benchmark_name = reports[0].benchmark_name + "_mean";
  mean_data->iterations = total_iters;
  mean_data->real_accumulated_time = real_accumulated_time_stat.Sum();
  mean_data->cpu_accumulated_time = cpu_accumulated_time_stat.Sum();
  mean_data->bytes_per_second = bytes_per_second_stat.Mean();
  mean_data->items_per_second = items_per_second_stat.Mean();
  mean_data->max_heapbytes_used = max_heapbytes_used_stat.Max();

  // Only add label to mean/stddev if it is same for all runs
  mean_data->report_label = reports[0].report_label;
  for (size_t i = 1; i < reports.size(); i++) {
    if (reports[i].report_label != reports[0].report_label) {
      mean_data->report_label = "";
      break;
    }
  }

  stddev_data->benchmark_name = reports[0].benchmark_name + "_stddev";
  stddev_data->report_label = mean_data->report_label;
  stddev_data->iterations = total_iters;
  // We multiply by total_iters since PrintRunData expects a total time.
  stddev_data->real_accumulated_time =
      real_accumulated_time_stat.StdDev() * total_iters;
  stddev_data->cpu_accumulated_time =
      cpu_accumulated_time_stat.StdDev() * total_iters;
  stddev_data->bytes_per_second = bytes_per_second_stat.StdDev();
  stddev_data->items_per_second = items_per_second_stat.StdDev();
  stddev_data->max_heapbytes_used = max_heapbytes_used_stat.StdDev();
}

std::string ConsoleReporter::PrintMemoryUsage(double bytes) {
  if (!get_memory_usage || bytes < 0.0)
    return "";

  std::stringstream ss;
  ss << " " << HumanReadableNumber(bytes) << "B peak-mem";
  return ss.str();
}

bool ConsoleReporter::ReportContext(const BenchmarkContextData& context) {
  name_field_width_ = context.name_field_width;

  std::cout << "Benchmarking on " << context.num_cpus << " X "
            << context.mhz_per_cpu << " MHz CPU"
            << ((context.num_cpus > 1) ? "s" : "") << "\n";

  int remainder_ms;
  char time_buf[32];
  std::cout << walltime::Print(walltime::Now(), "%Y/%m/%d-%H:%M:%S",
                               true,   // use local timezone
                               time_buf, &remainder_ms) << "\n"; 

  // Show details of CPU model, caches, TLBs etc.
//  if (!context.cpu_info.empty())
//    std::cout << "CPU: " << context.cpu_info.c_str();

  if (context.cpu_scaling_enabled) {
    std::cerr << "CPU scaling is enabled: Benchmark timings may be noisy.\n";
  }

  int output_width = fprintf(stdout, "%s%-*s %10s %10s %10s\n",
                             Prefix(), name_field_width_, "Benchmark",
                             "Time(ns)", "CPU(ns)", "Iterations");
  std::cout << std::string(output_width - 1, '-').c_str() << "\n";

  return true;
}

void ConsoleReporter::ReportRuns(const std::vector<BenchmarkRunData>& reports) {
  for (std::vector<BenchmarkRunData>::const_iterator it = reports.begin();
       it != reports.end(); ++it) {
    CHECK_EQ(reports[0].benchmark_name, it->benchmark_name);
    PrintRunData(*it);
  }

  // We don't report aggregated data if there was a single run.
  if (reports.size() < 2)
    return;

  BenchmarkRunData mean_data;
  BenchmarkRunData stddev_data;
  internal::ComputeStats(reports, &mean_data, &stddev_data);

  // Output using PrintRun.
  PrintRunData(mean_data);
  PrintRunData(stddev_data);
  fprintf(stdout, "\n");
}

void ConsoleReporter::PrintRunData(const BenchmarkRunData& result) {
  // Format bytes per second
  std::string rate;
  if (result.bytes_per_second > 0) {
    std::stringstream ss;
    ss << " " << HumanReadableNumber(result.bytes_per_second) << "B/s";
    rate = ss.str();
  }

  // Format items per second
  std::string items;
  if (result.items_per_second > 0) {
    std::stringstream ss;
    ss << " " << HumanReadableNumber(result.items_per_second) << " items/s";
    items = ss.str();
  }

  ColorPrintf(COLOR_DEFAULT, "%s", Prefix());
  ColorPrintf(COLOR_GREEN, "%-*s ",
              name_field_width_, result.benchmark_name.c_str());
  ColorPrintf(COLOR_YELLOW, "%10.0f %10.0f ",
              (result.real_accumulated_time * 1e9) /
                  (static_cast<double>(result.iterations)),
              (result.cpu_accumulated_time * 1e9) /
                  (static_cast<double>(result.iterations)));
  ColorPrintf(COLOR_CYAN, "%10lld", result.iterations);
  ColorPrintf(COLOR_DEFAULT, "%*s %s %s%s\n", 16, rate.c_str(), items.c_str(),
              result.report_label.c_str(),
              PrintMemoryUsage(result.max_heapbytes_used).c_str());
}

void MemoryUsage() {
  //if (benchmark_mc) {
  //  benchmark_mc->Reset();
  //} else {
    get_memory_usage = true;
  //}
}

void UseRealTime() {
  use_real_time = true;
}

void PrintUsageAndExit() {
  fprintf(stdout, "benchmark [--benchmark_filter=<regex>]\n"
// TODO           "          [--benchmark_min_iters=<min_iters>]\n"
// TODO           "          [--benchmark_max_iters=<max_iters>]\n"
// TODO           "          [--benchmark_min_time=<min_time>]\n"
//                "          [--benchmark_memory_usage]\n"
// TODO           "          [--benchmark_repetitions=<num_repetitions>]\n"
                  "          [--color_print={true|false}]\n"
                  "          [--v=<verbosity>]\n");
  exit(0);
}

void ParseCommandLineFlags(int* argc, const char** argv) {
  for (int i = 1; i < *argc; ++i) {
    if (ParseStringFlag(argv[i], "benchmark_filter",
                        &FLAGS_benchmark_filter) ||
        /* TODO(dominic)
        ParseInt32Flag(argv[i], "benchmark_min_iters",
                       &FLAGS_benchmark_min_iters) ||
        ParseInt32Flag(argv[i], "benchmark_max_iters",
                       &FLAGS_benchmark_max_iters) ||
        ParseDoubleFlag(argv[i], "benchmark_min_time",
                        &FLAGS_benchmark_min_time) ||
        // TODO(dominic)
//        ParseBoolFlag(argv[i], "gbenchmark_memory_usage",
//                      &FLAGS_gbenchmark_memory_usage) ||
        ParseInt32Flag(argv[i], "benchmark_repetitions",
                       &FLAGS_benchmark_repetitions) ||
                       */
        ParseBoolFlag(argv[i], "color_print", &FLAGS_color_print) ||
        ParseInt32Flag(argv[i], "v", &FLAGS_v)) {
      for (int j = i; j != *argc; ++j)
        argv[j] = argv[j + 1];

      --(*argc);
      --i;
    } else if (IsFlag(argv[i], "help"))
      PrintUsageAndExit();
  }
}

}  // end namespace internal

// A clock that provides a fast mechanism to check if we're nearly done.
class State::FastClock {
 public:
  enum Type { REAL_TIME, CPU_TIME };
  explicit FastClock(Type type)
      : type_(type), approx_time_(NowMicros()) {

    sem_init(&bg_done_, 0, 0);
    pthread_create(&bg_, NULL, &BGThreadWrapper, this);
  }

  ~FastClock() {
    sem_post(&bg_done_);
    pthread_join(bg_, NULL);
    sem_destroy(&bg_done_);
  }

  // Returns true if the current time is guaranteed to be past "when_micros".
  // This method is very fast.
  inline bool HasReached(int64_t when_micros) {
    return approx_time_ >= when_micros;
    // NOTE: this is the same as we're dealing with an int64_t
    //return (base::subtle::NoBarrier_Load(&approx_time_) >= when_micros);
  }

  // Returns the current time in microseconds past the epoch.
  int64_t NowMicros() const {
    double t = 0;
    switch (type_) {
      case REAL_TIME:
        t = walltime::Now();
        break;
      case CPU_TIME:
        t = MyCPUUsage() + ChildrenCPUUsage();
        break;
    }
    return static_cast<int64_t>(t * 1e6);
  }

  // Reinitialize if necessary (since clock type may be change once benchmark
  // function starts running - see UseRealTime).
  void InitType(Type type) {
    type_ = type;
    approx_time_ = NowMicros();
    // NOTE: This is the same barring a memory barrier
    // base::subtle::Release_Store(&approx_time_, NowMicros());
  }

 private:
  Type type_;
  int64_t approx_time_;  // Last time measurement taken by bg_
  pthread_t bg_;  // Background thread that updates last_time_ once every ms

  sem_t bg_done_;

  static void* BGThreadWrapper(void* that) {
    ((FastClock*)that)->BGThread();
    return NULL;
  }

  void BGThread() {
    int done = 0;
    do {
      SleepForMicroseconds(1000);
      approx_time_ = NowMicros();
      // NOTE: same code but no memory barrier. think on it.
      //base::subtle::Release_Store(&approx_time_, NowMicros());
      sem_getvalue(&bg_done_, &done);
    } while (done == 0);
  }

  DISALLOW_COPY_AND_ASSIGN(FastClock);
};

namespace internal {

const int Benchmark::kNumCpuMarker;

// Information kept per benchmark we may want to run
struct Benchmark::Instance {
  Instance()
      : rangeXset(false), rangeX(kNoRange),
        rangeYset(false), rangeY(kNoRange) {}

  std::string name;
  Benchmark* bm;
  bool      rangeXset;
  int       rangeX;
  bool      rangeYset;
  int       rangeY;
  int       threads;    // Number of concurrent threads to use

  bool multithreaded() const { return !bm->thread_counts_.empty(); }
};

}  // end namespace internal

struct State::SharedState {
  const internal::Benchmark::Instance* instance;
  pthread_mutex_t mu;
  int starting;  // Number of threads that have entered STARTING state
  int stopping;  // Number of threads that have entered STOPPING state
  int threads;   // Number of total threads that are running concurrently
  internal::Benchmark::ThreadStats stats;
  std::vector<internal::BenchmarkRunData> runs;  // accumulated runs
  std::string label;

  SharedState(const internal::Benchmark::Instance* b, int t)
      : instance(b), starting(0), stopping(0), threads(t) {
  }
  DISALLOW_COPY_AND_ASSIGN(SharedState);
};

namespace internal {

Benchmark::Benchmark(const char* name, BenchmarkFunction f)
    : name_(name), function_(f) {
  mutex_lock l(&benchmark_mutex);
  if (families == nullptr)
    families = new std::vector<Benchmark*>;
  registration_index_ = families->size();
  families->push_back(this);
}

Benchmark::~Benchmark() {
  mutex_lock l(&benchmark_mutex);
  CHECK((*families)[registration_index_] == this);
  (*families)[registration_index_] = NULL;
  // Shrink the vector if convenient.
  while (!families->empty() && families->back() == NULL)
    families->pop_back();
}

Benchmark* Benchmark::Arg(int x) {
  mutex_lock l(&benchmark_mutex);
  rangeX_.push_back(x);
  return this;
}

Benchmark* Benchmark::Range(int start, int limit) {
  std::vector<int> arglist;
  AddRange(&arglist, start, limit, kRangeMultiplier);

  mutex_lock l(&benchmark_mutex);
  for (size_t i = 0; i < arglist.size(); ++i)
    rangeX_.push_back(arglist[i]);
  return this;
}

Benchmark* Benchmark::DenseRange(int start, int limit) {
  CHECK_GE(start, 0);
  CHECK_LE(start, limit);
  mutex_lock l(&benchmark_mutex);
  for (int arg = start; arg <= limit; ++arg)
    rangeX_.push_back(arg);
  return this;
}

Benchmark* Benchmark::ArgPair(int x, int y) {
  mutex_lock l(&benchmark_mutex);
  rangeX_.push_back(x);
  rangeY_.push_back(y);
  return this;
}

Benchmark* Benchmark::RangePair(int lo1, int hi1, int lo2, int hi2) {
  std::vector<int> arglist1, arglist2;
  AddRange(&arglist1, lo1, hi1, kRangeMultiplier);
  AddRange(&arglist2, lo2, hi2, kRangeMultiplier);

  mutex_lock l(&benchmark_mutex);
  rangeX_.resize(arglist1.size());
  std::copy(arglist1.begin(), arglist1.end(), rangeX_.begin());
  rangeY_.resize(arglist2.size());
  std::copy(arglist2.begin(), arglist2.end(), rangeY_.begin());
  return this;
}

Benchmark* Benchmark::Apply(void (*custom_arguments)(Benchmark* benchmark)) {
  custom_arguments(this);
  return this;
}

Benchmark* Benchmark::Threads(int t) {
  CHECK_GT(t, 0);
  mutex_lock l(&benchmark_mutex);
  thread_counts_.push_back(t);
  return this;
}

Benchmark* Benchmark::ThreadRange(int min_threads, int max_threads) {
  CHECK_GT(min_threads, 0);
  CHECK_GE(max_threads, min_threads);

  mutex_lock l(&benchmark_mutex);
  AddRange(&thread_counts_, min_threads, max_threads, 2);
  return this;
}

Benchmark* Benchmark::ThreadPerCpu() {
  mutex_lock l(&benchmark_mutex);
  thread_counts_.push_back(kNumCpuMarker);
  return this;
}

void Benchmark::AddRange(std::vector<int>* dst, int lo, int hi, int mult) {
  CHECK_GE(lo, 0);
  CHECK_GE(hi, lo);

  // Add "lo"
  dst->push_back(lo);

  // Now space out the benchmarks in multiples of "mult"
  for (int32_t i = 1; i < std::numeric_limits<int32_t>::max()/mult; i *= mult) {
    if (i >= hi) break;
    if (i > lo)
      dst->push_back(i);
  }
  // Add "hi" (if different from "lo")
  if (hi != lo)
    dst->push_back(hi);
}

std::vector<Benchmark::Instance> Benchmark::CreateBenchmarkInstances(
    int rangeXindex, int rangeYindex) {
  // Special list of thread counts to use when none are specified
  std::vector<int> one_thread;
  one_thread.push_back(1);

  std::vector<Benchmark::Instance> instances;

  const bool is_multithreaded = (!thread_counts_.empty());
  const std::vector<int>* thread_counts =
      (is_multithreaded ? &thread_counts_ : &one_thread);
  for (size_t t = 0; t < thread_counts->size(); ++t) {
    int num_threads = (*thread_counts)[t];
    if (num_threads == kNumCpuMarker)
      num_threads = NumCPUs();

    Instance instance;
    instance.name = name_;
    instance.bm = this;
    instance.threads = num_threads;

    if (rangeXindex != kNoRange) {
      instance.rangeX = rangeX_[rangeXindex];
      instance.rangeXset = true;
      AppendHumanReadable(instance.rangeX, &instance.name);
    }
    if (rangeYindex != kNoRange) {
      instance.rangeY = rangeY_[rangeYindex];
      instance.rangeYset = true;
      AppendHumanReadable(instance.rangeY, &instance.name);
    }

    // Add the number of threads used to the name
    if (is_multithreaded) {
      std::stringstream ss;
      ss << "/threads:" << instance.threads;
      instance.name += ss.str();
    }

    instances.push_back(instance);
  }

  return instances;
}

// Extract the list of benchmark instances that match the specified
// regular expression.
void Benchmark::FindBenchmarks(const std::string& spec,
                               std::vector<Instance>* benchmarks) {
  // Make regular expression out of command-line flag
  regex_t re;
  int ec = regcomp(&re, spec.c_str(), REG_EXTENDED | REG_NOSUB);
  if (ec != 0) {
    size_t needed = regerror(ec, &re, NULL, 0);
    char* errbuf = new char[needed];
    regerror(ec, &re, errbuf, needed);
    std::cerr << "Could not compile benchmark re: " << errbuf << "\n";
    delete[] errbuf;
    return;
  }

  mutex_lock l(&benchmark_mutex);
  for (Benchmark* family : *families) {
    if (family == nullptr) continue;  // Family was deleted

    // Match against filter.
    if (regexec(&re, family->name_.c_str(), 0, NULL, 0) != 0) {
#ifdef DEBUG
      std::cout << "Skipping " << family->name_ << "\n";
#endif
      continue;
    }

    std::vector<Benchmark::Instance> instances;
    if (family->rangeX_.empty() && family->rangeY_.empty()) {
      instances = family->CreateBenchmarkInstances(kNoRange, kNoRange);
      benchmarks->insert(benchmarks->end(), instances.begin(), instances.end());
    } else if (family->rangeY_.empty()) {
      for (size_t x = 0; x < family->rangeX_.size(); ++x) {
        instances = family->CreateBenchmarkInstances(x, kNoRange);
        benchmarks->insert(benchmarks->end(),
                           instances.begin(), instances.end());
      }
    } else {
      for (size_t x = 0; x < family->rangeX_.size(); ++x) {
        for (size_t y = 0; y < family->rangeY_.size(); ++y) {
          instances = family->CreateBenchmarkInstances(x, y);
          benchmarks->insert(benchmarks->end(),
                             instances.begin(), instances.end());
        }
      }
    }
  }
}

void Benchmark::MeasureOverhead() {
  State::FastClock clock(State::FastClock::CPU_TIME);
  State::SharedState state(NULL, 1);
  State runner(&clock, &state, 0);
  while (runner.KeepRunning()) {}
  overhead = state.runs[0].real_accumulated_time /
      static_cast<double>(state.runs[0].iterations);
#ifdef DEBUG
  std::cout << "Per-iteration overhead for doing nothing: " << overhead << "\n";
#endif
}

void Benchmark::RunInstance(const Instance& b, BenchmarkReporter* br) {
  use_real_time = false;
  running_benchmark = true;
  // get_memory_usage = FLAGS_gbenchmark_memory_usage;
  State::FastClock clock(State::FastClock::CPU_TIME);

  // Initialize the test runners.
  State::SharedState state(&b, b.threads);
  {
    std::unique_ptr<State> runners[b.threads];
    // TODO: create thread objects
    for (int i = 0; i < b.threads; ++i)
      runners[i].reset(new State(&clock, &state, i));

    // Run them all.
    for (int i = 0; i < b.threads; ++i) {
      State* r = runners[i].release();
      if (b.multithreaded()) {
        // TODO: start pthreads (member of state?) and set up thread local
        // pointers to stats
        //pool->Add(base::NewCallback(r, &State::Run));
      } else {
        pthread_setspecific(thread_stats_key, thread_stats);
        r->Run();
      }
    }
    if (b.multithreaded()) {
      // TODO: join all the threads
      //pool->JoinAll();
    }
  }
/*
  double mem_usage = 0;
  if (get_memory_usage) {
    // Measure memory usage
    Notification mem_done;
    BenchmarkRun mem_run;
    BenchmarkRun::SharedState mem_shared(&b, 1);
    mem_run.Init(&clock, &mem_shared, 0);
    {
      testing::MallocCounter mc(testing::MallocCounter::THIS_THREAD_ONLY);
      benchmark_mc = &mc;
      mem_run.Run(&mem_done);
      mem_done.WaitForNotification();
      benchmark_mc = NULL;
      mem_usage = mc.PeakHeapGrowth();
    }
  }
*/
  running_benchmark = false;

  for (internal::BenchmarkRunData& report : state.runs) {
    double seconds = (use_real_time ? report.real_accumulated_time :
                                      report.cpu_accumulated_time);
    // TODO: add the thread index here?
    report.benchmark_name = b.name;
    report.report_label = state.label;
    report.bytes_per_second = state.stats.bytes_processed / seconds;
    report.items_per_second = state.stats.items_processed / seconds;
    report.max_heapbytes_used = MeasurePeakHeapMemory(b);
  }

  br->ReportRuns(state.runs);
}

// Run the specified benchmark, measure its peak memory usage, and
// return the peak memory usage.
double Benchmark::MeasurePeakHeapMemory(const Instance& b) {
  if (!get_memory_usage)
    return 0.0;
  double bytes = 0.0;
 /*  TODO(dominich)
  // Should we do multi-threaded runs?
  const int num_threads = 1;
  const int num_iters = 1;
  {
//    internal::MallocCounter mc(internal::MallocCounter::THIS_THREAD_ONLY);
    running_benchmark = true;
    timer_manager = new TimerManager(1, NULL);
//    benchmark_mc = &mc;
    timer_manager->StartTimer();

    b.Run(num_iters);

    running_benchmark = false;
    delete timer_manager;
    timer_manager = NULL;
//    benchmark_mc = NULL;
//    bytes = mc.PeakHeapGrowth();
  }
  */
  return bytes;
}

}  // end namespace internal
  
State::State(FastClock* clock, SharedState* s, int t)
    : thread_index(t),
      state_(STATE_INITIAL),
      clock_(clock),
      shared_(s),
      iterations_(0),
      start_cpu_(0.0),
      start_time_(0.0),
      stop_time_micros_(0.0),
      start_pause_(0.0),
      pause_time_(0.0),
      total_iterations_(0),
      interval_micros_(
          static_cast<int64_t>(1e6 * FLAGS_benchmark_min_time /
                               FLAGS_benchmark_repetitions)) {
}

bool State::KeepRunning() {
  // Fast path
  if (!clock_->HasReached(stop_time_micros_ + pause_time_)) {
    ++iterations_;
    return true;
  }

  switch(state_) {
    case STATE_INITIAL: return StartRunning();
    case STATE_STARTING: CHECK(false); return true;
    case STATE_RUNNING: return FinishInterval();
    case STATE_STOPPING: return MaybeStop();
    case STATE_STOPPED: CHECK(false); return true;
  }
  CHECK(false);
  return false;
}

void State::PauseTiming() {
  start_pause_ = walltime::Now();
}

void State::ResumeTiming() {
  pause_time_ += walltime::Now() - start_pause_;
}

void State::SetBytesProcessed(int64_t bytes) {
  CHECK_EQ(STATE_STOPPED, state_);
  mutex_lock l(&shared_->mu);
  internal::Benchmark::ThreadStats* thread_stats =
      (internal::Benchmark::ThreadStats*) pthread_getspecific(thread_stats_key);
  thread_stats->bytes_processed = bytes;
}

void State::SetItemsProcessed(int64_t items) {
  CHECK_EQ(STATE_STOPPED, state_);
  mutex_lock l(&shared_->mu);
  internal::Benchmark::ThreadStats* thread_stats =
      (internal::Benchmark::ThreadStats*) pthread_getspecific(thread_stats_key);
  thread_stats->items_processed = items;
}

void State::SetLabel(const std::string& label) {
  CHECK_EQ(STATE_STOPPED, state_);
  mutex_lock l(&shared_->mu);
  shared_->label = label;
}

int State::range_x() const {
  CHECK(shared_->instance->rangeXset);
  /*
  <<
      "Failed to get range_x as it was not set. Did you register your "
      "benchmark with a range parameter?";
      */
  return shared_->instance->rangeX;
}

int State::range_y() const {
  CHECK(shared_->instance->rangeYset);
 /* <<
      "Failed to get range_y as it was not set. Did you register your "
      "benchmark with a range parameter?";
      */
  return shared_->instance->rangeY;
}

bool State::StartRunning() {
  {
    mutex_lock l(&shared_->mu);
    CHECK_EQ(state_, STATE_INITIAL);
    state_ = STATE_STARTING;
    is_continuation_ = false;
    CHECK_LT(shared_->starting, shared_->threads);
    ++shared_->starting;
    if (shared_->starting == shared_->threads) {
      // Last thread to start.
      clock_->InitType(
          use_real_time ? FastClock::REAL_TIME : FastClock::CPU_TIME);
    } else {
      // Wait for others.
      // TODO(dominic): semaphore!
      // while (pthread_getsemaphore(shared_->starting_sem_) !=
      // shared_->threads) { }
      //shared_->mu.Await(base::Condition(this, &State::AllStarting));
    }
    CHECK_EQ(state_, STATE_STARTING);
    state_ = STATE_RUNNING;
  }
  NewInterval();
  return true;
}

bool State::AllStarting() {
  CHECK_LE(shared_->starting, shared_->threads);
  return shared_->starting == shared_->threads;
}

void State::NewInterval() {
  stop_time_micros_ = clock_->NowMicros() + interval_micros_;
  if (!is_continuation_) {
#ifdef DEBUG
    std::cout << "Starting new interval; stopping in " << interval_micros_
              << "\n";
#endif
    iterations_ = 0;
    pause_time_ = 0;
    start_cpu_ = MyCPUUsage() + ChildrenCPUUsage();
    start_time_ = walltime::Now();
  } else {
#ifdef DEBUG
    std::cout << "Continuing interval; stopping in " << interval_micros_
              << "\n";
#endif
  }
}

bool State::FinishInterval() {
  if (iterations_ < FLAGS_benchmark_min_iters / FLAGS_benchmark_repetitions &&
      interval_micros_ < 5000000) {
    interval_micros_ *= 2;
#ifdef DEBUG
    std::cout << "Interval was too short; trying again for "
              << interval_micros_ << " useconds.\n";
#endif
    is_continuation_ = false;
    NewInterval();
    return true;
  }

  internal::BenchmarkRunData data;
  data.thread_index = thread_index;
  data.iterations = iterations_;
  data.thread_index = thread_index;

  const double accumulated_time = walltime::Now() - start_time_;
  const double total_overhead = 0.0; // TODO: overhead * iterations_;
  CHECK_LT(pause_time_, accumulated_time);
  CHECK_LT(pause_time_ + total_overhead, accumulated_time);
  data.real_accumulated_time =
      accumulated_time - (pause_time_ + total_overhead);
  data.cpu_accumulated_time = (MyCPUUsage() + ChildrenCPUUsage()) - start_cpu_;
  total_iterations_ += iterations_;

  bool keep_going = false;
  {
    mutex_lock l(&shared_->mu);
    if (is_continuation_)
      shared_->runs.back() = data;
    else
      shared_->runs.push_back(data);
    keep_going = RunAnotherInterval();
    if (!keep_going) {
      ++shared_->stopping;
      if (shared_->stopping < shared_->threads) {
        // Other threads are still running, so continue running but without
        // timing to present an expected background load to the other threads.
        state_ = STATE_STOPPING;
        keep_going = true;
      } else {
        state_ = STATE_STOPPED;
      }
    }
  }

  if (state_ == STATE_RUNNING) {
    is_continuation_ = true;
    NewInterval();
  }
  return keep_going;
}

bool State::RunAnotherInterval() const {
  if (total_iterations_ < FLAGS_benchmark_min_iters)
    return true;
  if (total_iterations_ > FLAGS_benchmark_max_iters)
    return false;
  if (static_cast<int>(shared_->runs.size()) >= FLAGS_benchmark_repetitions)
    return false;
  return true;
}

bool State::MaybeStop() {
  mutex_lock l(&shared_->mu);
  if (shared_->stopping < shared_->threads) {
    CHECK_EQ(state_, STATE_STOPPING);
    return true;
  }
  state_ = STATE_STOPPED;
  return false;
}

void State::Run() {
  internal::Benchmark::ThreadStats* thread_stats =
      (internal::Benchmark::ThreadStats*) pthread_getspecific(thread_stats_key);
  thread_stats->Reset();
  shared_->instance->bm->function_(*this);
  {
    mutex_lock l(&shared_->mu);
    shared_->stats.Add(*thread_stats);
  }
}

namespace internal {

void RunMatchingBenchmarks(const std::string& spec,
                           BenchmarkReporter* reporter) {
  CHECK(reporter != NULL);
  if (spec.empty()) return;

  std::vector<internal::Benchmark::Instance> benchmarks;
  internal::Benchmark::FindBenchmarks(spec, &benchmarks);

  // Determine the width of the name field using a minimum width of 10.
  // Also determine max number of threads needed.
  int name_field_width = 10;
  for (const internal::Benchmark::Instance& benchmark : benchmarks) {
    // Add width for _stddev and threads:XX
    if (benchmark.threads > 1 && FLAGS_benchmark_repetitions > 1) {
      name_field_width = std::max<int>(name_field_width,
                                       benchmark.name.size() + 17);
    } else if (benchmark.threads> 1) {
      name_field_width = std::max<int>(name_field_width,
                                       benchmark.name.size() + 10);
    } else if (FLAGS_benchmark_repetitions > 1) {
      name_field_width = std::max<int>(name_field_width,
                                       benchmark.name.size() + 7);
    } else {
      name_field_width = std::max<int>(name_field_width,
                                       benchmark.name.size());
    }
  }

  // Print header here
  BenchmarkContextData context;
  context.num_cpus = NumCPUs();
  context.mhz_per_cpu =  CyclesPerSecond() / 1000000.0f;
//  context.cpu_info = base::CompactCPUIDInfoString();
  context.cpu_scaling_enabled = CpuScalingEnabled();
  context.name_field_width = name_field_width;

  if (reporter->ReportContext(context)) {
    for (internal::Benchmark::Instance& benchmark : benchmarks) {
      //std::unique_ptr<thread::ThreadPool> pool;
      //if (benchmark.threads > 0) {
      //  pool = new thread::ThreadPool(benchmark.threads);
      //  pool->StartWorkers();
      //}
      Benchmark::RunInstance(/*pool, */benchmark, reporter);
    }
  }
}

void FindMatchingBenchmarkNames(const std::string& spec,
                                std::vector<std::string>* benchmark_names) {
  if (spec.empty()) return;

  std::vector<internal::Benchmark::Instance> benchmarks;
  internal::Benchmark::FindBenchmarks(spec, &benchmarks);
  std::transform(benchmarks.begin(), benchmarks.end(), benchmark_names->begin(),
                 [] (const internal::Benchmark::Instance& b) { return b.name; } );
}

}  // end namespace internal

void RunSpecifiedBenchmarks() {
  std::string spec = FLAGS_benchmark_filter;
  if (spec.empty() || spec == "all")
    spec = ".";         // Regexp that matches all benchmarks
  internal::ConsoleReporter default_reporter;
  internal::RunMatchingBenchmarks(spec, &default_reporter);
}

void Initialize(int* argc, const char** argv) {
  //AtomicOps_Internalx86CPUFeaturesInit();
  pthread_mutex_init(&benchmark_mutex, nullptr);
  pthread_key_create(&thread_stats_key, DeleteThreadStats);
  thread_stats = new internal::Benchmark::ThreadStats();
  walltime::Initialize();
  internal::ParseCommandLineFlags(argc, argv); 
  internal::Benchmark::MeasureOverhead();
}

}  // end namespace benchmark
