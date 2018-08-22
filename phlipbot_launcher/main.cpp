#include <filesystem>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include <boost/program_options.hpp>

#include <hadesmem/config.hpp>
#include <hadesmem/debug_privilege.hpp>
#include <hadesmem/detail/smart_handle.hpp>
#include <hadesmem/injector.hpp>
#include <hadesmem/module.hpp>
#include <hadesmem/process.hpp>
#include <hadesmem/process_helpers.hpp>

namespace fs = std::experimental::filesystem;
namespace po = boost::program_options;

using HandlerT = std::function<int(po::variables_map const&)>;

// TODO(phlip9): cmd line interface?
// > phlipbot_launcher inject "dll path"
// > phlipbot_launcher eject
// > phlipbot_launcher watch
//    events: (either just poll or use some async io library (libuv?
//    boost::asio? windows api?))
//      + WoW.exe process stops or dll errors => stop watching? / or restart and
//      inject?
//      + dll changes => eject old dll then inject new dll
//      + launcher gets CTRL+C => eject dll and shutdown
// TODO(phlip9): watch for changes in the dll and then reinject

std::unique_ptr<hadesmem::Process>
get_proc_from_options(po::variables_map const& vm)
{
  if (vm.count("pid")) {
    int pid = vm["pid"].as<int>();
    return std::make_unique<hadesmem::Process>(pid);
  } else {
    auto const& pname = vm["pname"].as<std::wstring>();
    return std::make_unique<hadesmem::Process>(
      hadesmem::GetProcessByName(pname));
  }
}

int handler_inject(po::variables_map const& vm)
{
  // need privileges to inject
  hadesmem::GetSeDebugPrivilege();

  // TODO(phlip9): optionally start new process if no existing process

  // get the WoW process handle
  auto process = get_proc_from_options(vm);
  std::wcout << "process id = " << process->GetId() << "\n";

  // TODO(phlip9): do nothing if dll already inject

  // inject bot dll into WoW process
  auto const& dll_name = vm["dll"].as<std::wstring>();
  uint32_t flags = 0;
  flags |= hadesmem::InjectFlags::kPathResolution;
  flags |= hadesmem::InjectFlags::kAddToSearchOrder;
  HMODULE module = hadesmem::InjectDll(*process, dll_name, flags);

  std::wcout << "successfully injected bot dll at base address = "
             << hadesmem::detail::PtrToHexString(module) << "\n";

  // call remote Load() function
  hadesmem::CallResult<DWORD_PTR> const unload_res =
    hadesmem::CallExport(*process, module, "Load");

  std::wcout << "Called bot dll's Load() function\n";
  std::wcout << "Return value = " << unload_res.GetReturnValue() << "\n";
  std::wcout << "LastError = " << unload_res.GetLastError() << "\n";

  return 0;
}

int handler_eject(po::variables_map const& vm)
{
  // need privileges to eject
  hadesmem::GetSeDebugPrivilege();

  // get the WoW process handle
  auto process = get_proc_from_options(vm);
  std::wcout << "process id = " << process->GetId() << "\n";

  // get the phlipbot.dll handle in the WoW process
  auto const& dll_name = vm["dll"].as<std::wstring>();
  hadesmem::Module const module{*process, dll_name};

  // TODO(phlip9): do nothing if no dll injected

  // call remote Unload() function
  hadesmem::CallResult<DWORD_PTR> const load_res =
    hadesmem::CallExport(*process, module.GetHandle(), "Unload");

  std::wcout << "Called bot dll's Unload() function" << std::endl;
  std::wcout << "Return value = " << load_res.GetReturnValue() << std::endl;
  std::wcout << "LastError = " << load_res.GetLastError() << std::endl;

  // free the bot dll from the remote process
  hadesmem::FreeDll(*process, module.GetHandle());

  std::wcout << "Free'd the bot dll.\n";

  return 0;
}

int handler_watch(po::variables_map const& vm)
{
  // TODO(phlip9): implement watching
  std::wcout << "Watching dll for changes\n";
  return vm.count("help");
}

int main_inner(int argc, wchar_t** argv)
{
  std::map<std::wstring, HandlerT> const cmd_handlers{
    {L"inject", handler_inject},
    {L"eject", handler_eject},
    {L"watch", handler_watch}};

  po::options_description desc("phlipbot_launcher [inject|eject|watch]");
  auto add = desc.add_options();
  add("help", "print usage");
  add(
    "dll,d",
    po::wvalue<std::wstring>()->default_value(L"phlipbot.dll", "phlipbot.dll"),
    "filename or path to the dll");
  add("pid,p", po::value<int>(), "the target process pid");
  add("pname,n",
      po::wvalue<std::wstring>()->default_value(L"WoW.exe", "WoW.exe"),
      "the target process name");
  add("command", po::wvalue<std::wstring>(), "inject|eject|watch");

  po::positional_options_description pos_desc;
  pos_desc.add("command", 1);

  po::wcommand_line_parser parser(argc, argv);
  parser.options(desc);
  parser.positional(pos_desc);

  po::variables_map vm;
  po::store(parser.run(), vm);
  po::notify(vm);

  if (vm.count("help")) {
    std::cout << desc << "\n";
    return 0;
  }

  if (vm.count("command")) {
    auto const& command = vm["command"].as<std::wstring>();

    if (cmd_handlers.count(command) != 1) {
      std::wcerr << "Error: invalid command: \"" << command << "\"\n\n";
      std::cout << desc << "\n";
      return 1;
    }

    auto const& handler = cmd_handlers.at(command);

    return handler(vm);
  }

  std::wcerr << "Error: missing command\n\n";
  std::cout << desc << "\n";

  return 1;
}

int main()
{
  try {
    // Get the command line arguments as wide strings
    int argc;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    hadesmem::detail::SmartLocalFreeHandle smart_argv{argv};

    if (argv == nullptr) {
      std::wcerr << "Error: CommandLineToArgvW failed\n";
      return 1;
    }

    return main_inner(argc, argv);
  } catch (...) {
    std::cerr << boost::current_exception_diagnostic_information() << "\n";
    return 1;
  }
}