// Build-time tool: writes the application .ico from the same renderer the
// tray uses, so the artwork has a single source and no binary lives in git.

#include "icons.h"

#include <cstdio>
#include <vector>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        fwprintf(stderr, L"usage: icongen <out.ico>\n");
        return 1;
    }

    const int sizes[] = {16, 20, 24, 32, 40, 48, 64, 256};
    const int count = static_cast<int>(sizeof(sizes) / sizeof(sizes[0]));

    std::vector<std::vector<BYTE>> images;
    for (int size : sizes) {
        std::vector<uint32_t> px;
        RenderStateIcon(IconState::On, size, px);

        // DIB entry: header (double height for XOR+AND), bottom-up BGRA, empty AND mask.
        BITMAPINFOHEADER bih = {};
        bih.biSize = sizeof(bih);
        bih.biWidth = size;
        bih.biHeight = size * 2;
        bih.biPlanes = 1;
        bih.biBitCount = 32;
        bih.biCompression = BI_RGB;
        const int maskStride = ((size + 31) / 32) * 4;

        std::vector<BYTE> img(sizeof(bih) + size * size * 4 + maskStride * size, 0);
        memcpy(img.data(), &bih, sizeof(bih));
        BYTE* dst = img.data() + sizeof(bih);
        for (int y = 0; y < size; ++y)
            memcpy(dst + (size - 1 - y) * size * 4, &px[y * size], size * 4);
        images.push_back(std::move(img));
    }

#pragma pack(push, 1)
    struct IconDir { WORD reserved, type, count; };
    struct IconDirEntry { BYTE w, h, colors, reserved; WORD planes, bpp; DWORD bytes, offset; };
#pragma pack(pop)

    std::vector<BYTE> file;
    auto append = [&](const void* data, size_t size) {
        const BYTE* p = static_cast<const BYTE*>(data);
        file.insert(file.end(), p, p + size);
    };

    IconDir dir = {0, 1, static_cast<WORD>(count)};
    append(&dir, sizeof(dir));
    DWORD offset = sizeof(IconDir) + count * sizeof(IconDirEntry);
    for (int i = 0; i < count; ++i) {
        BYTE dim = static_cast<BYTE>(sizes[i] >= 256 ? 0 : sizes[i]);
        IconDirEntry e = {dim, dim, 0, 0, 1, 32, static_cast<DWORD>(images[i].size()), offset};
        append(&e, sizeof(e));
        offset += static_cast<DWORD>(images[i].size());
    }
    for (const auto& img : images) append(img.data(), img.size());

    HANDLE f = CreateFileW(argv[1], GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    bool ok = f != INVALID_HANDLE_VALUE &&
              WriteFile(f, file.data(), static_cast<DWORD>(file.size()), &written, nullptr) && written == file.size();
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (!ok) {
        fwprintf(stderr, L"cannot write %ls\n", argv[1]);
        return 1;
    }
    return 0;
}
