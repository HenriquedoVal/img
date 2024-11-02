#pragma comment(lib, "user32")
#pragma comment(lib, "d2d1")
#pragma comment(lib, "ole32")
#pragma comment(lib, "windowscodecs")

#pragma comment(linker, "/subsystem:console /entry:mainCRTStartup")

#define UNICODE
#define _UNICODE

#include <assert.h>
#include <stdio.h>
#include <math.h>

#include <Windows.h>
#include <wincodec.h>

#include "d2d1_c.h"
#include "d2d1_helpers.c"

#define DEFAULT_MULTIFRAME_INTERVAL_MS 90
#define DPI_AWARE


typedef struct {
    int screen_width;
    int screen_height;
    float max_img_width;
    float max_img_height;
} SystemInfo;

typedef struct {
    IWICBitmapDecoder *decoder;
    IWICFormatConverter *wic_bitmap;
    FLOAT width;
    FLOAT height;
    FLOAT offset_x;
    FLOAT offset_y;
    UINT frame_count;
    UINT current_frame;
    UINT frame_delay;
} Image;

static SystemInfo g_sys = {0};
static Image g_img = {0};
static IWICImagingFactory *g_imaging_factory = NULL;


HRESULT set_decoder_from_file(PCWSTR path)
{
    HRESULT hr = g_imaging_factory->lpVtbl->CreateDecoderFromFilename(
            g_imaging_factory,
            path,
            NULL,
            GENERIC_READ,
            WICDecodeMetadataCacheOnLoad,
            &g_img.decoder
    );
    if (FAILED(hr)) return hr;

    g_img.frame_delay = DEFAULT_MULTIFRAME_INTERVAL_MS;
    hr = g_img.decoder->lpVtbl->GetFrameCount(g_img.decoder, &g_img.frame_count);
    return hr;
}


HRESULT set_current_frame(void)
{
    if (g_img.wic_bitmap) {
        g_img.wic_bitmap->lpVtbl->Release(g_img.wic_bitmap);
    }

    IWICBitmapFrameDecode *frame = NULL;
    HRESULT hr = g_img.decoder->lpVtbl->GetFrame(g_img.decoder, g_img.current_frame, &frame);
    if (FAILED(hr)) return hr;

    if (g_img.frame_count) {

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
                if (frame_delay) g_img.frame_delay = frame_delay;
                PropVariantClear(&prop);

                hr = meta->lpVtbl->GetMetadataByName(meta, L"/imgdesc/Left", &prop);
                if (FAILED(hr) || prop.vt != VT_UI2) return -1;
                UINT left = prop.uiVal;
                PropVariantClear(&prop);
                g_img.offset_x = (FLOAT)left;

                hr = meta->lpVtbl->GetMetadataByName(meta, L"/imgdesc/Top", &prop);
                if (FAILED(hr) || prop.vt != VT_UI2) return -1;
                UINT top = prop.uiVal;
                PropVariantClear(&prop);
                g_img.offset_y = (FLOAT)top;
            }

            meta->lpVtbl->Release(meta);
        }
    }

    // Convert the image format to 32bppPBGRA
    // (DXGI_FORMAT_B8G8R8A8_UNORM + D2D1_ALPHA_MODE_PREMULTIPLIED).
    IWICFormatConverter *converter = NULL;
    hr = g_imaging_factory->lpVtbl->CreateFormatConverter(g_imaging_factory, &converter);
    if (FAILED(hr)) return hr;

    hr = converter->lpVtbl->Initialize(
            converter,
            (IWICBitmapSource *)frame,
            &GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,
            NULL,
            0.f,
            WICBitmapPaletteTypeMedianCut
    );
    if (FAILED(hr)) return hr;
    frame->lpVtbl->Release(frame);

    UINT width, height;
    hr = converter->lpVtbl->GetSize(converter, &width, &height);
    if (FAILED(hr)) return hr;

    if (width > g_sys.max_img_width || height > g_sys.max_img_height) {
        float factor = min(g_sys.max_img_width / width, g_sys.max_img_height / height);
        width = (UINT)(width * factor);
        height = (UINT)(height * factor);
    }

    g_img.wic_bitmap = converter;
    g_img.width = (FLOAT)width;
    g_img.height = (FLOAT)height;

    return hr;
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
                    g_img.wic_bitmap,
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
                g_img.offset_x,
                g_img.offset_y,
                g_img.width + g_img.offset_x,
                g_img.height + g_img.offset_y
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

            if (g_img.frame_count > 1) {
                g_img.current_frame++;
                g_img.current_frame %= g_img.frame_count;
                hr = set_current_frame();
                if (FAILED(hr)) return -1;
                SetTimer(window, timer_event_id, g_img.frame_delay, NULL);
            }

            return 0;

        case WM_TIMER:
            InvalidateRect(window, NULL, FALSE);
            return 0;

        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT) {
                SetCursor(NULL);
                return 0;
            }
            break;

        case WM_KEYDOWN:
            int key = (int)wparam;
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


static char *get_error_string(unsigned id_or_hr, LPCVOID module)
{
    LPSTR win32_buffer = NULL;

    unsigned dwFlags = 
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS;

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
            (void **)&g_imaging_factory
    );
    if (FAILED(hr)) {
        printf("Error: Could not initialize COM WIC. OS msg: %s",
               get_error_string(hr, GetModuleHandle(L"ole32")));
        return hr;
    }

    g_sys.screen_width  = GetSystemMetrics(SM_CXSCREEN);
    g_sys.screen_height = GetSystemMetrics(SM_CYSCREEN);
    if (!g_sys.screen_width || !g_sys.screen_height) {
        printf("%s\n", "Error: Could not get screen size.\n");
        return -1;
    }

    DWORD style = WS_VISIBLE | WS_POPUP;
    DWORD ex_style = 0;

    const wchar_t class_name[] = L"Simple Image Viewer";

    WNDCLASS wc = {
        .lpfnWndProc = WindowProc,
        .hInstance = instance,
        .lpszClassName = class_name
    };

    RegisterClass(&wc);

    HWND window = CreateWindowEx(
        ex_style,
        class_name,
        class_name,
        style,
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

    PCWSTR path = cmd_line;
    hr = set_decoder_from_file(path);
    if (FAILED(hr)) {
        printf("Error: Could not decode image. OS msg: %s",
               get_error_string(hr, GetModuleHandle(L"ole32")));
        return hr;
    }

    UINT dpi = GetDpiForWindow(window);

    // Max img size is 85% of screen with dpi correction
    g_sys.max_img_width  = g_sys.screen_width  * .85f * 96.f / dpi;
    g_sys.max_img_height = g_sys.screen_height * .85f * 96.f / dpi;

    hr = set_current_frame();
    if (FAILED(hr)) {
        printf("Error: Could not get frame of image. OS msg: %s",
               get_error_string(hr, GetModuleHandle(L"ole32")));
        return hr;
    }

    RECT rect = { .right = (LONG)g_img.width, .bottom = (LONG)g_img.height };

#ifdef DPI_AWARE

    if (!AdjustWindowRectExForDpi(&rect, style, FALSE, ex_style, dpi)) {
        printf("Error: Could not create window. OS msg: %s",
               get_error_string(GetLastError(), NULL));
        return -1;
    }

    int window_width  = (int)ceil((rect.right - rect.left) * dpi / 96.f);
    int window_height = (int)ceil((rect.bottom - rect.top) * dpi / 96.f);

#else

    if (!AdjustWindowRectEx(&rect, style, FALSE, ex_style)) {
        printf("Error: Could not create window. OS msg: %s",
               get_error_string(GetLastError(), NULL));
        return -1;
    }

    int window_width  = rect.right - rect.left;
    int window_height = rect.bottom - rect.top;

#endif

    int window_x = g_sys.screen_width  / 2 - window_width  / 2;
    int window_y = g_sys.screen_height / 2 - window_height / 2;

    SetWindowPos(
            window,
            (HWND)0,
            window_x,
            window_y,
            window_width,
            window_height,
            SWP_SHOWWINDOW
    );

    if (ex_style == WS_EX_LAYERED) {
        SetLayeredWindowAttributes(window, 0, 0, LWA_COLORKEY);  // LWA_ALPHA
    }

    ShowWindow(window, cmd_show);

    MSG msg = {0};
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_img.decoder->lpVtbl->Release(g_img.decoder);
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

    if (argc <= 1) {
        printf("Error: no input image is provided\n");
        return 1;
    }

    if (argc > 2) {
        printf("Warning: only {%s} will be evaluated\n", argv[1]);
    }

    wchar_t *path = to_wchar(argv[1]);
    int ret = wWinMain(GetModuleHandle(NULL), NULL, path, 10);

    return ret;
}
