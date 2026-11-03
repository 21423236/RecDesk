#include "region_selector.h"
#include <windowsx.h>
#include <algorithm>

namespace recdesk {
namespace {

struct Selection {
  POINT start{}, end{};
  RECT desktop{};
  bool dragging = false, done = false, accepted = false;
};

RECT rectangle(const Selection& state) {
  return {std::min(state.start.x, state.end.x), std::min(state.start.y, state.end.y),
    std::max(state.start.x, state.end.x), std::max(state.start.y, state.end.y)};
}

LRESULT CALLBACK selectionProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  auto state = reinterpret_cast<Selection*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    state = static_cast<Selection*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
  }
  if (!state) return DefWindowProcW(window, message, wParam, lParam);
  switch (message) {
    case WM_LBUTTONDOWN:
      state->start = state->end = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      state->dragging = true;
      SetCapture(window);
      return 0;
    case WM_MOUSEMOVE:
      if (state->dragging) {
        RECT client{}; GetClientRect(window, &client);
        state->end = {std::clamp<LONG>(GET_X_LPARAM(lParam), 0, client.right),
          std::clamp<LONG>(GET_Y_LPARAM(lParam), 0, client.bottom)};
        InvalidateRect(window, nullptr, TRUE);
      }
      return 0;
    case WM_LBUTTONUP: {
      if (!state->dragging) return 0;
      state->dragging = false;
      ReleaseCapture();
      RECT rect = rectangle(*state);
      if (rect.right - rect.left >= 2 && rect.bottom - rect.top >= 2) {
        state->accepted = true;
        state->done = true;
      }
      return 0;
    }
    case WM_KEYDOWN:
      if (wParam == VK_ESCAPE) state->done = true;
      return 0;
    case WM_RBUTTONDOWN:
    case WM_CLOSE:
      state->done = true;
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      HDC dc = BeginPaint(window, &paint);
      RECT client{}; GetClientRect(window, &client);
      FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
      RECT area = rectangle(*state);
      HBRUSH fill = CreateSolidBrush(RGB(90, 170, 145));
      FillRect(dc, &area, fill);
      DeleteObject(fill);
      HPEN pen = CreatePen(PS_SOLID, 3, RGB(50, 255, 190));
      HGDIOBJ oldPen = SelectObject(dc, pen);
      HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
      Rectangle(dc, area.left, area.top, area.right, area.bottom);
      SelectObject(dc, oldPen); SelectObject(dc, oldBrush); DeleteObject(pen);
      std::wstring text = L"拖动鼠标选择录制区域 · 松开确认 · Esc / 右键取消";
      if (state->dragging) text += L"    " + std::to_wstring(area.right - area.left) + L" × " + std::to_wstring(area.bottom - area.top);
      SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
      // Place instructions on the monitor under the cursor, not in a monitor gap.
      POINT cursor{}; GetCursorPos(&cursor);
      MONITORINFO monitor{sizeof(MONITORINFO)};
      GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor);
      int x = monitor.rcWork.left - state->desktop.left + 30;
      int y = monitor.rcWork.top - state->desktop.top + 30;
      TextOutW(dc, x, y, text.c_str(), static_cast<int>(text.size()));
      EndPaint(window, &paint);
      return 0;
    }
  }
  return DefWindowProcW(window, message, wParam, lParam);
}

}

std::optional<RECT> selectRegion(HINSTANCE instance) {
  Selection state;
  state.desktop = {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
  state.desktop.right = state.desktop.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
  state.desktop.bottom = state.desktop.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
  WNDCLASSW type{};
  type.lpfnWndProc = selectionProcedure;
  type.hInstance = instance;
  type.lpszClassName = L"RecDeskRegion";
  type.hCursor = LoadCursorW(nullptr, IDC_CROSS);
  RegisterClassW(&type);
  HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
    type.lpszClassName, L"选择录制区域", WS_POPUP, state.desktop.left, state.desktop.top,
    state.desktop.right - state.desktop.left, state.desktop.bottom - state.desktop.top,
    nullptr, nullptr, instance, &state);
  if (!window) throw std::runtime_error("无法创建区域选择窗口");
  SetLayeredWindowAttributes(window, 0, 155, LWA_ALPHA);
  ShowWindow(window, SW_SHOW);
  SetForegroundWindow(window);
  MSG message{};
  while (!state.done) {
    BOOL status = GetMessageW(&message, nullptr, 0, 0);
    if (status <= 0) { if (!status) PostQuitMessage(static_cast<int>(message.wParam)); break; }
    TranslateMessage(&message); DispatchMessageW(&message);
  }
  DestroyWindow(window);
  if (!state.accepted) return std::nullopt;
  RECT result = rectangle(state);
  OffsetRect(&result, state.desktop.left, state.desktop.top);
  return result;
}

}
