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

#include "window.hpp"
#include "dxhook.hpp"

// TODO(phlip9): refactor out pieces
// TODO(phlip9): refactor out gui, input, etc into bot class to reduce global state
// TODO(phlip9): try printing all units or something

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

void SetDefaultRenderState(IDirect3DDevice9* device, HWND const hwnd)
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
    ImGui::Text("Hello, world! %s", "asdfasdf");
    ImGui::Text("Hello, world! %s", "asdfasdf");
    ImGui::Text("Hello, world! %s", "asdfasdf");
    ImGui::Text("Hello, world! %s", "asdfasdf");
    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);

  }
  ImGui::End();

  ImGui::Render();
}

void CleanupImGui(HWND const hwnd)
{
  if (!GetImGuiInitialized()) {
    return;
  }

  ImGui_ImplDX9_Shutdown();

  // unhook our window handler and replace it with the original
  auto const oWndProc = GetOriginalWndProc();
  SetWindowLongPtr(hwnd, GWL_WNDPROC, reinterpret_cast<LONG>(oWndProc));

  SetImGuiInitialized(false);
}

void Render(IDirect3DDevice9* device, HWND const hwnd)
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
  SetDefaultRenderState(device, hwnd);

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


extern "C" HRESULT WINAPI IDirect3DDevice9_EndScene_Detour(
  hadesmem::PatchDetourBase* detour, IDirect3DDevice9* device)
{
  HWND const hwnd = phlipbot::GetWindowFromDevice(device);
  phlipbot::SetCurrentWindow(hwnd);

  // Initialize ImGui if it's not initialized already
  if (!GetImGuiInitialized()) {
    phlipbot::LogWindowTitle(hwnd);

    InitializeImGui(hwnd, device);
  }

  if (GetImGuiInitialized()) {
    try {
      Render(device, hwnd);
    } catch (...) {
      HADESMEM_DETAIL_TRACE_A("Failed to render");
      HADESMEM_DETAIL_TRACE_A(
        boost::current_exception_diagnostic_information().c_str());
    }
  }

  // call the original end scene
  auto const end_scene =
    detour->GetTrampolineT<phlipbot::IDirect3DDevice9_EndScene_Fn>();
  auto ret = end_scene(device);

  return ret;
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
  auto const reset =
    detour->GetTrampolineT<phlipbot::IDirect3DDevice9_Reset_Fn>();
  auto ret = reset(device, pp);

  HADESMEM_DETAIL_TRACE_FORMAT_A("Ret: [%ld]", ret);

  return ret;
}


extern "C" __declspec(dllexport) unsigned int Load()
{
	HADESMEM_DETAIL_TRACE_A("phlipbot dll Load() called.");

  hadesmem::Process const process{ ::GetCurrentProcessId() };

  // get the WoW.exe main window handle
  boost::optional<HWND> omain_hwnd = phlipbot::FindMainWindow(process);
  if (!omain_hwnd) {
    HADESMEM_DETAIL_TRACE_A("ERROR: Could not find main window handle");
    return EXIT_FAILURE;
  }
  HWND main_hwnd = *omain_hwnd;
  phlipbot::SetCurrentWindow(main_hwnd);

  // log the main window title
  try {
    phlipbot::LogWindowTitle(main_hwnd);
  } catch (...) {
    return EXIT_FAILURE;
  }

  // hook d3d9 device reset and end scene
  try {
    phlipbot::DetourD3D9(process, main_hwnd,
      &IDirect3DDevice9_EndScene_Detour,
      &IDirect3DDevice9_Reset_Detour);
  } catch (...) {
    HADESMEM_DETAIL_TRACE_A("ERROR: Failed to detour end scene and reset");
    return EXIT_FAILURE;
  }

	return EXIT_SUCCESS;
}


extern "C" __declspec(dllexport) unsigned int Unload()
{
	HADESMEM_DETAIL_TRACE_A("phlipbot dll Unload() called");

  // TODO: cleanup directx device? window handle?

  phlipbot::UndetourD3D9();

  HWND const& hwnd = phlipbot::GetCurrentWindow();

  // TODO(phlip9): refactor out current window
  CleanupImGui(hwnd);

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