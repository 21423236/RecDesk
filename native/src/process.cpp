#include "encoder_process.h"

#include <array>
#include <charconv>
#include <fstream>

namespace recdesk {

Process::~Process() { terminate(); finish(); }

void Process::start(const std::filesystem::path& executable, const std::vector<std::wstring>& args,
  const std::filesystem::path& logFile) {
  if (process_) throw std::runtime_error("编码进程已启动");
  SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE readInput{}, writeInput{}, readOutput{}, writeOutput{};
  if (!CreatePipe(&readInput, &writeInput, &security, 0)) throw std::runtime_error("无法创建控制管道");
  Handle childInput(readInput);
  input_.reset(writeInput);
  if (!CreatePipe(&readOutput, &writeOutput, &security, 0)) throw std::runtime_error("无法创建日志管道");
  output_.reset(readOutput);
  Handle childOutput(writeOutput);
  if (!SetHandleInformation(input_.get(), HANDLE_FLAG_INHERIT, 0) ||
      !SetHandleInformation(output_.get(), HANDLE_FLAG_INHERIT, 0)) throw std::runtime_error("无法配置进程管道");

  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = childInput.get();
  startup.StartupInfo.hStdOutput = childOutput.get();
  startup.StartupInfo.hStdError = childOutput.get();
  SIZE_T attributeSize = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
  std::vector<unsigned char> attributes(attributeSize);
  startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
  if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributeSize))
    throw std::runtime_error("无法初始化子进程属性");
  HANDLE handles[] = {childInput.get(), childOutput.get()};
  BOOL updated = UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
    handles, sizeof(handles), nullptr, nullptr);
  job_.reset(CreateJobObjectW(nullptr, nullptr));
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!job_ || !SetInformationJobObject(job_.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    throw std::runtime_error("无法创建编码进程作业");
  }
  PROCESS_INFORMATION process{};
  auto command = commandLine(executable, args);
  BOOL created = updated && CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
    CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
    &startup.StartupInfo, &process);
  DWORD error = GetLastError();
  DeleteProcThreadAttributeList(startup.lpAttributeList);
  if (!created) throw std::runtime_error("无法启动 FFmpeg: " + utf8(errorMessage(error)));
  process_.reset(process.hProcess);
  Handle thread(process.hThread);
  if (!AssignProcessToJobObject(job_.get(), process_.get())) {
    TerminateProcess(process_.get(), 1);
    throw std::runtime_error("无法将编码进程加入作业");
  }
  childInput.reset();
  childOutput.reset();
  lastProgress_ = clock100ns();
  lastFrameProgress_ = lastProgress_.load();
  reader_ = std::thread([this, logFile] { this->readOutput(logFile); });
  if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
    terminate();
    throw std::runtime_error("无法运行编码进程");
  }
}

bool Process::running() const { return process_ && WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT; }
bool Process::wait(DWORD milliseconds) const { return !process_ || WaitForSingleObject(process_.get(), milliseconds) == WAIT_OBJECT_0; }
DWORD Process::exitCode() const {
  DWORD result = 1;
  if (process_) GetExitCodeProcess(process_.get(), &result);
  return result;
}
void Process::requestStop() {
  if (input_ && running()) { DWORD written = 0; WriteFile(input_.get(), "q\n", 2, &written, nullptr); }
}
void Process::terminate() { if (running()) { TerminateProcess(process_.get(), 1); wait(5000); } }
void Process::finish() { if (reader_.joinable()) reader_.join(); }
std::wstring Process::diagnostic() const { std::lock_guard lock(mutex_); return wide(tail_); }

void Process::readOutput(const std::filesystem::path& logFile) {
  std::ofstream log(logFile, std::ios::binary | std::ios::app);
  std::array<char, 4096> buffer{};
  std::string pending;
  DWORD count = 0;
  std::uint64_t loggedBytes = 0;
  while (ReadFile(output_.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) && count) {
    // Progress arrives indefinitely; cap the on-disk diagnostic log at 8 MiB.
    if (loggedBytes < 8 * 1024 * 1024) { log.write(buffer.data(), count); log.flush(); loggedBytes += count; }
    {
      std::lock_guard lock(mutex_);
      tail_.append(buffer.data(), count);
      if (tail_.size() > 16384) tail_.erase(0, tail_.size() - 16384);
    }
    pending.append(buffer.data(), count);
    size_t end = 0;
    while ((end = pending.find('\n')) != std::string::npos) {
      auto line = pending.substr(0, end);
      pending.erase(0, end + 1);
      auto equal = line.find('=');
      if (equal == std::string::npos) continue;
      std::int64_t value = 0;
      auto begin = line.data() + equal + 1;
      auto parsed = std::from_chars(begin, line.data() + line.size(), value);
      if (parsed.ec != std::errc{}) continue;
      if (line.substr(0, equal) == "out_time_us") {
        if (value > mediaTimeUs_.load()) lastProgress_ = clock100ns();
        mediaTimeUs_ = value;
      }
      if (line.substr(0, equal) == "frame") {
        if (value > static_cast<std::int64_t>(frames_.load())) {
          lastProgress_ = clock100ns();
          lastFrameProgress_ = lastProgress_.load();
        }
        frames_ = static_cast<std::uint64_t>(value);
      }
    }
    if (pending.size() > 65536) pending.clear();
  }
}

}
