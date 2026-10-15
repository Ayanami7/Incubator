#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#ifndef GLFW_EXPOSE_NATIVE_WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#endif
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

#include "runtime/core/error/error.h"
#include "runtime/platform/window/window.h"

namespace WindowCapture
{
    struct Image
    {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> rgba;
    };

    // Captures the visible client area after presentation; this is display evidence,
    // while the offscreen texture review remains responsible for exact color values.
    inline Image capture(Incubator::Window::Window& window)
    {
        const auto fail = [](const char* operation) {
            throw Incubator::Error::Exception(
                Incubator::Error::Code::RENDER,
                fmt::format("Window capture {} failed with Win32 error {}", operation, GetLastError()));
        };

        struct Resources
        {
            HDC screenDc = nullptr;
            HDC memoryDc = nullptr;
            HBITMAP bitmap = nullptr;
            HGDIOBJ previous = nullptr;

            ~Resources()
            {
                if (previous)
                    SelectObject(memoryDc, previous);
                if (bitmap)
                    DeleteObject(bitmap);
                if (memoryDc)
                    DeleteDC(memoryDc);
                if (screenDc)
                    ReleaseDC(nullptr, screenDc);
            }
        } resources;

        const auto native = glfwGetWin32Window(static_cast<GLFWwindow*>(window.getNativeHandle()));
        if (!SetWindowPos(native, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE))
            fail("SetWindowPos");
        RECT bounds{};
        if (!GetClientRect(native, &bounds))
            fail("GetClientRect");
        POINT origin{};
        if (!ClientToScreen(native, &origin))
            fail("ClientToScreen");

        Image image;
        image.width = bounds.right - bounds.left;
        image.height = bounds.bottom - bounds.top;
        if (image.width <= 0 || image.height <= 0)
            throw Incubator::Error::Exception(Incubator::Error::Code::RENDER,
                                              "Window capture requires a nonempty client area");
        image.rgba.resize(static_cast<size_t>(image.width) * image.height * 4);

        // Flip-model Vulkan presentation may leave a window DC's redirection surface stale.
        // Read the composed desktop at the unobstructed client's screen coordinates instead.
        resources.screenDc = GetDC(nullptr);
        if (!resources.screenDc)
            fail("GetDC");
        resources.memoryDc = CreateCompatibleDC(resources.screenDc);
        if (!resources.memoryDc)
            fail("CreateCompatibleDC");
        resources.bitmap = CreateCompatibleBitmap(resources.screenDc, image.width, image.height);
        if (!resources.bitmap)
            fail("CreateCompatibleBitmap");
        const auto previous = SelectObject(resources.memoryDc, resources.bitmap);
        if (!previous || previous == HGDI_ERROR)
            fail("SelectObject");
        resources.previous = previous;
        if (!BitBlt(resources.memoryDc, 0, 0, image.width, image.height, resources.screenDc, origin.x, origin.y,
                    SRCCOPY | CAPTUREBLT))
            fail("BitBlt");

        // GetDIBits requires the bitmap to be deselected from the memory DC.
        const auto selected = SelectObject(resources.memoryDc, resources.previous);
        if (!selected || selected == HGDI_ERROR)
            fail("SelectObject restore");
        resources.previous = nullptr;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = image.width;
        info.bmiHeader.biHeight = -image.height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        if (GetDIBits(resources.memoryDc, resources.bitmap, 0, static_cast<UINT>(image.height), image.rgba.data(),
                      &info, DIB_RGB_COLORS) != image.height)
            fail("GetDIBits");

        for (size_t offset = 0; offset < image.rgba.size(); offset += 4)
        {
            std::swap(image.rgba[offset], image.rgba[offset + 2]);
            image.rgba[offset + 3] = 255;
        }
        return image;
    }
    // Geometry comparisons use the displayed clear color; exact color checks remain offscreen.
    inline std::array<int, 4> bounds(const Image& image, int left = 8, int right = -1)
    {
        std::array<int, 4> rect{image.width, image.height, -1, -1};
        // Exclude desktop pixels in Windows' rounded client corners.
        for (int y = 8; y < image.height - 8; ++y)
        {
            for (int x = left; x < (right < 0 ? image.width - 8 : right); ++x)
            {
                const size_t offset = (static_cast<size_t>(y) * image.width + x) * 4;
                int difference = 0;
                for (int channel = 0; channel < 3; ++channel)
                    difference += std::abs(int(image.rgba[offset + channel]) - int(image.rgba[channel]));
                if (difference > 40)
                {
                    rect[0] = std::min(rect[0], x);
                    rect[1] = std::min(rect[1], y);
                    rect[2] = std::max(rect[2], x);
                    rect[3] = std::max(rect[3], y);
                }
            }
        }
        return rect;
    }

    inline void save(const Image& image, const std::filesystem::path& path)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        output << "P6\n" << image.width << ' ' << image.height << "\n255\n";
        for (size_t pixel = 0; pixel < image.rgba.size(); pixel += 4)
            output.write(reinterpret_cast<const char*>(image.rgba.data() + pixel), 3);
        if (!output.good())
            throw Incubator::Error::Exception(Incubator::Error::Code::RENDER, "Window capture could not be saved");
    }
}  // namespace WindowCapture
