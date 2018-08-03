#include <Shlwapi.h>

#include <d3d9.h>

#include <boost/optional.hpp>

#include <imgui/imgui.h>
#include <imgui/examples/directx9_example/imgui_impl_dx9.h>

#include <hadesmem/patcher.hpp>
#include <hadesmem/process.hpp>
#include <hadesmem/module.hpp>
#include <hadesmem/module_list.hpp>
#include <hadesmem/error.hpp>
#include <hadesmem/detail/type_traits.hpp>
#include <hadesmem/detail/last_error_preserver.hpp>
#include <hadesmem/detail/trace.hpp>
#include <hadesmem/detail/smart_handle.hpp>
#include <hadesmem/detail/scope_warden.hpp>

// TODO(phlip9): refactor out pieces

//
// Window stuff
//

// state passed to EnumWindowsCallback
struct enum_windows_aux {
  DWORD target_proc_id;
  HWND target_window_handle;
};

// GetMainWindow callback
BOOL CALLBACK EnumWindowsCallback(HWND const handle, LPARAM const lparam)
{
  enum_windows_aux& aux_data = *reinterpret_cast<enum_windows_aux*>(lparam);

  // query the owning process
  DWORD process_id;
  GetWindowThreadProcessId(handle, &process_id);

  // continue if this window doesn't belong to the target process
  if (process_id != aux_data.target_proc_id) {
    return TRUE;
  }

  // ensure not a child window
  if (GetWindow(handle, GW_OWNER) != nullptr) {
    return TRUE;
  }

  // found the primary window
  aux_data.target_window_handle = handle;
  return FALSE;
}

// find the root window of a process 
boost::optional<HWND> FindMainWindow(hadesmem::Process const& process)
{
  enum_windows_aux aux_data;
  aux_data.target_proc_id = process.GetId();
  aux_data.target_window_handle = nullptr;

  auto aux_ptr = reinterpret_cast<LPARAM>(&aux_data);
  EnumWindows(EnumWindowsCallback, aux_ptr);

  if (aux_data.target_window_handle) {
    return aux_data.target_window_handle;
  } else {
    return boost::none;
  }
}

HWND& GetCurrentWindow()
{
  static HWND wnd{ nullptr };
  return wnd;
}

void SetCurrentWindow(HWND const wnd)
{
  auto& curr_wnd = GetCurrentWindow();
  curr_wnd = wnd;
}

void LogWindowTitle(HWND const wnd)
{
  char buf[256];
  if (!GetWindowTextA(wnd, static_cast<LPSTR>(buf), sizeof(buf))) {
    HADESMEM_DETAIL_THROW_EXCEPTION(
      hadesmem::Error{}
        << hadesmem::ErrorString{ "GetWindowTextA failed" }
        << hadesmem::ErrorCodeWinLast{ ::GetLastError() });
  }
  HADESMEM_DETAIL_TRACE_FORMAT_A("window title = %s", buf);
}


//
// D3D9
//

struct d3d9_offsets
{
  uintptr_t end_scene;
  uintptr_t reset;
};

template <typename DeviceT>
HWND GetWindowFromDevice(DeviceT* const device)
{
  D3DDEVICE_CREATION_PARAMETERS cp;
  auto const get_cp_hr = device->GetCreationParameters(&cp);
  if (FAILED(get_cp_hr)) {
    HADESMEM_DETAIL_THROW_EXCEPTION(
      hadesmem::Error()
      << hadesmem::ErrorString("GetCreationParameters failed")
      << hadesmem::ErrorCodeWinHr{ get_cp_hr });
  }
  return cp.hFocusWindow;
}

IDirect3DDevice9Ex* GetD3D9Device(hadesmem::Process const& process, HWND const wnd)
{
  // get d3d9.dll module
  hadesmem::Module const d3d9_mod{ process, L"d3d9.dll" };

  // get the Direct3DCreate9Ex function
  auto const direct3d_create_9_ex =
    reinterpret_cast<decltype(&Direct3DCreate9Ex)>(
      ::GetProcAddress(d3d9_mod.GetHandle(), "Direct3DCreate9Ex"));
  if (!direct3d_create_9_ex) {
    HADESMEM_DETAIL_THROW_EXCEPTION(
      hadesmem::Error{}
        << hadesmem::ErrorString{ "GetProcAddress for Direct3DCreate9Ex failed" }
        << hadesmem::ErrorCodeWinLast{ ::GetLastError() });
  }

  // get the IDirect3D9Ex interface impl
  IDirect3D9Ex* d3d9_ex = nullptr;
  auto const create_d3d9_ex_hr =
    direct3d_create_9_ex(D3D_SDK_VERSION, &d3d9_ex);
  if (FAILED(create_d3d9_ex_hr)) {
    HADESMEM_DETAIL_THROW_EXCEPTION(
      hadesmem::Error{}
        << hadesmem::ErrorString{ "Direct3DCreate9Ex failed" }
        << hadesmem::ErrorCodeWinHr{ create_d3d9_ex_hr });
  }

  hadesmem::detail::SmartComHandle smart_d3d9_ex{ d3d9_ex };

  D3DPRESENT_PARAMETERS pp = {};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_FLIP;
  pp.BackBufferFormat = D3DFMT_A8R8G8B8;
  pp.BackBufferWidth = 2;
  pp.BackBufferHeight = 2;
  pp.BackBufferCount = 1;
  pp.hDeviceWindow = wnd;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

  // get the device
  IDirect3DDevice9Ex* device_ex = nullptr;
  auto const create_device_hr = d3d9_ex->CreateDeviceEx(
    D3DADAPTER_DEFAULT,
    D3DDEVTYPE_HAL,
    wnd,
    D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES,
    &pp,
    nullptr,
    &device_ex);
  if (FAILED(create_device_hr)) {
    HADESMEM_DETAIL_THROW_EXCEPTION(
      hadesmem::Error{}
        << hadesmem::ErrorString{ "IDirect3D9Ex::CreateDeviceEx failed" }
        << hadesmem::ErrorCodeWinHr{ create_device_hr });
  }

  LogWindowTitle(GetWindowFromDevice(device_ex));

  return device_ex;
}

d3d9_offsets GetD3D9Offsets(IDirect3DDevice9Ex* device_ex)
{
  auto const end_scene_fn = (*reinterpret_cast<void***>(device_ex))[42];
  auto const reset_fn = (*reinterpret_cast<void***>(device_ex))[16];

  HADESMEM_DETAIL_TRACE_FORMAT_A("IDirect3D9Ex::EndScene: %p", end_scene_fn);
  HADESMEM_DETAIL_TRACE_FORMAT_A("IDirect3D9Ex::Reset: %p", reset_fn);

  d3d9_offsets offsets = {};
  offsets.end_scene = reinterpret_cast<std::uintptr_t>(end_scene_fn);
  offsets.reset = reinterpret_cast<std::uintptr_t>(reset_fn);
  return offsets;
}


void LogModules()
{
  hadesmem::Process const process(::GetCurrentProcessId());

  hadesmem::ModuleList const module_list(process);

  HADESMEM_DETAIL_TRACE_A("WoW.exe dll modules:");
  for (auto const& module : module_list) {
    HADESMEM_DETAIL_TRACE_FORMAT_W(L"    %s", module.GetName().c_str());
  }
}


//
// Input
//

WNDPROC& GetOriginalWndProc()
{
  static WNDPROC oWndProc{ nullptr };
  return oWndProc;
}

void SetOriginalWndProc(WNDPROC const wndProc)
{
  auto& oWndProc = GetOriginalWndProc();
  oWndProc = wndProc;
}

LRESULT CALLBACK ImGui_ImplDX9_WndProcHandler(HWND, UINT msg, WPARAM wParam, LPARAM lParam)
{
    ImGuiIO& io = ImGui::GetIO();
    switch (msg)
    {
    case WM_LBUTTONDOWN:
        io.MouseDown[0] = true;
        return true;
    case WM_LBUTTONUP:
        io.MouseDown[0] = false;
        return true;
    case WM_RBUTTONDOWN:
        io.MouseDown[1] = true;
        return true;
    case WM_RBUTTONUP:
        io.MouseDown[1] = false;
        return true;
    case WM_MBUTTONDOWN:
        io.MouseDown[2] = true;
        return true;
    case WM_MBUTTONUP:
        io.MouseDown[2] = false;
        return true;
    case WM_MOUSEWHEEL:
        io.MouseWheel += GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? +1.0f : -1.0f;
        return true;
    case WM_MOUSEMOVE:
        io.MousePos.x = (signed short)(lParam);
        io.MousePos.y = (signed short)(lParam >> 16);
        return true;
    case WM_KEYDOWN:
        if (wParam < 256)
            io.KeysDown[wParam] = 1;
        return true;
    case WM_KEYUP:
        if (wParam < 256)
            io.KeysDown[wParam] = 0;
        return true;
    case WM_CHAR:
        // You can also use ToAscii()+GetKeyboardState() to retrieve characters.
        if (wParam > 0 && wParam < 0x10000)
            io.AddInputCharacter((unsigned short)wParam);
        return true;
    }
    return false;
}

// TODO(phlip9): Refactor
void ToggleImGuiVisible();

LRESULT CALLBACK
WindowProcCallback(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  // TODO(phlip9): custom handler

  // Shift+F7 to toggle GUI visibility
  bool const shift_down = !!(::GetAsyncKeyState(VK_SHIFT) & 0x8000);
  if (msg == WM_KEYDOWN
    && !((lParam >> 30) & 0x1)
    && wParam == VK_F9
    && shift_down
  ) {
    ToggleImGuiVisible();
    return true;
  }

  // Try the ImGui input handler
  ImGui_ImplDX9_WndProcHandler(hwnd, msg, wParam, lParam);

  // Fall through to the original handler
  auto const oWndProc = GetOriginalWndProc();
  if (oWndProc && CallWindowProc(oWndProc, hwnd, msg, wParam, lParam)) {
    return true;
  }

  return false;
}


//
// ImGui
//

bool& GetImGuiInitialized()
{
  static bool initialized{ false };
  return initialized;
}
void SetImGuiInitialized(bool const val)
{
  auto& initialized = GetImGuiInitialized();
  initialized = val;
}

bool& GetImGuiVisible()
{
  static bool visible{ true };
  return visible;
}
void SetImGuiVisible(bool const val)
{
  auto& visible = GetImGuiVisible();
  visible = val;
}
void ToggleImGuiVisible()
{
  auto& visible = GetImGuiVisible();
  visible = !visible;
}

void InitializeImGui(HWND const wnd, IDirect3DDevice9* device)
{
  HADESMEM_DETAIL_ASSERT(!GetImGuiInitialized());
  HADESMEM_DETAIL_TRACE_FORMAT_A("wnd: [%p], device: [%p]", wnd, device);

  ImGuiIO& io = ImGui::GetIO();
  io.Fonts->AddFontDefault();
  io.MouseDrawCursor = false;

  if (!GetOriginalWndProc()) {
    auto const wndProc = reinterpret_cast<LONG>(WindowProcCallback);
    auto const oWndProc = reinterpret_cast<WNDPROC>(
      SetWindowLongPtr(wnd, GWL_WNDPROC, wndProc));
    SetOriginalWndProc(oWndProc);
  }

  ImGui_ImplDX9_Init(wnd, device);
  SetImGuiInitialized(true);
}

void SetDefaultRenderState(IDirect3DDevice9* device)
{
  device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
  device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
  device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
  device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO);
  device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
  device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
  device->SetRenderState(D3DRS_LASTPIXEL, TRUE);
  device->SetRenderState(D3DRS_FOGENABLE, FALSE);
  device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
  device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0000000F);
  device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
  device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
  device->SetRenderState(D3DRS_ANTIALIASEDLINEENABLE, FALSE);

  device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
  device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_CURRENT);
  device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
  device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
  device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_PASSTHRU);
  device->SetTextureStageState(
    0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);

  device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
  device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
  device->SetSamplerState(0, D3DSAMP_ADDRESSW, D3DTADDRESS_WRAP);
  device->SetSamplerState(0, D3DSAMP_BORDERCOLOR, 0);
  device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
  device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
  device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
  device->SetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, 0);
  device->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, 0);
  device->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, 1);
  device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, 0);
  device->SetSamplerState(0, D3DSAMP_ELEMENTINDEX, 0);
  device->SetSamplerState(0, D3DSAMP_DMAPOFFSET, 0);

  device->SetVertexShader(nullptr);
  device->SetPixelShader(nullptr);

  auto const hwnd = GetCurrentWindow();

  RECT rect = {};
  ::GetClientRect(hwnd, &rect);

  D3DVIEWPORT9 vp = {};
  vp.Width = rect.right - rect.left;
  vp.Height = rect.bottom - rect.top;
  vp.MaxZ = 1;
  device->SetViewport(&vp);
}

void RenderImGui()
{
  ImGui_ImplDX9_NewFrame();

  ImGui::SetNextWindowSize(ImVec2(400, 500), ImGuiSetCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiSetCond_FirstUseEver);

  if (ImGui::Begin("phlipbot"))
  {
    ImGui::Text("Hello, world! %s", "asdfasdf");
    //ImGui::Separator();
    ImGui::Text("Hello, world! %s", "asdfasdf");
    //ImGui::Separator();
    ImGui::Text("Hello, world! %s", "asdfasdf");
    //ImGui::Separator();
    ImGui::Text("Hello, world! %s", "asdfasdf");
    ImGui::Text("Hello, world! %s", "asdfasdf");
    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);

  }
  ImGui::End();

  ImGui::Render();
}

void CleanupImGui()
{
  if (!GetImGuiInitialized()) {
    return;
  }

  ImGui_ImplDX9_Shutdown();

  // unhook our window handler and replace it with the original
  auto const wnd = GetCurrentWindow();
  auto const oWndProc = GetOriginalWndProc();
  SetWindowLongPtr(wnd, GWL_WNDPROC, reinterpret_cast<LONG>(oWndProc));

  SetImGuiInitialized(false);
}

void Render(IDirect3DDevice9* device)
{
  auto const coop_level = device->TestCooperativeLevel();
  if (FAILED(coop_level)) {
    HADESMEM_DETAIL_TRACE_A(
      "device->TestCooperativeLevel failed, skipping this frame");
    return;
  }

  // Save the current device state so we can restore it after our we render
  IDirect3DStateBlock9* state_block = nullptr;
  auto const create_sb_hr = device->CreateStateBlock(D3DSBT_ALL, &state_block);
  if (FAILED(create_sb_hr)) {
    HADESMEM_DETAIL_THROW_EXCEPTION(
      hadesmem::Error{}
        << hadesmem::ErrorString{ "device->CreateStateBlock failed" }
        << hadesmem::ErrorCodeWinHr{ create_sb_hr });
  }
  hadesmem::detail::SmartComHandle state_block_cleanup{ state_block };

  // Set up needed to render GUI
  SetDefaultRenderState(device);

  // Render our GUI
  if (GetImGuiVisible()) {
    RenderImGui();
  }

  // Restore the original device state
  auto const apply_sb_hr = state_block->Apply();
  if (FAILED(apply_sb_hr)) {
    HADESMEM_DETAIL_THROW_EXCEPTION(
      hadesmem::Error{}
        << hadesmem::ErrorString{ "state_block->Apply failed" }
        << hadesmem::ErrorCodeWinHr{ apply_sb_hr });
  }
}


//
// D3D9 EndScene Detour
//

typedef HRESULT(WINAPI* IDirect3DDevice9_EndScene_Fn)(IDirect3DDevice9* device);

std::unique_ptr<hadesmem::PatchDetour<IDirect3DDevice9_EndScene_Fn>>&
GetIDirect3DDevice9EndSceneDetour() noexcept
{
  static std::unique_ptr<hadesmem::PatchDetour<IDirect3DDevice9_EndScene_Fn>>
    detour;
  return detour;
}

extern "C" HRESULT WINAPI IDirect3DDevice9_EndScene_Detour(
  hadesmem::PatchDetourBase* detour, IDirect3DDevice9* device)
{
  // Initialize ImGui if it's not initialized already
  if (!GetImGuiInitialized()) {
    HWND wnd = GetWindowFromDevice(device);
    LogWindowTitle(wnd);
    SetCurrentWindow(wnd);

    InitializeImGui(wnd, device);
  }

  if (GetImGuiInitialized()) {
    try {
      Render(device);
    } catch (...) {
      HADESMEM_DETAIL_TRACE_A("Failed to render");
      HADESMEM_DETAIL_TRACE_A(
        boost::current_exception_diagnostic_information().c_str());
    }
  }

  // call the original end scene
  auto const end_scene = detour->GetTrampolineT<IDirect3DDevice9_EndScene_Fn>();
  auto ret = end_scene(device);

  return ret;
}


//
// D3D9 Reset Detour
//

typedef HRESULT(WINAPI* IDirect3DDevice9_Reset_Fn)(
  IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pp);

std::unique_ptr<hadesmem::PatchDetour<IDirect3DDevice9_Reset_Fn>>&
GetIDirect3DDevice9ResetDetour() noexcept
{
  static std::unique_ptr<hadesmem::PatchDetour<IDirect3DDevice9_Reset_Fn>>
    detour;
  return detour;
}

extern "C" HRESULT WINAPI
IDirect3DDevice9_Reset_Detour(
  hadesmem::PatchDetourBase* detour,
  IDirect3DDevice9* device,
  D3DPRESENT_PARAMETERS* pp
) {
  HADESMEM_DETAIL_TRACE_FORMAT_A("Args: [%p] [%p]", device, pp);

  // Uninitialize ImGui on device reset
  if (GetImGuiInitialized()) {
    ImGui_ImplDX9_InvalidateDeviceObjects();
    SetImGuiInitialized(false);
  }

  // call the original device reset
  auto const reset = detour->GetTrampolineT<IDirect3DDevice9_Reset_Fn>();
  auto ret = reset(device, pp);

  HADESMEM_DETAIL_TRACE_FORMAT_A("Ret: [%ld]", ret);

  return ret;
}


//
// Detour Helpers
//

template <typename T, typename U, typename V>
void DetourFn(
  hadesmem::Process const& process,
  std::string const& name,
  std::unique_ptr<T>& detour,
  U const& orig_fn,
  V const& detour_fn
) {
  if (!detour) {
    if (orig_fn) {
      detour.reset(new T(process, orig_fn, detour_fn));
      detour->Apply();
      HADESMEM_DETAIL_TRACE_FORMAT_A("%s detoured", name.c_str());
    } else {
      HADESMEM_DETAIL_TRACE_FORMAT_A("Could not find %s export", name.c_str());
    }
  } else {
    HADESMEM_DETAIL_TRACE_FORMAT_A("%s already detoured", name.c_str());
  }
}

template <typename T>
void UndetourFn(
  std::string const& name,
  std::unique_ptr<T>& detour
) {
  if (detour) {
    detour->Remove();
    HADESMEM_DETAIL_TRACE_FORMAT_A("%s undetoured", name.c_str());

    auto& ref_count = detour->GetRefCount();
    while (ref_count.load()) {
      HADESMEM_DETAIL_TRACE_FORMAT_A("Spinning on %s ref count", name.c_str());
    }
    HADESMEM_DETAIL_TRACE_FORMAT_A("%s free of refs", name.c_str());

    detour = nullptr;
  } else {
    HADESMEM_DETAIL_TRACE_FORMAT_A(
      "%s is not detoured, skipping removal", name.c_str());
  }
}


//
// D3D9 Detours
//

void DetourD3D9(hadesmem::Process const& process, d3d9_offsets const& offsets)
{
  DetourFn(
    process,
    "IDirect3DDevice9::EndScene",
    GetIDirect3DDevice9EndSceneDetour(),
    reinterpret_cast<IDirect3DDevice9_EndScene_Fn>(offsets.end_scene),
    IDirect3DDevice9_EndScene_Detour);

  DetourFn(
    process,
    "IDirect3DDevice9::Reset",
    GetIDirect3DDevice9ResetDetour(),
    reinterpret_cast<IDirect3DDevice9_Reset_Fn>(offsets.reset),
    IDirect3DDevice9_Reset_Detour);
}

void UndetourD3D9()
{
  UndetourFn("IDirect3DDevice9::EndScene", GetIDirect3DDevice9EndSceneDetour());
  UndetourFn("IDirect3DDevice9::Reset", GetIDirect3DDevice9ResetDetour());
}


extern "C" __declspec(dllexport) unsigned int Load()
{
	HADESMEM_DETAIL_TRACE_A("phlipbot dll Load() called.");

  hadesmem::Process const process{ ::GetCurrentProcessId() };

  // get the WoW.exe main window handle
  boost::optional<HWND> omain_hwnd = FindMainWindow(process);
  if (!omain_hwnd) {
    HADESMEM_DETAIL_TRACE_A("ERROR: Could not find main window handle");
    return EXIT_FAILURE;
  }
  HWND main_hwnd = *omain_hwnd;

  SetCurrentWindow(main_hwnd);

  // log the main window title
  try {
    LogWindowTitle(main_hwnd);
  } catch (...) {
    return EXIT_FAILURE;
  }

  // try to get the current directx device bound to the main WoW window
  IDirect3DDevice9Ex* device_ex;
  try {
    device_ex = GetD3D9Device(process, main_hwnd);
  } catch (...) {
    HADESMEM_DETAIL_TRACE_A("Failed to get directx device");
    HADESMEM_DETAIL_TRACE_A(
      boost::current_exception_diagnostic_information().c_str());
    return EXIT_FAILURE;
  }

  // try to get the EndScene function
  d3d9_offsets offsets;
  try {
    offsets = GetD3D9Offsets(device_ex);
  } catch (...) {
    HADESMEM_DETAIL_TRACE_A("Failed to get d3d9 end scene fn");
    HADESMEM_DETAIL_TRACE_A(
      boost::current_exception_diagnostic_information().c_str());
    return EXIT_FAILURE;
  }

  // hook d3d9 device reset and end scene
  DetourD3D9(process, offsets);

	return EXIT_SUCCESS;
}


extern "C" __declspec(dllexport) unsigned int Unload()
{
	HADESMEM_DETAIL_TRACE_A("phlipbot dll Unload() called");

  // TODO: cleanup directx device? window handle?

  UndetourD3D9();

  CleanupImGui();

	return EXIT_SUCCESS;
}


BOOL APIENTRY DllMain(
  HMODULE /* hModule */,
  DWORD  ul_reason_for_call,
  LPVOID /* lpReserved */
) {
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
      HADESMEM_DETAIL_TRACE_A("DLL_PROCESS_ATTACH");
      break;
    case DLL_PROCESS_DETACH:
      HADESMEM_DETAIL_TRACE_A("DLL_PROCESS_ATTACH");
      break;
    case DLL_THREAD_ATTACH:
      HADESMEM_DETAIL_TRACE_A("DLL_THREAD_ATTACH");
      break;
    case DLL_THREAD_DETACH:
      HADESMEM_DETAIL_TRACE_A("DLL_THREAD_DETACH");
      break;
    }
    return TRUE;
}

