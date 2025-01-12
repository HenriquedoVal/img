#pragma comment(lib, "user32")
#pragma comment(lib, "d2d1")
#pragma comment(lib, "ole32")
#pragma comment(lib, "windowscodecs")

#pragma comment(linker, "/subsystem:console /entry:mainCRTStartup")

#define UNICODE
#define _UNICODE

#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <math.h>
#include <wchar.h>

#include <Windows.h>
#include <wincodec.h>

#include "d2d1_c.h"
#include "d2d1_helpers.c"

#define DEFAULT_MULTIFRAME_INTERVAL_MS 60
#define DPI_AWARE


static struct {
    struct {
        IWICImagingFactory *imaging_factory;
        IWICBitmapDecoder *decoder;
        IWICFormatConverter *bitmap;
    } wic;
    struct {
        FLOAT width;
        FLOAT height;
        FLOAT offset_x;
        FLOAT offset_y;
        UINT frame_count;
        UINT current_frame;
        UINT frame_delay;
    } img;
    struct {
        int screen_width;
        int screen_height;
        float max_img_width;
        float max_img_height;
    } sys;
    struct {
        wchar_t *path;
        wchar_t **file_names;
        size_t count;
        int req_file_idx;
    } dir;
    DWORD style;
    DWORD ex_style;
} IMG = {0};


static char *get_error_string(unsigned id_or_hr, LPCVOID module)
{
    LPSTR win32_buffer = NULL;

    unsigned dwFlags = FORMAT_MESSAGE_ALLOCATE_BUFFER
                     | FORMAT_MESSAGE_FROM_SYSTEM
                     | FORMAT_MESSAGE_IGNORE_INSERTS;

    size_t size = FormatMessageA(
        dwFlags,
        module,
        id_or_hr,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&win32_buffer,
        0,
        NULL
    );

    if (!size)
        return NULL;

    char *our_buffer = HeapAlloc(GetProcessHeap(), 0, size + 1);
    memcpy(our_buffer, win32_buffer, size);
    our_buffer[size] = '\0';

    LocalFree(win32_buffer);

    return our_buffer;
}


bool set_dir(wchar_t *cmd_line)
{
    IMG.dir.req_file_idx = -1;
    IMG.dir.path = L".";

    wchar_t *last_backslash = NULL;
    if (cmd_line != NULL) {
        last_backslash = wcsrchr(cmd_line, '\\');
        if (last_backslash != NULL) {
            *last_backslash = '\0';
            size_t size = wcslen(cmd_line) * 2 + 2;
            IMG.dir.path = malloc(size);
            errno_t err = wcscpy_s(IMG.dir.path, size, cmd_line); assert(!err);
        }
    }

    wchar_t path_pattern[MAX_PATH];
    int wrote = swprintf(path_pattern, MAX_PATH, L"%s\\*", IMG.dir.path);

    WIN32_FIND_DATAW find_data;
    HANDLE find = FindFirstFileW(path_pattern, &find_data);  // .
    if (find == INVALID_HANDLE_VALUE) return false;
    FindNextFileW(find, &find_data);  // ..

    while (FindNextFileW(find, &find_data)) IMG.dir.count++;
    FindClose(find);

    size_t size_file_names = sizeof(wchar_t *) * IMG.dir.count;
    IMG.dir.file_names = malloc(size_file_names);

    wchar_t *cmd_line_file_name = last_backslash != NULL ? last_backslash + 1 : cmd_line;
    find = FindFirstFileW(path_pattern, &find_data);  // .
    FindNextFileW(find, &find_data);  // ..
    for (size_t i = 0; i < IMG.dir.count; ++i) {
        FindNextFileW(find, &find_data);

        // check if cmdline arg is found
        if (cmd_line_file_name && wcscmp(cmd_line_file_name, find_data.cFileName) == 0)
            IMG.dir.req_file_idx = (int)i;

        // append file name to struct
        size_t size = wcslen(find_data.cFileName) * 2 + 2;
        IMG.dir.file_names[i] = malloc(size);
        errno_t err = wcscpy_s(IMG.dir.file_names[i], size, find_data.cFileName); assert(!err);
    }
    FindClose(find);

    return !(cmd_line && IMG.dir.req_file_idx < 0);
}


HRESULT set_current_frame(void)
{
    if (IMG.wic.bitmap) IMG.wic.bitmap->lpVtbl->Release(IMG.wic.bitmap);

    IWICBitmapFrameDecode *frame = NULL;
    HRESULT hr = IMG.wic.decoder->lpVtbl->GetFrame(IMG.wic.decoder, IMG.img.current_frame, &frame);
    if (FAILED(hr)) {
        printf("Error: Could not get a frame from decoder. OS msg: %s",
               get_error_string(hr, GetModuleHandle(L"ole32")));
        return hr;
    }

    if (IMG.img.frame_count) {
        IWICMetadataQueryReader *meta;
        hr = frame->lpVtbl->GetMetadataQueryReader(frame, &meta);
        if (SUCCEEDED(hr)) {
            PROPVARIANT prop;
            PropVariantInit(&prop);

            // Gif
            hr = meta->lpVtbl->GetMetadataByName(meta, L"/grctlext/Delay", &prop);
            if (SUCCEEDED(hr) && prop.vt == VT_UI2) {
                UINT frame_delay;
                hr = UIntMult(prop.uiVal, 10, &frame_delay);
                if (FAILED(hr)) return -1;
                if (frame_delay) IMG.img.frame_delay = frame_delay;
                PropVariantClear(&prop);

                hr = meta->lpVtbl->GetMetadataByName(meta, L"/imgdesc/Left", &prop);
                if (FAILED(hr) || prop.vt != VT_UI2) return -1;
                UINT left = prop.uiVal;
                PropVariantClear(&prop);
                IMG.img.offset_x = (FLOAT)left;

                hr = meta->lpVtbl->GetMetadataByName(meta, L"/imgdesc/Top", &prop);
                if (FAILED(hr) || prop.vt != VT_UI2) return -1;
                UINT top = prop.uiVal;
                PropVariantClear(&prop);
                IMG.img.offset_y = (FLOAT)top;
            }

            meta->lpVtbl->Release(meta);
        }
    }

    // Convert the image format to 32bppPBGRA
    // (DXGI_FORMAT_B8G8R8A8_UNORM + D2D1_ALPHA_MODE_PREMULTIPLIED).
    IWICFormatConverter *converter = NULL;
    hr = IMG.wic.imaging_factory->lpVtbl->CreateFormatConverter(IMG.wic.imaging_factory, &converter);
    if (FAILED(hr)) {
        printf("Error: Could not create format converter. OS msg: %s",
               get_error_string(hr, GetModuleHandle(L"ole32")));
        return hr;
    }

    hr = converter->lpVtbl->Initialize(
            converter,
            (IWICBitmapSource *)frame,
            &GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,
            NULL,
            0.f,
            WICBitmapPaletteTypeMedianCut
    );
    if (FAILED(hr)) {
        printf("Error: Could not initialize the converter. OS msg: %s",
               get_error_string(hr, GetModuleHandle(L"ole32")));
        return hr;
    }
    frame->lpVtbl->Release(frame);

    UINT width, height;
    hr = converter->lpVtbl->GetSize(converter, &width, &height);
    if (FAILED(hr)) {
        printf("Error: Could not get size of frame. OS msg: %s",
               get_error_string(hr, GetModuleHandle(L"ole32")));
        return hr;
    }

    if (width > IMG.sys.max_img_width || height > IMG.sys.max_img_height) {
        float factor = min(IMG.sys.max_img_width / width, IMG.sys.max_img_height / height);
        width = (UINT)(width * factor);
        height = (UINT)(height * factor);
    }

    IMG.wic.bitmap = converter;
    IMG.img.width = (FLOAT)width;
    IMG.img.height = (FLOAT)height;

    return hr;
}


bool select_image(int dir_idx, bool log_err)
{
    wchar_t path[MAX_PATH];
    int wrote = swprintf(path, MAX_PATH, L"%s\\%s", IMG.dir.path, IMG.dir.file_names[dir_idx]); assert(wrote > 0);
    HRESULT hr = IMG.wic.imaging_factory->lpVtbl->CreateDecoderFromFilename(
            IMG.wic.imaging_factory,
            path,
            NULL,
            GENERIC_READ,
            WICDecodeMetadataCacheOnLoad,
            &IMG.wic.decoder
    );
    if (FAILED(hr)) {
        if (log_err) 
            printf("Error: Could not get image decoder. OS msg: %s",
                   get_error_string(hr, GetModuleHandle(L"ole32")));
        return false;
    }

    hr = IMG.wic.decoder->lpVtbl->GetFrameCount(IMG.wic.decoder, &IMG.img.frame_count);
    if (FAILED(hr)) {
        if (log_err) 
            printf("Error: Could not get frame count. OS msg: %s",
                   get_error_string(hr, GetModuleHandle(L"ole32")));
        return false;
    }

    return true;
}


bool adjust_window_according_to_image(HWND window)
{
    UINT dpi = GetDpiForWindow(window);
    // Max img size is 85% of screen with dpi correction
    IMG.sys.max_img_width  = IMG.sys.screen_width  * .85f * 96.f / dpi;
    IMG.sys.max_img_height = IMG.sys.screen_height * .85f * 96.f / dpi;

    IMG.img.offset_x = 0;
    IMG.img.offset_y = 0;
    IMG.img.current_frame = 0;
    IMG.img.frame_delay = DEFAULT_MULTIFRAME_INTERVAL_MS;
    HRESULT hr = set_current_frame();
    if (FAILED(hr)) return false;

    RECT rect = { .right = (LONG)IMG.img.width, .bottom = (LONG)IMG.img.height };

#ifdef DPI_AWARE

    if (!AdjustWindowRectExForDpi(&rect, IMG.style, false, IMG.ex_style, dpi)) {
        printf("Error: Could not adjust window (dpi). OS msg: %s",
               get_error_string(GetLastError(), NULL));
        return false;
    }

    int window_width  = (int)ceil((rect.right - rect.left) * dpi / 96.f);
    int window_height = (int)ceil((rect.bottom - rect.top) * dpi / 96.f);

#else

    if (!AdjustWindowRectEx(&rect, IMG.style, false, IMG.ex_style)) {
        printf("Error: Could not adjust window. OS msg: %s",
               get_error_string(GetLastError(), NULL));
        return false;
    }

    int window_width  = rect.right - rect.left;
    int window_height = rect.bottom - rect.top;

#endif

    int window_x = IMG.sys.screen_width  / 2 - window_width  / 2;
    int window_y = IMG.sys.screen_height / 2 - window_height / 2;

    bool success = SetWindowPos(
            window,
            (HWND)0,
            window_x,
            window_y,
            window_width,
            window_height,
            SWP_SHOWWINDOW
    );
    assert(success);

    if (IMG.ex_style == WS_EX_LAYERED) {
        SetLayeredWindowAttributes(window, 0, 0, LWA_COLORKEY);  // LWA_ALPHA  
    }

    return true;
}


void set_prev_image(void)
{
    do {
        if (IMG.dir.req_file_idx == 0) IMG.dir.req_file_idx = (int)IMG.dir.count;
        IMG.dir.req_file_idx--;
    } while (!select_image(IMG.dir.req_file_idx, false));
}


void set_next_image(void)
{
    do {
        IMG.dir.req_file_idx++;
        IMG.dir.req_file_idx %= IMG.dir.count;
    } while (!select_image(IMG.dir.req_file_idx, false));
}


void clear_and_resize(ID2D1HwndRenderTarget *target, HWND window)
{
    bool success = adjust_window_according_to_image(window);
    assert(success);
    if (target != NULL) {
        RECT rc;
        GetClientRect(window, &rc);
        D2D1_SIZE_U size_u = {(unsigned)rc.right, (unsigned)rc.bottom};

        // Can't base on img bc of dpi
        // D2D1_SIZE_U size_u = {(unsigned)IMG.img.width, (unsigned)IMG.img.height};

        HRESULT hr = target->lpVtbl->Resize(target, &size_u);
        if (SUCCEEDED(hr)) {
            PAINTSTRUCT ps;
            BeginPaint(window, &ps);
            target->lpVtbl->BeginDraw(target);
            D2D1_COLOR_F black = {.a = 1.0f};
            target->lpVtbl->Clear(target, &black);
            D2D1_TAG tag1, tag2;
            hr = target->lpVtbl->EndDraw(target, &tag1, &tag2);
            EndPaint(window, &ps);
        }
        if (FAILED(hr) || hr == D2DERR_RECREATE_TARGET) {
            printf("failed\n");
            if (target != NULL) {
                target->lpVtbl->Release(target);
                target = NULL;
            }
        }
    }
    InvalidateRect(window, NULL, false);
}


LRESULT CALLBACK WindowProc(HWND window, unsigned msg, WPARAM wparam, LPARAM lparam)
{
    static ID2D1Factory *factory;
    static ID2D1HwndRenderTarget *target = NULL;

    static ID2D1Bitmap *bitmap = NULL;
    static UINT_PTR timer_event_id = 1;

    HRESULT hr;

    switch (msg) {
        case WM_CREATE:
            hr = D2D1CreateFactory(
                    D2D1_FACTORY_TYPE_SINGLE_THREADED,
                    &IID_ID2D1Factory,
                    NULL,
                    (void **)&factory
            );
            if (FAILED(hr)) return -1;

            return 0;
            
        case WM_PAINT:
            if (target == NULL) {
                RECT rc;
                GetClientRect(window, &rc);
                D2D1_SIZE_U size_u = {(unsigned)rc.right, (unsigned)rc.bottom};
                // D2D1_SIZE_U size_u = {(unsigned)IMG.img.width, (unsigned)IMG.img.height};

                D2D1_HWND_RENDER_TARGET_PROPERTIES hwnd_prop = {
                    .hwnd = window,
                    .pixelSize = size_u,
                    .presentOptions = D2D1_PRESENT_OPTIONS_NONE
                };

                D2D1_RENDER_TARGET_PROPERTIES prop = helper_RenderTargetProperties();

                hr = factory->lpVtbl->CreateHwndRenderTarget(
                    factory,
                    &prop,
                    &hwnd_prop,
                    &target
                );
                if (FAILED(hr)) return -1;
            }

            if (bitmap) bitmap->lpVtbl->Release(bitmap);
            hr = target->lpVtbl->CreateBitmapFromWicBitmap(
                    target,
                    IMG.wic.bitmap,
                    NULL,
                    &bitmap
            );
            if (FAILED(hr)) return -1;

            PAINTSTRUCT ps;
            BeginPaint(window, &ps);
            target->lpVtbl->BeginDraw(target);

            // D2D1_COLOR_F gray = {0.18f, 0.18f, 0.18f, 1.0f};
            // target->lpVtbl->Clear(target, &gray);

            D2D1_RECT_F rect = {
                IMG.img.offset_x,
                IMG.img.offset_y,
                IMG.img.width + IMG.img.offset_x,
                IMG.img.height + IMG.img.offset_y
            };
            target->lpVtbl->DrawBitmap(
                    target,
                    bitmap,
                    &rect,
                    1.0f,
                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                    NULL
            );

            D2D1_TAG tag1, tag2;
            hr = target->lpVtbl->EndDraw(target, &tag1, &tag2);
            if (FAILED(hr) || hr == D2DERR_RECREATE_TARGET) {
                if (target != NULL) {
                    target->lpVtbl->Release(target);
                    target = NULL;
                }
            }
            EndPaint(window, &ps);

            if (IMG.img.frame_count > 1) {
                IMG.img.current_frame++;
                IMG.img.current_frame %= IMG.img.frame_count;
                hr = set_current_frame();
                if (FAILED(hr)) return -1;
                SetTimer(window, timer_event_id, IMG.img.frame_delay, NULL);
            }

            return 0;

        case WM_TIMER:
            InvalidateRect(window, NULL, false);
            return 0;

        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT) {
                SetCursor(NULL);
                return 0;
            }
            break;

        case WM_KEYDOWN:
            int key = (int)wparam;
            if (key == 'J') {
                KillTimer(window, timer_event_id);
                set_next_image();
                clear_and_resize(target, window);
            }

            if (key == 'K') {
                KillTimer(window, timer_event_id);
                set_prev_image();
                clear_and_resize(target, window);
            }

            if (key != 'Q' && key != 27) {  // 27 == ESC 
                return 0;
            }
            // else we do nothing and fallthrough to WM_CLOSE

        case WM_CLOSE:
            // Better wrap target in this check bc we don't know if
            // it was invalidated on last EndDraw
            if (target != NULL) {
                target->lpVtbl->Release(target);
                target = NULL;
            }
            factory->lpVtbl->Release(factory);
            DestroyWindow(window);
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProc(window, msg, wparam, lparam);
}


int WINAPI wWinMain(HINSTANCE instance, HINSTANCE _prev_inst, PWSTR cmd_line, int cmd_show)
{
    HRESULT hr = CoInitialize(NULL);
    if (FAILED(hr)) {
        printf("Error: Could not initialize COM. OS msg: %s",
               get_error_string(hr, GetModuleHandle(L"ole32")));
        return hr;
    }

    hr = CoCreateInstance(
            &CLSID_WICImagingFactory,
            NULL,
            CLSCTX_INPROC_SERVER,
            &IID_IWICImagingFactory,
            (void **)&IMG.wic.imaging_factory
    );
    if (FAILED(hr)) {
        printf("Error: Could not initialize COM WIC. OS msg: %s",
               get_error_string(hr, GetModuleHandle(L"ole32")));
        return hr;
    }

    IMG.sys.screen_width  = GetSystemMetrics(SM_CXSCREEN);
    IMG.sys.screen_height = GetSystemMetrics(SM_CYSCREEN);
    if (!IMG.sys.screen_width || !IMG.sys.screen_height) {
        printf("%s\n", "Error: Could not get screen size.\n");
        return -1;
    }

    IMG.style = WS_VISIBLE | WS_POPUP;
    IMG.ex_style = 0;

    const wchar_t class_name[] = L"Simple Image Viewer";

    WNDCLASS wc = {
        .lpfnWndProc = WindowProc,
        .hInstance = instance,
        .lpszClassName = class_name
    };

    RegisterClass(&wc);

    HWND window = CreateWindowEx(
        IMG.ex_style,
        class_name,
        class_name,
        IMG.style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        NULL,
        NULL,
        instance,
        NULL
    );
    if (window == NULL) {
        printf("Error: Could not create window. OS msg: %s",
               get_error_string(GetLastError(), NULL));
        return -1;
    }

    bool success = set_dir(cmd_line);
    if (!success) {
        printf("Error: Given arg is not a valid img\n");
        return -1;
    }

    if (IMG.dir.req_file_idx < 0) {  // if there was no file path as arg
        do {
            IMG.dir.req_file_idx++;
            if (IMG.dir.req_file_idx >= IMG.dir.count) {
                printf("Error: Could not find suitable file\n");
                return -1;
            }
        } while (!select_image(IMG.dir.req_file_idx, false));
    } else {
        success = select_image(IMG.dir.req_file_idx, true);
    }

    if (!success) return -1;

    adjust_window_according_to_image(window);
    ShowWindow(window, cmd_show);

    MSG msg = {0};
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    IMG.wic.decoder->lpVtbl->Release(IMG.wic.decoder);
    CoUninitialize();

    return 0;
}


wchar_t *to_wchar(const char *str)
{
    size_t len = strlen(str);
    size_t size = len * 2 + 1;

    wchar_t *ret = HeapAlloc(GetProcessHeap(), 0, size);
    assert(ret != NULL);
    size_t ret_val = 0;
    errno_t err = mbstowcs_s(&ret_val, ret, size, str, len);
    assert(!err);

    return ret;
}


int main(int argc, char **argv)
{
#ifdef DPI_AWARE
    SetProcessDPIAware();
#endif

    wchar_t *path = argc > 1 ? to_wchar(argv[1]) : NULL;
    int ret = wWinMain(GetModuleHandle(NULL), NULL, path, SW_SHOW);

    return ret;
}
