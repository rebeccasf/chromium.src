// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/test/chromedriver/chrome_launcher.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/bind.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_file.h"
#include "base/format_macros.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/process/kill.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/sys_string_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/common/chrome_constants.h"
#include "chrome/common/chrome_result_codes.h"
#include "chrome/common/chrome_version.h"
#include "chrome/test/chromedriver/chrome/chrome_android_impl.h"
#include "chrome/test/chromedriver/chrome/chrome_desktop_impl.h"
#include "chrome/test/chromedriver/chrome/chrome_finder.h"
#include "chrome/test/chromedriver/chrome/chrome_remote_impl.h"
#include "chrome/test/chromedriver/chrome/device_manager.h"
#include "chrome/test/chromedriver/chrome/devtools_client_impl.h"
#include "chrome/test/chromedriver/chrome/devtools_event_listener.h"
#include "chrome/test/chromedriver/chrome/devtools_http_client.h"
#include "chrome/test/chromedriver/chrome/status.h"
#include "chrome/test/chromedriver/chrome/user_data_dir.h"
#include "chrome/test/chromedriver/chrome/web_view.h"
#include "chrome/test/chromedriver/constants/version.h"
#include "chrome/test/chromedriver/log_replay/chrome_replay_impl.h"
#include "chrome/test/chromedriver/log_replay/replay_http_client.h"
#include "chrome/test/chromedriver/net/net_util.h"
#include "components/crx_file/crx_verifier.h"
#include "components/embedder_support/switches.h"
#include "crypto/rsa_private_key.h"
#include "crypto/sha2.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "third_party/zlib/google/zip.h"
#include "url/gurl.h"

#if BUILDFLAG(IS_POSIX)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#elif BUILDFLAG(IS_WIN)
#include "chrome/test/chromedriver/keycode_text_conversion.h"

#include <windows.h>
#endif

#include "base/strings/string_number_conversions.h"

namespace {

const char* const kCommonSwitches[] = {
    embedder_support::kDisablePopupBlocking,
    "enable-automation",
    "allow-pre-commit-input",
};

#if 0
const char* const kDesktopSwitches[] = {
    "disable-hang-monitor",
    "disable-prompt-on-repost",
    "disable-sync",
    "no-first-run",
    "disable-background-networking",
    "disable-client-side-phishing-detection",
    "disable-default-apps",
    "log-level=0",
    "password-store=basic",
    "use-mock-keychain",
    "test-type=webdriver",
    // TODO(yoichio): This is temporary switch to support chrome internal
    // components migration from the old web APIs.
    // After completion of the migration, we should remove this.
    // See crbug.com/911943 for detail.
    "enable-blink-features=ShadowDOMV0",
    "no-service-autorun",
};
#endif

#if BUILDFLAG(IS_WIN)

const char* const kWindowsDesktopSwitches[] = {
    "disable-backgrounding-occluded-windows",
};

#endif

const char* const kAndroidSwitches[] = {
    "disable-fre", "enable-remote-debugging",
};

const char kEnableCrashReport[] = "enable-crash-reporter-for-testing";
const base::FilePath::CharType kDevToolsActivePort[] =
    FILE_PATH_LITERAL("DevToolsActivePort");

enum ChromeType { Remote, Desktop, Android, Replay };

#if BUILDFLAG(IS_POSIX)
// The values for kReadFD and kWriteFD come from
// content/browser/devtools/devtools_pipe_handler.cc
const int kReadFD = 3;
const int kWriteFD = 4;
#endif

Status PrepareDesktopCommandLine(const Capabilities& capabilities,
                                 bool enable_chrome_logs,
                                 base::CommandLine* prepared_command,
                                 base::ScopedTempDir* user_data_dir_temp_dir,
                                 base::ScopedTempDir* extension_dir,
                                 std::vector<std::string>* extension_bg_pages,
                                 base::FilePath* user_data_dir) {
  base::FilePath program = capabilities.binary;
  if (program.empty()) {
    if (!FindChrome(&program))
      return Status(kUnknownError, base::StringPrintf("cannot find %s binary",
                                                      kBrowserShortName));
  } else if (!base::PathExists(program)) {
    return Status(
        kUnknownError,
        base::StringPrintf("no %s binary at %" PRFilePath,
                           base::ToLowerASCII(kBrowserShortName).c_str(),
                           program.value().c_str()));
  }
  base::CommandLine command(program);
  Switches switches;

  for (auto* common_switch : kCommonSwitches)
    switches.SetUnparsedSwitch(common_switch);
#if 0 //FIXME if enabled, chromedriver cannot find chrome on windows
  for (auto* desktop_switch : kDesktopSwitches)
    switches.SetUnparsedSwitch(desktop_switch);
#if BUILDFLAG(IS_WIN)
  for (auto* win_desktop_switch : kWindowsDesktopSwitches)
    switches.SetUnparsedSwitch(win_desktop_switch);
#endif
#endif

  // Chrome logs are normally sent to a file (whose location can be controlled
  // via the logPath capability). We expose a flag, --enable-chrome-logs, that
  // sends the logs to stderr instead. This is useful when ChromeDriver was
  // launched by another program that wishes to consume the browser output.
  if (enable_chrome_logs) {
    if (!capabilities.log_path.empty()) {
      LOG(WARNING) << "The 'logPath' capability has no effect when using"
                   << "--enable-chrome-logs; Chrome logs will go to stderr";
    }
    switches.SetSwitch("enable-logging", "stderr");
  } else {
    // An empty argument to Chrome's enable-logging flag causes logs to go to a
    // file on Release builds (where the default is LOG_TO_FILE), and to both a
    // file and stderr on Debug builds (where the default is LOG_TO_ALL).
    switches.SetSwitch("enable-logging");
  }

  if (capabilities.accept_insecure_certs) {
    switches.SetSwitch("ignore-certificate-errors");
  }
  for (const auto& excluded_switch : capabilities.exclude_switches) {
    switches.RemoveSwitch(excluded_switch);
  }
  switches.SetFromSwitches(capabilities.switches);
  if (!switches.HasSwitch("remote-debugging-port") &&
      !switches.HasSwitch("remote-debugging-pipe")) {
    switches.SetSwitch("remote-debugging-port", "0");
  }
  if (capabilities.exclude_switches.count("user-data-dir") > 0) {
    LOG(WARNING) << "excluding user-data-dir switch is not supported";
  }
  if (capabilities.exclude_switches.count("remote-debugging-port") > 0) {
    LOG(WARNING) << "excluding remote-debugging-port switch is not supported";
  }
  if (switches.HasSwitch("user-data-dir")) {
    base::FilePath::StringType userDataDir =
      switches.GetSwitchValueNative("user-data-dir");
    if (userDataDir.empty())
      return Status(kInvalidArgument, "user data dir can not be empty");
    *user_data_dir = base::FilePath(userDataDir);
  } else {
    //command.AppendArg("data:,");
    if (!user_data_dir_temp_dir->CreateUniqueTempDir())
      return Status(kUnknownError, "cannot create temp dir for user data dir");
    switches.SetSwitch("user-data-dir",
                       user_data_dir_temp_dir->GetPath().AsUTF8Unsafe());
    *user_data_dir = user_data_dir_temp_dir->GetPath();
  }

  Status status = internal::PrepareUserDataDir(
      *user_data_dir, capabilities.prefs.get(), capabilities.local_state.get());
  if (status.IsError())
    return status;

  if (capabilities.exclude_switches.count("load-extension") > 0) {
    if (capabilities.extensions.size() > 0)
      return Status(
          kUnknownError,
          "cannot exclude load-extension switch when extensions are specified");
  } else {
    if (!extension_dir->CreateUniqueTempDir()) {
      return Status(kUnknownError,
                    "cannot create temp dir for unpacking extensions");
    }
    status = internal::ProcessExtensions(capabilities.extensions,
                                         extension_dir->GetPath(), &switches,
                                         extension_bg_pages);
    if (status.IsError())
      return status;
  }
  switches.AppendToCommandLine(&command);

  for (size_t i = 0; i < capabilities.arguments.size(); i++)
    command.AppendArg(capabilities.arguments[i]);

  *prepared_command = command;
  return Status(kOk);
}

Status WaitForDevToolsAndCheckVersion(
    const DevToolsEndpoint& endpoint,
    network::mojom::URLLoaderFactory* factory,
    const SyncWebSocketFactory& socket_factory,
    const Capabilities* capabilities,
    int wait_time,
    std::unique_ptr<DevToolsHttpClient>* user_client,
    bool* retry,
    ChromeType ct,
    std::string fp = "") {
  std::unique_ptr<DeviceMetrics> device_metrics;
  if (capabilities && capabilities->device_metrics) {
    device_metrics =
        std::make_unique<DeviceMetrics>(*capabilities->device_metrics);
  }

  std::unique_ptr<std::set<WebViewInfo::Type>> window_types;
  if (capabilities && !capabilities->window_types.empty()) {
    window_types = std::make_unique<std::set<WebViewInfo::Type>>(
        capabilities->window_types);
  } else {
    window_types = std::make_unique<std::set<WebViewInfo::Type>>();
  }

  base::CommandLine* cmd_line = base::CommandLine::ForCurrentProcess();
  if (cmd_line->HasSwitch("launch-timeout")) {
    std::string s_timeout = cmd_line->GetSwitchValueASCII("launch-timeout");
    base::StringToInt(s_timeout, &wait_time);
  }

  std::unique_ptr<DevToolsHttpClient> client;
  if (cmd_line->HasSwitch("devtools-replay")) {
    base::CommandLine::StringType log_path =
        cmd_line->GetSwitchValueNative("devtools-replay");
    base::FilePath log_file_path(log_path);
    client = std::make_unique<ReplayHttpClient>(
        endpoint, factory, socket_factory, std::move(device_metrics),
        std::move(window_types), capabilities->page_load_strategy,
        log_file_path);
  } else {
    client = std::make_unique<DevToolsHttpClient>(
        endpoint, factory, socket_factory, std::move(device_metrics),
        std::move(window_types), capabilities->page_load_strategy);
  }

  const base::TimeTicks initial = base::TimeTicks::Now();
  const base::TimeTicks deadline = initial + base::Seconds(wait_time);
  Status status = client->Init(deadline - initial);
  if (status.IsError())
    return status;

  const BrowserInfo* browser_info = client->browser_info();
  if (browser_info->is_android &&
    browser_info->android_package != capabilities->android_package) {
    return Status(
      kSessionNotCreated,
      base::StringPrintf("please close '%s' and try again",
                          browser_info->android_package.c_str()));
  }

  *retry = true;
  if (cmd_line->HasSwitch("disable-build-check")) {
    LOG(WARNING) << "You are using an unsupported command-line switch: "
                    "--disable-build-check. Please don't report bugs that "
                    "cannot be reproduced with this switch removed.";
  } else if (browser_info->major_version != CHROME_VERSION_MAJOR) {
    if (browser_info->major_version == 0) {
      // TODO(https://crbug.com/932013): Content Shell doesn't report a version
      // number. Skip version checking with a warning.
      LOG(WARNING) << "Unable to retrieve " << kBrowserShortName
                   << " version. Unable to verify browser compatibility.";
    } else if (browser_info->major_version == CHROME_VERSION_MAJOR + 1) {
      // TODO(https://crbug.com/chromedriver/2656): Since we don't currently
      // release ChromeDriver for dev or canary channels, allow using
      // ChromeDriver version n (e.g., Beta) with Chrome version n+1 (e.g., Dev
      // or Canary), with a warning.
      LOG(WARNING) << "This version of " << kChromeDriverProductFullName
                   << " has not been tested with " << kBrowserShortName
                   << " version " << browser_info->major_version << ".";
    } else {
      *retry = false;
      std::string version_info = base::StringPrintf(
          "This version of %s only supports %s version %d\nCurrent browser "
          "version is %s",
          kChromeDriverProductFullName, kBrowserShortName, CHROME_VERSION_MAJOR,
          browser_info->browser_version.c_str());
      if (ct == ChromeType::Desktop && !fp.empty())
        version_info.append(" with binary path " + fp);
      else if (ct == ChromeType::Android)
        version_info.append(" with package name " +
                            capabilities->android_package);
      return Status(kSessionNotCreated, version_info);
    }
  }

  // Always try GetWebViewsInfo at least once if the client
  // initialized successfully.
  do {
    WebViewsInfo views_info;
    status = client->GetWebViewsInfo(&views_info);
    if (status.IsError())
      return status;
    for (size_t i = 0; i < views_info.GetSize(); ++i) {
      if (views_info.Get(i).type == WebViewInfo::kPage ||
          views_info.Get(i).type == WebViewInfo::kApp
          || (views_info.Get(i).type == WebViewInfo::kOther &&
              !base::StartsWith(views_info.Get(i).url, "chrome-extension://", base::CompareCase::SENSITIVE))) { //node-remote page
        *user_client = std::move(client);
        return Status(kOk);
      }
    }
    base::PlatformThread::Sleep(base::Milliseconds(50));
  } while (base::TimeTicks::Now() < deadline);

  return Status(kUnknownError, "unable to discover open pages");
}

Status CreateBrowserwideDevToolsClientAndConnect(
    const DevToolsEndpoint& endpoint,
    const PerfLoggingPrefs& perf_logging_prefs,
    const SyncWebSocketFactory& socket_factory,
    const std::vector<std::unique_ptr<DevToolsEventListener>>&
        devtools_event_listeners,
    const std::string& web_socket_url,
    std::unique_ptr<DevToolsClient>* browser_client) {
  std::string url(web_socket_url);
  if (url.length() == 0) {
    url = endpoint.GetBrowserDebuggerUrl();
  }
  std::unique_ptr<DevToolsClient> client(new DevToolsClientImpl(
      socket_factory, url, DevToolsClientImpl::kBrowserwideDevToolsClientId));
  for (const auto& listener : devtools_event_listeners) {
    // Only add listeners that subscribe to the browser-wide |DevToolsClient|.
    // Otherwise, listeners will think this client is associated with a webview,
    // and will send unrecognized commands to it.
    if (listener->subscribes_to_browser())
      client->AddListener(listener.get());
  }
  // Provide the client regardless of whether it connects, so that Chrome always
  // has a valid |devtools_websocket_client_|. If not connected, no listeners
  // will be notified, and client will just return kDisconnected errors if used.
  *browser_client = std::move(client);
  // To avoid unnecessary overhead, only connect if tracing is enabled, since
  // the browser-wide client is currently only used for tracing.
  if (!perf_logging_prefs.trace_categories.empty()) {
    Status status = (*browser_client)->ConnectIfNecessary();
    if (status.IsError())
      return status;
  }
  return Status(kOk);
}

Status LaunchRemoteChromeSession(
    network::mojom::URLLoaderFactory* factory,
    const SyncWebSocketFactory& socket_factory,
    const Capabilities& capabilities,
    std::vector<std::unique_ptr<DevToolsEventListener>>
        devtools_event_listeners,
    std::unique_ptr<Chrome>* chrome) {
  Status status(kOk);
  std::unique_ptr<DevToolsHttpClient> devtools_http_client;
  bool retry = true;
  status = WaitForDevToolsAndCheckVersion(
      DevToolsEndpoint(capabilities.debugger_address), factory, socket_factory,
      &capabilities, 60, &devtools_http_client, &retry, ChromeType::Remote);
  if (status.IsError()) {
    return Status(
        kUnknownError,
        base::StringPrintf("cannot connect to %s at %s",
                           base::ToLowerASCII(kBrowserShortName).c_str(),
                           capabilities.debugger_address.ToString().c_str()),
        status);
  }

  std::unique_ptr<DevToolsClient> devtools_websocket_client;
  status = CreateBrowserwideDevToolsClientAndConnect(
      DevToolsEndpoint(capabilities.debugger_address),
      capabilities.perf_logging_prefs, socket_factory, devtools_event_listeners,
      devtools_http_client->browser_info()->web_socket_url,
      &devtools_websocket_client);
  if (status.IsError()) {
    LOG(WARNING) << "Browser-wide DevTools client failed to connect: "
                 << status.message();
  }

  *chrome = std::make_unique<ChromeRemoteImpl>(
      std::move(devtools_http_client), std::move(devtools_websocket_client),
      std::move(devtools_event_listeners), capabilities.page_load_strategy);
  return Status(kOk);
}

#if BUILDFLAG(IS_POSIX)
Status PipeSetUp(base::LaunchOptions* options, int* write_fd, int* read_fd) {
  int chrome_to_driver_pipe_fds[2];
  int driver_to_chrome_pipe_fds[2];

  if (pipe(chrome_to_driver_pipe_fds) == -1 ||
      pipe(driver_to_chrome_pipe_fds) == -1)
    return Status(kUnknownError, "cannot set up pipe");

  options->fds_to_remap.emplace_back(driver_to_chrome_pipe_fds[0], kReadFD);
  options->fds_to_remap.emplace_back(chrome_to_driver_pipe_fds[1], kWriteFD);

  close(driver_to_chrome_pipe_fds[0]);
  close(chrome_to_driver_pipe_fds[1]);

  *write_fd = driver_to_chrome_pipe_fds[1];
  *read_fd = chrome_to_driver_pipe_fds[0];

  return Status(kOk);
}
#endif

Status LaunchDesktopChrome(network::mojom::URLLoaderFactory* factory,
                           const SyncWebSocketFactory& socket_factory,
                           const Capabilities& capabilities,
                           std::vector<std::unique_ptr<DevToolsEventListener>>
                               devtools_event_listeners,
                           std::unique_ptr<Chrome>* chrome,
                           bool w3c_compliant) {
  base::CommandLine command(base::CommandLine::NO_PROGRAM);
  base::ScopedTempDir user_data_dir_temp_dir;
  base::FilePath user_data_dir;
  base::ScopedTempDir extension_dir;
  Status status = Status(kOk);
  std::vector<std::string> extension_bg_pages;
  int devtools_port = 0;
  bool retry = true;

  if (capabilities.switches.HasSwitch("remote-debugging-port")) {
    std::string port_switch =
        capabilities.switches.GetSwitchValue("remote-debugging-port");
    bool conversion_result = base::StringToInt(port_switch, &devtools_port);
    if (!conversion_result || devtools_port < 0 || 65535 < devtools_port) {
      return Status(
          kUnknownError,
          "remote-debugging-port flag has invalid value: " + port_switch);
    }
  }

  if (!devtools_port && capabilities.switches.HasSwitch("user-data-dir")) {
    status = internal::RemoveOldDevToolsActivePortFile(base::FilePath(
        capabilities.switches.GetSwitchValueNative("user-data-dir")));
    if (status.IsError()) {
      return status;
    }
  }
  const base::CommandLine* cmd_line = base::CommandLine::ForCurrentProcess();
  bool enable_chrome_logs = cmd_line->HasSwitch("enable-chrome-logs");
  status = PrepareDesktopCommandLine(capabilities, enable_chrome_logs, &command,
                                     &user_data_dir_temp_dir, &extension_dir,
                                     &extension_bg_pages, &user_data_dir);
  if (status.IsError())
    return status;

  base::LaunchOptions options;

#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_FUCHSIA)
  // If minidump path is set in the capability, enable minidump for crashes.
  if (!capabilities.minidump_path.empty()) {
    VLOG(0) << "Minidump generation specified. Will save dumps to: "
            << capabilities.minidump_path;

#if BUILDFLAG(IS_WIN)
    // EnvironmentMap uses wide string
    options.environment[L"CHROME_HEADLESS"] = L"1";
    options.environment[L"BREAKPAD_DUMP_LOCATION"] =
        base::SysUTF8ToWide(capabilities.minidump_path);
#else
    options.environment["CHROME_HEADLESS"] = "1";
    options.environment["BREAKPAD_DUMP_LOCATION"] = capabilities.minidump_path;
#endif

    if (!command.HasSwitch(kEnableCrashReport))
      command.AppendSwitch(kEnableCrashReport);
  }

#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
  // We need to allow new privileges so that chrome's setuid sandbox can run.
  options.allow_new_privs = true;
#endif
#endif  // BUILDFLAG(IS_WIN) || BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_FUCHSIA)

#if !BUILDFLAG(IS_WIN)
  if (!capabilities.log_path.empty())
    options.environment["CHROME_LOG_FILE"] = capabilities.log_path;
  if (capabilities.detach)
    options.new_process_group = true;
#endif

#if BUILDFLAG(IS_POSIX)

  int write_fd;
  int read_fd;

  if (capabilities.switches.HasSwitch("remote-debugging-pipe")) {
    Status status = PipeSetUp(&options, &write_fd, &read_fd);
  }

  base::ScopedFD devnull;
  if (!cmd_line->HasSwitch("verbose") && !enable_chrome_logs &&
      cmd_line->GetSwitchValueASCII("log-level") != "ALL") {
    // On Debug builds of Chrome, the default logging outputs to both a file and
    // stderr. Redirect stderr to /dev/null, so that Chrome log spew doesn't
    // confuse users.
    devnull.reset(HANDLE_EINTR(open("/dev/null", O_WRONLY)));
    if (!devnull.is_valid())
      return Status(kUnknownError, "couldn't open /dev/null");
    options.fds_to_remap.push_back(
        std::make_pair(devnull.get(), STDERR_FILENO));
  }
#elif BUILDFLAG(IS_WIN)
  if (enable_chrome_logs) {
    // On Windows, we must inherit the stdout/stderr handles, or the output from
    // the browser will not be part of our output and thus not capturable by
    // processes that call us.
    options.stdin_handle = INVALID_HANDLE_VALUE;
    options.stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    options.stderr_handle = GetStdHandle(STD_ERROR_HANDLE);
    options.handles_to_inherit.push_back(options.stdout_handle);
    options.handles_to_inherit.push_back(options.stderr_handle);
  }

  if (!SwitchToUSKeyboardLayout())
    VLOG(0) << "Cannot switch to US keyboard layout - some keys may be "
        "interpreted incorrectly";
#endif

#if BUILDFLAG(IS_MAC)
  // Chrome is a third party process with respect to ChromeDriver. This allows
  // Chrome to get its own permissions attributed on Mac instead of relying on
  // ChromeDriver.
  options.disclaim_responsibility = true;
#endif  // BUILDFLAG(IS_MAC)

#if BUILDFLAG(IS_WIN)
  std::string command_string = base::WideToUTF8(command.GetCommandLineString());
#else
  std::string command_string = command.GetCommandLineString();
#endif
  VLOG(0) << "Launching " << base::ToLowerASCII(kBrowserShortName) << ": "
          << command_string;
  base::Process process = base::LaunchProcess(command, options);
  if (!process.IsValid())
    return Status(
        kUnknownError,
        base::StringPrintf("Failed to create %s process.", kBrowserShortName));

  // Attempt to connect to devtools in order to send commands to Chrome. If
  // attempts fail, check if Chrome has crashed and return error.
  std::unique_ptr<DevToolsHttpClient> devtools_http_client;
  int exit_code;
  base::TerminationStatus chrome_status =
      base::TERMINATION_STATUS_STILL_RUNNING;
  base::TimeTicks deadline = base::TimeTicks::Now() + base::Seconds(60);
  while (base::TimeTicks::Now() < deadline) {
    if (!devtools_port) {
      status =
          internal::ParseDevToolsActivePortFile(user_data_dir, &devtools_port);
    } else {
      status = Status(kOk);
    }
    if (status.IsOk()) {
      // std::ostringstream is used in case to convert Windows wide string to
      // string
      std::ostringstream oss;
      oss << command.GetProgram();
      status = WaitForDevToolsAndCheckVersion(
          DevToolsEndpoint(devtools_port), factory, socket_factory,
          &capabilities, 1, &devtools_http_client, &retry, ChromeType::Desktop,
          oss.str());
      if (!retry) {
        break;
      }
    }
    if (status.IsOk()) {
      break;
    }
    // Check to see if Chrome has crashed.
    chrome_status = base::GetTerminationStatus(process.Handle(), &exit_code);
    if (chrome_status != base::TERMINATION_STATUS_STILL_RUNNING) {
#if BUILDFLAG(IS_WIN)
      if (exit_code == chrome::RESULT_CODE_NORMAL_EXIT_PROCESS_NOTIFIED)
#else
      if (WEXITSTATUS(exit_code) ==
          chrome::RESULT_CODE_NORMAL_EXIT_PROCESS_NOTIFIED)
#endif
        return Status(kInvalidArgument,
                      "user data directory is already in use, "
                      "please specify a unique value for --user-data-dir "
                      "argument, or don't use --user-data-dir");
      std::string termination_reason =
          internal::GetTerminationReason(chrome_status);
      Status failure_status =
          Status(kUnknownError, base::StringPrintf("%s failed to start: %s.",
                                                   kBrowserShortName,
                                                   termination_reason.c_str()));
      failure_status.AddDetails(status.message());
      // There is a use case of someone passing a path to a binary to us in
      // capabilities that is not an actual Chrome binary but a script that
      // intercepts our arguments and then starts Chrome itself. This method
      // of starting Chrome should be done carefully. The right way to do it
      // is to do an exec of Chrome at the end of the script so that Chrome
      // remains a subprocess of ChromeDriver. This allows us to have the
      // correct process handle so that we can terminate Chrome after the
      // test has finished or in the case of any failure. If you can't exec
      // the Chrome binary at the end of your script, you must find a way to
      // properly handle our termination signal so that you don't have zombie
      // Chrome processes running after the test is completed.
      failure_status.AddDetails(base::StringPrintf(
          "The process started from %s location %s is no longer running, "
          "so %s is assuming that %s has crashed.",
          base::ToLowerASCII(kBrowserShortName).c_str(),
          command.GetProgram().AsUTF8Unsafe().c_str(),
          kChromeDriverProductShortName, kBrowserShortName));
      return failure_status;
    }
    base::PlatformThread::Sleep(base::Milliseconds(50));
  }

  if (status.IsError()) {
    VLOG(0) << "Failed to connect to " << kBrowserShortName
            << ". Attempting to kill it.";
    if (!process.Terminate(0, true)) {
      if (base::GetTerminationStatus(process.Handle(), &exit_code) ==
          base::TERMINATION_STATUS_STILL_RUNNING)
        return Status(kUnknownError,
                      base::StringPrintf("cannot kill %s", kBrowserShortName),
                      status);
    }
    return status;
  }

  std::unique_ptr<DevToolsClient> devtools_websocket_client;
  status = CreateBrowserwideDevToolsClientAndConnect(
      DevToolsEndpoint(devtools_port), capabilities.perf_logging_prefs,
      socket_factory, devtools_event_listeners,
      devtools_http_client->browser_info()->web_socket_url,
      &devtools_websocket_client);
  if (status.IsError()) {
    LOG(WARNING) << "Browser-wide DevTools client failed to connect: "
                 << status.message();
  }

  std::unique_ptr<ChromeDesktopImpl> chrome_desktop =
      std::make_unique<ChromeDesktopImpl>(
          std::move(devtools_http_client), std::move(devtools_websocket_client),
          std::move(devtools_event_listeners), capabilities.page_load_strategy,
          std::move(process), command, &user_data_dir_temp_dir, &extension_dir,
          capabilities.network_emulation_enabled);
  if (!capabilities.extension_load_timeout.is_zero()) {
    for (size_t i = 0; i < extension_bg_pages.size(); ++i) {
      VLOG(0) << "Waiting for extension bg page load: "
              << extension_bg_pages[i];
      std::unique_ptr<WebView> web_view;
      status = chrome_desktop->WaitForPageToLoad(
          extension_bg_pages[i], capabilities.extension_load_timeout, &web_view,
          w3c_compliant);
      if (status.IsError()) {
        return Status(kUnknownError,
                      "failed to wait for extension background page to load: " +
                          extension_bg_pages[i],
                      status);
      }
    }
  }
  *chrome = std::move(chrome_desktop);
  return Status(kOk);
}

Status LaunchAndroidChrome(network::mojom::URLLoaderFactory* factory,
                           const SyncWebSocketFactory& socket_factory,
                           const Capabilities& capabilities,
                           std::vector<std::unique_ptr<DevToolsEventListener>>
                               devtools_event_listeners,
                           DeviceManager* device_manager,
                           std::unique_ptr<Chrome>* chrome) {
  Status status(kOk);
  std::unique_ptr<Device> device;
  int devtools_port = capabilities.android_devtools_port;
  if (capabilities.android_device_serial.empty()) {
    status = device_manager->AcquireDevice(&device);
  } else {
    status = device_manager->AcquireSpecificDevice(
        capabilities.android_device_serial, &device);
  }
  if (status.IsError())
    return status;

  Switches switches(capabilities.switches);
  for (auto* common_switch : kCommonSwitches)
    switches.SetUnparsedSwitch(common_switch);
  for (auto* android_switch : kAndroidSwitches)
    switches.SetUnparsedSwitch(android_switch);
  if (capabilities.accept_insecure_certs) {
    switches.SetSwitch("ignore-certificate-errors");
  }
  for (auto excluded_switch : capabilities.exclude_switches)
    switches.RemoveSwitch(excluded_switch);
  status = device->SetUp(
      capabilities.android_package, capabilities.android_activity,
      capabilities.android_process, capabilities.android_device_socket,
      capabilities.android_exec_name, switches.ToString(),
      capabilities.android_use_running_app,
      capabilities.android_keep_app_data_dir, &devtools_port);
  if (status.IsError()) {
    device->TearDown();
    return status;
  }

  std::unique_ptr<DevToolsHttpClient> devtools_http_client;
  bool retry = true;
  status = WaitForDevToolsAndCheckVersion(
      DevToolsEndpoint(devtools_port), factory, socket_factory, &capabilities,
      60, &devtools_http_client, &retry, ChromeType::Android);
  if (status.IsError()) {
    device->TearDown();
    return status;
  }

  std::unique_ptr<DevToolsClient> devtools_websocket_client;
  status = CreateBrowserwideDevToolsClientAndConnect(
      DevToolsEndpoint(devtools_port), capabilities.perf_logging_prefs,
      socket_factory, devtools_event_listeners,
      devtools_http_client->browser_info()->web_socket_url,
      &devtools_websocket_client);
  if (status.IsError()) {
    LOG(WARNING) << "Browser-wide DevTools client failed to connect: "
                 << status.message();
  }

  *chrome = std::make_unique<ChromeAndroidImpl>(
      std::move(devtools_http_client), std::move(devtools_websocket_client),
      std::move(devtools_event_listeners), capabilities.page_load_strategy,
      std::move(device));
  return Status(kOk);
}

Status LaunchReplayChrome(network::mojom::URLLoaderFactory* factory,
                          const SyncWebSocketFactory& socket_factory,
                          const Capabilities& capabilities,
                          std::vector<std::unique_ptr<DevToolsEventListener>>
                              devtools_event_listeners,
                          std::unique_ptr<Chrome>* chrome,
                          bool w3c_compliant) {
  base::CommandLine command(base::CommandLine::NO_PROGRAM);
  base::ScopedTempDir user_data_dir_temp_dir;
  base::ScopedTempDir extension_dir;
  Status status = Status(kOk);
  std::vector<std::string> extension_bg_pages;

  if (capabilities.switches.HasSwitch("user-data-dir")) {
    status = internal::RemoveOldDevToolsActivePortFile(base::FilePath(
        capabilities.switches.GetSwitchValueNative("user-data-dir")));
    if (status.IsError()) {
      return status;
    }
  }

#if BUILDFLAG(IS_WIN)
  if (!SwitchToUSKeyboardLayout())
    VLOG(0) << "Cannot switch to US keyboard layout - some keys may be "
               "interpreted incorrectly";
#endif

  std::unique_ptr<DevToolsHttpClient> devtools_http_client;
  bool retry = true;
  status = WaitForDevToolsAndCheckVersion(
      DevToolsEndpoint(0), factory, socket_factory, &capabilities, 1,
      &devtools_http_client, &retry, ChromeType::Replay);
  if (status.IsError())
    return status;
  std::unique_ptr<DevToolsClient> devtools_websocket_client;
  status = CreateBrowserwideDevToolsClientAndConnect(
      DevToolsEndpoint(0), capabilities.perf_logging_prefs, socket_factory,
      devtools_event_listeners,
      devtools_http_client->browser_info()->web_socket_url,
      &devtools_websocket_client);
  if (status.IsError()) {
    LOG(WARNING) << "Browser-wide DevTools client failed to connect: "
                 << status.message();
  }
  base::Process dummy_process;
  std::unique_ptr<ChromeDesktopImpl> chrome_impl =
      std::make_unique<ChromeReplayImpl>(
          std::move(devtools_http_client), std::move(devtools_websocket_client),
          std::move(devtools_event_listeners), capabilities.page_load_strategy,
          std::move(dummy_process), command, &user_data_dir_temp_dir,
          &extension_dir, capabilities.network_emulation_enabled);

  if (!capabilities.extension_load_timeout.is_zero()) {
    for (size_t i = 0; i < extension_bg_pages.size(); ++i) {
      VLOG(0) << "Waiting for extension bg page load: "
              << extension_bg_pages[i];
      std::unique_ptr<WebView> web_view;
      status = chrome_impl->WaitForPageToLoad(
          extension_bg_pages[i], capabilities.extension_load_timeout, &web_view,
          w3c_compliant);
      if (status.IsError()) {
        return Status(kUnknownError,
                      "failed to wait for extension background page to load: " +
                          extension_bg_pages[i],
                      status);
      }
    }
  }
  *chrome = std::move(chrome_impl);
  return Status(kOk);
}

}  // namespace

Status LaunchChrome(network::mojom::URLLoaderFactory* factory,
                    const SyncWebSocketFactory& socket_factory,
                    DeviceManager* device_manager,
                    const Capabilities& capabilities,
                    std::vector<std::unique_ptr<DevToolsEventListener>>
                        devtools_event_listeners,
                    std::unique_ptr<Chrome>* chrome,
                    bool w3c_compliant) {
  if (capabilities.IsRemoteBrowser()) {
    // TODO(johnchen): Clean up naming for ChromeDriver sessions created
    // by connecting to an already-running Chrome at a given debuggerAddress.
    return LaunchRemoteChromeSession(factory, socket_factory, capabilities,
                                     std::move(devtools_event_listeners),
                                     chrome);
  }
  const base::CommandLine* cmd_line = base::CommandLine::ForCurrentProcess();
  if (capabilities.IsAndroid()) {
    return LaunchAndroidChrome(factory, socket_factory, capabilities,
                               std::move(devtools_event_listeners),
                               device_manager, chrome);
  } else if (cmd_line->HasSwitch("devtools-replay")) {
    return LaunchReplayChrome(factory, socket_factory, capabilities,
                              std::move(devtools_event_listeners), chrome,
                              w3c_compliant);
  } else {
    return LaunchDesktopChrome(factory, socket_factory, capabilities,
                               std::move(devtools_event_listeners), chrome,
                               w3c_compliant);
  }
}

namespace internal {

void ConvertHexadecimalToIDAlphabet(std::string* id) {
  for (size_t i = 0; i < id->size(); ++i) {
    int val;
    if (base::HexStringToInt(
            base::MakeStringPiece(id->begin() + i, id->begin() + i + 1),
            &val)) {
      (*id)[i] = val + 'a';
    } else {
      (*id)[i] = 'a';
    }
  }
}

std::string GenerateExtensionId(const std::string& input) {
  uint8_t hash[16];
  crypto::SHA256HashString(input, hash, sizeof(hash));
  std::string output = base::ToLowerASCII(base::HexEncode(hash, sizeof(hash)));
  ConvertHexadecimalToIDAlphabet(&output);
  return output;
}

Status GetExtensionBackgroundPage(const base::DictionaryValue* manifest,
                                  const std::string& id,
                                  std::string* bg_page) {
  std::string bg_page_name;
  bool persistent =
      manifest->FindBoolPath("background.persistent").value_or(true);
  if (manifest->FindPath("background.scripts"))
    bg_page_name = "_generated_background_page.html";
  manifest->GetString("background.page", &bg_page_name);
  if (bg_page_name.empty() || !persistent)
    return Status(kOk);
  GURL baseUrl("chrome-extension://" + id + "/");
  *bg_page = baseUrl.Resolve(bg_page_name).spec();
  return Status(kOk);
}

Status ProcessExtension(const std::string& extension,
                        const base::FilePath& temp_dir,
                        base::FilePath* path,
                        std::string* bg_page) {
  // Decodes extension string.
  // Some WebDriver client base64 encoders follow RFC 1521, which require that
  // 'encoded lines be no more than 76 characters long'. Just remove any
  // newlines.
  std::string extension_base64;
  base::RemoveChars(extension, "\n", &extension_base64);
  std::string decoded_extension;
  if (!base::Base64Decode(extension_base64, &decoded_extension))
    return Status(kUnknownError, "cannot base64 decode");

  base::ScopedTempDir temp_crx_dir;
  if (!temp_crx_dir.CreateUniqueTempDir())
    return Status(kUnknownError, "cannot create temp dir");
  base::FilePath extension_crx = temp_crx_dir.GetPath().AppendASCII("temp.crx");
  int size = static_cast<int>(decoded_extension.length());
  if (base::WriteFile(extension_crx, decoded_extension.c_str(), size) != size) {
    return Status(kUnknownError, "cannot write file");
  }

  // If the file is a crx file, extract the extension's ID from its public key.
  // Otherwise generate a random public key and use its derived extension ID.
  std::string public_key_base64;
  std::string magic_header = decoded_extension.substr(0, 4);
  if (magic_header.size() != 4)
    return Status(kUnknownError, "cannot extract magic number");

  const bool is_crx_file = magic_header == "Cr24";
  std::string id;

  if (is_crx_file) {
    crx_file::VerifierResult result = crx_file::Verify(
        extension_crx, crx_file::VerifierFormat::CRX3,
        {} /** required_key_hashes */, {} /** required_file_hash */,
        &public_key_base64, &id, /*compressed_verified_contents=*/nullptr);
    if (result == crx_file::VerifierResult::ERROR_HEADER_INVALID) {
      return Status(kUnknownError,
                    "CRX verification failed to parse extension header. Chrome "
                    "supports only CRX3 format. Does the extension need to be "
                    "updated?");
    } else if (result != crx_file::VerifierResult::OK_FULL) {
      return Status(kUnknownError,
                    base::StringPrintf("CRX verification failed: %d", result));
    }
  } else {
    // Not a CRX file. Generate RSA keypair to get a valid extension id.
    std::unique_ptr<crypto::RSAPrivateKey> key_pair(
        crypto::RSAPrivateKey::Create(2048));
    if (!key_pair)
      return Status(kUnknownError, "cannot generate RSA key pair");
    std::vector<uint8_t> public_key_vector;
    if (!key_pair->ExportPublicKey(&public_key_vector))
      return Status(kUnknownError, "cannot extract public key");
    std::string public_key =
        std::string(reinterpret_cast<char*>(&public_key_vector.front()),
                    public_key_vector.size());
    id = GenerateExtensionId(public_key);
    base::Base64Encode(public_key, &public_key_base64);
  }

  // Unzip the crx file.
  base::FilePath extension_dir = temp_dir.AppendASCII("extension_" + id);
  if (!zip::Unzip(extension_crx, extension_dir))
    return Status(kUnknownError, "cannot unzip");

  // Parse the manifest and set the 'key' if not already present.
  base::FilePath manifest_path(extension_dir.AppendASCII("manifest.json"));
  std::string manifest_data;
  if (!base::ReadFileToString(manifest_path, &manifest_data))
    return Status(kUnknownError, "cannot read manifest");
  std::unique_ptr<base::Value> manifest_value =
      base::JSONReader::ReadDeprecated(manifest_data);
  base::DictionaryValue* manifest;
  if (!manifest_value || !manifest_value->GetAsDictionary(&manifest))
    return Status(kUnknownError, "invalid manifest");

  std::string manifest_key_base64;
  if (manifest->GetString("key", &manifest_key_base64)) {
    // If there is a key in both the header and the manifest, use the key in the
    // manifest. This allows chromedriver users users who generate dummy crxs
    // to set the manifest key and have a consistent ID.
    std::string manifest_key;
    if (!base::Base64Decode(manifest_key_base64, &manifest_key))
      return Status(kUnknownError, "'key' in manifest is not base64 encoded");
    std::string manifest_id = GenerateExtensionId(manifest_key);
    if (id != manifest_id) {
      if (is_crx_file) {
        LOG(WARNING)
            << "Public key in crx header is different from key in manifest"
            << std::endl << "key from header:   " << public_key_base64
            << std::endl << "key from manifest: " << manifest_key_base64
            << std::endl << "generated extension id from header key:   " << id
            << std::endl << "generated extension id from manifest key: "
            << manifest_id;
      }
      id = manifest_id;
    }
  } else {
    manifest->SetString("key", public_key_base64);
    base::JSONWriter::Write(*manifest, &manifest_data);
    if (base::WriteFile(
            manifest_path, manifest_data.c_str(), manifest_data.size()) !=
        static_cast<int>(manifest_data.size())) {
      return Status(kUnknownError, "cannot add 'key' to manifest");
    }
  }

  // Get extension's background page URL, if there is one.
  std::string bg_page_tmp;
  Status status = GetExtensionBackgroundPage(manifest, id, &bg_page_tmp);
  if (status.IsError())
    return status;

  *path = extension_dir;
  if (bg_page_tmp.size())
    *bg_page = bg_page_tmp;
  return Status(kOk);
}

void UpdateExtensionSwitch(Switches* switches,
                           const char name[],
                           const std::string& extension) {
  std::string value = switches->GetSwitchValue(name);
  if (value.length())
    value += ",";
  value += extension;
  switches->SetSwitch(name, value);
}

Status ProcessExtensions(const std::vector<std::string>& extensions,
                         const base::FilePath& temp_dir,
                         Switches* switches,
                         std::vector<std::string>* bg_pages) {
  std::vector<std::string> bg_pages_tmp;
  std::vector<std::string> extension_paths;
  for (size_t i = 0; i < extensions.size(); ++i) {
    base::FilePath path;
    std::string bg_page;
    Status status = ProcessExtension(extensions[i], temp_dir, &path, &bg_page);
    if (status.IsError()) {
      return Status(
          kSessionNotCreated,
          base::StringPrintf("cannot process extension #%" PRIuS, i + 1),
          status);
    }
    extension_paths.push_back(path.AsUTF8Unsafe());
    if (bg_page.length())
      bg_pages_tmp.push_back(bg_page);
  }

  if (extension_paths.size()) {
    std::string extension_paths_value = base::JoinString(extension_paths, ",");
    UpdateExtensionSwitch(switches, "load-extension", extension_paths_value);
  }
  bg_pages->swap(bg_pages_tmp);
  return Status(kOk);
}

Status WritePrefsFile(
    const std::string& template_string,
    const base::DictionaryValue* custom_prefs,
    const base::FilePath& path) {
  base::JSONReader::ValueWithError parsed_json =
      base::JSONReader::ReadAndReturnValueWithError(template_string);
  base::DictionaryValue* prefs;
  if (!parsed_json.value || !parsed_json.value->GetAsDictionary(&prefs)) {
    return Status(kUnknownError, "cannot parse internal JSON template: " +
                                     parsed_json.error_message);
  }

  if (custom_prefs) {
    for (base::DictionaryValue::Iterator it(*custom_prefs); !it.IsAtEnd();
         it.Advance()) {
      prefs->Set(it.key(), std::make_unique<base::Value>(it.value().Clone()));
    }
  }

  std::string prefs_str;
  base::JSONWriter::Write(*prefs, &prefs_str);
  VLOG(0) << "Populating " << path.BaseName().value()
          << " file: " << PrettyPrintValue(*prefs);
  if (static_cast<int>(prefs_str.length()) != base::WriteFile(
          path, prefs_str.c_str(), prefs_str.length())) {
    return Status(kUnknownError, "failed to write prefs file");
  }
  return Status(kOk);
}

Status PrepareUserDataDir(
    const base::FilePath& user_data_dir,
    const base::DictionaryValue* custom_prefs,
    const base::DictionaryValue* custom_local_state) {
  base::FilePath default_dir =
      user_data_dir.AppendASCII(chrome::kInitialProfile);
  if (!base::CreateDirectory(default_dir))
    return Status(kUnknownError, "cannot create default profile directory");

  std::string preferences;
  base::FilePath preferences_path =
      default_dir.Append(chrome::kPreferencesFilename);

  if (base::PathExists(preferences_path))
    base::ReadFileToString(preferences_path, &preferences);
  else
    preferences = kPreferences;

  Status status =
      WritePrefsFile(preferences,
                     custom_prefs,
                     default_dir.Append(chrome::kPreferencesFilename));
  if (status.IsError())
    return status;

  std::string local_state;
  base::FilePath local_state_path =
      user_data_dir.Append(chrome::kLocalStateFilename);

  if (base::PathExists(local_state_path))
    base::ReadFileToString(local_state_path, &local_state);
  else
    local_state = kLocalState;

  status = WritePrefsFile(local_state,
                          custom_local_state,
                          user_data_dir.Append(chrome::kLocalStateFilename));
  if (status.IsError())
    return status;

  // Write empty "First Run" file, otherwise Chrome will wipe the default
  // profile that was written.
  if (base::WriteFile(
          user_data_dir.Append(chrome::kFirstRunSentinel), "", 0) != 0) {
    return Status(kUnknownError, "failed to write first run file");
  }
  return Status(kOk);
}

Status ParseDevToolsActivePortFile(const base::FilePath& user_data_dir,
                                   int* port) {
  base::FilePath port_filepath = user_data_dir.Append(kDevToolsActivePort);
  if (!base::PathExists(port_filepath)) {
    return Status(kUnknownError, "DevToolsActivePort file doesn't exist");
  }
  std::string buffer;
  bool result = base::ReadFileToString(port_filepath, &buffer);
  if (!result) {
    return Status(kUnknownError, "Could not read in devtools port number");
  }
  std::vector<std::string> split_port_strings = base::SplitString(
      buffer, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  if (split_port_strings.size() < 2) {
    return Status(kUnknownError,
                  std::string("Devtools port number file contents <") + buffer +
                      std::string("> were in an unexpected format"));
  }
  if (!base::StringToInt(split_port_strings.front(), port)) {
    return Status(kUnknownError,
                  "Could not convert devtools port number to int");
  }
  return Status(kOk);
}

Status RemoveOldDevToolsActivePortFile(const base::FilePath& user_data_dir) {
  base::FilePath port_filepath = user_data_dir.Append(kDevToolsActivePort);
  // Note that calling DeleteFile on a path that doesn't exist returns True.
  if (base::DeleteFile(port_filepath)) {
    return Status(kOk);
  }
  return Status(
      kUnknownError,
      base::StringPrintf(
          "Could not remove old devtools port file. Perhaps the given "
          "user-data-dir at %s is still attached to a running %s or "
          "Chromium process",
          user_data_dir.AsUTF8Unsafe().c_str(), kBrowserShortName));
}

std::string GetTerminationReason(base::TerminationStatus status) {
  switch (status) {
    case base::TERMINATION_STATUS_STILL_RUNNING:
      return "still running";
    case base::TERMINATION_STATUS_NORMAL_TERMINATION:
      return "exited normally";
    case base::TERMINATION_STATUS_ABNORMAL_TERMINATION:
      return "exited abnormally";
    case base::TERMINATION_STATUS_PROCESS_WAS_KILLED:
#if BUILDFLAG(IS_CHROMEOS)
    case base::TERMINATION_STATUS_PROCESS_WAS_KILLED_BY_OOM:
#endif
    case base::TERMINATION_STATUS_OOM:
      return "was killed";
#if BUILDFLAG(IS_ANDROID)
    case base::TERMINATION_STATUS_OOM_PROTECTED:
      return "protected from oom";
#endif
    case base::TERMINATION_STATUS_PROCESS_CRASHED:
      return "crashed";
    case base::TERMINATION_STATUS_LAUNCH_FAILED:
      return "failed to launch";
#if BUILDFLAG(IS_WIN)
    case base::TERMINATION_STATUS_INTEGRITY_FAILURE:
      return "integrity failure";
#endif
    case base::TERMINATION_STATUS_MAX_ENUM:
      NOTREACHED();
      return "max enum";
  }
  NOTREACHED() << "Unknown Termination Status.";
  return "unknown";
}

}  // namespace internal
